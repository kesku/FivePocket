#pragma once
#include "native_post_output_swap_wrapper.h"
#include "native_swap_completion.h"

namespace recomp::gpu {
// Separate receipt: the90word helper's four fences cannot satisfy this swap.
// The IRQ flag records the owner's acknowledgement callback, not independent
// proof of guest execution. The registry must validate its fresh guest scope.
template<std::uint64_t Ticket> struct NativeSurfaceSwapGpuResult {
    static_assert(Ticket==2 || Ticket==3);
    std::array<std::uint8_t,16> eventBytes{};
    std::uint64_t beforeCompletionNs=0,afterCompletionNs=0;
    std::uint32_t nativeFences=0,interruptIdleFences=0;
    bool interruptAcknowledged=false;
};
using NativePostOutputSwapGpuResult=NativeSurfaceSwapGpuResult<2>;
using NativeDeferredSwapGpuResult=NativeSurfaceSwapGpuResult<3>;
template<std::uint64_t Ticket> inline bool validNativeSurfaceSwapCompletion(const NativeSurfaceSwapWrapper<Ticket>& plan,
        const NativeSurfaceSwapGpuResult<Ticket>& result) noexcept {
    if(result.nativeFences!=6 || result.interruptIdleFences!=1 || !result.interruptAcknowledged ||
        result.afterCompletionNs<result.beforeCompletionNs ||
        result.afterCompletionNs-result.beforeCompletionNs>=8'589'934'592ULL)return false;
    for(unsigned i=0;i<4;++i){
        if(i>=2 && !plan.events()[i].literal)return false;
        const auto value=i<2?static_cast<std::uint32_t>((i==0?result.beforeCompletionNs:result.afterCompletionNs)/2):*plan.events()[i].literal;
        for(unsigned b=0;b<4;++b)
            if(result.eventBytes[i*4+b]!=static_cast<std::uint8_t>(value>>(i<2?8*b:24-8*b)))return false;
    }
    return true;
}
inline bool validNativePostOutputSwapCompletion(const NativePostOutputSwapWrapper& plan,const NativePostOutputSwapGpuResult& result) noexcept {
    return validNativeSurfaceSwapCompletion(plan,result);
}
inline bool validNativeDeferredSwapCompletion(const NativeDeferredSwapWrapper& plan,const NativeDeferredSwapGpuResult& result) noexcept {
    return validNativeSurfaceSwapCompletion(plan,result);
}
}
