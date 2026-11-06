#pragma once
#include "gta5_inline_draw.h"

namespace recomp::gpu {
// Diagnostic storage capture ONLY. This is the submitted format54 view of the
// original82388A38 -> 823C2738 raw10 resolve destination, not RGBA8 sampling.
// Xenia maps format54 to A2B10G10R10_UNORM; exponent/filter/swizzle interpretation
// belongs to a future sampler. Preserve all six submitted words unchanged.
// The previously proven tiled1280x720 resolve occupies1280x736x4 bytes, including
// padding. The generic unknown-layout page prefix is NOT a complete substitute.
inline std::optional<Gta5InlineDraw::Texture> captureNativeRaw10Base(unsigned slot,
        const std::array<std::uint32_t,6>& fetch,const Gta5ResourceReader& ownedGraphics,
        std::uint64_t byteBudget=64u*1024*1024) {
    constexpr unsigned bytes=1280u*736u*4u;
    if(slot>=32 || !ownedGraphics || byteBudget<bytes ||
       fetch[0]!=0x8A004802 || (fetch[1]&0xFFFu)!=0xB6 || fetch[2]!=0x0059E4FF ||
       fetch[3]!=0x00A86C14 || fetch[4]!=3 || fetch[5]!=0x210)return std::nullopt;
    const auto description=XenosTextureDescription::decode(fetch);
    if(!description || !description->baseAddress ||
       std::uint64_t{description->baseAddress}+bytes>0x20000000ULL)return std::nullopt;
    auto storage=ownedGraphics(description->baseAddress,bytes);
    if(!storage || storage->size()!=bytes)return std::nullopt;
    return Gta5InlineDraw::Texture{slot,fetch,false,std::move(*storage)};
}
}
