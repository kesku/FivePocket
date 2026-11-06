#pragma once
#include "native_post_output_swap_wrapper.h"
#include <functional>

namespace recomp::gpu {
// Only words78..116 of a separately typed second/third swap. The owner must separately complete
// the earlier swap/counter operations and later invalidate/literal events.
// This sequencer neither executes those operations nor authorizes presentation,
// guest entry, memory access, MMIO, timing/vblank, or RPTR retirement.
template<std::uint64_t Ticket> class NativeSurfaceSwapInterruptSequence final {
public:
    using Wrapper=NativeSurfaceSwapWrapper<Ticket>;
    using Action=std::function<bool(const typename Wrapper::Operation&)>;
    enum class Phase : std::uint32_t {
        Ready,ApplyingPrefix,AwaitingGuest,GuestRunning,GuestReturned,ApplyingAck,Acknowledged,Failed
    };
    explicit NativeSurfaceSwapInterruptSequence(const Wrapper& plan):plan_(plan){}
    bool prefix(std::uint32_t base,std::uint32_t mask,const Action& action){
        if(!action)return false;
        return prefixWith(base,mask,action);
    }
    // Statically bound caller lambda: no new std::function allocation in the
    // already-accepted native swap. Empty type-erased actions use prefix above.
    template<class Callable> bool prefixWith(std::uint32_t base,std::uint32_t mask,const Callable& action){
        if(phase_!=Phase::Ready || !plan_.scratchStateMatches(base,mask))return false;
        if(!apply(0,8,Phase::ApplyingPrefix,action))return false;
        phase_=Phase::AwaitingGuest;return true;
    }
    bool enterGuest() noexcept {
        if(phase_!=Phase::AwaitingGuest)return false;
        phase_=Phase::GuestRunning;return true;
    }
    bool guestReturned() noexcept {
        if(phase_!=Phase::GuestRunning)return false;
        phase_=Phase::GuestReturned;next_=9;return true;
    }
    bool acknowledge(const Action& action){
        if(!action)return false;
        return acknowledgeWith(action);
    }
    template<class Callable> bool acknowledgeWith(const Callable& action){
        if(phase_!=Phase::GuestReturned)return false;
        // CPU2 ACK, true scratch0==0 wait, callback poison. Unlike the first
        // scaler, scratch1 is neither written nor used as an acknowledgement.
        if(!apply(9,12,Phase::ApplyingAck,action))return false;
        phase_=Phase::Acknowledged;return true; // NOT full wrapper completion.
    }
    void fail() noexcept {phase_=Phase::Failed;}
    Phase phase() const noexcept {return phase_;}
    std::uint32_t nextOperation() const noexcept {return next_;}
    const Wrapper& plan() const noexcept {return plan_;}
private:
    template<class Callable> bool apply(unsigned first,unsigned end,Phase active,const Callable& action){
        phase_=active;
        try {
            for(unsigned i=first;i<end;++i){
                next_=i;
                if(!action(plan_.operations()[i]) || phase_!=active){fail();return false;}
                next_=i+1;
            }
        }catch(...){fail();throw;}
        return true;
    }
    Wrapper plan_;
    Phase phase_=Phase::Ready;
    std::uint32_t next_=0;
};
using NativePostOutputSwapInterruptSequence=NativeSurfaceSwapInterruptSequence<2>;
using NativeDeferredSwapInterruptSequence=NativeSurfaceSwapInterruptSequence<3>;
}
