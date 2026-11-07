#pragma once
#include "gta5_logical_tile_clear.h"
#include "native_offscreen_resolve_request.h"

namespace recomp::gpu {
// Default callers cannot discharge original GPU cache-maintenance work.
// FencedNativeImages is only a planning contract: the consumer must own the
// same native images, fence prior submissions and barrier writes before copy.
// It never permits clearing guest dirty bits or acknowledging a guest fence.
enum class Gta5ResolveSynchronization { RejectCacheMaintenance, FencedNativeImages };
// API82388A38 -> native logical image regions. In the original823C2738,
// COPY_DEST_BASE includes (destinationPoint-sourceTopLeft), then each bin's
// origin is added to it. Full-size native images already contain that origin:
// destination = logicalSource + destinationPoint - sourceTopLeft, exactly once.
// This plans color copies/clear-after-copy, not tile recording or publication.
struct Gta5LogicalTileResolve {
    struct Region {
        unsigned tile=0;
        std::array<unsigned,4> source{},destination{};
    };
    Gta5DriverTiling layout;
    unsigned slot=0,tiles=0;
    unsigned requiredCacheMaintenance=0;
    std::array<unsigned,4> rectangle{};
    XenosTexture2DLayout storage;
    std::optional<std::array<std::uint8_t,4>> clearAfterCopy;
    std::vector<Region> regions;
};
inline std::optional<Gta5LogicalTileSurfaces> describeGta5LogicalTileResolveSurfaces(const Gta5ClearCapture& c,
        Gta5ResolveSynchronization synchronization=Gta5ResolveSynchronization::RejectCacheMaintenance) {
    if(!c.state || c.state->size()<13860)return std::nullopt;
    const auto& state=*c.state;
    // Resolve copies an existing native image, not the original clear shader.
    // Original823BAAC8's shader-ready11071 bit1 does not change surface identity
    // or the82388A38/823C2738 copy address/clear-value prefix. Keep all remaining
    // auxiliary/query/unknown-state exclusions; this is not full driver replay.
    if((state[11068]&0xD0) || (state[11071]&0x20))return std::nullopt;
    // Original823C2738 sets11072 C0 after a resolve;823CCEB8 emits
    // WAIT_UNTIL(5C8,20000), optional E00/E40 invalidation, then clears C0.
    // These change synchronization requirements, not image identity/layout.
    if((state[11072]&0xC0) && synchronization!=Gta5ResolveSynchronization::FencedNativeImages)return std::nullopt;
    return describeGta5LogicalTileSurfaces(c);
}
// Shared coordinate/selection validation only. Callers validate copy format,
// flags and source aspect separately; this helper grants no execution rights.
inline std::optional<Gta5LogicalTileResolve> planGta5LogicalTileResolveGeometry(
        const Gta5ClearCapture& surfaces,const NativeOffscreenResolveRequest& r,
        const NativeOffscreenResolveArguments& a,const XenosTexture2DLayout& storage,unsigned slot,
        Gta5ResolveSynchronization synchronization=Gta5ResolveSynchronization::RejectCacheMaintenance) {
    // Physical owned-header/layout/alias proof with resolve-specific state.
    // Clear-call arguments in 'surfaces' are not resolve-call arguments.
    auto source=describeGta5LogicalTileResolveSurfaces(surfaces,synchronization);
    if(!source || slot>=5 || r.mip || r.slice)return std::nullopt;
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto sized=[](const auto& b,unsigned n){return b && b->size()==n;};
    const auto zero=[&](const auto& b){return sized(b,4) && word(*b,0)==0;};
    if(!r.stack || r.stack%4 || r.stack>UINT32_MAX-103 || !zero(a.stack92) || !zero(a.stack100) ||
       !source->layout.boundTargets[slot] || !surfaces.surfaces[slot].header)return std::nullopt;
    Gta5LogicalTileResolve p;p.layout=source->layout;p.slot=slot;p.storage=storage;
    p.requiredCacheMaintenance=(*surfaces.state)[11072]&0xC0;
    p.rectangle={0,0,p.storage.description.width,p.storage.description.height};
    if(r.rectangle) {
        if(r.rectangle%4 || r.rectangle>UINT32_MAX-15 || !sized(a.rectangle,16))return std::nullopt;
        for(unsigned i=0;i<4;++i)p.rectangle[i]=word(*a.rectangle,i*4);
    }
    const auto& rect=p.rectangle;
    // Original resolve geometry rounds right/bottom up8. Padding ownership and
    // partial microtile destinations need separate support; never silently crop
    // the rounded clear. The game's1280x384/336 stripe copies are exact here.
    if(rect[0]>=rect[2] || rect[1]>=rect[3] || rect[2]>p.layout.width || rect[3]>p.layout.height)
        return std::nullopt;
    for(auto v:rect)if(v%8)return std::nullopt;
    std::array<unsigned,2> point{};
    if(r.destinationPoint) {
        if(r.destinationPoint%4 || r.destinationPoint>UINT32_MAX-7 || !sized(a.point,8))return std::nullopt;
        point={word(*a.point,0),word(*a.point,4)};
    }
    const std::array<std::int64_t,2> delta{std::int64_t{point[0]}-rect[0],std::int64_t{point[1]}-rect[1]};
    if(delta[0]%32 || delta[1]%32)return std::nullopt;
    const auto& state=*surfaces.state;
    if(word(state,13088))return std::nullopt;
    const auto selection=word(state,13084);
    for(unsigned i=0;i<p.layout.tiles.size();++i) {
        const auto& tile=p.layout.tiles[i];
        if(!(selection&tile.selectLow))continue;
        auto clipped=rect;
        for(unsigned j=0;j<4;++j)clipped[j]=j<2?std::max(rect[j],tile.logical[j]):std::min(rect[j],tile.logical[j]);
        if(clipped[0]>=clipped[2] || clipped[1]>=clipped[3])continue;
        Gta5LogicalTileResolve::Region region;region.tile=i;region.source=clipped;
        for(unsigned j=0;j<4;++j) {
            if(clipped[j]%8)return std::nullopt;
            const auto coordinate=std::int64_t{clipped[j]}+delta[j%2];
            if(coordinate<0 || coordinate>(j%2?p.storage.description.height:p.storage.description.width))return std::nullopt;
            region.destination[j]=static_cast<unsigned>(coordinate);
        }
        p.tiles|=1u<<i;p.regions.push_back(region);
    }
    return p;
}
inline std::optional<Gta5LogicalTileResolve> planGta5LogicalTileResolve(
        const Gta5ClearCapture& surfaces,const NativeOffscreenResolveRequest& r,
        const NativeOffscreenResolveArguments& a,const XenosTextureDescription& destination,
        Gta5ResolveSynchronization synchronization=Gta5ResolveSynchronization::RejectCacheMaintenance) {
    const unsigned slot=r.flags&7,mode=r.flags&0x70;
    if(slot>=4 || (r.flags&~0x117u) || (mode && mode!=0x10))return std::nullopt;
    const auto source=describeGta5LogicalTileResolveSurfaces(surfaces,synchronization);
    const auto view=planNativeOffscreenResolveDestination(destination,0);
    if(!source || source->formats[slot]!=0 || !view ||
       view->original.sign!=std::array<unsigned,4>{0,0,0,0})return std::nullopt;
    auto p=planGta5LogicalTileResolveGeometry(surfaces,r,a,view->storage,slot,synchronization);
    if(!p)return std::nullopt;
    if(r.flags&0x100) {
        const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
            (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
        const auto sized=[](const auto& b,unsigned n){return b && b->size()==n;};
        if(!r.clearColor || r.clearColor%16 || r.clearColor>UINT32_MAX-15 || !sized(a.color,16))return std::nullopt;
        std::array<float,4> color{};
        for(unsigned i=0;i<4;++i){const auto bits=word(*a.color,i*4);std::memcpy(&color[i],&bits,4);}
        p->clearAfterCopy=packGta5Color8Clear(color,0);
        if(!p->clearAfterCopy)return std::nullopt;
    }
    return p;
}
// Region readback -> destination storage, including actual copy R/B swap.
// Sampling alpha swizzle does not replace the copied source alpha byte.
inline std::optional<std::vector<std::uint8_t>> packGta5LogicalTileResolveRegion(
        const Gta5LogicalTileResolve& p,const Gta5LogicalTileResolve::Region& region,
        const std::vector<std::uint8_t>& rgba,const std::vector<std::uint8_t>& previous) {
    if(region.source[0]>=region.source[2] || region.source[1]>=region.source[3] ||
       rgba.size()!=std::uint64_t{region.source[2]-region.source[0]}*(region.source[3]-region.source[1])*4 ||
       region.destination[2]-region.destination[0]!=region.source[2]-region.source[0] ||
       region.destination[3]-region.destination[1]!=region.source[3]-region.source[1])return std::nullopt;
    if(p.storage.description.swizzle[0]==0)return storeXenosTexture2DRegion(p.storage.description,region.destination,rgba,previous);
    auto swapped=rgba;for(std::size_t i=0;i<swapped.size();i+=4)std::swap(swapped[i],swapped[i+2]);
    return storeXenosTexture2DRegion(p.storage.description,region.destination,swapped,previous);
}
}
