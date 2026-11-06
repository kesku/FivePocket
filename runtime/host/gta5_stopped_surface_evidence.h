#pragma once
#include "gta5_inline_draw.h"

namespace recomp::gpu {
// Terminal evidence only. A failed graphics-arena read does not establish that
// a bound surface is stale: the original API may bind a current-stack header.
// Keep this separate from execution admission and never replace a current
// binding with a cached object. The supplied reader must enforce current-worker
// stack/heap ownership under the exclusive guest gate.
struct Gta5StoppedSurfaceEvidence {
    std::uint32_t stack=0,link=0;
    struct Surface {std::uint32_t object=0;std::optional<std::vector<std::uint8_t>> argumentHeader;};
    struct Frame {std::uint32_t stack=0,parent=0,savedLink=0;};
    std::array<Surface,5> surfaces{};
    std::vector<Frame> frames;
};
inline Gta5StoppedSurfaceEvidence captureGta5StoppedSurfaceEvidence(
        const std::vector<std::uint8_t>& state,std::uint32_t stack,std::uint32_t link,
        const Gta5ResourceReader& argument) {
    Gta5StoppedSurfaceEvidence result;result.stack=stack;result.link=link;
    const auto read=[&](std::uint32_t at,unsigned size,unsigned align)
            ->std::optional<std::vector<std::uint8_t>> {
        if(!argument || !at || at%align || at>UINT32_MAX-(size-1))return std::nullopt;
        auto bytes=argument(at,size);
        return bytes && bytes->size()==size?std::move(bytes):std::nullopt;
    };
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    if(state.size()==13860)for(unsigned i=0;i<5;++i) {
        auto& surface=result.surfaces[i];surface.object=word(state,12816+4*i);
        surface.argumentHeader=read(surface.object,40,4);
    }
    for(unsigned i=0;i<8;++i) {
        const auto frame=read(stack,4,16);if(!frame)break;
        const auto parent=word(*frame,0);
        if(parent<=stack || parent-stack>65536 || parent%16)break;
        const auto saved=read(parent-8,4,4);if(!saved)break;
        result.frames.push_back({stack,parent,word(*saved,0)});stack=parent;
    }
    return result;
}
}
