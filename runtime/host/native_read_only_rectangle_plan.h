#pragma once
#include "native_translated_draw.h"
#include "xenos_depth_stencil_state.h"

namespace recomp::gpu {
// Exact 8235E3A0 post-restoration rectangle. This is NOT the older ABI21
// all-tests-disabled case, nor general permission to skip vertex-only draws.
// Captured00700275/EQUAL1 and00700575/NOT_EQUAL7 both have Z disabled,
// a dormant Z-write bit, and KEEP on every stencil outcome. The latter is
// reached through8234D610 ->82329B98 ->8235E3A0 with reference7/mode0;
// 82354668 writes that reference into the original renderer state.
// Pinned Xenia95a5c3ee vulkan_pipeline_cache.cc
// gates z_write_enable on z_enable. Therefore no depth or stencil word changes,
// irrespective of incoming depth, stencil value, coverage or MSAA samples.
//
// The matched ABI26 vertex identity has already been analysed: position only,
// no memory allocation/export. The ABSENT pixel identity is a separate caller
// obligation and is checked here as an explicit argument, not inferred from an
// empty shader capture. Colour bindings/mask and both query mechanisms are
// independently constrained. This backend does not admit EVENT_WRITE_ZPD;
// its scene/maintenance command consumers reject that unknown opcode. No ZPD
// report/lifecycle can be carried through this proof. A nonzero report address,
// visibility control other than this disabled selector, or busy native query
// owner rejects. SAMPLE_COUNT_CTL=2 is pinned, NOT decoded as "disabled".
//
// HiZ134/714 are original hierarchical-cache states; the native backend uses
// actual depth/stencil values, not guest HiZ contents. This proof does NOT
// acknowledge any accompanying command packet, query result or cache receipt.
// Physical content must be preserved by the caller's mutation transaction.
inline bool planNativeReadOnlyScreenRectangle(const Gta5InlineDraw& draw,
        const NativeTranslatedDrawProgram& analysedScreenProgram,
        std::uint32_t pixelObject,bool nativeQueriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* value){if(why)*why=value;return false;};
    if(!validateNativeScreenSpaceRectangleProgram(analysedScreenProgram))return no("program-contract");
    if(pixelObject)return no("pixel-stage-present");
    if(!nativeQueriesIdle)return no("query-owner-not-idle");
    // Nonindexed dispatch is a separate caller obligation: Gta5InlineDraw
    // intentionally has no indexed-draw members.
    if(draw.primitive!=8 || draw.vertexCount!=3 || draw.stride!=analysedScreenProgram.stride)
        return no("rectangle-shape");
    if(draw.deviceState.size()!=13860 || !draw.textures.empty())return no("state-extent-or-textures");
    const auto word=[&](unsigned offset)->std::optional<std::uint32_t> {
        if(offset+4>draw.deviceState.size())return std::nullopt;
        const auto* p=draw.deviceState.data()+offset;
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
    };
    if(draw.pixelShader || word(13068)!=0u)return no("pixel-stage-present");
    for(unsigned offset:{12816u,12820u,12824u,12828u})
        if(word(offset)!=0u)return no("colour-binding");
    const auto r=[&](unsigned reg){return gta5DrawRegister(draw,reg);};
    if(r(0x2104)!=0u)return no("colour-writes");
    if(r(0x2293)!=0x10u || r(0x2324)!=2u || r(0x2325)!=0u)return no("query-state");
    // Whole observed tuples, not a Cartesian product of independently allowed
    // values. Interpolator shading/sampling cannot create an output in this
    // position-only VS / absent PS path, but unrelated modes remain unsupported.
    const bool equalOne=r(0x2200)==0x00700275u && r(0x210D)==0x00FF0701u &&
        r(0x2182)==0u && r(0x2203)==0x134u;
    const bool notEqualSeven=r(0x2200)==0x00700575u && r(0x210D)==0x00FF0707u &&
        r(0x2182)==0x00030003u && r(0x2203)==0x714u;
    if(!equalOne && !notEqualSeven)return no("depth-stencil-profile");
    const auto depth=decodeXenosDepthStencilState(*r(0x2200),*r(0x210D),0);
    if(!depth || depth->depthEnable || !depth->stencilEnable || depth->backfaceEnable ||
       depth->front.fail || depth->front.pass || depth->front.depthFail)return no("output-effect");
    // Reject unrelated shader/export/raster modes even if they look read-only.
    constexpr unsigned required[][2]{{0x2000,0x0A020280},{0x2002,0x10000},
        {0x2180,0x10000},{0x2181,0},{0x2183,0},{0x2184,0},
        {0x2202,0xAA000005},{0x2204,0x80000},
        {0x2205,0x18002},{0x2206,0x43F},{0x231C,0}};
    for(const auto& entry:required)if(r(entry[0])!=entry[1])return no("raster-profile");
    if(!nativeTranslatedViewport(draw))return no("viewport");
    if(!nativeTranslatedScissor(draw))return no("scissor");
    if(why)*why=notEqualSeven?"read-only-depth-stencil-not-equal-seven":"read-only-depth-stencil";
    return true;
}
}
