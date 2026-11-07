// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace iospresenterphase {
// Optional measurement control only. One owning main thread calls observe at
// complete-frame boundaries; this helper reads no clock and performs no I/O.
class Schedule final {
public:
    static constexpr std::uint64_t PhaseNs=15'000'000'000ULL;
    static constexpr unsigned FrozenPhase=6;
    struct Observation {
        bool valid;
        unsigned phase,mode;
        bool changed,frozen;
    };
    Observation observe(std::uint64_t nowNs) noexcept {
        if(invalid_)return result(false,false);
        if(!anchored_){
            anchored_=true;anchor_=nowNs;last_=nowNs;phase_=0;
            return result(true,true);
        }
        // Compare before subtracting. A rollback invalidates the measurement
        // permanently, including after the schedule has reached its frozen arm.
        if(nowNs<last_){invalid_=true;return result(false,false);}
        last_=nowNs;
        const auto elapsed=nowNs-anchor_;
        const auto raw=elapsed/PhaseNs;
        const unsigned next=raw>=FrozenPhase?FrozenPhase:static_cast<unsigned>(raw);
        const bool changed=next!=phase_;
        phase_=next;
        return result(true,changed);
    }
private:
    bool anchored_=false,invalid_=false;
    std::uint64_t anchor_=0,last_=0;
    unsigned phase_=0;
    Observation result(bool valid,bool changed)const noexcept {
        return {valid,phase_,phase_==FrozenPhase?1u:phase_%2u,changed,phase_==FrozenPhase};
    }
};
} // namespace iospresenterphase
