// SPDX-License-Identifier: GPL-3.0-or-later
#import "LauncherInputBridge.h"
#include "LauncherInput.hpp"
#include "FullTouchInput.hpp"
#include "AgentMenuDelivery.hpp"
#include <chrono>
#include <cstdio>

using namespace recomp::apple;
using namespace recomp::apple::launcher;
namespace {
std::uint64_t milliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void requireMain() {
    if(!NSThread.isMainThread){std::fputs("APPLE_LAUNCHER_INPUT_FATAL foreign_thread=1\n",stderr);std::_Exit(4);}
}
struct Bridge final {
    __weak PortLauncherInput* owner;
    __weak PortLauncherController* controller;
    Input input;
    std::unique_ptr<gamepad::EventObservation> observation;
    NSUInteger epoch;
    bool closed=false;
    agentmenu::Hooks agentHooks{};
    agentinput::Publication executingAgent{};
    std::uint64_t sourceId=0;
    bool sourceConnected=false;
    recomp::input::GamepadState sourceState{};
    struct EventScope {
        Bridge& bridge;
        agentinput::Publication previous;
        EventScope(Bridge& value,const agentmenu::Receipt& receipt) noexcept:bridge(value),previous(value.executingAgent) {
            bridge.executingAgent=receipt.available?receipt.sample.consumed:agentinput::Publication{};
        }
        ~EventScope(){bridge.executingAgent=previous;}
    };
#if defined(APPLE_LAUNCHER_INPUT_TESTING)
    std::uint64_t eventTime=0;
#endif
    Bridge(PortLauncherInput* value,PortLauncherController* view,const agentmenu::Hooks* hooks=nullptr):owner(value),controller(view),input(emit,this),epoch(view.inputEpoch),agentHooks(hooks?*hooks:agentmenu::Hooks{}){}
    bool prepare() noexcept {
        auto* strong=owner;auto* view=controller;
        if(closed || !strong || !view || view.inputClosed){[strong close];return false;}
        if(epoch!=view.inputEpoch){input.endHold();epoch=view.inputEpoch;}
        if(view.hostBusy){input.suspend();return false;}
        return true;
    }
    static void emit(void* opaque,Command command,bool repeat) noexcept {
        auto& bridge=*static_cast<Bridge*>(opaque);
        // Keep the Bridge owner alive through the final native access, even
        // when a delegate drops its last external reference during a command.
        __attribute__((objc_precise_lifetime)) PortLauncherInput* strong=bridge.owner;
        auto* view=bridge.controller;
        if(!strong || !bridge.prepare())return;
        @try {[view handleCommand:NSInteger(command) repeat:repeat];}
        @catch(NSException*) {[strong close];return;}
        bridge.prepare();
    }
    static void event(void* opaque,std::uint64_t id,bool connected,const recomp::input::GamepadState& state) noexcept {
        requireMain();auto& bridge=*static_cast<Bridge*>(opaque);
        // EventScope restores native state after UI callbacks have returned.
        // ARC must not release this owner at its last Objective-C use above it.
        __attribute__((objc_precise_lifetime)) PortLauncherInput* strong=bridge.owner;
        if(!strong)return;
        bridge.sourceId=id;bridge.sourceConnected=connected;bridge.sourceState=state;
        const bool ready=bridge.prepare();
        if(!ready) {
            // Only an enabled receipt route may consume exact neutral while
            // preparing. Commands remain gated by prepare()/hostBusy. No hook
            // means the original busy path returns here unchanged.
            auto* view=bridge.controller;
            if(!bridge.agentHooks || bridge.closed || !view || view.inputClosed ||
               !view.hostBusy || !agentmenu::exactNeutral(state))return;
            agentmenu::Receipt receipt{};
            bridge.agentHooks.before(bridge.agentHooks.owner,connected,state,receipt);
            EventScope scope(bridge,receipt);
            bridge.input.event(id,connected,state,
#if defined(APPLE_LAUNCHER_INPUT_TESTING)
                bridge.eventTime
#else
                milliseconds()
#endif
            );
            if(!bridge.closed)bridge.agentHooks.after(bridge.agentHooks.owner,receipt,state,true);
            return;
        }
        agentmenu::Receipt receipt{};
        if(bridge.agentHooks)bridge.agentHooks.before(bridge.agentHooks.owner,connected,state,receipt);
        EventScope scope(bridge,receipt);
        // Observe the SAME source event consumed by navigation. Never create
        // a handler, poll the controller, change guest state or claim physical
        // origin from an event that could have come from a virtual controller.
        @try {
#if !defined(APPLE_LAUNCHER_READOUT_MUTANT_OMIT)
            auto* view=bridge.controller;
            const bool present=connected && id!=0;
            view.controllerText=present?[NSString stringWithFormat:
                @"Connected controller event %llu\nButtons 0x%04x · packet %u\nLeft (%d, %d) · right (%d, %d)\nTriggers L %u · R %u\nA Select · B Back · LB/RB Tabs · Start Save\nAdapter event; physical origin unverified.",
                (unsigned long long)id,unsigned(state.buttons),state.packet,
                int(state.leftX),int(state.leftY),int(state.rightX),int(state.rightY),
                unsigned(state.leftTrigger),unsigned(state.rightTrigger)]:
                @"No controller connected.\nTouch navigation remains available.\nA Select · B Back · LB/RB Tabs · Start Save";
#endif
        } @catch(NSException*) {[strong close];return;}
        bridge.input.event(id,connected,state,
#if defined(APPLE_LAUNCHER_INPUT_TESTING)
            bridge.eventTime
#else
            milliseconds()
#endif
        );
        if(bridge.agentHooks && !bridge.closed)
            bridge.agentHooks.after(bridge.agentHooks.owner,receipt,state,true);
    }
};
}
@interface PortLauncherInput () {
    std::unique_ptr<Bridge> _bridge;
    NSTimer* _repeatTimer;
}
- (void)repeatAt:(std::uint64_t)now;
- (void)startRepeatTimer;
@end
@implementation PortLauncherInput
- (instancetype)initWithSource:(AppleGamepadInput*)source controller:(PortLauncherController*)view {
    requireMain();
    if(!source || !view || view.inputClosed)return nil;
    if((self=[super init])) {
        _bridge=std::make_unique<Bridge>(self,view);
        _bridge->observation=source->observe(Bridge::event,_bridge.get());
        if(!_bridge->observation || _bridge->closed){_bridge->observation.reset();return nil;}
        [self startRepeatTimer];
    }
    return self;
}
- (instancetype)initWithAppSource:(fulltouch::Gamepad*)source controller:(PortLauncherController*)view {
    return [self initWithAppSource:source controller:view agentHooks:nullptr];
}
- (instancetype)initWithAppSource:(fulltouch::Gamepad*)source controller:(PortLauncherController*)view agentHooks:(const agentmenu::Hooks*)hooks {
    requireMain();
    if(!source || !view || view.inputClosed)return nil;
    if(hooks && (!bool(*hooks) || hooks->source!=source))return nil;
    if((self=[super init])) {
        _bridge=std::make_unique<Bridge>(self,view,hooks);
        _bridge->observation=source->observe(Bridge::event,_bridge.get());
        if(!_bridge->observation || _bridge->closed){_bridge->observation.reset();return nil;}
        [self startRepeatTimer];
    }
    return self;
}
#if defined(APPLE_LAUNCHER_INPUT_TESTING)
- (instancetype)initWithChannel:(gamepad::EventChannel*)channel controller:(PortLauncherController*)view {
    requireMain();if(!channel || !view || view.inputClosed)return nil;
    if((self=[super init])) {
        _bridge=std::make_unique<Bridge>(self,view);
        _bridge->observation=channel->observe(Bridge::event,_bridge.get());
        if(!_bridge->observation)return nil;
    }
    return self; // No fixture timer; its synthetic deadlines are explicit.
}
- (void)testRepeatAt:(std::uint64_t)now {[self repeatAt:now];}
- (void)testEventAt:(std::uint64_t)now {requireMain();_bridge->eventTime=now;}
#endif
- (void)startRepeatTimer {
    __weak PortLauncherInput* weak=self;
    _repeatTimer=[NSTimer timerWithTimeInterval:1.0/60.0 repeats:YES block:^(NSTimer*){[weak repeatAt:milliseconds()];}];
    [NSRunLoop.mainRunLoop addTimer:_repeatTimer forMode:NSRunLoopCommonModes];
}
- (BOOL)active {return NSThread.isMainThread && _bridge && !_bridge->closed && _bridge->observation && _bridge->observation->active();}
- (agentinput::Publication)executingAgentPublication {
    requireMain();
    if(!_bridge || _bridge->closed || !_bridge->agentHooks)return {};
    return _bridge->executingAgent;
}
- (BOOL)consumeIdleAppSource {
    requireMain();
    if(!self.active || !_bridge->agentHooks || !_bridge->prepare())return NO;
    const auto status=_bridge->agentHooks.source->agentStatus();
    if(!status.active || !status.manualNeutral)return NO;
    recomp::input::GamepadState state{};
    const int connected=_bridge->agentHooks.source->snapshot(0,&state);
    if((connected!=0 && connected!=1) || bool(connected)!=_bridge->sourceConnected ||
       (connected && !_bridge->sourceId) || !agentmenu::exactNeutral(state) ||
       recomp::input::encodeGamepadState(state)!=recomp::input::encodeGamepadState(_bridge->sourceState))return NO;
    // A real UI consumer reread: same observed source identity, exact current
    // native bytes, existing input/prepare/busy gates, then its ordinary hooks.
    // No new physical/virtual controller event or image is claimed.
    // A disconnected neutral sample remains disconnected. The authorized
    // app-owned agent publisher is distinct from physical source presence.
    Bridge::event(_bridge.get(),_bridge->sourceId,connected==1,state);
    return self.active;
}
- (void)repeatAt:(std::uint64_t)now {
    requireMain();
    if(!self.active){[self close];return;}
    if(_bridge->prepare())_bridge->input.tick(now);
}
- (void)close {
    requireMain();[_repeatTimer invalidate];_repeatTimer=nil;
    if(!_bridge)return;
    _bridge->closed=true;_bridge->input.close();_bridge->observation.reset();
}
- (void)dealloc {[self close];}
@end
