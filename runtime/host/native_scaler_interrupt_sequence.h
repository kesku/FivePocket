#pragma once
#include "native_scaler_wrapper.h"
#include <functional>

namespace recomp::gpu {
// Ordered wrapper, split around the actual guest interrupt. This implements no action
// nor proves GPU work or callback execution. Its owner supplies real scratch
// reads/writes, native idle ordering, and an independently verified guest call.
// Partial action failure is terminal: no retry, rollback, tail or RPTR retire.
class NativeScalerInterruptSequence final {
public:
    using Action=std::function<bool(const NativeScalerWrapper::Operation&)>;
    enum class Phase : std::uint32_t { Ready,AwaitingGuest,GuestRunning,GuestReturned,Acknowledged,Failed,Complete };
    explicit NativeScalerInterruptSequence(const NativeScalerWrapper& plan):plan_(plan) {}
    bool prefix(std::uint32_t base,std::uint32_t mask,const Action& action) {
        if (phase_!=Phase::Ready || !action || !plan_.scratchStateMatches(base,mask)) return false;
        // Ten ordered obligations precede the CPU2 interrupt at word35.
        if (!apply(0,10,action)) return false;
        phase_=Phase::AwaitingGuest;next_=10;return true;
    }
    bool enterGuest() noexcept {
        if (phase_!=Phase::AwaitingGuest) return false;
        phase_=Phase::GuestRunning;return true;
    }
    bool guestReturned() noexcept {
        if (phase_!=Phase::GuestRunning) return false;
        phase_=Phase::GuestReturned;next_=11;return true;
    }
    bool acknowledge(const Action& action) {
        if (phase_!=Phase::GuestReturned || !action) return false;
        // ACK register, actual scratch0==0, poison callback, actual scratch1==0.
        // Never pass an Interrupt operation to the action provider or consume
        // any scaler/update/tail command after the four acknowledgement steps.
        if (!apply(11,15,action)) return false;
        phase_=Phase::Acknowledged;next_=15;return true;
    }
    bool completeTail(const Action& action) {
        if (phase_!=Phase::Acknowledged || !action) return false;
        // Seven native display operations, invalidate, diagnostic NOP, two
        // ordered literal events. Real work belongs to the bound action owner.
        if (!apply(15,26,action)) return false;
        phase_=Phase::Complete;next_=26;return true;
    }
    void fail() noexcept {phase_=Phase::Failed;}
    Phase phase() const noexcept {return phase_;}
    std::uint32_t nextOperation() const noexcept {return next_;}
    const NativeScalerWrapper& plan() const noexcept {return plan_;}
private:
    bool apply(unsigned first,unsigned end,const Action& action) {
        phase_=Phase::Failed; // Including exceptions and partially applied actions.
        for (unsigned i=first;i<end;++i) {
            next_=i;
            if (!action(plan_.operations()[i])) return false;
            next_=i+1;
        }
        return true;
    }
    NativeScalerWrapper plan_;
    Phase phase_=Phase::Ready;
    std::uint32_t next_=0;
};
}
