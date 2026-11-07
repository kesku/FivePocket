#pragma once
#include "native_swap_wrapper.h"
#include <functional>

namespace recomp::gpu {
// Native host-observed timing policy, not Xbox/GPU timestamp or vblank evidence.
struct NativeSwapGpuResult {
    std::array<std::uint8_t,16> eventBytes{};
    std::uint64_t beforeCompletionNs=0,afterCompletionNs=0;
    std::uint32_t nativeFences=0;
};
using NativeCompletionClock=std::function<std::optional<std::uint64_t>()>;
inline bool validNativeSwapCompletion(const NativeSwapWrapper& plan,const NativeSwapGpuResult& result) noexcept {
    if(result.nativeFences!=4 || result.afterCompletionNs<result.beforeCompletionNs ||
        result.afterCompletionNs-result.beforeCompletionNs>=8'589'934'592ULL)return false;
    for(unsigned i=0;i<4;++i){
        if(i>=2 && !plan.events()[i].literal)return false;
        const auto value=i<2?static_cast<std::uint32_t>((i==0?result.beforeCompletionNs:result.afterCompletionNs)/2):*plan.events()[i].literal;
        for(unsigned b=0;b<4;++b)
            if(result.eventBytes[i*4+b]!=static_cast<std::uint8_t>(value>>(i<2?8*b:24-8*b)))return false;
    }
    return true;
}
}
