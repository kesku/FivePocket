// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_gamepad_state.h"
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <thread>

namespace recomp::apple::gamepad {
// An event observer of the existing adapter, NOT a second GCController handler
// or a guest-state setter. Lifecycle and notifications belong to its main thread.
using EventObserver = void(*)(void*,std::uint64_t,bool,const recomp::input::GamepadState&) noexcept;
struct EventSlot final {
    const std::thread::id thread=std::this_thread::get_id();
    EventObserver callback=nullptr;
    void* user=nullptr;
    bool alive=true;
    std::uint64_t generation=0;
};
class EventObservation final {
public:
    EventObservation(std::weak_ptr<EventSlot> slot,std::uint64_t generation):slot_(slot),generation_(generation){}
    EventObservation(const EventObservation&)=delete;
    EventObservation& operator=(const EventObservation&)=delete;
    ~EventObservation() {close();}
    bool active() const noexcept {
        const auto slot=slot_.lock();
        return slot && slot->thread==std::this_thread::get_id() && slot->alive &&
            slot->generation==generation_ && slot->callback;
    }
    void close() noexcept {
        const auto slot=slot_.lock();
        if(!slot)return;
        if(slot->thread!=std::this_thread::get_id()) {
            std::fputs("APPLE_GAMEPAD_EVENTS_FATAL foreign_thread=1\n",stderr);std::_Exit(4);
        }
        if(slot->generation==generation_) {slot->callback=nullptr;slot->user=nullptr;}
    }
private:
    std::weak_ptr<EventSlot> slot_;
    std::uint64_t generation_;
};
class EventChannel final {
public:
    EventChannel()=default;
    EventChannel(const EventChannel&)=delete;
    EventChannel& operator=(const EventChannel&)=delete;
    ~EventChannel() {
        requireOwner();slot_->alive=false;slot_->callback=nullptr;slot_->user=nullptr;
    }
    std::unique_ptr<EventObservation> observe(EventObserver callback,void* user) {
        if(slot_->thread!=std::this_thread::get_id() || !callback || !user || slot_->callback)return {};
        const auto generation=slot_->generation+1;
        // Allocate the closing token before publishing its borrowed user. If
        // allocation throws, the channel must retain no callback to that owner.
        auto observation=std::make_unique<EventObservation>(slot_,generation);
        slot_->generation=generation;slot_->callback=callback;slot_->user=user;
        return observation;
    }
    void publish(std::uint64_t id,bool connected,const recomp::input::GamepadState& state) noexcept {
        requireOwner();
#if !defined(APPLE_GAMEPAD_EVENTS_MUTANT_OMIT)
        // Copy before calling: a listener may close its token during this call.
        const auto callback=slot_->callback;auto* const user=slot_->user;
        if(callback) {
            const bool present=connected && id!=0;
            callback(user,present?id:0,present,present?state:recomp::input::GamepadState{});
        }
#else
        (void)id;(void)connected;(void)state;
#endif
    }
private:
    void requireOwner() const noexcept {if(slot_->thread!=std::this_thread::get_id())std::_Exit(4);}
    std::shared_ptr<EventSlot> slot_=std::make_shared<EventSlot>();
};
}
