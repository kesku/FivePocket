#pragma once
#include "native_color_viewport.h"

namespace recomp::gpu {
struct NativeColorRenderStateRequest {
    NativeColorViewportRequest viewport;
    unsigned role=0,edramBaseTile=0,colorFormat=0,samples=0,colorMask=0;
    // Source, destination, operation, in Xenos enum values, for RGB and alpha.
    std::array<unsigned,3> rgbBlend{},alphaBlend{};
    unsigned interpolatorMask=0,centerMask=0,vertexRegisters=0,pixelRegisters=0;
};
// Narrow fixed state for the two mapped color programs (role 0 R8, 1 small).
// All 42 words must be explicitly known. No reset-state assumption. Register
// encodings follow pinned Xenia 95a5c3ee registers.h/xenos.h. Single-sample
// interpolation uses centers; depth/stencil, alpha test and alpha-to-mask are
// disabled. Only target 0 writes RGBA, with source-alpha blending on every lane.
// The coordinate gate already excludes culling, polygon offset, clipping modes
// and index changes outside the tested W1 strip mapping.
//
// This is a STATE REQUEST, not shader analysis/linkage authentication or full
// live admission. Hardware ALLOC extent, shader resource scheduling, texture /
// ROP precision, depth/copy passes, cache/WAIT/register effects and ownership
// remain separate. No resource reads, allocation, GPU execution or guest writes.
template<class ReadRegister>
bool matchesNativeColorRenderRegisters(const ReadRegister& read,unsigned role) noexcept {
    if(role>1 || !matchesNativeColorViewportRegisters(read))return false;
    const std::array<std::array<std::uint32_t,2>,14> required{{
        {{0x2001,0xC02D0}},{{0x2104,15}},
        {{0x2180,role?0x10110203u:0x10110303u}},{{0x2181,8}},{{0x2182,0x10003}},
        {{0x2183,0}},{{0x2184,0}},{{0x2200,0x700770}},{{0x2201,0x7060706}},
        {{0x2202,0xAA000007}},{{0x2203,0x10}},
        {{0x2307,0xFF000}},{{0x2308,0xFF100}},{{0x2312,0xFFFF}}
    }};
    for(const auto& pair:required){
        const auto value=read(pair[0]);
        if(!value || *value!=pair[1])return false;
    }
    return true;
}
inline bool planNativeColorRenderState(const DrawCommandCandidate& draw,unsigned role,
        NativeColorRenderStateRequest& output) noexcept {
    NativeColorRenderStateRequest candidate;
    if(!planNativeColorViewportRequest(draw,candidate.viewport) ||
       !matchesNativeColorRenderRegisters([&](unsigned index)->std::optional<std::uint32_t> {
        const auto it=draw.registers.find(index);
        if(it==draw.registers.end() || it->second.bits.known!=0xFFFFFFFF)return std::nullopt;
        return it->second.bits.value;
    },role))return false;
    candidate.role=role;candidate.edramBaseTile=720;candidate.colorFormat=12;
    candidate.samples=1;candidate.colorMask=15;
    candidate.rgbBlend={{6,7,0}};candidate.alphaBlend=candidate.rgbBlend;
    candidate.interpolatorMask=3;candidate.centerMask=3;
    candidate.vertexRegisters=4;candidate.pixelRegisters=role?3:4;
    output=candidate;return true;
}
}
