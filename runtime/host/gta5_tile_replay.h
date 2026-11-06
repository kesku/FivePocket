#pragma once
#include "gta5_logical_tile_clear.h"

namespace recomp::gpu {
// Original worker 836BDDC0, consumed by 836BDE28's SET_BIN_SELECT_LO (0x62).
// Input flags are worker flags, NOT BeginTiling flags. The returned value is
// the GPU bin selection, despite the historical "ReplayMask" API name. The high
// marker bit is used for first-pass-only worker work. No GPU admission implied.
inline std::optional<std::uint32_t> gta5DriverTileReplayMask(
        std::uint32_t workerFlags,unsigned bin) {
    if(bin>=16)return std::nullopt;
    auto mask=workerFlags&0x80000000u?0x2AAAAAAAu:
        workerFlags&0x40000000u?0x15555555u:UINT32_MAX;
    if(workerFlags&0x20000000u) {
        mask&=3u<<(bin*2);
        if(!bin && !(workerFlags&0x80000000u))mask|=0x80000000u;
    }
    return mask;
}

struct Gta5LogicalTileCarry {
    unsigned sourceTile=0,destinationTile=0;
    std::array<unsigned,4> source{},destination{};
};
// Consecutive replay passes reuse the SAME physical EDRAM target slots. In
// full-logical-size native storage, transfer the prior pass's POST-operation
// contents into the next logical tile at equal EDRAM-local coordinates. This
// is not a clear or a resolve. In particular it must not copy PRE-clear pixels
// that were saved for a resolve. Unknown EDRAM padding outside the previous
// logical tile requires separate backing storage and currently rejects.
// Caller proves pass completion/order and holds current owned headers; this
// planner does not retire guest recording lists, callbacks or ring submissions.
inline std::optional<Gta5LogicalTileCarry> planGta5LogicalTileCarry(
        const Gta5ClearCapture& surfaces,unsigned source,unsigned destination) {
    if(!surfaces.state || surfaces.state->size()<13860)return std::nullopt;
    const auto& state=*surfaces.state;
    // Physical reuse is not an invocation of the clear shader. Shader-ready
    // bit1 cannot alter the local-address mapping. Keep every other previous
    // state exclusion and all ownership/layout/unknown-padding checks.
    if((state[11068]&0xD0) || (state[11071]&0x20) || (state[11072]&0xC0))return std::nullopt;
    const auto p=describeGta5LogicalTileSurfaces(surfaces);
    if(!p || destination>=p->layout.tiles.size() || source>=p->layout.tiles.size() ||
       destination!=source+1)return std::nullopt;
    const auto& from=p->layout.tiles[source];const auto& to=p->layout.tiles[destination];
    for(unsigned i=0;i<4;++i)if(i<2?to.local[i]<from.local[i]:to.local[i]>from.local[i])
        return std::nullopt;
    Gta5LogicalTileCarry result;result.sourceTile=source;result.destinationTile=destination;
    result.destination=to.logical;
    for(unsigned i=0;i<4;++i)result.source[i]=to.local[i]+from.origin[i%2];
    return result;
}
}
