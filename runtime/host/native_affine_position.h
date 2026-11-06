#pragma once
#include "gta5_inline_draw.h"
#include <bit>
#include <cmath>
#include <cstring>

namespace recomp::gpu {
// Generated only after proving position=(fetchXY*constant+constant,0,1).
// The consumer must additionally match the complete effective shader identity.
struct NativeAffinePositionXY {
    std::array<unsigned,2> offsetBytes{},scaleWords{},biasWords{};
};
inline bool nativeAffineFullRectangle(const Gta5InlineDraw& draw,const NativeAffinePositionXY& p) {
    if(draw.primitive!=4 || draw.vertexCount!=6 || !draw.stride || draw.stride>256*1024 ||
       draw.vertices.size()!=std::uint64_t{6}*draw.stride)return false;
    for(unsigned axis=0;axis<2;++axis)
        if(draw.stride<4 || p.offsetBytes[axis]>draw.stride-4 || p.offsetBytes[axis]%4 ||
           p.scaleWords[axis]>=1024 || p.biasWords[axis]>=1024)return false;
    const auto value=[](std::uint32_t bits){float v;std::memcpy(&v,&bits,4);return v;};
    std::array<unsigned,2> masks{};
    for(unsigned i=0;i<6;++i) {
        unsigned corner=0;
        for(unsigned axis=0;axis<2;++axis) {
            const auto* b=draw.vertices.data()+i*draw.stride+p.offsetBytes[axis];
            const float input=value((std::uint32_t{b[0]}<<24)|(std::uint32_t{b[1]}<<16)|(std::uint32_t{b[2]}<<8)|b[3]);
            const float scale=value(draw.vertexConstants[p.scaleWords[axis]]),bias=value(draw.vertexConstants[p.biasWords[axis]]);
            if(!std::isfinite(input) || !std::isfinite(scale) || !std::isfinite(bias))return false;
            // Same unfused/zero-multiply policy as the translated precise GLSL.
            const volatile float product=(input==0 || scale==0)?0:input*scale;
            const float position=product+bias;
            if(position!=-1 && position!=1)return false;
            corner|=unsigned(position==1)<<axis;
        }
        masks[i/3]|=1u<<corner;
    }
    // Two nondegenerate corner triangles sharing an opposite-corner diagonal.
    // Either winding/diagonal is valid when the separately checked cull is off.
    return std::popcount(masks[0])==3 && std::popcount(masks[1])==3 &&
        (masks[0]|masks[1])==15 && ((masks[0]&masks[1])==6 || (masks[0]&masks[1])==9);
}
}
