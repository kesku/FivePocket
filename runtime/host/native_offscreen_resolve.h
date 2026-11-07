#pragma once
#include "native_offscreen_draw_plan.h"
#include "xenos_texture_mip_chain.h"

namespace recomp::gpu {
struct NativeOffscreenResolveDestination {
    XenosTextureDescription original;
    unsigned level=0;
    XenosTexture2DLayout storage;
};
// Explicit subresource selection, not a replacement submitted fetch. Original
// 823C2890..823C2940 uses the base address directly for mip0/slice0, otherwise
// 82343AD8 supplies the mip address/pitch/extent. The existing unpacked planner
// agrees with that original helper for tiled RGBA8, including NPOT dimensions.
// No mip generation, sampling, chain completion or allocation ownership implied.
inline std::optional<NativeOffscreenResolveDestination> planNativeOffscreenResolveDestination(
        const XenosTextureDescription& destination,unsigned level) {
    const auto d=XenosTextureDescription::decode(destination.fetch);
    if(!d || d->format!=6 || !d->tiled || d->packedMips || d->integerComponents ||
       d->exponentBias || d->mipMin || d->swizzle[1]!=1 ||
       (d->swizzle[3]!=3 && d->swizzle[3]!=5) ||
       !((d->swizzle[0]==0 && d->swizzle[2]==2) || (d->swizzle[0]==2 && d->swizzle[2]==0)))return std::nullopt;
    const auto chain=planXenosUnpackedMipChain(d->fetch);
    if(!chain || level>=chain->levels.size())return std::nullopt;
    // Independent base/mip planes only. Do not admit ambiguous overlapping
    // storage views merely because the requested level has a valid extent.
    if(chain->mipBytes && std::uint64_t{d->baseAddress}<std::uint64_t{d->mipAddress}+chain->mipBytes &&
       std::uint64_t{d->mipAddress}<std::uint64_t{d->baseAddress}+chain->baseBytes)return std::nullopt;
    return NativeOffscreenResolveDestination{*d,level,chain->levels[level].layout};
}
// Default full-rectangle color0 resolve into one tiled RGBA8 texture. Caller
// validates original82388A38 arguments, bound source identity/lifetime, and
// before/after allocation ownership. No clear, exponent or format conversion.
inline std::optional<std::vector<std::uint8_t>> packNativeOffscreenResolve(
        const NativeOffscreenDrawPlan& source,const XenosTextureDescription& destination,
        const std::vector<std::uint8_t>& rgba,const std::vector<std::uint8_t>& previous) {
    const auto layout=planXenosTexture2D(destination);
    if(!layout || !source.width || !source.height || source.width>1280 || source.height>720)return std::nullopt;
    const auto& d=layout->description;
    if(d.format!=6 || !d.tiled || d.width!=source.width || d.height!=source.height ||
       rgba.size()!=std::uint64_t{source.width}*source.height*4 || previous.size()!=layout->sourceBytes ||
       d.swizzle[1]!=1 || (d.swizzle[3]!=3 && d.swizzle[3]!=5) ||
       !((d.swizzle[0]==0 && d.swizzle[2]==2) || (d.swizzle[0]==2 && d.swizzle[2]==0)))return std::nullopt;
    // Original823C2970..823C2AB8 sets COPY_DEST_INFO.swap when fetch word3
    // bits1..3 (sampled red selector) are nonzero. Admit only the two ordinary
    // RGB orders above. Destination alpha swizzle is a sampling operation;
    // keep the actual shader alpha byte in storage, never force it to255.
    if(d.swizzle[0]==0)return storeXenosTexture2D(d,rgba,previous);
    auto bgra=rgba;
    for(std::size_t i=0;i<bgra.size();i+=4)std::swap(bgra[i],bgra[i+2]);
    return storeXenosTexture2D(d,bgra,previous);
}
inline std::optional<std::vector<std::uint8_t>> packNativeOffscreenResolveLevel(
        const NativeOffscreenDrawPlan& source,const XenosTextureDescription& destination,unsigned level,
        const std::vector<std::uint8_t>& rgba,const std::vector<std::uint8_t>& previous) {
    const auto selected=planNativeOffscreenResolveDestination(destination,level);
    return selected?packNativeOffscreenResolve(source,selected->storage.description,rgba,previous):std::nullopt;
}
// Explicit top-left mip region, as emitted by 82369828 at82369C90..82369CEC:
// rectangle=(0,0,baseWidth>>level,baseHeight>>level), destination point=(0,0).
// The source surface itself is not resized. All source bytes must be available;
// untouched source pixels and other destination levels remain unchanged.
// Callers still validate the ORIGINAL rectangle, copy flags and ownership.
inline std::optional<std::vector<std::uint8_t>> packNativeOffscreenResolveMipRegion(
        const NativeOffscreenDrawPlan& source,const XenosTextureDescription& destination,unsigned level,
        unsigned width,unsigned height,const std::vector<std::uint8_t>& rgba,
        const std::vector<std::uint8_t>& previous) {
    const auto selected=planNativeOffscreenResolveDestination(destination,level);
    if(!selected || !source.width || !source.height || source.width>1280 || source.height>720 ||
       !width || !height || width>source.width || height>source.height ||
       width!=selected->storage.description.width || height!=selected->storage.description.height ||
       rgba.size()!=std::uint64_t{source.width}*source.height*4)return std::nullopt;
    auto region=source;region.width=width;region.height=height;
    std::vector<std::uint8_t> cropped(std::size_t{width}*height*4);
    for(unsigned y=0;y<height;++y)
        std::copy_n(rgba.begin()+std::size_t{y}*source.width*4,width*4,cropped.begin()+std::size_t{y}*width*4);
    return packNativeOffscreenResolve(region,selected->storage.description,cropped,previous);
}
}
