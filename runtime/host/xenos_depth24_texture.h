#pragma once
#include "xenos_float24_clear.h"
#include "xenos_texture_2d.h"
#include <cstring>

namespace recomp::gpu {
// Explicit D24FS8 raw32 address view, not a color conversion. The depth value
// occupies bits8..31 and stencil bits0..7. Keep the original descriptor at the
// owning image/copy plan; the returned format6 view is only for byte addressing.
// The pinned ReX/Xenia texture_load_depth_float shader drops the low8 bits and
// expands20e4 into R32_SFLOAT. General RGBA import remains strict.
inline std::optional<XenosTexture2DLayout> planXenosDepth24FloatStorage(const XenosTextureDescription& destination) {
    const auto d=XenosTextureDescription::decode(destination.fetch);
    if(!d || d->format!=23 || !d->tiled || d->mipMin || d->mipMax || d->mipAddress ||
       d->packedMips || d->exponentBias || d->sign!=std::array<unsigned,4>{0,0,0,0} ||
       d->swizzle!=std::array<unsigned,4>{0,1,2,3})return std::nullopt;
    auto words=d->fetch;words[1]=(words[1]&~63u)|6u;words[3]&=~1u;
    const auto raw=XenosTextureDescription::decode(words);
    return raw?planXenosTexture2D(*raw):std::nullopt;
}
// Raw host float32 bytes, before scalar-to-RGBA view replication. All20e4
// bit patterns are finite; storage imports may contain depths in[0,2), unlike
// this project's clamped native raster producer. No stencil-as-alpha sampling.
inline std::optional<std::vector<std::uint8_t>> decodeXenosDepth24FloatTexture(
        const XenosTextureDescription& description,const std::vector<std::uint8_t>& source) {
    const auto storage=planXenosDepth24FloatStorage(description);
    if(!storage || source.size()!=storage->sourceBytes)return std::nullopt;
    auto linear=linearizeXenosTexture2D(storage->description,source);if(!linear)return std::nullopt;
    for(std::size_t at=0;at<linear->size();at+=4) {
        const auto word=std::uint32_t{(*linear)[at]}|(std::uint32_t{(*linear)[at+1]}<<8)|
            (std::uint32_t{(*linear)[at+2]}<<16)|(std::uint32_t{(*linear)[at+3]}<<24);
        const auto value=decodeXenosFloat24(word>>8);if(!value)return std::nullopt;
        std::memcpy(linear->data()+at,&*value,4);
    }
    return linear;
}
}
