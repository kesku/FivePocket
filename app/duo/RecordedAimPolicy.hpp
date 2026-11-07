// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace recomp::apple {
// Separate opt-in protocol. V1 remains unchanged. Only LT may persist between
// reviewed actions; movement buttons, left stick and arbitrary macros are not
// representable here. A lease has one fixed, non-renewable deadline.
enum class AimOperation : unsigned { Begin=0, Move=1, Fire=2, End=3 };
struct RecordedAimCommand {
    std::uint32_t sequence=0, lease=0, frame=0, durationMs=0;
    std::uint64_t expires=0;
    AimOperation operation=AimOperation::End;
    int x=0,y=0;
};
inline bool parseRecordedAim(std::string_view text,std::string_view session,
                             RecordedAimCommand& out) {
    if(text.size()>512 || session.size()!=32)return false;
    std::array<std::string_view,11> f{};std::size_t count=0;
    while(!text.empty()) {
        const auto start=text.find_first_not_of(" \r\n\t");
        if(start==text.npos)break;
        text.remove_prefix(start);
        const auto end=text.find_first_of(" \r\n\t");
        if(count==f.size())return false;
        f[count++]=text.substr(0,end);
        if(end==text.npos)break;
        text.remove_prefix(end);
    }
    if(count!=f.size() || f[0]!="PORT_AIM_V2" || f[1]!=session)return false;
    auto number=[](std::string_view value,auto& dest) {
        auto [p,e]=std::from_chars(value.data(),value.data()+value.size(),dest);
        return e==std::errc{} && p==value.data()+value.size();
    };
    RecordedAimCommand c;unsigned operation=0;
    if(!number(f[2],c.sequence) || !number(f[3],c.lease) ||
       !number(f[4],c.expires) || !number(f[5],c.frame) ||
       !number(f[6],operation) || !number(f[7],c.durationMs) ||
       !number(f[8],c.x) || !number(f[9],c.y) || f[10]!="END")return false;
    if(!c.sequence || c.sequence>1024 || !c.lease || c.lease>c.sequence ||
       operation>3 || c.x < -1000 || c.x>1000 || c.y < -1000 || c.y>1000)return false;
    c.operation=static_cast<AimOperation>(operation);
    switch(c.operation) {
    case AimOperation::Begin:
        if(c.lease!=c.sequence || c.durationMs<1000 || c.durationMs>30000 || c.x || c.y)return false;
        break;
    case AimOperation::Move:
        if(c.durationMs<50 || c.durationMs>2000 || (!c.x && !c.y))return false;
        break;
    case AimOperation::Fire:
        if(c.durationMs<50 || c.durationMs>150 || c.x || c.y)return false;
        break;
    case AimOperation::End:
        if(c.durationMs || c.x || c.y)return false;
        break;
    }
    out=c;return true;
}

struct RecordedAimState {
    int rightX=0,rightY=0,leftTrigger=0,rightTrigger=0;
    bool operator==(const RecordedAimState&) const = default;
};
enum class AimEvent { None, Begin, Move, Fire, Settle, End, Expired, Closed };

class RecordedAimPolicy {
    std::uint32_t next_=1,lease_=0,generation_=0;
    std::uint64_t deadline_=0,pulseEnd_=0;
    bool active_=false,closed_=false,pulsing_=false,ended_=false;
    RecordedAimState state_{};
    AimEvent event_=AimEvent::None;
    void set(RecordedAimState value,AimEvent event) noexcept {
        state_=value;event_=event;++generation_;
    }
    void stop(AimEvent event) noexcept {
        active_=false;pulsing_=false;set({},event);
    }
public:
    std::uint32_t next() const noexcept{return next_;}
    std::uint32_t lease() const noexcept{return lease_;}
    std::uint32_t generation() const noexcept{return generation_;}
    std::uint64_t deadline() const noexcept{return deadline_;}
    bool active() const noexcept{return active_;}
    bool closed() const noexcept{return closed_;}
    bool settling() const noexcept{return active_ && !pulsing_;}
    RecordedAimState state() const noexcept{return state_;}
    AimEvent event() const noexcept{return event_;}
    void close() noexcept {
        if(closed_)return;
        closed_=true;stop(AimEvent::Closed);
    }
    // Call from the same main-runloop owner as GameController publication.
    // Deadline enforcement is logical, not a hard-real-time UI guarantee.
    void tick(std::uint64_t monotonicMs,bool foreground,bool connected) noexcept {
        if(closed_)return;
        if(!foreground || !connected){close();return;}
        if(active_ && monotonicMs>=deadline_){stop(AimEvent::Expired);return;}
        if(active_ && pulsing_ && monotonicMs>=pulseEnd_) {
            pulsing_=false;
            set({0,0,1000,0},AimEvent::Settle);
        }
    }
    bool admit(const RecordedAimCommand& c,std::uint64_t unixNow,
               std::uint64_t monotonicMs,std::uint32_t frame,
               bool foreground,bool connected,bool neutral) noexcept {
        tick(monotonicMs,foreground,connected);
        if(closed_ || c.sequence!=next_ || !c.sequence || c.sequence>1024)return false;
        // Also validate direct callers; parsing is not the only admission gate.
        if(!c.lease || c.lease>c.sequence || c.x < -1000 || c.x>1000 || c.y < -1000 || c.y>1000)return false;
        if(c.operation==AimOperation::End) {
            // Authenticated release is safe even if visual evidence has aged.
            // Expiry already released controls, but the sender may have been
            // in flight. Consume its one release so sequence cannot wedge.
            if(!lease_ || ended_ || c.lease!=lease_ || c.durationMs || c.x || c.y)return false;
            ended_=true;++next_;stop(AimEvent::End);return true;
        }
        if(!frame || c.frame>frame || frame-c.frame>300 ||
           c.expires<unixNow || c.expires-unixNow>30)return false;
        if(c.operation==AimOperation::Begin) {
            if(active_ || !neutral || c.lease!=c.sequence || c.durationMs<1000 ||
               c.durationMs>30000 || c.x || c.y || monotonicMs>UINT64_MAX-c.durationMs)return false;
            active_=true;lease_=c.lease;deadline_=monotonicMs+c.durationMs;pulsing_=false;ended_=false;
            ++next_;set({0,0,1000,0},AimEvent::Begin);return true;
        }
        if(!active_ || pulsing_ || c.lease!=lease_)return false;
        if(c.operation==AimOperation::Move) {
            if(c.durationMs<50 || c.durationMs>2000 || (!c.x && !c.y))return false;
        } else if(c.operation==AimOperation::Fire) {
            if(c.durationMs<50 || c.durationMs>150 || c.x || c.y)return false;
        } else return false;
        // A pulse cannot outlive or renew the original lease, even by a tick.
        if(c.durationMs>deadline_-monotonicMs)return false;
        pulseEnd_=monotonicMs+c.durationMs;pulsing_=true;++next_;
        set({c.x,c.y,1000,c.operation==AimOperation::Fire?1000:0},
            c.operation==AimOperation::Fire?AimEvent::Fire:AimEvent::Move);
        return true;
    }
};
}
