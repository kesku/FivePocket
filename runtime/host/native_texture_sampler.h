#pragma once
#include "xenos_texture_mip_chain.h"
#include "native_color8_resolve.h"

namespace recomp::gpu {
// Normalized2D, inherited point/linear filters, complete unpacked mip storage.
// This is an explicit Vulkan sampling policy, NOT a claim of bit-identical
// Xenos LOD/anisotropy/filter precision. Shader overrides require another plan.
// Pinned Xenia95a5c3ee xenos.h fetch fields; ReX Vulkan GetSamplerParameters
// uses these point/linear/clamp modes and handles bias in the shader.
struct NativeTextureSamplerPlan {
    std::array<unsigned,2> clamp{};
    unsigned magFilter=0,minFilter=0,mipFilter=0,maxLevel=0,triClamp=0;
    bool operator==(const NativeTextureSamplerPlan&)const=default;
};
inline std::optional<NativeTextureSamplerPlan> planNativeTextureSampler2D(
        const std::array<std::uint32_t,6>& fetch) {
    const auto chain=planXenosUnpackedMipChain(fetch);
    if(!chain)return std::nullopt;
    const auto d=XenosTextureDescription::decode(fetch);
    const auto mag=(fetch[3]>>19)&3,min=(fetch[3]>>21)&3,mip=(fetch[3]>>23)&3;
    if(mag>1 || min>1 || mip>1 || d->clamp[0]>2 || d->clamp[1]>2 ||
        (fetch[3]&0xFE000000u) || // Anisotropic/arbitrary filters and border size.
        (fetch[4]&0xFFFFFC00u) || // Anisotropic walk, LOD bias and gradient adjustment.
        (fetch[5]&0x1E4u))return std::nullopt; // BC alpha override/aniso bias.
    // Z clamp is irrelevant for a2D fetch. TriJuice/tri_clamp is retained for
    // audit; its hardware refinement is not modeled by this Vulkan policy.
    return NativeTextureSamplerPlan{{d->clamp[0],d->clamp[1]},mag,min,mip,d->mipMax,(fetch[5]>>3)&3};
}
// Explicit base-LOD face-local cube policy. Reject seamless wrapping, LOD bias,
// anisotropy and unequal min/mag filters rather than guessing the derivative
// behavior of a shader that requests neither computed nor register LOD.
inline std::optional<NativeTextureSamplerPlan> planNativeTextureSamplerCubeBase(
        const std::array<std::uint32_t,6>& fetch) {
    const auto d=XenosTextureDescription::decode(fetch);
    if(!d || d->dimension!=XenosTextureDescription::Dimension::Cube ||
       d->clamp!=std::array<unsigned,3>{2,2,2})return std::nullopt;
    for(unsigned face=0;face<6;++face)if(!planNativeColor8ResolveFace(*d,face))return std::nullopt;
    const auto mag=(fetch[3]>>19)&3,min=(fetch[3]>>21)&3,mip=(fetch[3]>>23)&3;
    if(mag>1 || min!=mag || mip>1 || (fetch[3]&0xFE000000u) ||
       (fetch[4]&0xFFFFFC00u) || (fetch[5]&0x1E4u))return std::nullopt;
    return NativeTextureSamplerPlan{{2,2},mag,min,mip,0,(fetch[5]>>3)&3};
}
}
