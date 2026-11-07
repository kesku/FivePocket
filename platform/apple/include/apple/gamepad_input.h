// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_gamepad_state.h"
#include "apple/gamepad_events.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>

namespace recomp::apple {
namespace gamepad {
// Matches XboxGamepadMapper's 15 named buttons. No OS navigation remapping.
enum Button : unsigned {Up,Down,Left,Right,Start,Back,LeftStick,RightStick,LeftShoulder,RightShoulder,Guide,A,B,X,Y,ButtonCount};
constexpr std::array<std::uint16_t,ButtonCount> ButtonBits{1,2,4,8,16,32,64,128,256,512,1024,4096,8192,16384,32768};
struct Normalized {
    std::array<bool,ButtonCount> buttons{};
    // Values already calibrated/deadzoned by GameController; Y is up-positive.
    float leftX=0,leftY=0,rightX=0,rightY=0,leftTrigger=0,rightTrigger=0;
    bool leftTriggerPressed=false,rightTriggerPressed=false;
};
struct Supported {
    std::array<bool,ButtonCount> buttons{};
    bool leftX=false,leftY=false,rightX=false,rightY=false,leftTrigger=false,rightTrigger=false;
};
inline std::int16_t stick(float value) noexcept {
    if (!std::isfinite(value)) return 0;
    value=std::clamp(value,-1.0f,1.0f);
    // Java Math.round(float) ties toward positive infinity, unlike std::lround.
    const float scaled=value*(value<0?32768.0f:32767.0f);
    return static_cast<std::int16_t>(std::floor(double(scaled)+0.5));
}
inline std::uint8_t trigger(float value) noexcept {
    if (!std::isfinite(value)) return 0;
    return static_cast<std::uint8_t>(std::floor(double(std::clamp(value,0.0f,1.0f)*255.0f)+0.5));
}
inline std::uint16_t buttons(const std::array<bool,ButtonCount>& values) noexcept {
    std::uint16_t bits=0;
    for (unsigned i=0;i<ButtonCount;++i) if (values[i]) bits|=ButtonBits[i];
    return bits;
}
inline recomp::input::GamepadState map(const Normalized& pad) noexcept {
    recomp::input::GamepadState result;
    result.buttons=buttons(pad.buttons);
#if defined(APPLE_GAMEPAD_MUTANT_BUTTONS)
    result.buttons=static_cast<std::uint16_t>((result.buttons&~0x3000)|((result.buttons&0x1000)<<1)|((result.buttons&0x2000)>>1));
#endif
    result.leftX=stick(pad.leftX);result.leftY=stick(pad.leftY);
    result.rightX=stick(pad.rightX);result.rightY=stick(pad.rightY);
#if defined(APPLE_GAMEPAD_MUTANT_Y)
    result.leftY=stick(-pad.leftY);result.rightY=stick(-pad.rightY);
#endif
    result.leftTrigger=trigger(pad.leftTrigger);result.rightTrigger=trigger(pad.rightTrigger);
    return result;
}
inline recomp::input::GamepadState capabilities(const Supported& pad) noexcept {
    recomp::input::GamepadState result;
    result.buttons=buttons(pad.buttons);
    result.leftX=pad.leftX?32767:0;result.leftY=pad.leftY?32767:0;
    result.rightX=pad.rightX?32767:0;result.rightY=pad.rightY?32767:0;
    result.leftTrigger=pad.leftTrigger?255:0;result.rightTrigger=pad.rightTrigger?255:0;
    return result;
}
inline std::uint32_t pressed(const Normalized& pad) noexcept {
    return buttons(pad.buttons)|(pad.leftTriggerPressed?0x10000u:0)|(pad.rightTriggerPressed?0x20000u:0);
}
struct Candidate {std::uint64_t id;unsigned rank;};
inline std::uint64_t choose(std::uint64_t current,std::span<const Candidate> candidates) noexcept {
    std::uint64_t best=0;unsigned rank=0;
    for (const auto candidate:candidates) {
        if (!candidate.id || !candidate.rank) continue;
        if (candidate.id==current) return current;
        if (candidate.rank>rank) {best=candidate.id;rank=candidate.rank;}
    }
    return best;
}
inline bool adopts(std::uint64_t current,std::uint64_t eventDevice,bool physical,bool buttonEvent,
                   std::uint32_t before,std::uint32_t after) noexcept {
#if defined(APPLE_GAMEPAD_MUTANT_CHOICE)
    (void)buttonEvent;(void)before;(void)after;
    return physical && eventDevice && eventDevice!=current;
#else
    return physical && buttonEvent && eventDevice && eventDevice!=current && (after&~before)!=0;
#endif
}
// This model is testable without injecting an event into GameController or the
// game. The production adapter exclusively publishes actual connected profiles.
class Published final {
public:
    void publish(bool connected,recomp::input::GamepadState next,recomp::input::GamepadState supported) {
        std::lock_guard lock(mutex_);
        if (!connected) {next={};supported={};}
        next.packet=state_.packet;
        if (connected!=connected_ || recomp::input::encodeGamepadState(next)!=recomp::input::encodeGamepadState(state_)) ++next.packet;
        state_=next;supported_=supported;connected_=connected;
    }
    int read(std::uint32_t user,recomp::input::GamepadState* result,bool caps=false) noexcept {
        if (!result) return -1;
        *result={};
        std::lock_guard lock(mutex_);
        if (user || !connected_) return 0;
        *result=caps?supported_:state_;
        return 1;
    }
private:
    std::mutex mutex_;
    bool connected_=false;
    recomp::input::GamepadState state_{},supported_{};
};
}

// Explicitly synthetic, development-only D-pad supplement. Every method and
// destruction belongs to the main thread. Closing clears only this lease;
// disconnect, selection change or adapter destruction permanently expires it.
class RecordedDpadLease final {
public:
    ~RecordedDpadLease();
    RecordedDpadLease(const RecordedDpadLease&)=delete;
    RecordedDpadLease& operator=(const RecordedDpadLease&)=delete;
    bool active() const noexcept;
    bool set(std::uint16_t lowFourBits) noexcept;
    void close() noexcept;
private:
    friend class AppleGamepadInput;
    friend struct RecordedDpadLeaseTestAccess;
    RecordedDpadLease(std::function<bool()> active,std::function<bool(std::uint16_t)> set,
                      std::function<void()> close);
    std::function<bool()> active_;
    std::function<bool(std::uint16_t)> set_;
    std::function<void()> close_;
    bool closed_=false;
};

// Construct/destroy on the main thread; stop guest callbacks before destruction.
// snapshot/capabilities may run on any guest thread and never call Objective-C.
// The main run loop must be serviced. One adapter owns otherwise-unclaimed
// GameController profiles; pre-existing handlers/non-main handler queues are
// not overwritten. No wireless discovery or virtual-pad creation. The explicit
// recordedDpad lease below is the only synthetic supplement.
class AppleGamepadInput final {
public:
    AppleGamepadInput();
    ~AppleGamepadInput();
    AppleGamepadInput(const AppleGamepadInput&)=delete;
    AppleGamepadInput& operator=(const AppleGamepadInput&)=delete;
    int snapshot(std::uint32_t user,recomp::input::GamepadState* result) noexcept;
    int capabilities(std::uint32_t user,recomp::input::GamepadState* result) noexcept;
    // Main-thread, exclusive observational listener. Every adapter publication
    // arrives immediately, including short taps. Never replaces GC handlers or
    // changes guest snapshots. Token expires when this adapter is destroyed.
    std::unique_ptr<gamepad::EventObservation> observe(gamepad::EventObserver callback,void* user);
    // Main-thread opt-in. The argument must bridge a live GCVirtualController,
    // not its GCController. Requires its exact selected, connected controller
    // and our unmodified handler. Never takes selection from another controller.
    std::unique_ptr<RecordedDpadLease> recordedDpad(void* bridgedGCVirtualController);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
