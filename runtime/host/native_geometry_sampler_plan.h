#pragma once
#include "native_texture_sampler.h"

namespace recomp::gpu {
// Separate native geometry policy; legacy UI/explicit-LOD gates stay strict.
// ReX Vulkan texture_cache.cpp GetSamplerParameters/CreateSampler: inherited
// anisotropy forces linear min/mag/mip; Xenos enum 1..5 maps to 1..16:1.
// Vulkan chooses sample footprints. Xenos walk flags/TriJuice are retained for
// audit, not claimed to have hardware-identical refinements on the host.
// Shader filter overrides, biases, arbitrary filters and packed tails require
// different contracts. Never sanitize the submitted fetch to make it pass.
struct NativeGeometrySamplerPlan {
    NativeTextureSamplerPlan sampling;
    unsigned anisotropy=0,walkFlags=0,maxAnisotropy=1;
    bool operator==(const NativeGeometrySamplerPlan&)const=default;
};
inline std::optional<NativeGeometrySamplerPlan> planNativeGeometrySampler2D(
        const std::array<std::uint32_t,6>& fetch) {
    const auto chain=planXenosUnpackedMipChain(fetch);
    if(!chain)return std::nullopt;
    const auto d=XenosTextureDescription::decode(fetch);
    const auto mag=(fetch[3]>>19)&3,min=(fetch[3]>>21)&3,mip=(fetch[3]>>23)&3;
    const auto aniso=(fetch[3]>>25)&7,walk=(fetch[4]>>10)&3;
    if(mag>1 || min>1 || mip>1 || d->clamp[0]>2 || d->clamp[1]>2 ||
       aniso>5 || (!aniso && walk) || (fetch[3]&0xF0000000u) ||
       (fetch[4]&0xFFFFF000u) || (fetch[5]&0x1E4u))return std::nullopt;
    return NativeGeometrySamplerPlan{
        {{d->clamp[0],d->clamp[1]},aniso?1u:mag,aniso?1u:min,aniso?1u:mip,d->mipMax,(fetch[5]>>3)&3},
        aniso,walk,aniso?1u<<(aniso-1):1u};
}
}
