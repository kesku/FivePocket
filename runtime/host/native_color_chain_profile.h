#pragma once
#include "native_gpu_word_updates.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace recomp::gpu {
// Shape of the existing bounded native renderer, not guest state admission.
// Checking this ABI does not validate SPIR-V instructions, shader identity,
// raster semantics, a GPU fence or ownership of any guest allocation.
template<bool StoreCopy,bool Initialize,bool NativeDepth>
struct NativeColorChainProfile {
    static_assert(!Initialize || StoreCopy);
    static_assert(!NativeDepth || Initialize);
    static constexpr unsigned BufferCount=StoreCopy?6:3;
    static constexpr unsigned CodeCount=Initialize?7:(StoreCopy?6:4);
    static constexpr unsigned ComputeCount=CodeCount-4;
    static constexpr unsigned OutputBytes=1280*720*8+32;
    static constexpr unsigned BackingBytes=10485760+32;
    static constexpr unsigned CopyBytes=1280*736*4+32;
    static constexpr unsigned EventBytes=40;
    using BufferSizes=std::array<unsigned,BufferCount>;
    using Code=std::array<std::vector<std::uint32_t>,CodeCount>;
    using DepthCode=std::array<std::vector<std::uint32_t>,2>;

    static bool validBufferSizes(const BufferSizes& sizes) noexcept {
        if(sizes[0]<1382400 || sizes[0]>1382912 || sizes[0]%4 ||
           sizes[1]<4096 || sizes[1]>4608 || sizes[1]%4 || sizes[2]!=OutputBytes)return false;
        if constexpr(StoreCopy)
            if(sizes[3]!=BackingBytes || sizes[4]!=CopyBytes || sizes[5]!=EventBytes)return false;
        return true;
    }
    template<std::size_t N>
    static bool validModuleFraming(const std::array<std::vector<std::uint32_t>,N>& code) noexcept {
        for(const auto& words:code)
            if(words.size()<5 || words.size()>65536/4 || words[0]!=0x07230203)return false;
        return true;
    }
    static bool validDepthSourceBytes(std::size_t bytes) noexcept {
        return bytes && bytes<=4096 && bytes%4==0;
    }
};

// Events are native staging slots, not inferred guest EVENT_WRITE semantics.
inline bool validNativeColorChainEvents(const NativeWordUpdatePlan& plan) noexcept {
    if(plan.bufferBytes!=40 || (plan.count!=0 && plan.count!=2))return false;
    for(std::size_t i=0;i<plan.count;++i)
        if(plan.words[i].byteOffset!=16+i*4 || plan.words[i].phase!=NativeWordPhase::AfterGraphics)return false;
    return true;
}
}
