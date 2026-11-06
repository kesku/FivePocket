#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace recomp::gpu {
struct LinearR8Plane {
    std::uint32_t physicalAddress=0,width=0,height=0,pitch=0;
};
inline bool validLinearR8Plane(const LinearR8Plane& p) {
    return !(p.physicalAddress&4095) && p.physicalAddress<=0x1FFFF000 &&
        ((p.width==1280 && p.height==720 && p.pitch==1280) ||
         (p.width==640 && p.height==360 && p.pitch==768)) &&
        std::uint64_t{p.physicalAddress}+p.pitch*p.height<=0x20000000;
}
// Two observed base-level layouts only. Remaining descriptor fields are matched
// exactly; this does not implement filtering, wrapping, LOD or texture caching.
inline bool decodeLinearR8Plane(const std::array<std::uint32_t,6>& words,LinearR8Plane& output) {
    if((words[0]&~(511u<<22))!=0x00024802 || (words[1]&4095)!=2 ||
        words[2]>>26 || words[3]!=0x00281400 || words[4]!=3 || words[5]!=0x210)return false;
    LinearR8Plane candidate{words[1]&0xFFFFF000,(words[2]&8191)+1,((words[2]>>13)&8191)+1,
        ((words[0]>>22)&511)*32};
    if(!validLinearR8Plane(candidate))return false;
    output=candidate;return true;
}
// Owned input includes every padded row. Publish tightly packed R8 atomically.
// Descriptor output swizzle is R,R,R,1; this helper returns only the R8 plane.
inline bool unpackLinearR8Plane(const LinearR8Plane& p,const std::uint8_t* bytes,std::size_t size,
        std::vector<std::uint8_t>& output) {
    if(!validLinearR8Plane(p) || !bytes || size!=p.pitch*p.height)return false;
    try {
        std::vector<std::uint8_t> candidate(p.width*p.height);
        for(std::uint32_t y=0;y<p.height;++y)
            std::memcpy(candidate.data()+y*p.width,bytes+y*p.pitch,p.width);
        output=std::move(candidate);return true;
    } catch (...) {return false;}
}
}
