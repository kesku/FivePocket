#pragma once
#include "gta5_bound_surface_capture.h"
#include "native_edram32_bands.h"
#include "native_translated_draw.h"
#include "xenos_depth_stencil_state.h"

namespace recomp::gpu {
// This is an effect-only identity, not a shader implementation. Its private
// producer must certify the exact VS/PS: position/interpolator outputs only,
// colour0-only pixel output, no depth/memory/discard or unknown control paths.
// Never construct it merely because a captured draw failed translation.
struct NativePostSampledStencilIdentity {std::vector<std::uint8_t> vertex,pixel;};
struct NativePostSampledStencilProof {
    unsigned examined=0,passing=0;
    std::uint64_t ownerNextSerial=0;
};

// Conservative all-scissor proof for the observed NOT_EQUAL16 rectangle.
// Geometry and texture arithmetic need not execute only if EVERY possible
// raster sample fails the unchanged stencil test. This cannot complete a
// visibility query, publish a texture, make an unknown owner known, or admit
// even one passing sample. The caller holds the exclusive physical owner gate.
inline bool proveNativePostSampledStencilRejection(const Gta5InlineDraw& draw,
        const Gta5BoundSurfaceCapture& bindings,const NativePostSampledStencilIdentity& identity,
        const std::vector<std::uint8_t>& vertex,const std::vector<std::uint8_t>& pixel,
        bool indexed,bool queriesIdle,const NativeEdram32Bands& owner,
        NativePostSampledStencilProof& proof,const char** why=nullptr) {
    proof={};
    const auto no=[&](const char* reason){if(why)*why=reason;return false;};
    if(identity.vertex.size()!=96 || identity.pixel.size()!=324 ||
       vertex!=identity.vertex || pixel!=identity.pixel)return no("shader-identity");
    if(indexed || draw.primitive!=8 || draw.vertexCount!=3 || draw.stride!=24 ||
       draw.vertices.size()!=72)return no("rectangle-shape");
    if(draw.deviceState.size()!=13860 || !draw.textures.empty())return no("state-extent-or-textures");
    if(!queriesIdle)return no("query-owner-not-idle");
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("shader-binding");
    const auto r=[&](unsigned reg){return gta5DrawRegister(draw,reg);};
    // VIZ_QUERY enable bit0 is clear; retained ID bits are not completion.
    // SAMPLE_COUNT_CTL2 is pinned, not interpreted as disabling native queries.
    if(r(0x2293)!=0x14u || r(0x2324)!=2u || r(0x2325)!=0u)return no("query-state");
    if(r(0x2200)!=0x00700571u || r(0x210D)!=0x1010u || r(0x210E)!=0u)
        return no("depth-stencil-profile");
    const auto ds=decodeXenosDepthStencilState(*r(0x2200),*r(0x210D),0);
    if(!ds || !ds->stencilEnable || ds->depthEnable || ds->depthWrite || ds->backfaceEnable ||
       ds->front.compare!=5 || ds->front.fail || ds->front.pass || ds->front.depthFail ||
       ds->front.writeMask || ds->front.reference!=16 || ds->front.readMask!=16)
        return no("depth-stencil-effect");
    constexpr unsigned required[][2]{
        {0x2000,0x14000500},{0x2001,0x302D0},{0x2002,0x10000},{0x2104,15},
        {0x210F,0x44200000},{0x2110,0x44200000},{0x2111,0xC3B40000},
        {0x2112,0x43B40000},{0x2113,0xBF800000},{0x2114,0x3F800000},
        {0x2180,0x10050701},{0x2181,0x106},{0x2182,0x10001},{0x2183,0},{0x2184,0},
        {0x2201,0x10001},{0x2202,0xAA000003},{0x2203,0x1018},{0x2204,0x80000},
        {0x2205,0x18000},{0x2206,0x43F},{0x2302,5},{0x231C,0}};
    for(const auto& item:required)if(r(item[0])!=item[1])return no("raster-profile");
    if((draw.deviceState[11068]&0x10) || word(draw.deviceState,12408))return no("expanded-or-binning");
    const auto scissor=nativeTranslatedScissor(draw);
    if(!scissor || !(*scissor)[2] || !(*scissor)[3] || !nativeTranslatedViewport(draw))
        return no("scissor-viewport");
    for(unsigned i=0;i<5;++i) {
        const auto object=word(draw.deviceState,12816+4*i);const auto& s=bindings.surfaces[i];
        if(s.object!=object)return no("surface-binding");
        if(i!=0 && i!=4){if(object)return no("extra-surface");continue;}
        if(!object || object%4 || object>UINT32_MAX-39 || !s.header || s.header->size()!=40)
            return no("surface-header");
        if(word(*s.header,24)!=0x14000500 || word(*s.header,28)!=(i==0?0x302D0u:0x10000u))
            return no("surface-info");
        const auto dimensions=word(*s.header,36);
        if((dimensions>>18)+1!=1280 || ((dimensions>>3)&0x7FFFu)+1!=720)return no("surface-dimensions");
    }
    if(bindings.surfaces[0].object==bindings.surfaces[4].object)return no("surface-relationship");
    // Original8233D160 switches to the second32-row-aligned image band and
    // repeats this effect (for720rows, y384..720 rather than full-screen).
    // Never assume that second rectangle has the first one's384-row region.
    // Require the whole fixed depth view known, then test every pixel of the
    // CURRENT bounded scissor (including non-tile-aligned regions). Everything
    // outside it is unchanged; unknown physical content is never invented.
    const auto depth=owner.readDepthWords(0,1280,1280,720);
    if(!depth || depth->size()!=1280u*720u)return no("physical-stencil-unknown");
    proof.ownerNextSerial=owner.nextSerial();
    for(unsigned y=(*scissor)[1];y<(*scissor)[1]+(*scissor)[3];++y)
      for(unsigned x=(*scissor)[0];x<(*scissor)[0]+(*scissor)[2];++x) {
        const auto value=(*depth)[y*1280+x];
        ++proof.examined;
        if(xenosCompare(ds->front.compare,ds->front.reference&ds->front.readMask,value&ds->front.readMask))
            ++proof.passing;
    }
    if(proof.passing)return no("stencil-samples-pass-unimplemented-draw");
    if(why)*why="all-scissored-physical-stencil-samples-rejected";
    return true;
}
}
