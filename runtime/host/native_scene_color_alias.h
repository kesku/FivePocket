#pragma once
#include "gta5_logical_tile_clear.h"
#include "native_edram32_bands.h"

namespace recomp::gpu {
// Logical tiled-scene RGBA8 output -> physical raw32 colour rectangle. This
// describes ONE just-completed bin, never the whole logical image at EndTiling.
// The next bin reuses physical row zero. A shorter final bin leaves the earlier
// bin's physical tail intact; adding the logical Y origin would be incorrect.
// Supplied bytes must come from the same target's fenced, initialized readback at
// the matching bin boundary, not from an older capture or an entire late image.
struct NativeSceneColorAliasPlan {
    NativeOffscreenDrawPlan physical;
    std::array<unsigned,4> sourceRectangle{};
};
inline std::optional<NativeSceneColorAliasPlan> planNativeSceneColorAlias(
        const Gta5ClearCapture& capture,unsigned bin,unsigned slot) {
    const auto surfaces=describeGta5LogicalTileSurfaces(capture);
    if(!surfaces || slot>=4 || !surfaces->layout.targets[slot] || surfaces->formats[slot] ||
       bin>=surfaces->layout.tiles.size())return std::nullopt;
    const auto& t=surfaces->layout.tiles[bin];
    if(t.local[0] || t.local[1] || t.logical[0] || t.origin[0] ||
       t.logical[2]!=surfaces->layout.width || t.local[2]%80 || t.local[3]%16)return std::nullopt;
    const auto& h=*capture.surfaces[slot].header;
    const auto word=[&](unsigned at){return (std::uint32_t{h[at]}<<24)|(std::uint32_t{h[at+1]}<<16)|
        (std::uint32_t{h[at+2]}<<8)|h[at+3];};
    const auto pitch=word(24)&0x3FFF,base=word(28)&0x7FF;
    // A full-width, origin-zero subset makes the physical footprint exact;
    // other scissor/origin/pitch arrangements require a separate mapping.
    if(pitch!=t.local[2])return std::nullopt;
    const auto count=nativeOffscreenTileCount(pitch,t.local[3],0);
    if(!count || *count>2048-base)return std::nullopt;
    return NativeSceneColorAliasPlan{{capture.surfaces[slot].object,t.local[2],t.local[3],pitch,base,*count,0},t.logical};
}
using NativeSceneColorAliases=std::array<NativeSceneColorAliasPlan,4>;
// Invalidate exactly this bin's possible colour AND depth writes before GPU
// work. A shorter later bin must leave the previous physical tail untouched.
// Only the observed four-RGBA8 + depth layout is admitted. Unknown layouts
// leave the caller's global owner empty and grant no execution/publication.
inline std::optional<NativeSceneColorAliases> prepareNativeSceneColorAliases(
        NativeEdram32Mutation& mutation,const Gta5ClearCapture& capture,unsigned bin) {
    const auto surfaces=describeGta5LogicalTileSurfaces(capture);
    if(!surfaces || surfaces->boundAspects!=0x3F)return std::nullopt;
    NativeSceneColorAliases plans;
    for(unsigned slot=0;slot<4;++slot) {
        const auto plan=planNativeSceneColorAlias(capture,bin,slot);
        if(!plan)return std::nullopt;
        plans[slot]=*plan;
    }
    const auto& header=*capture.surfaces[4].header;
    const unsigned depthBase=((unsigned(header[28])<<24)|(unsigned(header[29])<<16)|
        (unsigned(header[30])<<8)|header[31])&0x7FF;
    // The surface descriptor verifies common pitch, single-sampling, depth
    // format and non-overlap. The colour planner proves full-width local rows.
    auto& candidate=mutation.candidate();
    if(!candidate.invalidate(depthBase,plans[0].physical.tileCount))return std::nullopt;
    for(const auto& plan:plans)
        if(!candidate.invalidate(plan.physical.baseTile,plan.physical.tileCount))return std::nullopt;
    return plans;
}
// Stage actual post-prefix bytes. No commit here: original scene-resource
// publication and current-input checks must still succeed in the live caller.
template<class Readback>
bool stageCompletedNativeSceneColors(NativeEdram32Mutation& mutation,
        const NativeSceneColorAliases& plans,Readback&& readback) {
    for(unsigned slot=0;slot<plans.size();++slot) {
        const auto bytes=readback(slot);
        if(!bytes || !recordCompletedNativeRaw32Draw(mutation,plans[slot].physical,*bytes))return false;
    }
    return true;
}
}
