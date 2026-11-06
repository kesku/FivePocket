#pragma once
#include "gta5_inline_draw.h"
#include "native_texture_sampler.h"

namespace recomp::gpu {
struct NativeMipTextureCapture {
    std::array<std::uint32_t,6> fetch{};
    std::vector<std::uint8_t> base,mips;
};
// Bookkeeping for a completed, fenced draw that consumed independent copies.
// Canonical addresses come from the current allocation owner, never a mask.
// Merely constructing this record does NOT establish GPU completion.
struct NativeMipTextureUse {
    XenosTextureDescription description;
    std::uint32_t baseAddress=0,mipAddress=0,baseBytes=0,mipBytes=0;
};
using NativeTextureOwnerResolver=std::function<std::optional<std::uint32_t>(std::uint32_t,std::uint32_t)>;
inline std::optional<NativeMipTextureUse> describeNativeMipTextureUse(
        const std::array<std::uint32_t,6>& fetch,const NativeTextureOwnerResolver& owner) {
    const auto d=XenosTextureDescription::decode(fetch);
    const auto chain=planXenosUnpackedMipChain(fetch);
    if(!owner || !d || !chain || !chain->mipBytes)return std::nullopt;
    const auto base=owner(d->baseAddress,chain->baseBytes),mips=owner(d->mipAddress,chain->mipBytes);
    if(!base || !mips || !*base || !*mips || std::uint64_t{*base}+chain->baseBytes>0x100000000ULL ||
       std::uint64_t{*mips}+chain->mipBytes>0x100000000ULL)return std::nullopt;
    return NativeMipTextureUse{*d,*base,*mips,chain->baseBytes,chain->mipBytes};
}
inline bool sameNativeMipTextureStorage(const NativeMipTextureUse& a,const NativeMipTextureUse& b) {
    const auto& x=a.description;const auto& y=b.description;
    return a.baseAddress==b.baseAddress && a.mipAddress==b.mipAddress && a.baseBytes==b.baseBytes && a.mipBytes==b.mipBytes &&
        x.width==y.width && x.height==y.height && x.depthOrLayers==y.depthOrLayers && x.pitchTexels==y.pitchTexels &&
        x.dimension==y.dimension && x.format==y.format && x.tiled==y.tiled && x.endian==y.endian &&
        x.mipMin==y.mipMin && x.mipMax==y.mipMax && x.packedMips==y.packedMips && x.stacked==y.stacked;
}
// Both planes are read through the caller's allocation-owning graphics reader
// under its exclusive execution gate. Keep the original submitted descriptor
// and address namespace; never flatten a mip chain into a single-level fetch,
// mask a guessed physical alias, or manufacture bytes for missing levels.
// This collects inputs only. Shader, target, sampler fidelity and draw-state
// admission remain independent obligations of the eventual renderer.
inline std::optional<NativeMipTextureCapture> captureNativeMipTextureStorage(
        const std::array<std::uint32_t,6>& fetch,const Gta5ResourceReader& graphics,
        std::uint64_t byteBudget=64u*1024*1024) {
    // Storage capture is independent of sampler admission. An unsupported
    // anisotropic/biased sampler does not make its owned image bytes unreadable.
    // This helper never authorizes that sampling policy or a draw.
    if(!graphics)return std::nullopt;
    const auto chain=planXenosUnpackedMipChain(fetch);
    const auto description=XenosTextureDescription::decode(fetch);
    if(!chain || !description || !chain->baseBytes ||
       std::uint64_t{chain->baseBytes}+chain->mipBytes>std::min<std::uint64_t>(byteBudget,64u*1024*1024))return std::nullopt;
    auto base=graphics(description->baseAddress,chain->baseBytes);
    if(!base || base->size()!=chain->baseBytes)return std::nullopt;
    std::vector<std::uint8_t> mips;
    if(chain->mipBytes) {
        auto data=graphics(description->mipAddress,chain->mipBytes);
        if(!data || data->size()!=chain->mipBytes)return std::nullopt;
        mips=std::move(*data);
    }
    return NativeMipTextureCapture{fetch,std::move(*base),std::move(mips)};
}
// Preserve the existing renderer-facing contract: callers relying on this
// name still require the implemented sampler as well as complete storage.
inline std::optional<NativeMipTextureCapture> captureNativeMipTexture(
        const std::array<std::uint32_t,6>& fetch,const Gta5ResourceReader& graphics,
        std::uint64_t byteBudget=64u*1024*1024) {
    if(!planNativeTextureSampler2D(fetch))return std::nullopt;
    return captureNativeMipTextureStorage(fetch,graphics,byteBudget);
}
}
