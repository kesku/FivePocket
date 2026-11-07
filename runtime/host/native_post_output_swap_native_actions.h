#pragma once
#include "native_post_output_swap_wrapper.h"
#include "native_graphics_state.h"
#include <functional>
#include <map>

namespace recomp::gpu {
// Writes ONLY a caller-owned prepared native journal/state. The caller must
// perform every guest scratch write/read before applying the corresponding
// operation. No guest memory, IRQ execution, presentation or commit is implied.
template<std::uint64_t Ticket> class NativeSurfaceSwapNativeActions final {
public:
    using Wrapper=NativeSurfaceSwapWrapper<Ticket>;
    using Journal=std::map<std::uint32_t,std::pair<std::uint32_t,std::uint32_t>>;
    NativeSurfaceSwapNativeActions(const Wrapper& plan,Journal& journal,NativeGraphicsState& state)
        :plan_(plan),journal_(journal),state_(state){
        // Construct before ANY native queue/guest action. apply never inserts.
        for(unsigned reg:{0x578u,0x57Cu,0x57Du,0x5C8u,0x45Eu,0x21F9u})journal_.try_emplace(reg,std::make_pair(0u,0u));
    }
    NativeSurfaceSwapNativeActions(const NativeSurfaceSwapNativeActions&)=delete;
    NativeSurfaceSwapNativeActions& operator=(const NativeSurfaceSwapNativeActions&)=delete;
    bool apply(const typename Wrapper::Operation& op,const std::function<bool()>& idle){
        if(failed_ || applying_ || next_>=plan_.operations().size())return fail();
        const auto& expected=plan_.operations()[next_];
        if(op.kind!=expected.kind || op.wordOffset!=expected.wordOffset || op.target!=expected.target || op.value!=expected.value)return fail();
        applying_=true;
        try{
            using Kind=typename Wrapper::Kind;
            switch(op.kind){
            case Kind::BinMaskLow:state_.binMask=(state_.binMask&0xFFFFFFFF00000000ULL)|op.value;state_.knownBinWords|=1;break;
            case Kind::WaitIdle:
                if(!idle || !idle() || failed_){applying_=false;return fail();}
                journal_.find(0x5C8)->second={op.value,0xFFFFFFFF};break;
            case Kind::ScratchWrite:journal_.find(0x578+op.target)->second={op.value,0xFFFFFFFF};break;
            case Kind::WaitScratch:break;
            case Kind::CallbackAcknowledge:journal_.find(op.target)->second={op.value,0xFFFFFFFF};break;
            case Kind::Interrupt:applying_=false;return fail();
            }
            applying_=false;++next_;if(next_==8)++next_;return true;
        }catch(...){applying_=false;(void)fail();throw;}
    }
    bool complete() const noexcept {return !failed_ && !applying_ && next_==12;}
    bool failed() const noexcept {return failed_;}
private:
    bool fail() noexcept {failed_=true;return false;}
    Wrapper plan_;
    Journal& journal_;NativeGraphicsState& state_;
    unsigned next_=0;bool failed_=false,applying_=false;
};
using NativePostOutputSwapNativeActions=NativeSurfaceSwapNativeActions<2>;
using NativeDeferredSwapNativeActions=NativeSurfaceSwapNativeActions<3>;
}
