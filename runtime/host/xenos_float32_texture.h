#pragma once
#include "xenos_texture_2d.h"

namespace recomp::gpu {
// Explicit k_32_FLOAT (format 36) raw 32-bit address view, not a colour
// conversion. Each texel is one IEEE binary32 already in that layout, so the
// only work is the guest tiling and endian permutation, which the format 6 path
// performs byte-for-byte identically at four bytes per texel.
//
// This deliberately does NOT widen planXenosTexture2D's format allow-list.
// linearizeXenosTexture2D has no allow-list of its own and keys off that plan,
// so admitting 36 there would silently make k_32_FLOAT textures importable as
// colour where they currently fail explicitly, and their bytes would be read as
// RGBA8. The same reasoning produced planXenosDepth24FloatStorage for D24FS8;
// this mirrors it exactly.
//
// The returned view's format 6 is for byte addressing only. Callers keep the
// original descriptor for the owning image and for any sampling decision.
// Both resolve destination formats this tree writes, with their element sizes.
// k_32_FLOAT is one binary32; k_16_16_16_16_FLOAT is four binary16. Neither
// needs a conversion, only the guest tiling and endian permutation.
// k_16_16(25) joins them: two 16-bit fixed-point channels, four bytes, and the
// same raw addressing. Its numeric interpretation is the resolve's business,
// not this view's -- all this decides is how many owned bytes a texel occupies.
inline constexpr unsigned xenosRawFloatStorageTexelBytes(unsigned format) {
    return format==36 || format==25?4u:format==32?8u:0u;
}
inline std::optional<XenosTexture2DLayout> planXenosFloatStorage(
        const XenosTextureDescription& destination) {
    const auto d=XenosTextureDescription::decode(destination.fetch);
    const auto texel=d?xenosRawFloatStorageTexelBytes(d->format):0u;
    if(!d || !texel || d->mipMin || d->mipMax || d->mipAddress ||
       d->packedMips || d->exponentBias)return std::nullopt;
    // Sign selects the numeric interpretation of a component, which cannot turn
    // a binary32 into a normalised integer; the observed destination carries 1
    // in all four fields. Anything beyond that pair is a conversion this raw
    // view is not entitled to represent.
    for(auto sign:d->sign)if(sign>1)return std::nullopt;
    for(auto swizzle:d->swizzle)if(swizzle>5)return std::nullopt;
    auto words=d->fetch;
    // k_32_FLOAT borrows the format 6 path, which is byte-identical at four
    // bytes per texel. k_16_16_16_16_FLOAT keeps its own format, because eight
    // bytes per texel is tiled with a different shift and has no 4-byte alias.
    if(d->format==36 || d->format==25)words[1]=(words[1]&~63u)|6u;
    words[0]&=~0x3FCu;           // The four 2-bit sign fields, bits 2..9.
    words[3]&=~1u;               // integerComponents is a numeric select too.
    const auto raw=XenosTextureDescription::decode(words);
    if(!raw)return std::nullopt;
    auto plan=planXenosTexture2D(*raw);
    // The element size is the whole basis for reusing the byte-copy path.
    if(plan && plan->bytesPerTexel!=texel)return std::nullopt;
    return plan;
}
// Retained name for the k_32_FLOAT-only callers and tests.
inline std::optional<XenosTexture2DLayout> planXenosFloat32Storage(
        const XenosTextureDescription& destination) {
    const auto d=XenosTextureDescription::decode(destination.fetch);
    return d && d->format==36?planXenosFloatStorage(destination):std::nullopt;
}
}
