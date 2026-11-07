#pragma once
#include "gta5_inline_draw.h"

namespace recomp::gpu {
// The register blocks copied by the original BeginVertices state flush. This
// describes verified shadow fields, not a guessed flat register array. Derived
// scissor/window state is supplied separately by the actual scissor API.
inline std::optional<unsigned> gta5DrawRegisterOffset(unsigned index) {
    struct Block {unsigned first,count,offset;};
    constexpr Block blocks[]{
        {0x2000,16,10368},{0x2100,21,10444},{0x2180,5,10528},
        {0x2200,12,10548},{0x2280,21,10596},{0x2300,38,10680},
        {0x2380,8,10832},{0x4900,40,10112}
    };
    for(const auto& b:blocks)if(index>=b.first && index-b.first<b.count)
        return b.offset+(index-b.first)*4;
    return std::nullopt;
}

// 823BA300 emits one DWORD for each high-first mask bit. Native draws consume
// these same CPU shadows at EndVertices; accept only an exact verified mapping.
// Return the required owned source extent, including any unselected holes.
inline std::optional<unsigned> gta5DrawStateUploadBytes(std::uint32_t device,
        std::uint64_t mask,std::uint32_t first,std::uint32_t source) {
    if(!mask || first>0xFFFF)return std::nullopt;
    unsigned bytes=0;
    for(unsigned i=0;i<64;++i)if(mask&(std::uint64_t{1}<<(63-i))) {
        const auto offset=gta5DrawRegisterOffset(first+i);
        const auto address=std::uint64_t{source}+4*i;
        if(!offset || address>0xFFFFFFFCULL ||
            std::uint64_t{device}+*offset!=address)return std::nullopt;
        bytes=4*(i+1);
    }
    return bytes;
}

inline std::optional<std::uint32_t> gta5DrawRegister(const Gta5InlineDraw& draw,unsigned index) {
    if(index>=0x2080 && index<=0x2082)
        return draw.scissorRegisters?std::optional<std::uint32_t>{(*draw.scissorRegisters)[index-0x2080]}:std::nullopt;
    if(const auto offset=gta5DrawRegisterOffset(index)) {
        const auto at=*offset;
        if(draw.deviceState.size()<at+4)return std::nullopt;
        const auto* p=draw.deviceState.data()+at;
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
    }
    return std::nullopt;
}
}
