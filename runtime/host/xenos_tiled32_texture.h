#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace recomp::gpu {
struct Tiled32Texture {
    std::uint32_t physicalAddress=0;
    std::array<std::uint8_t,4> swizzle{};
};
// Layout-only snapshot profile, not a sampler/texture binding. Exact single
// 32x32 tile, pitch32, unsigned normalized RGBA8, 8-in-32 endian, base mip only.
// Remaining descriptor fields match the observed profile, not inferred hardware
// behavior. No filtering, wrapping, LOD, residency, cache or EDRAM operations.
inline bool decodeTiled32Texture(const std::array<std::uint32_t,6>& words,Tiled32Texture& output) {
    if (words[0]!=0x80400002 || (words[1]&0xFFF)!=0x86 || words[2]!=0x3E01F ||
        (words[3]&~0x1FFEu)!=0xA80000 || words[4]!=3 || words[5]!=0x210) return false;
    Tiled32Texture candidate;candidate.physicalAddress=words[1]&~0xFFFu;
    if (candidate.physicalAddress>0x1FFFF000) return false;
    for(unsigned i=0;i<4;++i){const auto s=(words[3]>>(1+3*i))&7;
        if(s>5)return false;candidate.swizzle[i]=static_cast<std::uint8_t>(s);}
    output=candidate;return true;
}
// Specialization of the documented Xenos 2D bank/pipe layout for exactly one
// 32x32 tile with four bytes per texel. Returns false outside this narrow tile.
inline bool tiled32TexelOffset(std::uint32_t x,std::uint32_t y,std::uint32_t& output) {
    if(x>=32 || y>=32)return false;
    output=((x&3)<<2)|((x&12)<<3)|((((x>>4)^(y>>3))&1)<<7)|((y&1)<<4)|((y&30)<<7);
    return true;
}
inline bool decodeTiled32Pixels(const Tiled32Texture& plan,const std::uint8_t* bytes,
        std::size_t size,std::array<std::uint8_t,4096>& output) {
    if(!bytes || size!=4096 || (plan.physicalAddress&4095) || plan.physicalAddress>0x1FFFF000)return false;
    for(auto s:plan.swizzle)if(s>5)return false;
    std::array<std::uint8_t,4096> candidate{};
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){
        std::uint32_t at=0;if(!tiled32TexelOffset(x,y,at) || at>4092)return false;
        for(unsigned c=0;c<4;++c){const auto s=plan.swizzle[c];
            // Stored BE word, then the descriptor selects its low-byte-first lanes.
            candidate[(y*32+x)*4+c]=s<4?bytes[at+3-s]:s==4?0:255;}
    }
    output=candidate;return true;
}
}
