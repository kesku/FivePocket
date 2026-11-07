#pragma once
#include <cstdint>
#include <optional>

namespace recomp::gpu {
struct Gta5ShaderCodeRange {
    unsigned stage=0;
    std::uint32_t virtualAddress=0,byteCount=0;
    bool requiresVertexPatching=false;
};
// Original GTA V shader resource layout, derived from 823BAAC8 / 823BDC30.
// Reader must return a BE-decoded word from owned, stable CPU virtual memory.
// Offsets at pixel+64 / vertex+896+8*variant are RELATIVE, not pointers.
// This describes original code only. The vertex driver then patches a copied
// program at 823C3A88 (and may link interpolators); don't bind the raw original
// as though it were the effective shader submitted for this draw.
template<class ReadWord>
std::optional<Gta5ShaderCodeRange> describeGta5ShaderCode(
        std::uint32_t object,unsigned stage,unsigned variant,const ReadWord& read) {
    if(!object || (object&3) || stage>1 || variant>31 || (stage==1 && variant))return std::nullopt;
    const auto word=[&](std::uint32_t offset)->std::optional<std::uint32_t> {
        const auto at=std::uint64_t{object}+offset;
        if(at>0xFFFFFFFCULL)return std::nullopt;
        return read(static_cast<std::uint32_t>(at));
    };
    const auto relative=word(stage?64:896+8*variant);
    const auto base=word(stage?24:32);
    if(!relative || !base || (*relative&3) || *relative>65536)return std::nullopt;
    const auto offset=word(*relative+(stage?40:872));
    const auto length=word(*relative+(stage?44:876));
    if(!offset || !length || !*base || !*length || *length>65536 || (*length&3))return std::nullopt;
    const auto address=std::uint64_t{*base}+*offset;
    if((address&3) || address+*length>0x100000000ULL)return std::nullopt;
    return Gta5ShaderCodeRange{stage,static_cast<std::uint32_t>(address),*length,stage==0};
}
}
