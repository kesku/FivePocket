#pragma once
#include "gta5_bound_surface_capture.h"
#include "native_edram32_bands.h"
#include "native_translated_draw.h"
#include "xenos_depth_stencil_state.h"

namespace recomp::gpu {
// Exact analysed original pairs, NOT executable shader capability. The private
// generator must establish position/interpolator vertex outputs and colour-only
// pixel effects across ALL possible control paths, excluding depth/memory/kill
// effects. A passing stencil sample still requires an implemented renderer.
enum class NativeStencilRejectedRectangleProfile {
    DirectionalExpanded,
    ConditionalUnexpanded,
};
struct NativeStencilRejectedRectangleIdentity {
    std::vector<std::uint8_t> vertex,pixel;
    NativeStencilRejectedRectangleProfile profile=NativeStencilRejectedRectangleProfile::DirectionalExpanded;
};
struct NativeStencilRejectedRectangleProof {
    unsigned examined=0,passing=0;
    std::uint64_t ownerNextSerial=0;
};

// Nonindexed dispatch and the analysed exact shader match are required here,
// rather than inferred from shader object addresses or saved diagnostic data.
// An unknown colour destination is permissible ONLY because no raster sample
// can pass. It remains unknown afterwards; no pixels are created or published.
inline bool proveNativeStencilRejectedRectangle(const Gta5InlineDraw& draw,
        const Gta5BoundSurfaceCapture& bindings,
        const NativeStencilRejectedRectangleIdentity& identity,
        const std::vector<std::uint8_t>& currentVertex,const std::vector<std::uint8_t>& currentPixel,
        bool indexed,bool nativeQueriesIdle,const NativeEdram32Bands& owner,
        NativeStencilRejectedRectangleProof& proof,const char** why=nullptr) {
    proof={};
    const auto no=[&](const char* value){if(why)*why=value;return false;};
    const bool conditional=identity.profile==NativeStencilRejectedRectangleProfile::ConditionalUnexpanded;
    if(!conditional && identity.profile!=NativeStencilRejectedRectangleProfile::DirectionalExpanded)
        return no("unknown-effect-profile");
    if(identity.vertex.size()!=168 || identity.pixel.size()!=(conditional?1344u:576u) ||
       currentVertex!=identity.vertex || currentPixel!=identity.pixel)return no("shader-identity");
    if(indexed || draw.primitive!=8 || draw.vertexCount!=3 || draw.stride!=36 || draw.vertices.size()!=108)
        return no("rectangle-shape");
    if(draw.deviceState.size()!=13860 || !draw.textures.empty())return no("state-extent-or-textures");
    if(!nativeQueriesIdle)return no("query-owner-not-idle");
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("shader-binding");
    const auto r=[&](unsigned reg){return gta5DrawRegister(draw,reg);};
    if(r(0x2293)!=0x10u || r(0x2324)!=2u || r(0x2325)!=0u)return no("query-state");
    // Same closed query contract as native_read_only_rectangle_plan.h:
    // SAMPLE_COUNT_CTL2 is pinned, NOT claimed to mean disabled; unsupported
    // ZPD packets remain rejected by the command consumers, not acknowledged.
    if(r(0x2200)!=0x00700271u || r(0x210D)!=0x00000701u)return no("depth-stencil-profile");
    const auto state=decodeXenosDepthStencilState(*r(0x2200),*r(0x210D),0);
    if(!state || !state->stencilEnable || state->depthEnable || state->depthWrite ||
       state->backfaceEnable || state->front.compare!=2 || state->front.fail ||
       state->front.pass || state->front.depthFail || state->front.writeMask)return no("depth-stencil-effect");
    // Keep each observed state tuple intact; accepting the union of individual
    // register values would silently admit combinations never checked.
    if(r(0x2001)!=(conditional?0x302D0u:0xC02D0u) ||
       r(0x2180)!=(conditional?0x10111000u:0x10110700u) ||
       r(0x2201)!=(conditional?0x10001u:0x10101u) ||
       r(0x2202)!=(conditional?0xAA000005u:0xAA00000Du))return no("effect-profile-state");
    constexpr unsigned required[][2]{{0x2000,0x14000500},{0x2002,0x10000},
        {0x2104,7},{0x2181,4},{0x2182,0x30003},{0x2183,0},{0x2184,0},
        {0x2203,0x18},{0x2204,0x80000},
        {0x2205,0x18000},{0x2206,0x43F},{0x231C,0}};
    for(const auto& v:required)if(r(v[0])!=v[1])return no("raster-profile");
    if(draw.deviceState[11068]&0x10)return no("binning-flag");
    const auto scissor=nativeTranslatedScissor(draw);
    const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || *scissor!=std::array<unsigned,4>{0,0,1280,720} || !viewport)return no("scissor-viewport");
    for(unsigned i=0;i<5;++i) {
        const auto object=word(draw.deviceState,12816+4*i);const auto& s=bindings.surfaces[i];
        if(s.object!=object)return no("surface-binding");
        if(i!=0 && i!=4) {if(object)return no("extra-surface");continue;}
        if(!object || object%4 || object>UINT32_MAX-39 || !s.header || s.header->size()!=40)
            return no("surface-header");
        if(word(*s.header,24)!=0x14000500 || word(*s.header,28)!=(i==0?0x302D0u:0x10000u))
            return no("surface-info");
        const auto dimensions=word(*s.header,36);
        if((dimensions>>18)+1!=1280 || ((dimensions>>3)&0x7FFFu)+1!=720)return no("surface-dimensions");
    }
    // Original823AB3D8 converts header format3 to register12 when the slot0
    // expanded-blending flag (device+12408) is1, preserving every other bit.
    // The two formats share7e3 storage, but NOT the same blending precision.
    // No colour arithmetic is claimed by this zero-passing-sample proof.
    if(word(draw.deviceState,12408)!=(conditional?0u:1u) ||
       bindings.surfaces[0].object==bindings.surfaces[4].object)
        return no("surface-relationship");
    const auto raw=owner.readDepthWords(0,1280,1280,720);
    if(!raw || raw->size()!=1280u*720u)return no("physical-stencil-unknown");
    proof.ownerNextSerial=owner.nextSerial();
    for(auto value:*raw) {
        ++proof.examined;
        if(xenosCompare(state->front.compare,state->front.reference&state->front.readMask,value&state->front.readMask))
            ++proof.passing;
    }
    if(proof.passing)return no("stencil-samples-pass-unimplemented-draw");
    if(why)*why="all-physical-stencil-samples-rejected";
    return true;
}
}
