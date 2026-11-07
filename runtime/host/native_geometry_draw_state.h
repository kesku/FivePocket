#pragma once
#include "native_translated_draw.h"
#include "xenos_depth_stencil_state.h"

namespace recomp::gpu {
struct NativeGeometryDrawState {
    std::array<float,4> viewport{};
    std::array<unsigned,4> scissor{};
    XenosDepthStencilState depth;
    float minDepth=0,maxDepth=1;
    unsigned samples=0,cull=0;
    bool clockwise=false;
};
// The observed indexed loading geometry's raster/output state. This does NOT
// prove shader identity, target/texture ownership or numeric inputs. Those
// remain connected execution requirements, not numbers to relax at a stop.
inline std::optional<NativeGeometryDrawState> planNativeGeometryDrawState(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program) {
    if(program.abiVersion!=13 || !program.indexed || !program.geometryDepth20e4 || !program.geometrySampleDepth || program.geometryColor8Format>1 ||
       program.geometryCentroidMask>0xFFFF || draw.primitive!=4 || draw.stride!=program.stride ||
       draw.deviceState.size()!=13860)return std::nullopt;
    const auto r=[&](unsigned reg){return gta5DrawRegister(draw,reg);};
    const auto viewport=nativeTranslatedViewport(draw);
    const auto scissor=nativeTranslatedScissor(draw);
    if(!viewport || !scissor)return std::nullopt;
    constexpr unsigned required[][2]{
        {0x2003,0},{0x200E,0},{0x200F,0x20002000},{0x2100,0xFFFFFF},{0x2101,0},{0x2102,0},
        {0x2104,15},{0x210C,0xFFFF00},{0x2183,0},{0x2184,0},
        {0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x13},{0x2204,0x80000},
        {0x2206,0x43F},{0x2207,0},{0x2208,4},{0x2209,0x10001},{0x220A,0x10001},{0x220B,0x10001},
        {0x2300,0x400},{0x2301,0x8001},{0x2302,4},{0x2303,0x40000000},{0x2304,0x3F800000},
        {0x2305,0x40000000},{0x2306,0x3F800000},{0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& v:required)if(r(v[0])!=v[1])return std::nullopt;
    const auto surface=*r(0x2000),color=*r(0x2001),depthInfo=*r(0x2002),mode=*r(0x2205);
    // Non-tiling render mode and 2x native storage. Pitch/base ownership is
    // checked against the actual surface headers by the caller.
    if((surface&~0x3FFFu)!=0x2010000 || (color&~0x7FFu)!=(program.geometryColor8Format<<16) ||
       (depthInfo&~0x7FFu)!=0x10000 || (mode!=0x18000 && mode!=0x18002))return std::nullopt;
    const bool complex=*r(0x2180)==0x10610D0A;
    if(complex) {
        if(r(0x2181)!=4 || r(0x2182)!=0x7F007F || r(0x2200)!=0x700263 || r(0x210D)!=0xFFFF || mode!=0x18000)return std::nullopt;
    } else if((*r(0x2180)!=0x10310A05 && *r(0x2180)!=0x10310A06) ||
            r(0x2181)!=8 || r(0x2182)!=0x7000F || r(0x2200)!=0x708367 || r(0x210D)!=0xFFFF01 || mode!=0x18002)return std::nullopt;
    const unsigned centroid=complex?0:0xFFF8;
    if(program.geometryCentroidMask!=centroid)return std::nullopt;
    const auto depth=decodeXenosDepthStencilState(*r(0x2200),*r(0x210D),*r(0x210C));if(!depth)return std::nullopt;
    float scale,offset;const auto a=*r(0x2113),b=*r(0x2114);std::memcpy(&scale,&a,4);std::memcpy(&offset,&b,4);
    if(scale!=(complex?0.f:1.f) || offset!=(complex?1.f:0.f))return std::nullopt;
    return NativeGeometryDrawState{*viewport,*scissor,*depth,offset,offset+scale,2,mode&3,false};
}
}
