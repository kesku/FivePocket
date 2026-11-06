#pragma once
#include "gta5_bound_surface_capture.h"
#include "native_indexed_view_exclusion.h"
#include "native_vertex_clip_program.h"

namespace recomp::gpu {
// This is NOT a renderer. It admits only a position computation for the exact
// observed resource-free shader pair. The compiler must have rejected memory
// exports and other external effects in both stages. Pixel KILL is irrelevant
// only AFTER every original triangle is proved outside the view AND scissor.
// No query result, target contents, or command retirement is manufactured.
inline bool planNativeClippedIndexed(const NativeClippedFanProgram& program,
        const Gta5InlineDraw& draw,std::span<const std::uint32_t> indices,
        const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& vertex,
        const std::vector<std::uint8_t>& pixel,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* value){if(why)*why=value;return false;};
    if(program.stride!=24 || program.vertexIdentity.size()!=132 || program.pixelIdentity.size()!=360 ||
       vertex!=program.vertexIdentity || pixel!=program.pixelIdentity)return no("clipped-indexed-shader-identity");
    if(!queriesIdle)return no("clipped-indexed-query-owner");
    if(draw.primitive!=4 || draw.stride!=24 || draw.vertexCount<3 || draw.vertexCount>65536 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*24 || draw.vertices.size()>256*1024 ||
       indices.empty() || indices.size()>65535 || indices.size()%3)return no("clipped-indexed-extent");
    for(auto i:indices)if(i>=draw.vertexCount)return no("clipped-indexed-index-range");
    if(draw.deviceState.size()!=13860)return no("clipped-indexed-state-extent");
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("clipped-indexed-shader-binding");
    // Exact current filled perspective triangle state. Do not infer that
    // viewport transform, clipping, point expansion, predication or queries
    // are harmless merely because the captured positions were far outside.
    constexpr unsigned required[][2]{
        {0x2000,0x14000500},{0x2001,0x302D0},{0x2002,0x10000},
        {0x2104,15},{0x210D,0xFFFF00},{0x210F,0x44200000},{0x2110,0x44200000},
        {0x2111,0xC3B40000},{0x2112,0x43B40000},{0x2113,0xB4400000},{0x2114,0x34800000},
        {0x2180,0x10010201},{0x2181,4},{0x2182,0x10001},{0x2183,0},{0x2184,0},
        {0x2200,0x708767},{0x2201,0x10001},{0x2202,0xAA000005},{0x2203,0x33},
        {0x2204,0x80000},{0x2205,0x18002},{0x2206,0x43F},{0x2207,0},{0x2208,4},
        {0x2284,0},{0x2292,0},{0x2293,0x10},{0x2300,0x400},{0x2301,0},{0x2302,4},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF},{0x231C,0},{0x2324,2},{0x2325,0}};
    for(const auto& r:required)if(gta5DrawRegister(draw,r[0])!=r[1])return no("clipped-indexed-raster-profile");
    if(word(draw.deviceState,12408) || draw.deviceState[11068] || draw.deviceState[12498] ||
       draw.deviceState[12563])return no("clipped-indexed-recording-query-mode");
    const auto scissor=nativeTranslatedScissor(draw);
    if(!nativeTranslatedViewport(draw) || !scissor || *scissor!=std::array<unsigned,4>{0,0,1280,720})
        return no("clipped-indexed-viewport-scissor");
    for(unsigned i=0;i<5;++i) {
        const auto object=word(draw.deviceState,12816+4*i);const auto& s=bound.surfaces[i];
        if(s.object!=object)return no("clipped-indexed-surface-binding");
        if(i!=0 && i!=4){if(object)return no("clipped-indexed-extra-surface");continue;}
        if(!object || object%4 || object>UINT32_MAX-39 || !s.header || s.header->size()!=40 ||
           word(*s.header,24)!=0x14000500 || word(*s.header,28)!=(i?0x10000u:0x302D0u) ||
           word(*s.header,36)!=0x13FC1678u)return no("clipped-indexed-surface-profile");
    }
    if(bound.surfaces[0].object==bound.surfaces[4].object)return no("clipped-indexed-surface-alias");
    if(why)*why="clipped-indexed-position-proof-pending";
    return true;
}
}
