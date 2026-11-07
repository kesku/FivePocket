// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/gamepad_input.h"
#include "apple/recorded_dpad_policy.h"
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace recomp::apple {
namespace {
void requireMain() {
    if (![NSThread isMainThread]) {
        std::fputs("APPLE_GAMEPAD_FATAL non_main_thread_lifecycle_or_event=1\n",stderr);std::_Exit(4);
    }
}
using Buttons=std::array<GCControllerButtonInput*,gamepad::ButtonCount>;
Buttons controls(GCExtendedGamepad* p) {
    return {p.dpad.up,p.dpad.down,p.dpad.left,p.dpad.right,p.buttonMenu,p.buttonOptions,
            p.leftThumbstickButton,p.rightThumbstickButton,p.leftShoulder,p.rightShoulder,p.buttonHome,
            p.buttonA,p.buttonB,p.buttonX,p.buttonY};
}
gamepad::Normalized read(GCExtendedGamepad* p) {
    gamepad::Normalized state;
    const auto b=controls(p);
    for (unsigned i=0;i<b.size();++i) state.buttons[i]=b[i].pressed;
    state.leftX=p.leftThumbstick.xAxis.value;state.leftY=p.leftThumbstick.yAxis.value;
    state.rightX=p.rightThumbstick.xAxis.value;state.rightY=p.rightThumbstick.yAxis.value;
    state.leftTrigger=p.leftTrigger.value;state.rightTrigger=p.rightTrigger.value;
    state.leftTriggerPressed=p.leftTrigger.pressed;state.rightTriggerPressed=p.rightTrigger.pressed;
    return state;
}
gamepad::Supported support(GCExtendedGamepad* p) {
    gamepad::Supported state;
    const auto b=controls(p);
    for (unsigned i=0;i<b.size();++i) state.buttons[i]=b[i]!=nil;
    state.leftX=p.leftThumbstick.xAxis!=nil;state.leftY=p.leftThumbstick.yAxis!=nil;
    state.rightX=p.rightThumbstick.xAxis!=nil;state.rightY=p.rightThumbstick.yAxis!=nil;
    state.leftTrigger=p.leftTrigger!=nil;state.rightTrigger=p.rightTrigger!=nil;
    return state;
}
bool buttonEvent(GCExtendedGamepad* profile,GCControllerElement* element) {
    if (element==profile.dpad || element==profile.dpad.xAxis || element==profile.dpad.yAxis ||
        element==profile.leftTrigger || element==profile.rightTrigger) return true;
    for (auto button:controls(profile)) if (button && button==element) return true;
    return false;
}
std::uint64_t identity(GCController* controller) {return reinterpret_cast<std::uintptr_t>((__bridge void*)controller);}
}

struct AppleGamepadInput::Impl {
    struct Gate {Impl* owner;};
    struct Entry {
        GCController* controller;
        GCExtendedGamepad* profile;
        GCExtendedGamepadValueChangedHandler handler;
        gamepad::Normalized state;
        ~Entry() {
            // Another owner replacing this handler does not give us authority
            // to erase theirs during disconnect/destruction.
            if (profile.valueChangedHandler==handler) profile.valueChangedHandler=nil;
        }
    };
    gamepad::Published published;
    gamepad::EventChannel events;
    gamepad::RecordedDpadPolicy recorded;
    id recordedController=nil;
    std::shared_ptr<Gate> gate=std::make_shared<Gate>(Gate{this});
    std::vector<std::unique_ptr<Entry>> entries;
    std::vector<gamepad::Candidate> candidates;
    std::uint64_t chosen=0;
    id connectedObserver=nil,disconnectedObserver=nil;

    Impl() {
        requireMain();
        std::weak_ptr<Gate> weak=gate;
        void (^changed)(NSNotification*)=^(NSNotification*) {
            requireMain();
            if (auto lock=weak.lock();lock && lock->owner) lock->owner->refresh();
        };
        try {
            connectedObserver=[NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:changed];
            disconnectedObserver=[NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:changed];
            refresh();
        } catch(...) {
            // Failed construction skips ~Impl(), but the notification center
            // still owns these registrations. Expire queued callbacks first.
            gate->owner=nullptr;
            if (connectedObserver) [NSNotificationCenter.defaultCenter removeObserver:connectedObserver];
            if (disconnectedObserver) [NSNotificationCenter.defaultCenter removeObserver:disconnectedObserver];
            throw;
        }
    }
    ~Impl() {
        requireMain();gate->owner=nullptr;
        [NSNotificationCenter.defaultCenter removeObserver:connectedObserver];
        [NSNotificationCenter.defaultCenter removeObserver:disconnectedObserver];
        entries.clear();published.publish(false,{},{});gate.reset();
    }
    bool recordedAvailable() const {
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
        auto* const virtualController=(GCVirtualController*)recordedController;
        if (!recorded.owner() || !virtualController || !virtualController.controller) return false;
        for (const auto& e:entries) if (identity(e->controller)==recorded.owner())
            return virtualController.controller==e->controller && !e->controller.snapshot &&
                [GCController.controllers containsObject:e->controller] &&
                e->controller.extendedGamepad==e->profile &&
                e->profile.valueChangedHandler==e->handler &&
                e->controller.handlerQueue==dispatch_get_main_queue();
#endif
        return false;
    }
    void publish() {
        // Reuse storage across high-rate stick events. Refresh every candidate
        // value, and finish selection before invoking any reentrant observer.
        candidates.clear();
        candidates.reserve(entries.size());
        for (const auto& e:entries) candidates.push_back({identity(e->controller),e->controller.attachedToDevice?3u:2u});
        chosen=gamepad::choose(chosen,candidates);
        if (recorded.reconcile(chosen,recordedAvailable())) {
            recordedController=nil;
            std::fputs("APPLE_GAMEPAD_RECORDED_DPAD_INVALIDATED physical_input=0\n",stderr);
        }
        for (const auto& e:entries) if (identity(e->controller)==chosen) {
            auto state=gamepad::map(e->state);
            auto caps=gamepad::capabilities(support(e->profile));
            state.buttons|=recorded.bits(chosen);
            caps.buttons|=recorded.capabilities(chosen);
            published.publish(true,state,caps);
            recomp::input::GamepadState snapshot{};published.read(0,&snapshot);
            events.publish(chosen,true,snapshot);return;
        }
        published.publish(false,{},{});
        events.publish(0,false,{});
    }
    void refresh() {
        requireMain();
        NSArray<GCController*>* controllers=GCController.controllers;
        std::erase_if(entries,[&](const auto& e){
            return ![controllers containsObject:e->controller] || e->controller.snapshot ||
                e->controller.extendedGamepad!=e->profile || e->profile.valueChangedHandler!=e->handler ||
                e->controller.handlerQueue!=dispatch_get_main_queue();
        });
        for (GCController* controller in controllers) {
            if (controller.snapshot || !controller.extendedGamepad) continue;
            bool exists=false;for (const auto& e:entries) if (e->controller==controller) exists=true;
            if (exists) continue;
            GCExtendedGamepad* profile=controller.extendedGamepad;
            // Avoid taking callbacks/queue policy away from another owner.
            if (profile.valueChangedHandler || controller.handlerQueue!=dispatch_get_main_queue()) {
                std::fputs("APPLE_GAMEPAD_UNAVAILABLE existing_handler_or_non_main_queue=1\n",stderr);continue;
            }
            auto entry=std::make_unique<Entry>();entry->controller=controller;entry->profile=profile;entry->state=read(profile);
            std::weak_ptr<Gate> weak=gate;
            entry->handler=^(GCExtendedGamepad* source,GCControllerElement* element) {
                requireMain();
                if (auto lock=weak.lock();lock && lock->owner) lock->owner->event(source,element);
            };
            profile.valueChangedHandler=entry->handler;
            entries.push_back(std::move(entry));
        }
        publish();
    }
    void event(GCExtendedGamepad* profile,GCControllerElement* element) {
        requireMain();
        for (auto& entry:entries) if (entry->profile==profile) {
            // A queued callback may outlive our ownership of this profile.
            // Apply the same ownership checks as refresh before consuming it.
            const bool actual=!entry->controller.snapshot && [GCController.controllers containsObject:entry->controller] &&
                entry->controller.extendedGamepad==entry->profile &&
                entry->profile.valueChangedHandler==entry->handler &&
                entry->controller.handlerQueue==dispatch_get_main_queue();
            if (!actual) {refresh();return;}
            const auto state=read(profile);
            if (gamepad::adopts(chosen,identity(entry->controller),actual,buttonEvent(profile,element),
                                gamepad::pressed(entry->state),gamepad::pressed(state))) chosen=identity(entry->controller);
            entry->state=state;publish();return;
        }
    }
    bool recordedActive(std::uint64_t generation) {
        requireMain();refresh();return recorded.active(generation);
    }
    bool setRecorded(std::uint64_t generation,std::uint16_t bits) {
        requireMain();refresh();
        const bool accepted=recorded.set(generation,bits);
        publish();
        std::fprintf(stderr,"APPLE_GAMEPAD_RECORDED_DPAD_SET accepted=%u bits=%04x physical_input=0\n",
            unsigned(accepted),recorded.bits(chosen));
        return accepted;
    }
    void closeRecorded(std::uint64_t generation) {
        requireMain();
        if (!recorded.close(generation)) return;
        recordedController=nil;publish();
        std::fputs("APPLE_GAMEPAD_RECORDED_DPAD_CLOSED physical_input=0\n",stderr);
    }
};

RecordedDpadLease::RecordedDpadLease(std::function<bool()> active,std::function<bool(std::uint16_t)> set,
                                     std::function<void()> close)
    :active_(std::move(active)),set_(std::move(set)),close_(std::move(close)) {}
RecordedDpadLease::~RecordedDpadLease() {close();}
bool RecordedDpadLease::active() const noexcept {requireMain();return !closed_ && active_();}
bool RecordedDpadLease::set(std::uint16_t bits) noexcept {requireMain();return !closed_ && set_(bits);}
void RecordedDpadLease::close() noexcept {
    requireMain();if (closed_)return;closed_=true;close_();
}

AppleGamepadInput::AppleGamepadInput() {requireMain();impl_=std::make_unique<Impl>();}
AppleGamepadInput::~AppleGamepadInput() {requireMain();}
int AppleGamepadInput::snapshot(std::uint32_t user,recomp::input::GamepadState* result) noexcept {return impl_->published.read(user,result);}
int AppleGamepadInput::capabilities(std::uint32_t user,recomp::input::GamepadState* result) noexcept {return impl_->published.read(user,result,true);}
std::unique_ptr<gamepad::EventObservation> AppleGamepadInput::observe(gamepad::EventObserver callback,void* user) {
    requireMain();auto observation=impl_->events.observe(callback,user);
    if(observation)impl_->publish(); // Initial presence, not a synthetic button event.
    return observation;
}
std::unique_ptr<RecordedDpadLease> AppleGamepadInput::recordedDpad(void* bridgedGCVirtualController) {
    requireMain();impl_->refresh();
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    auto* const controller=(__bridge GCVirtualController*)bridgedGCVirtualController;
    if (!controller || ![controller isKindOfClass:GCVirtualController.class] ||
        !controller.controller || controller.controller.snapshot || impl_->recorded.owner()) return {};
    const auto owner=identity(controller.controller);
    if (owner!=impl_->chosen) return {};
    impl_->recordedController=controller;
    // Validate the actual connected profile and callback before issuing a token.
    bool available=false;
    for (const auto& e:impl_->entries) if (e->controller==controller.controller)
        available=[GCController.controllers containsObject:e->controller] &&
            e->controller.extendedGamepad==e->profile && e->profile.valueChangedHandler==e->handler &&
            e->controller.handlerQueue==dispatch_get_main_queue();
    const auto generation=impl_->recorded.acquire(owner,impl_->chosen,available);
    if (!generation) {impl_->recordedController=nil;return {};}
    std::weak_ptr<Impl::Gate> weak=impl_->gate;
    auto active=[weak,generation] {
        requireMain();const auto gate=weak.lock();
        return gate && gate->owner && gate->owner->recordedActive(generation);
    };
    auto set=[weak,generation](std::uint16_t bits) {
        requireMain();const auto gate=weak.lock();
        return gate && gate->owner && gate->owner->setRecorded(generation,bits);
    };
    auto close=[weak,generation] {
        requireMain();const auto gate=weak.lock();
        if (gate && gate->owner)gate->owner->closeRecorded(generation);
    };
    std::unique_ptr<RecordedDpadLease> lease;
    try {
        lease.reset(new RecordedDpadLease(std::move(active),std::move(set),std::move(close)));
    } catch(...) {
        // No state has been published yet. Allocation of the lease or its
        // std::function callbacks must not strand unowned recorded input.
        impl_->recorded.close(generation);
        impl_->recordedController=nil;
        throw;
    }
    impl_->publish();
    std::fprintf(stderr,"APPLE_GAMEPAD_RECORDED_DPAD_BOUND owner=%llx physical_input=0\n",
        static_cast<unsigned long long>(owner));
    return lease;
#else
    (void)bridgedGCVirtualController;return {};
#endif
}
}
