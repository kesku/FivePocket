#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace recomp::gpu {
// CPU planning only. Matches original 823ABBE0's register packet emission;
// does not authorize flattening Xbox tile-local targets into a native image.
struct Gta5DriverScissorPlan {
    struct Tile {
        std::uint32_t selectLow=0;
        std::array<std::uint32_t,3> registers{};
    };
    bool binned=false, auxiliaryRectangle=false;
    std::array<std::uint32_t,3> direct{};
    std::array<std::uint32_t,2> restoredSelection{};
    std::vector<Tile> tiles;
    std::vector<std::uint32_t> words;
};
inline std::optional<Gta5DriverScissorPlan> planGta5DriverScissor(
        const std::vector<std::uint8_t>& state,const std::array<std::uint32_t,4>& rectangle) {
    constexpr unsigned HeaderBytes=13128, TileRects=13128, TileOffsets=13368;
    // Fixed tables are 240 bytes apart: at most15 disjoint rectangle entries.
    constexpr unsigned MaxTiles=(TileOffsets-TileRects)/16;
    if(state.size()<HeaderBytes)return std::nullopt;
    const auto word=[&](unsigned at) {
        return (std::uint32_t{state[at]}<<24)|(std::uint32_t{state[at+1]}<<16)|
            (std::uint32_t{state[at+2]}<<8)|state[at+3];
    };
    const auto pack=[](std::uint32_t x,std::uint32_t y) {
        return (x&0x3FFFu)|((y&0x3FFFu)<<16);
    };
    Gta5DriverScissorPlan plan;
    plan.direct={0,pack(rectangle[0],rectangle[1]),pack(rectangle[2],rectangle[3])};
    const auto flags=state[11068];
    plan.binned=(flags&0x10)!=0;
    if(!plan.binned && (flags&0x20)) {
        plan.binned=true;
        for(unsigned i=0;i<5;++i)
            if(word(12816+i*4) && word(12816+i*4)!=word(13104+i*4))plan.binned=false;
    }
    if(!plan.binned) {
        plan.words={0x22080,plan.direct[0],plan.direct[1],plan.direct[2]};
        return plan;
    }
    const unsigned count=word(13124);
    if(count>MaxTiles || (count && state.size()<TileOffsets+count*8))return std::nullopt;
    plan.restoredSelection={word(13084),word(13088)};
    plan.words={0xC0006100,0}; // BIN_SELECT_HI=0 before per-tile state.
    const auto signedWord=[](std::uint32_t value) {
        return std::int64_t{value}-((value&0x80000000u)?0x100000000LL:0LL);
    };
    for(unsigned i=0;i<count;++i) {
        std::array<std::int64_t,4> clip{};
        for(unsigned c=0;c<4;++c) {
            const auto input=signedWord(rectangle[c]),bound=signedWord(word(TileRects+i*16+c*4));
            clip[c]=c<2?std::max(input,bound):std::min(input,bound);
        }
        if(clip[0]>=clip[2] || clip[1]>=clip[3])clip={};
        const auto ox=word(TileOffsets+i*8),oy=word(TileOffsets+i*8+4);
        Gta5DriverScissorPlan::Tile tile{3u<<(i*2),{
            ((0u-ox)&0x7FFFu)|(((0u-oy)&0x7FFFu)<<16),
            pack(static_cast<std::uint32_t>(clip[0]),static_cast<std::uint32_t>(clip[1])),
            pack(static_cast<std::uint32_t>(clip[2]),static_cast<std::uint32_t>(clip[3]))}};
        plan.tiles.push_back(tile);
        // Predicated SET_CONSTANT2 applies this tuple only to its selected bin.
        const std::array<std::uint32_t,7> words{0xC0006000,tile.selectLow,
            0xC0035501,0x2080,tile.registers[0],tile.registers[1],tile.registers[2]};
        plan.words.insert(plan.words.end(),words.begin(),words.end());
    }
    plan.auxiliaryRectangle=(state[11071]&0x20) && (flags&0x40);
    if(plan.auxiliaryRectangle) {
        const std::array<std::uint32_t,6> words{0xC0006000,1,0xC0025501,0x2081,plan.direct[1],plan.direct[2]};
        plan.words.insert(plan.words.end(),words.begin(),words.end());
    }
    const std::array<std::uint32_t,4> restored{0xC0006000,plan.restoredSelection[0],0xC0006100,plan.restoredSelection[1]};
    plan.words.insert(plan.words.end(),restored.begin(),restored.end());
    return plan;
}
}
