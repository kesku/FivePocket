#pragma once
#include "gta5_bound_surface_capture.h"
#include "native_geometry_sampler_plan.h"
#include "native_mip_texture_capture.h"
#include "native_translated_draw.h"
#include "xenos_depth_stencil_state.h"

namespace recomp::gpu {
// Exact observed perspective strip. Planning is not execution or ownership.
// Resource order follows the shader's first uses:2,6,1,3, not numeric sorting.
using NativeWorldStripTextures=std::array<NativeMipTextureCapture,4>;
struct NativeWorldStripIdentity {std::vector<std::uint8_t> vertex,pixel;};
struct NativeWorldStripPlan {
    unsigned colorObject=0,depthObject=0;
    std::array<float,4> viewport{};
    XenosDepthStencilState depthStencil;
    std::array<XenosTextureDescription,4> textures;
    std::array<NativeTextureSamplerPlan,4> samplers;
};
inline std::optional<NativeTextureSamplerPlan> planNativeWorldStripTexture(
        unsigned slot,const NativeMipTextureCapture& texture) {
    constexpr unsigned slots[]{2,6,1,3};
    constexpr unsigned fields[][6]{
        {0x81000002,0x54,0xFE07F,0xA80D10,3,0x210},
        {0x81004802,0x99,0x7F,0x281690,3,0x210},
        {0x84024802,0x86,0x1FE1FF,0xA80C14,0xC3,0x210},
        {0x82004802,0x52,0x3FE0FF,0x281510,3,0x210}};
    unsigned index=4;for(unsigned i=0;i<4;++i)if(slots[i]==slot)index=i;
    if(index==4)return std::nullopt;
    // Base and the real mip allocation addresses may vary, but no descriptor
    // numeric/sampling bits are sanitized. No guessed physical address alias.
    for(unsigned i=0;i<6;++i) {
        const unsigned mask=i==1 || (i==5 && slot==1)?0xFFFu:UINT32_MAX;
        if((texture.fetch[i]&mask)!=fields[index][i])return std::nullopt;
    }
    const auto desc=XenosTextureDescription::decode(texture.fetch);if(!desc)return std::nullopt;
    if(slot==6) {
        // k_16_16 unsigned normalization, two channels. The raw view proves
        // byte layout ONLY; the native image must import as R16G16_UNORM.
        const auto storage=planXenosFloatStorage(*desc);
        if(!storage || !desc->baseAddress || desc->format!=25 || desc->integerComponents ||
           desc->sign!=std::array<unsigned,4>{0,0,0,0} || !texture.mips.empty() ||
           texture.base.size()!=storage->sourceBytes || storage->sourceBytes!=16384)return std::nullopt;
        return NativeTextureSamplerPlan{{2,2},1,1,0,0,2};
    }
    const auto chain=planXenosUnpackedMipChain(texture.fetch);
    const auto sampler=planNativeGeometrySampler2D(texture.fetch);
    if(!chain || !sampler || sampler->anisotropy || texture.base.size()!=chain->baseBytes ||
       texture.mips.size()!=chain->mipBytes)return std::nullopt;
    return sampler->sampling;
}
inline std::optional<NativeWorldStripPlan> planNativeWorldStrip(
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bindings,
        const NativeWorldStripTextures& textures,const NativeWorldStripIdentity& identity,
        const std::vector<std::uint8_t>& vertex,const std::vector<std::uint8_t>& pixel,
        bool indexed,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* value){if(why)*why=value;return std::optional<NativeWorldStripPlan>{};};
    if(identity.vertex.size()!=252 || identity.pixel.size()!=912 || vertex!=identity.vertex || pixel!=identity.pixel)
        return no("shader-identity");
    if(indexed || draw.primitive!=6 || draw.vertexCount!=4 || draw.stride!=36 || draw.vertices.size()!=144)
        return no("strip-extent");
    if(draw.deviceState.size()!=13860)return no("state-extent");
    if(!queriesIdle)return no("query-owner-not-idle");
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("shader-binding");
    const auto r=[&](unsigned reg){return gta5DrawRegister(draw,reg);};
    // Original perspective clipping, smooth center interpolators, no cull/MSAA,
    // replace RGBA, depth GEQUAL/write, stencil ALWAYS/REPLACE(reference0).
    // The depth viewport is CONSTANT2^-24, not ABI28's1-Z transform.
    constexpr unsigned required[][2]{
        {0x2000,0x14000500},{0x2001,0x302D0},{0x2002,0x10000},
        {0x2104,15},{0x210D,0xFFFF00},{0x210F,0x44200000},{0x2110,0x44200000},
        {0x2111,0xC3B40000},{0x2112,0x43B40000},{0x2113,0},{0x2114,0x33800000},
        {0x2180,0x10310903},{0x2181,4},{0x2182,0xF000F},{0x2183,0},{0x2184,0},
        {0x2200,0x708767},{0x2201,0x10001},{0x2202,0xAA000005},{0x2203,0x33},
        {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},
        {0x2293,0x10},{0x2300,0x400},{0x2301,0},{0x2302,4},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF},{0x231C,0},{0x2324,2},{0x2325,0}};
    for(const auto& pair:required)if(r(pair[0])!=pair[1])return no("raster-profile");
    if(word(draw.deviceState,12408) || draw.deviceState[11068]&0x10)return no("surface-mode");
    const auto scissor=nativeTranslatedScissor(draw);
    const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || *scissor!=std::array<unsigned,4>{0,0,1280,720} || !viewport)return no("viewport-scissor");
    for(unsigned i=0;i<5;++i) {
        const auto object=word(draw.deviceState,12816+4*i);const auto& s=bindings.surfaces[i];
        if(s.object!=object)return no("surface-binding");
        if(i!=0 && i!=4){if(object)return no("extra-surface");continue;}
        if(!object || object%4 || object>UINT32_MAX-39 || !s.header || s.header->size()!=40)
            return no("surface-header");
        if(word(*s.header,24)!=0x14000500 || word(*s.header,28)!=(i?0x10000u:0x302D0u) ||
           word(*s.header,36)!=0x13FC1678u)return no("surface-profile");
    }
    if(bindings.surfaces[0].object==bindings.surfaces[4].object)return no("surface-alias");
    NativeWorldStripPlan plan;plan.colorObject=bindings.surfaces[0].object;plan.depthObject=bindings.surfaces[4].object;
    plan.viewport=*viewport;plan.depthStencil=*decodeXenosDepthStencilState(0x708767,0xFFFF00,0);
    constexpr unsigned slots[]{2,6,1,3};
    for(unsigned i=0;i<4;++i) {
        const auto sampler=planNativeWorldStripTexture(slots[i],textures[i]);
        if(!sampler)return no("texture-storage-sampler");
        plan.textures[i]=*XenosTextureDescription::decode(textures[i].fetch);plan.samplers[i]=*sampler;
    }
    if(why)*why="state-resources-planned-not-executed";
    return plan;
}
}
