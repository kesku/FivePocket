#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace recomp::gpu {
// Original 822CB6F8 prepares these tables before 8248AFF0 starts recording.
// Coordinates are in the logical target. Offsets move each tile into EDRAM's
// local coordinate system; they are NOT extra translations of the game world.
// This is a CPU layout description, not allocation ownership or GPU admission.
struct Gta5DriverTiling {
    struct Tile {
        std::array<std::uint32_t,4> logical{}, local{};
        std::array<std::uint32_t,2> origin{};
        std::uint32_t selectLow=0;
    };
    std::uint32_t width=0,height=0,beginFlags=0,primaryTarget=0;
    std::uint8_t flags=0,flags3=0;
    std::array<std::uint32_t,5> targets{},boundTargets{};
    std::vector<Tile> tiles;
    bool boundTargetsUseTiles=false,disjointFullCoverage=false;
};

// Recognize a bounded, positive layout produced by the original BeginTiling.
// Gaps/overlaps are retained, not silently filled or executed twice. A complete
// partition is necessary but NOT sufficient to replace replay with one draw:
// selected-bin work, per-bin clears, target aliases and resolves still matter.
inline std::optional<Gta5DriverTiling> describeGta5DriverTiling(
        const std::vector<std::uint8_t>& state) {
    constexpr unsigned Rects=13128,Offsets=13368,MaxTiles=(Offsets-Rects)/16;
    if(state.size()<13568 || !(state[11068]&0x30))return std::nullopt;
    const auto word=[&](unsigned at) {
        return (std::uint32_t{state[at]}<<24)|(std::uint32_t{state[at+1]}<<16)|
            (std::uint32_t{state[at+2]}<<8)|state[at+3];
    };
    const auto count=word(13124);
    if(!count || count>MaxTiles)return std::nullopt;
    Gta5DriverTiling result;
    result.width=word(13556);result.height=word(13560);
    result.beginFlags=word(13564);result.primaryTarget=word(13100);
    result.flags=state[11068];result.flags3=state[11071];
    if(!result.width || !result.height || result.width>0x3FFF || result.height>0x3FFF)
        return std::nullopt;
    bool matching=true;
    for(unsigned i=0;i<5;++i) {
        result.targets[i]=word(13104+i*4);result.boundTargets[i]=word(12816+i*4);
        if(result.boundTargets[i] && result.boundTargets[i]!=result.targets[i])matching=false;
    }
    result.boundTargetsUseTiles=(result.flags&0x10) || ((result.flags&0x20) && matching);
    const auto primary=result.targets[0]?result.targets[0]:result.targets[4];
    if(result.primaryTarget!=primary)return std::nullopt;
    std::uint64_t area=0;unsigned right=0,bottom=0;bool disjoint=true;
    for(unsigned i=0;i<count;++i) {
        Gta5DriverTiling::Tile tile;
        for(unsigned j=0;j<4;++j)tile.logical[j]=word(Rects+i*16+j*4);
        for(unsigned j=0;j<2;++j)tile.origin[j]=word(Offsets+i*8+j*4);
        const auto& r=tile.logical;
        if(r[0]>=r[2] || r[1]>=r[3] || r[2]>result.width || r[3]>result.height ||
           tile.origin[0]!=(r[0]&~31u) || tile.origin[1]!=(r[1]&~31u))return std::nullopt;
        tile.local={r[0]-tile.origin[0],r[1]-tile.origin[1],r[2]-tile.origin[0],r[3]-tile.origin[1]};
        tile.selectLow=3u<<(2*i);
        for(const auto& prior:result.tiles) {
            const auto& p=prior.logical;
            if(std::max(r[0],p[0])<std::min(r[2],p[2]) &&
               std::max(r[1],p[1])<std::min(r[3],p[3]))disjoint=false;
        }
        area+=std::uint64_t{r[2]-r[0]}*(r[3]-r[1]);
        right=std::max(right,r[2]);bottom=std::max(bottom,r[3]);
        result.tiles.push_back(tile);
    }
    // The original producer stores maxima, not sums or EDRAM tile heights.
    if(right!=result.width || bottom!=result.height)return std::nullopt;
    result.disjointFullCoverage=disjoint && area==std::uint64_t{result.width}*result.height;
    return result;
}
}
