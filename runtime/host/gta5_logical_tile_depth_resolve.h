#pragma once
#include "gta5_logical_tile_resolve.h"
#include "xenos_depth24_texture.h"

namespace recomp::gpu {
struct Gta5LogicalTileDepthResolve {
    Gta5LogicalTileResolve geometry;
    XenosTextureDescription destination;
};
inline std::optional<Gta5LogicalTileDepthResolve> planGta5LogicalTileDepthResolve(
        const Gta5ClearCapture& surfaces,const NativeOffscreenResolveRequest& request,
        const NativeOffscreenResolveArguments& arguments,const XenosTextureDescription& destination,
        Gta5ResolveSynchronization synchronization=Gta5ResolveSynchronization::RejectCacheMaintenance) {
    // Actual first scene call: source4, single-sample, no clear/conversion or
    // mip/slice operation. F1 is unused without a depth-clear flag.
    if(request.flags!=0x14 || request.mip || request.slice)return std::nullopt;
    const auto source=describeGta5LogicalTileResolveSurfaces(surfaces,synchronization);
    const auto storage=planXenosDepth24FloatStorage(destination);
    if(!source || source->formats[4]!=1 || !storage)return std::nullopt;
    auto geometry=planGta5LogicalTileResolveGeometry(surfaces,request,arguments,*storage,4,synchronization);
    if(!geometry)return std::nullopt;
    return Gta5LogicalTileDepthResolve{*geometry,*XenosTextureDescription::decode(destination.fetch)};
}
inline std::optional<std::vector<std::uint8_t>> packGta5LogicalTileDepthResolveRegion(
        const Gta5LogicalTileDepthResolve& plan,const Gta5LogicalTileResolve::Region& region,
        const std::vector<float>& depth,const std::vector<std::uint8_t>& stencil,
        const std::vector<std::uint8_t>& previous) {
    const auto& g=plan.geometry;
    if(g.slot!=4 || g.clearAfterCopy || region.source[0]>=region.source[2] || region.source[1]>=region.source[3] ||
       depth.size()!=std::uint64_t{region.source[2]-region.source[0]}*(region.source[3]-region.source[1]) ||
       stencil.size()!=depth.size() || region.destination[2]-region.destination[0]!=region.source[2]-region.source[0] ||
       region.destination[3]-region.destination[1]!=region.source[3]-region.source[1])return std::nullopt;
    std::vector<std::uint8_t> packed(depth.size()*4);
    for(std::size_t i=0;i<depth.size();++i) {
        // The native producer already writes quantized20e4. Do not round an
        // arbitrary D32 value here and hide a producer/consumer mismatch.
        const auto encoded=encodeExactXenosFloat24(depth[i]);
        if(!encoded || depth[i]>1)return std::nullopt;
        const auto word=(*encoded<<8)|stencil[i];
        for(unsigned b=0;b<4;++b)packed[i*4+b]=word>>(8*b);
    }
    return storeXenosTexture2DRegion(g.storage.description,region.destination,packed,previous);
}
}
