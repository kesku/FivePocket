#pragma once
#include "gta5_bound_surface_capture.h"
#include "native_translated_draw.h"
#include "xenos_depth24_texture.h"
#include "xenos_depth_stencil_state.h"

namespace recomp::gpu {
// State/resource contract for the observed ABI28 screen strip. This planner
// does NOT execute, establish physical target ownership or publish completion.
// In particular, RGB blending requires the existing 7e3 destination and the
// existing D24FS8 depth/stencil surface, not a cleared replacement target.
struct NativeScreenStripIdentity {std::vector<std::uint8_t> vertex,pixel;};
struct NativeScreenStripPlan {
    unsigned colorObject=0,depthObject=0,width=0,height=0,pitch=0;
    unsigned colorBase=0,depthBase=0,expandedVertices=0,textureIndex=0;
    std::array<float,4> viewport{};
    XenosDepthStencilState depthStencil;
    XenosTextureDescription texture;
};
inline std::optional<unsigned> nativeScreenStripExpandedCount(unsigned count,unsigned stride,std::size_t bytes) {
    if(count<3 || count>32768 || stride!=8 || bytes!=std::uint64_t{count}*stride)return std::nullopt;
    return (count-2)*3;
}
// Same index order as offline ABI28. Bounded for diagnostic/reference use;
// keeping degenerate triangles is essential to preserving strip parity.
inline std::optional<unsigned> nativeScreenStripIndex(unsigned count,unsigned expandedIndex) {
    const auto size=nativeScreenStripExpandedCount(count,8,std::uint64_t{count}*8);
    if(!size || expandedIndex>=*size)return std::nullopt;
    const unsigned triangle=expandedIndex/3,corner=expandedIndex%3;
    constexpr unsigned corners[2][3]{{2,0,1},{2,1,0}};
    return triangle+corners[triangle&1][corner];
}
inline std::optional<NativeScreenStripPlan> planNativeScreenStrip(
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bindings,
        const NativeScreenStripIdentity& identity,const std::vector<std::uint8_t>& currentVertex,
        const std::vector<std::uint8_t>& currentPixel,bool indexed,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* value){if(why)*why=value;return std::optional<NativeScreenStripPlan>{};};
    if(identity.vertex.size()!=300 || identity.pixel.size()!=492 ||
       currentVertex!=identity.vertex || currentPixel!=identity.pixel)return no("shader-identity");
    const auto count=nativeScreenStripExpandedCount(draw.vertexCount,draw.stride,draw.vertices.size());
    if(indexed || draw.primitive!=6 || !count)return no("strip-extent");
    if(draw.deviceState.size()!=13860)return no("state-extent");
    if(!queriesIdle)return no("query-owner-not-idle");
    const auto word=[](const auto& b,unsigned at) {return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("shader-binding");
    const auto r=[&](unsigned reg){return gta5DrawRegister(draw,reg);};
    if(r(0x2293)!=0x10u || r(0x2324)!=2u || r(0x2325)!=0u)return no("query-state");
    // The whole captured tuple: source ONE + destination ONE_MINUS_SRC_ALPHA,
    // RGB only; depth GREATER without writes, stencil NOT_EQUAL7/KEEP. No blend
    // rounding or shader execution is claimed just by decoding these values.
    if(r(0x2104)!=7u || r(0x2201)!=0x10701u || r(0x2202)!=0xAA000005u)
        return no("rgb-blend-profile");
    if(r(0x2200)!=0x00700543u || r(0x210D)!=0x707u)return no("depth-stencil-profile");
    const auto state=decodeXenosDepthStencilState(*r(0x2200),*r(0x210D),0);
    if(!state || !state->stencilEnable || !state->depthEnable || state->depthWrite ||
       state->depthCompare!=4 || state->backfaceEnable || state->front.compare!=5 ||
       state->front.fail || state->front.pass || state->front.depthFail || state->front.writeMask)
        return no("depth-stencil-effect");
    // Use Xenia's smooth, center-sampled single-sample interpolation policy.
    // 30003 selects the two active interpolators. Flat shading, MSAA, wrapping,
    // clipping changes and different parameter interfaces are NOT admitted.
    // This is an explicit emulator model, not an Xbox interpolation oracle.
    constexpr unsigned required[][2]{{0x2000,0x14000500},{0x2001,0xC02D0},{0x2002,0x10000},
        {0x210F,0x44200000},{0x2110,0x44200000},{0x2111,0xC3B40000},{0x2112,0x43B40000},
        {0x2113,0xBF800000},{0x2114,0x3F800000},
        {0x2180,0x10110202},{0x2181,4},{0x2182,0x30003},{0x2183,0},{0x2184,0},
        {0x2203,0x3B},{0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},
        {0x2300,0x400},{0x2301,0},{0x2302,4},{0x231C,0}};
    for(const auto& value:required)if(r(value[0])!=value[1])return no("raster-profile");
    if(draw.deviceState[11068]&0x10)return no("binning-flag");
    const auto scissor=nativeTranslatedScissor(draw);
    const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || *scissor!=std::array<unsigned,4>{0,0,1280,720} || !viewport)return no("viewport-scissor");
    for(unsigned i=0;i<5;++i) {
        const auto object=word(draw.deviceState,12816+4*i);const auto& s=bindings.surfaces[i];
        if(s.object!=object)return no("surface-binding");
        if(i!=0 && i!=4){if(object)return no("extra-surface");continue;}
        if(!object || object%4 || object>UINT32_MAX-39 || !s.header || s.header->size()!=40)
            return no("surface-header");
        if(word(*s.header,24)!=0x14000500 || word(*s.header,28)!=(i?0x10000u:0x302D0u))
            return no("surface-profile");
        const auto dimensions=word(*s.header,36);
        if((dimensions>>18)+1!=1280 || ((dimensions>>3)&0x7FFFu)+1!=720)return no("surface-dimensions");
    }
    // Original823AB3D8 converts format3 -> register12 for expanded blending.
    // Storage stays7e3RGB/A2. Never treat the register12 target as RGBA8 or as
    // a fresh64bpp allocation. Depth tiles0..719 and colour720..1439 are disjoint.
    if(word(draw.deviceState,12408)!=1 || bindings.surfaces[0].object==bindings.surfaces[4].object)
        return no("surface-relationship");
    unsigned textureIndex=UINT32_MAX;
    for(unsigned i=0;i<draw.textures.size();++i)if(draw.textures[i].slot==3) {
        if(textureIndex!=UINT32_MAX)return no("duplicate-texture-slot");
        textureIndex=i;
    }
    if(textureIndex==UINT32_MAX)return no("missing-texture-slot");
    const auto& texture=draw.textures[textureIndex];
    const auto desc=XenosTextureDescription::decode(texture.fetch);
    const auto storage=desc?planXenosDepth24FloatStorage(*desc):std::nullopt;
    if(!desc || !storage || texture.source.size()!=storage->sourceBytes || desc->width!=1280 ||
       desc->height!=720 || desc->pitchTexels!=1280 || desc->endian!=2 ||
       desc->clamp!=std::array<unsigned,3>{2,2,0})return no("depth-texture-layout");
    // Normalized, point min/mag/mip, no bias/aniso/offset, original RGBA
    // replication. The submitted base address remains variable and owned.
    if(texture.fetch[0]!=0x8A004802u || texture.fetch[2]!=0x0059E4FFu ||
       texture.fetch[3]!=0xD11u || texture.fetch[4]!=0 || texture.fetch[5]!=0x210u ||
       (texture.fetch[1]&0xFFFu)!=0x97u)return no("depth-texture-sampler");
    if(why)*why="state-resources-planned-not-executed";
    return NativeScreenStripPlan{bindings.surfaces[0].object,bindings.surfaces[4].object,
        1280,720,1280,720,0,*count,textureIndex,*viewport,*state,*desc};
}
}
