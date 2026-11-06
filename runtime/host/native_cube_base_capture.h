#pragma once
#include "native_color8_resolve.h"
#include "gta5_inline_draw.h"

namespace recomp::gpu {
// Storage capture ONLY, using the same bounded original-code-validated face
// projection as native cube resolves. Capturing all six faces does NOT enable
// cube sampling, PWL filtering, rectangle rendering or GPU ownership claims.
inline std::optional<Gta5InlineDraw::Texture> captureNativeCubeBase(unsigned slot,
        const std::array<std::uint32_t,6>& fetch,const Gta5ResourceReader& ownedGraphics) {
    const auto original=XenosTextureDescription::decode(fetch);
    if(slot>=32 || !ownedGraphics || !original || original->dimension!=XenosTextureDescription::Dimension::Cube)return std::nullopt;
    std::array<NativeColor8ResolveDestination,6> faces;
    std::uint64_t total=0;
    for(unsigned face=0;face<6;++face) {
        const auto plan=planNativeColor8ResolveFace(*original,face);
        if(!plan || plan->storage.sourceBytes!=std::uint64_t{original->width}*original->height*4 ||
           plan->storage.description.baseAddress!=std::uint64_t{original->baseAddress}+total)return std::nullopt;
        total+=plan->storage.sourceBytes;faces[face]=*plan;
    }
    if(!total || total>6u*512*512*4)return std::nullopt;
    Gta5InlineDraw::Texture result;result.slot=slot;result.fetch=fetch;result.source.reserve(total);
    // Each face independently passes the caller's current allocation-domain
    // ownership checks. No masking address bits, zero filling or partial success.
    for(const auto& face:faces) {
        const auto& storage=face.storage;
        const auto bytes=ownedGraphics(storage.description.baseAddress,storage.sourceBytes);
        if(!bytes || bytes->size()!=storage.sourceBytes)return std::nullopt;
        result.source.insert(result.source.end(),bytes->begin(),bytes->end());
    }
    return result;
}
}
