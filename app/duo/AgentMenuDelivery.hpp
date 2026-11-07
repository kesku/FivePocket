// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "FullTouchInput.hpp"
#include <atomic>
#include <limits>

namespace recomp::apple::agentmenu {
namespace ai=agentinput;
namespace ft=fulltouch;
struct Receipt {
    ft::AgentReceipt sample{};
    const void* owner=nullptr;
    std::uint64_t identity=0;
    bool available=false;
};
struct Hooks {
    ft::Gamepad* source=nullptr;
    void* owner=nullptr;
    void(*before)(void*,bool,const recomp::input::GamepadState&,Receipt&) noexcept=nullptr;
    void(*after)(void*,Receipt&,const recomp::input::GamepadState&,bool) noexcept=nullptr;
    explicit operator bool() const noexcept {return source && owner && before && after;}
};
inline bool exactNeutral(const recomp::input::GamepadState& state) noexcept {
    return !state.buttons && !state.leftX && !state.leftY && !state.rightX &&
        !state.rightY && !state.leftTrigger && !state.rightTrigger;
}
enum class EnvironmentEvent { SourceSample, MenuConsumed };
inline std::uint64_t allocateIdentity(std::atomic<std::uint64_t>& next) noexcept {
    auto value=next.load(std::memory_order_relaxed);
    while(value && value!=std::numeric_limits<std::uint64_t>::max()) {
        if(next.compare_exchange_weak(value,value+1,std::memory_order_relaxed))return value;
    }
    return 0;
}

// MAIN actor only. Caller supplies real Menu UI-consumption revision after an
// admitted event, never guest frames or a timer/ACK counter. This receipt is NOT
// a guest-memory-copy acknowledgement; that has its separate downstream path.
class Adapter final {
public:
    using Environment=bool(*)(void*,ai::Environment&) noexcept;
    using Event=void(*)(void*,EnvironmentEvent) noexcept;
    Adapter(ft::Gamepad& source,Environment environment,void* context,Event event=nullptr) noexcept
        :source_(source),environment_(environment),event_(event),context_(context),identity_(newIdentity()){}
    Adapter(const Adapter&)=delete;
    Adapter& operator=(const Adapter&)=delete;
    Hooks hooks() noexcept {return environment_ && event_ && context_ && identity_?Hooks{&source_,this,before,after}:Hooks{};}
    ai::Result lastResult() const noexcept {return last_;}
    void sample(bool connected,const recomp::input::GamepadState& event,Receipt& receipt) noexcept {
        receipt={};last_={ai::Reason::NoLease};
        ai::Environment e{};recomp::input::GamepadState state{};
        if(!connected || !identity_ || !environment_ || !context_ || !environment_(context_,e) || e.stage!=ai::Stage::Menu)return;
        ft::AgentReceipt sampled{};
        if(source_.sampleAgent(0,&state,e,&sampled)!=1 || !sampled.consumed.available ||
            recomp::input::encodeGamepadState(state)!=recomp::input::encodeGamepadState(event))return;
        receipt={sampled,this,identity_,true};
    }
    ai::Result consumed(Receipt& receipt,const recomp::input::GamepadState& event,bool admitted) noexcept {
        const auto sampled=receipt;receipt.available=false; // Single use even on refusal.
        last_={ai::Reason::ConsumerToken};
        if(!sampled.available || sampled.owner!=this || sampled.identity!=identity_)return last_;
        if(!admitted)return last_;
        ai::Environment e{};
        if(!environment_ || !environment_(context_,e)){last_={ai::Reason::SourceStale};return last_;}
        if(e.stage!=ai::Stage::Menu){last_={ai::Reason::StageMismatch};return last_;}
        last_=source_.confirmAgent(sampled.sample,event,e);return last_;
    }
private:
    static std::uint64_t newIdentity() noexcept {
        static std::atomic<std::uint64_t> next{1};return allocateIdentity(next);
    }
    static void before(void* context,bool connected,const recomp::input::GamepadState& event,Receipt& receipt) noexcept {
        auto& self=*static_cast<Adapter*>(context);
        // Invoked only on an admitted actual source event, never by a timer.
        self.event_(self.context_,EnvironmentEvent::SourceSample);
        self.sample(connected,event,receipt);
    }
    static void after(void* context,Receipt& receipt,const recomp::input::GamepadState& event,bool admitted) noexcept {
        auto& self=*static_cast<Adapter*>(context);
        // Counts EVERY accepted menu event, independent of lease/ACK success.
        // Main's writer must record real UI consumption, not guest progress.
        if(admitted)self.event_(self.context_,EnvironmentEvent::MenuConsumed);
        (void)self.consumed(receipt,event,admitted);
    }
    ft::Gamepad& source_;
    const Environment environment_;
    const Event event_;
    void* const context_;
    const std::uint64_t identity_;
    ai::Result last_{ai::Reason::NoLease};
};
}
