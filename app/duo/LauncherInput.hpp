// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LauncherNavigation.hpp"
#include "native_gamepad_state.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace recomp::apple::launcher {
// Apple receives one normalized D-pad source (not Android's duplicate key/HAT
// reports). Direction/left-stick arbitration and repeat timings follow NavInput.
// Caller supplies every event, a monotonic millisecond clock, and an explicit
// repeat tick. No polling controller state or sending synthetic guest inputs.
class Input final {
public:
    using Sink=void(*)(void*,Command,bool) noexcept;
    Input(Sink sink,void* user):sink_(sink),user_(user){}
    static constexpr std::uint64_t FirstRepeat=400,Repeat=110,FastRepeat=55;
    void event(std::uint64_t id,bool connected,const recomp::input::GamepadState& state,std::uint64_t now) noexcept {
        if(closed_)return;
        if(!clock(now)){reset();return;}
        if(!connected || !id){reset();identity_=0;return;}
        if(identity_!=id) {reset();identity_=id;}
        if(!ready_) {
            // A held control on attachment/focus return is not a fresh command.
            if(neutral(state))ready_=true;
            previous_=state.buttons;return;
        }
        int dpad=0;
        if(state.buttons&1)dpad|=bit(Command::Up);
        if(state.buttons&2)dpad|=bit(Command::Down);
        if(state.buttons&4)dpad|=bit(Command::Left);
        if(state.buttons&8)dpad|=bit(Command::Right);
        hold(0,oneDirection(dpad,held_[0]),now);
        if(closed_)return;
        const float x=float(state.leftX)/(state.leftX<0?32768.0f:32767.0f);
        const float y=-float(state.leftY)/(state.leftY<0?32768.0f:32767.0f); // Apple Y is up-positive.
        const auto stick=stickDirection(x,y,directionOf(held_[1]));
        hold(1,bit(stick),now);
        if(closed_)return;
        if(stick!=Command::None)stickAxis_=(bit(stick)&Vertical)?Vertical:Horizontal;
        const auto fresh=std::uint16_t(state.buttons&~previous_);previous_=state.buttons;
        for(const auto [mask,command]:Buttons)if(fresh&mask)emit(command,false);
    }
    void tick(std::uint64_t now) noexcept {
        if(closed_ || !clock(now)){reset();return;}
        if(repeating_==Command::None || now<due_)return;
        const auto command=repeating_;++repeats_;
        schedule(now,repeats_>=10?FastRepeat:Repeat);
        emit(command,true); // One tick, never a catch-up burst after a slow main loop.
    }
    void endHold() noexcept {ended_=held_;stopRepeat();}
    void suspend() noexcept {reset();identity_=0;}
    void close() noexcept {suspend();closed_=true;}
    bool closed() const noexcept {return closed_;}
    std::uint64_t due() const noexcept {return repeating_==Command::None?0:due_;}
private:
    static constexpr int Vertical=(1<<1)|(1<<2),Horizontal=(1<<3)|(1<<4);
    static constexpr std::array<std::pair<std::uint16_t,Command>,6> Buttons{{
        {4096,Command::Confirm},{8192,Command::Back},{256,Command::PreviousPage},
        {512,Command::NextPage},{16,Command::Menu},{32768,Command::Alternate}}};
    static int bit(Command command) noexcept {return command==Command::None?0:1<<int(command);}
    static Command directionOf(int bits) noexcept {
        for(int i=1;i<=4;++i)if(bits==(1<<i))return Command(i);
        return Command::None;
    }
    static int oneDirection(int now,int was) noexcept {
        if(now&was)return now&was;
        const int vertical=now&Vertical;return vertical?vertical:now;
    }
    static bool neutral(const recomp::input::GamepadState& state) noexcept {
        return !(state.buttons&0xB31Fu) && std::abs(int(state.leftX))<11468 && std::abs(int(state.leftY))<11468;
    }
    bool clock(std::uint64_t now) noexcept {
        if(clockSet_ && now<lastTime_)return false;
        clockSet_=true;lastTime_=now;return true;
    }
    void reset() noexcept {held_={};ended_={};stickAxis_=0;previous_=0;ready_=false;stopRepeat();}
    void emit(Command command,bool repeat) noexcept {
        if(!closed_ && sink_)sink_(user_,command,repeat);
    }
    void stopRepeat() noexcept {repeating_=Command::None;due_=0;repeats_=0;}
    void schedule(std::uint64_t now,std::uint64_t delay) noexcept {
        due_=now>std::numeric_limits<std::uint64_t>::max()-delay?std::numeric_limits<std::uint64_t>::max():now+delay;
    }
    int live() const noexcept {return (held_[0]&~ended_[0])|(held_[1]&~ended_[1]);}
    void press(unsigned source,Command direction,std::uint64_t now) noexcept {
        const auto b=bit(direction);held_[source]|=b;ended_[source]&=~b;
        const auto other=1-source;
        if((held_[other]&~ended_[other])&b)return;
        repeating_=direction;repeats_=0;schedule(now,FirstRepeat);emit(direction,false);
    }
    void release(unsigned source,Command direction,std::uint64_t now) noexcept {
        const auto b=bit(direction);held_[source]&=~b;ended_[source]&=~b;
        if(repeating_!=direction || (live()&b))return;
        stopRepeat();const auto vertical=live()&Vertical;
        const auto handOver=directionOf(vertical&-vertical);
        if(handOver!=Command::None){repeating_=handOver;schedule(now,FirstRepeat);}
    }
    void hold(unsigned source,int next,std::uint64_t now) noexcept {
        const auto was=held_[source];if(was==next)return;
        for(int i=1;i<=4;++i)if((was&(1<<i)) && !(next&(1<<i)))release(source,Command(i),now);
        for(int i=1;i<=4;++i)if(!(was&(1<<i)) && (next&(1<<i)))press(source,Command(i),now);
    }
    Command stickDirection(float x,float y,Command current) noexcept {
        if(!std::isfinite(x))x=0;if(!std::isfinite(y))y=0;
        const float ax=std::abs(x),ay=std::abs(y);
        if(ax<.35f && ay<.35f){stickAxis_=0;return Command::None;}
        if(stickAxis_) {
            const float own=stickAxis_==Vertical?ay:ax,other=stickAxis_==Vertical?ax:ay;
            if(own<.35f && other<.55f)stickAxis_=0;
        }
        if((current==Command::Left && x<-.35f) || (current==Command::Right && x>.35f) ||
           (current==Command::Up && y<-.35f) || (current==Command::Down && y>.35f))return current;
        auto wanted=Command::None;
        if(ax>.55f && ax>1.4f*ay)wanted=x>0?Command::Right:Command::Left;
        else if(ay>.55f)wanted=y>0?Command::Down:Command::Up;
        if(wanted==Command::None)return wanted;
        const auto axis=(bit(wanted)&Vertical)?Vertical:Horizontal;
        return !stickAxis_ || stickAxis_==axis?wanted:Command::None;
    }
    Sink sink_;void* user_;
    std::array<int,2> held_{},ended_{};
    Command repeating_=Command::None;
    int stickAxis_=0,repeats_=0;
    std::uint16_t previous_=0;
    std::uint64_t identity_=0,due_=0,lastTime_=0;
    bool ready_=false,closed_=false,clockSet_=false;
};
}
