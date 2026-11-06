#pragma once
#include "gta5_inline_draw.h"

namespace recomp::gpu {
struct Gta5ClearCall {
    std::uint32_t stack=0,device=0,flags=0,rectangle=0,color=0,stencil=0,link=0;
    std::uint64_t depthBits=0;
};
struct Gta5ClearCapture {
    Gta5ClearCall call;
    std::optional<std::vector<std::uint8_t>> state,rectangle,color;
    struct Surface {std::uint32_t object=0,tileCache=0;std::optional<std::vector<std::uint8_t>> header;};
    std::array<Surface,5> surfaces{};
    struct Frame {std::uint32_t stack=0,parent=0,savedLink=0;};
    std::vector<Frame> frames;
};
// Diagnostic evidence ONLY. Collect all available inputs without deciding
// native support, modifying game state or substituting missing values.
inline Gta5ClearCapture captureGta5ClearCall(const Gta5ClearCall& call,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics,
        const Gta5ResourceReader& argument) {
    Gta5ClearCapture out;out.call=call;
    const auto read=[](const Gta5ResourceReader& reader,std::uint32_t at,unsigned size,unsigned align)
            ->std::optional<std::vector<std::uint8_t>> {
        if(!reader || !at || at%align || at>UINT32_MAX-(size-1))return std::nullopt;
        auto bytes=reader(at,size);
        return bytes && bytes->size()==size?bytes:std::nullopt;
    };
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    out.state=read(heap,call.device,13864,4);
    out.rectangle=read(argument,call.rectangle,16,4);
    out.color=read(argument,call.color,16,16);
    if(out.state)for(unsigned i=0;i<5;++i) {
        auto& s=out.surfaces[i];s.object=word(*out.state,12816+4*i);s.tileCache=word(*out.state,13104+4*i);
        // Original8241B7E0 reads dimensions at+36; the earlier32-byte surface
        // capture did not include them. Separate color/depth binding evidence.
        s.header=read(s.object<0x20000000?heap:graphics,s.object,40,4);
    }
    auto stack=call.stack;
    for(unsigned i=0;i<8;++i) {
        const auto frame=read(argument,stack,4,16);if(!frame)break;
        const auto parent=word(*frame,0);
        // Monotone owned stack backchain, bounded per-frame gap; no arbitrary
        // memory scan and no assumption that a label ring was a call stack.
        if(parent<=stack || parent-stack>65536 || parent%16)break;
        const auto link=read(argument,parent-8,4,4);if(!link)break;
        out.frames.push_back({stack,parent,word(*link,0)});stack=parent;
    }
    return out;
}
}
