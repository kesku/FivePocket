#pragma once
#include "xenos_texture_2d.h"

namespace recomp::gpu {
// Explicit unsigned format54 storage import. Xenia's Vulkan texture table
// maps k_2_10_10_10_AS_16_16_16_16 to A2B10G10R10_UNORM_PACK32, not RGBA8 or
// 7e3 float. Preserve all packed bits; tiling/endian normalization alone uses
// the existing format6 byte-address view. The original descriptor, including
// sampler exponent and swizzle, MUST remain with the sampled image/consumer.
// No mip, signed/gamma, integer-component, address ownership or residency claim.
inline std::optional<XenosTexture2DLayout> planXenosRaw10UnsignedStorage(
        const XenosTextureDescription& description) {
    const auto d=XenosTextureDescription::decode(description.fetch);
    if(!d || d->format!=54 || d->integerComponents ||
       d->sign!=std::array<unsigned,4>{0,0,0,0})return std::nullopt;
    auto words=d->fetch;
    words[1]=(words[1]&~63u)|6u;
    // Sampling exponent is applied AFTER filtering/swizzling, not to stored
    // words. This raw address plan is never used as a sampling descriptor.
    words[3]&=~(63u<<13);
    const auto raw=XenosTextureDescription::decode(words);
    return raw?planXenosTexture2D(*raw):std::nullopt;
}
inline std::optional<std::vector<std::uint8_t>> linearizeXenosRaw10UnsignedTexture(
        const XenosTextureDescription& description,const std::vector<std::uint8_t>& source) {
    const auto storage=planXenosRaw10UnsignedStorage(description);
    if(!storage || source.size()!=storage->sourceBytes)return std::nullopt;
    return linearizeXenosTexture2D(storage->description,source);
}
}
