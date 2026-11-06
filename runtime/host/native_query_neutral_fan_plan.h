#pragma once
#include "native_vertex_clip.h"
#include "native_vertex_clip_program.h"

namespace recomp::gpu {
// This is a DRAW receipt, never a query completion/PM4 retirement receipt.
// A side-effect-free VS whose entire filled fan is outside a common clip AND
// scissor plane contributes no samples and no potential visibility. Pending
// original BEGIN/END events must remain pending; no counter origin is needed
// and no guest memory, query journal, fence, or command cursor is written here.
// The caller holds the exclusive guest gate through current capture, GPU fence
// and byte revalidation. This object conveys no lease beyond that gate.
struct NativeQueryNeutralFanProof {
    unsigned plane,vertices;
};
inline bool planNativeQueryNeutralFan(const NativeClippedFanProgram& program,
        const Gta5InlineDraw& draw,const std::vector<std::uint8_t>& vertex,
        const std::vector<std::uint8_t>& pixel,const char** why=nullptr) {
    const auto no=[&](const char* reason){if(why)*why=reason;return false;};
    // Full offline VS/PS analysis must reject memory exports, pixel KILL,
    // texture fetches and unsupported side effects. The private pack binds
    // those analyzed originals to the position module, not merely these sizes.
    if(program.stride!=36 || program.vertexIdentity.size()!=636 ||
       program.pixelIdentity.size()!=36 || vertex!=program.vertexIdentity ||
       pixel!=program.pixelIdentity)return no("neutral-fan-program");
    if(draw.primitive!=5 || draw.stride!=36 || draw.vertexCount<3 ||
       draw.vertexCount>65536 || draw.vertices.size()>256*1024 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*36 ||
       draw.deviceState.size()!=13860)return no("neutral-fan-inputs");
    const auto word=[&](unsigned at) {
        const auto* b=draw.deviceState.data()+at;
        return (unsigned(b[0])<<24)|(unsigned(b[1])<<16)|(unsigned(b[2])<<8)|b[3];
    };
    if(!draw.device || !draw.vertexShader || !draw.pixelShader ||
       word(13072)!=draw.vertexShader || word(13068)!=draw.pixelShader)
        return no("neutral-fan-bindings");
    // Separate from the query-IDLE fan profile. Do not mask its selector or
    // claim all active-query draws are inert just because colour writes are off.
    constexpr unsigned required[][2]{
        {0x2000,0x14000500},{0x2001,0x302D0},{0x2002,0x10000},
        {0x2104,0},{0x210D,0x707},{0x210F,0x44200000},{0x2110,0x44200000},
        {0x2111,0xC3B40000},{0x2112,0x43B40000},{0x2113,0},{0x2114,0},
        {0x2180,0x10010004},{0x2181,4},{0x2182,0x10001},{0x2183,0},{0x2184,0},
        {0x2200,0x700263},{0x2201,0x10001},{0x2202,0x8700001E},{0x2203,0x33},
        {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},{0x2207,0},{0x2208,4},
        {0x2284,0},{0x2292,0},{0x2293,0x15},{0x2300,0x400},{0x2301,0},{0x2302,4},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF},{0x231C,0},{0x2324,2},{0x2325,0}};
    for(const auto& r:required)if(gta5DrawRegister(draw,r[0])!=r[1])return no("neutral-fan-state");
    if(word(12408) || draw.deviceState[11068] || draw.deviceState[11072] ||
       draw.deviceState[12497]!=1 || draw.deviceState[12498] || draw.deviceState[12563] ||
       word(12424) || word(12428)!=0x400 || draw.deviceState[12433]!=10)
        return no("neutral-fan-query-recording-state");
    const auto scissor=nativeTranslatedScissor(draw);
    if(!nativeTranslatedViewport(draw) || !scissor || *scissor!=std::array<unsigned,4>{0,0,1280,720})
        return no("neutral-fan-view");
    if(why)*why="neutral-fan-current-gpu-proof-required";
    return true;
}
}
