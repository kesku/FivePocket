#pragma once
#include "gta5_bound_surface_capture.h"
#include "native_vertex_clip.h"
#include "native_vertex_clip_program.h"

namespace recomp::gpu {
enum class NativeSceneFanProfile : unsigned { Textured684=0, Untextured708=1 };
// Separate from the older color8 fan profile. This is only admission to a
// CURRENT position proof, never permission to skip a visible textured fan.
// Offline compilation must reject memory/side-effect exports in both stages.
inline bool planNativeClippedSceneFan(const NativeClippedFanProgram& program,
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const std::vector<std::uint8_t>& vertex,const std::vector<std::uint8_t>& pixel,
        bool queriesIdle,const char** why=nullptr,
        NativeSceneFanProfile profile=NativeSceneFanProfile::Textured684) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    if(profile!=NativeSceneFanProfile::Textured684 && profile!=NativeSceneFanProfile::Untextured708)
        return no("scene-fan-profile");
    const bool secondary=profile==NativeSceneFanProfile::Untextured708;
    if(program.stride!=36 || program.vertexIdentity.size()!=(secondary?708u:684u) ||
       program.pixelIdentity.size()!=(secondary?60u:240u) ||
       vertex!=program.vertexIdentity || pixel!=program.pixelIdentity)return no("scene-fan-shader-identity");
    if(!queriesIdle)return no("scene-fan-query-owner");
    if(draw.primitive!=5 || draw.stride!=36 || draw.vertexCount<3 || draw.vertexCount>65536 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*36 || draw.vertices.size()>256*1024)
        return no("scene-fan-extent");
    if(draw.deviceState.size()!=13860)return no("scene-fan-state-extent");
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("scene-fan-shader-binding");
    // Exact captured ordinary filled fan. PA_SC_VIZ_QUERY=0x10 has query ID8,
    // enable/kill bits OFF. The independent engine owner must also be idle.
    // Keep the legacy fan guard unchanged; don't mask arbitrary new state.
    const unsigned required[][2]{
        {0x2000,0x14000500},{0x2001,0xC02D0},{0x2002,0x10000},
        {0x2104,15},{0x210D,0xFFFF07},{0x210F,0x44200000},{0x2110,0x44200000},
        {0x2111,0xC3B40000},{0x2112,0x43B40000},{0x2113,0},{0x2114,0},
        // SQ program/interpolator state differs between the two exact pairs.
        // Geometry/clip/query/surface state below is NOT masked or generalized.
        {0x2180,secondary?0x10010104u:0x10110406u},{0x2181,4},
        {0x2182,secondary?0x10001u:0x30003u},{0x2183,0},{0x2184,0},
        {0x2200,0x700762},{0x2201,secondary?0x0B0A0B0Au:0x1010Au},{0x2202,0xAA000005},{0x2203,0x33},
        {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},{0x2207,0},{0x2208,4},
        {0x2284,0},{0x2292,0},{0x2293,0x10},{0x2300,0x400},{0x2301,0},{0x2302,4},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF},{0x231C,0},{0x2324,2},{0x2325,0}};
    for(const auto& r:required)if(gta5DrawRegister(draw,r[0])!=r[1])return no("scene-fan-raster-profile");
    // Original823AB3D8 maps header format3 to register12 for expanded blending
    // when this flag is1. Exclusion writes nothing; it asserts no equivalence
    // between their blending arithmetic. Keep both observed representations.
    if(word(draw.deviceState,12408)!=1 || draw.deviceState[11068] || draw.deviceState[12498] ||
       draw.deviceState[12563])return no("scene-fan-recording-query-mode");
    const auto scissor=nativeTranslatedScissor(draw);
    if(!nativeTranslatedViewport(draw) || !scissor || *scissor!=std::array<unsigned,4>{0,0,1280,720})
        return no("scene-fan-viewport-scissor");
    for(unsigned i=0;i<5;++i) {
        const auto object=word(draw.deviceState,12816+4*i);const auto& s=bound.surfaces[i];
        if(s.object!=object)return no("scene-fan-surface-binding");
        if(i!=0 && i!=4){if(object)return no("scene-fan-extra-surface");continue;}
        if(!object || object%4 || object>UINT32_MAX-39 || !s.header || s.header->size()!=40 ||
           word(*s.header,24)!=0x14000500 || word(*s.header,28)!=(i?0x10000u:0x302D0u) ||
           word(*s.header,36)!=0x13FC1678u)return no("scene-fan-surface-profile");
    }
    if(bound.surfaces[0].object==bound.surfaces[4].object)return no("scene-fan-surface-alias");
    if(why)*why="scene-fan-current-positions-pending";return true;
}
// Valid only after exact state/program admission above and fenced original VS
// readback. No Z-only/degenerate-only/saved-capture exclusion is accepted.
inline std::optional<unsigned> nativeSceneFanOutside(const Gta5InlineDraw& draw,
        std::span<const std::array<float,4>> positions) {
    if(draw.primitive!=5 || positions.size()!=draw.vertexCount || positions.size()<3)return std::nullopt;
    if(nativeAllVerticesBehindW(positions))return 4;
    const auto plane=nativeCommonOutsideHomogeneousXYPlane(positions);
    if(!plane || !nativeCommonOutsideScissorHalfPlane(draw,positions))return std::nullopt;
    return plane;
}
}
