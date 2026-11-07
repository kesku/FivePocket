#pragma once
#include "gta5_inline_draw.h"
#include "xenos_expanded16_float_texture.h"

namespace recomp::gpu {
// Owned storage evidence only. EXPAND uses binary16 in the Xenia texture
// table, but that is NOT a shader/sampler admission certificate. Preserve the
// submitted words (sign, swizzle and filtering included), never a normalized
// storage descriptor. An unavailable/short read is not a black texture.
inline std::optional<Gta5InlineDraw::Texture> captureNativeExpanded16Base(
        unsigned slot,const std::array<std::uint32_t,6>& fetch,
        const Gta5ResourceReader& ownedGraphics,std::uint64_t byteBudget=64u*1024*1024) {
    if(slot>=32 || !ownedGraphics)return std::nullopt;
    const auto description=XenosTextureDescription::decode(fetch);
    const auto layout=description?planXenosExpanded16FloatStorage(*description):std::nullopt;
    if(!layout || !description->baseAddress || description->baseAddress>=0x20000000u ||
       layout->sourceBytes>byteBudget ||
       std::uint64_t{description->baseAddress}+layout->sourceBytes>0x20000000ULL)return std::nullopt;
    auto bytes=ownedGraphics(description->baseAddress,layout->sourceBytes);
    if(!bytes || bytes->size()!=layout->sourceBytes)return std::nullopt;
    return Gta5InlineDraw::Texture{slot,fetch,false,std::move(*bytes)};
}
}
