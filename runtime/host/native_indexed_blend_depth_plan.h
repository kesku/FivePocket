#pragma once
#include "gta5_bound_surface_capture.h"
#include "native_geometry_sampler_plan.h"
#include "native_mip_texture_capture.h"
#include "native_translated_draw.h"
#include "xenos_depth_stencil_state.h"
#include <span>

namespace recomp::gpu {
using NativeIndexedBlendDepthTextures=std::array<NativeMipTextureCapture,3>;
struct NativeIndexedBlendDepthPlan {
    std::array<float,4> viewport{};
    XenosDepthStencilState depthStencil;
    std::array<XenosTextureDescription,3> textures;
    std::array<NativeTextureSamplerPlan,3> samplers;
};
// Existing ABI12 geometry is sufficient: original colour0 only, ten live
// interpolators, no shader depth/export side effects, three sampled resources.
// This exact consumer is NOT permission for other ABI12 states/shader pairs.
inline bool validateNativeIndexedBlendDepthProgram(const NativeTranslatedDrawProgram& p) {
    return p.abiVersion==12 && p.indexed && p.stride==28 && p.interpolators==10 &&
        p.textureSlot==3 && p.textureSlots==std::vector<unsigned>{3,2,11} &&
        p.vertexIdentity.size()==1140 && p.pixelIdentity.size()==540 &&
        p.vertexCode.size()>=5 && p.pixelCode.size()>=5 &&
        p.vertexCode.front()==0x07230203 && p.pixelCode.front()==0x07230203 &&
        !p.directTriangles && !p.affinePositionXY && !p.color8SourceClamped &&
        !p.cubeRectanglePwl && !p.explicitLodRectangle && !p.depthOnlyRectangle &&
        !p.rectangleMrt && !p.offsetSample && !p.compositeRectangle && !p.screenSpaceRectangle &&
        !p.fixed16RectangleDepth && !p.fixed16VertexRegisterCount && !p.geometryFixed16GradientDepth &&
        !p.geometryDepth20e4 && !p.geometrySampleDepth && p.geometryColor8Format==UINT32_MAX &&
        !p.geometryPixelEffects && !p.geometryDepthColorWitness && !p.geometryCentroidMask &&
        !p.geometryMrtOutputs && p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001} &&
        !p.screenStrip && !p.worldStrip;
}
inline std::optional<NativeTextureSamplerPlan> planNativeIndexedBlendDepthTexture(
        unsigned index,const NativeMipTextureCapture& texture) {
    // First-use order3,2,11: DXN, BC1, DXT5A. Only base addresses relocate.
    // Actual base-only descriptors, not guessed mips or substituted storage.
    constexpr unsigned fields[][6]{
        {0x84004002,0x71,0x3FE1FF,0xA81690,3,0x210},
        {0x88004002,0x52,0x7FE3FF,0xA81510,3,0x210},
        {0x81000002,0x7B,0x7E03F,0x2815B6,3,0x210}};
    if(index>=3)return std::nullopt;
    for(unsigned i=0;i<6;++i) {
        const auto mask=i==1?0xFFFu:UINT32_MAX;
        if((texture.fetch[i]&mask)!=fields[index][i])return std::nullopt;
    }
    const auto chain=planXenosUnpackedMipChain(texture.fetch);
    const auto sampler=planNativeGeometrySampler2D(texture.fetch);
    if(!chain || !sampler || sampler->anisotropy || texture.base.size()!=chain->baseBytes ||
       texture.mips.size()!=chain->mipBytes)return std::nullopt;
    return sampler->sampling;
}
inline std::optional<NativeIndexedBlendDepthTextures> captureNativeIndexedBlendDepthTextures(
        const Gta5TextureFetchReader& submitted,const Gta5ResourceReader& graphics) {
    if(!submitted || !graphics)return std::nullopt;
    NativeIndexedBlendDepthTextures result;constexpr unsigned slots[]{3,2,11};
    for(unsigned i=0;i<3;++i) {
        const auto fetch=submitted(slots[i]);if(!fetch)return std::nullopt;
        auto texture=captureNativeMipTextureStorage(*fetch,graphics,2*1024*1024);
        if(!texture || !planNativeIndexedBlendDepthTexture(i,*texture))return std::nullopt;
        result[i]=std::move(*texture);
    }
    return result;
}
inline std::optional<NativeIndexedBlendDepthPlan> planNativeIndexedBlendDepth(
        const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
        std::span<const std::uint32_t> indices,const Gta5BoundSurfaceCapture& bound,
        const NativeIndexedBlendDepthTextures& textures,const std::vector<std::uint8_t>& vertex,
        const std::vector<std::uint8_t>& pixel,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeIndexedBlendDepthPlan>{};};
    if(!validateNativeIndexedBlendDepthProgram(program) || vertex!=program.vertexIdentity ||
       pixel!=program.pixelIdentity)return no("indexed-blend-depth-program-identity");
    if(!queriesIdle)return no("indexed-blend-depth-query-owner");
    if(draw.primitive!=4 || draw.stride!=28 || draw.vertexCount<3 || draw.vertexCount>65536 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*28 || draw.vertices.size()>256*1024 ||
       indices.empty() || indices.size()>65535 || indices.size()%3)return no("indexed-blend-depth-extent");
    for(auto i:indices)if(i>=draw.vertexCount)return no("indexed-blend-depth-index-range");
    if(draw.deviceState.size()!=13860)return no("indexed-blend-depth-state-extent");
    const auto word=[](const auto& b,unsigned at){return(std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("indexed-blend-depth-shader-binding");
    constexpr unsigned required[][2]{
        {0x2000,0x14000500},{0x2001,0xC02D0},{0x2002,0x10000},
        {0x2104,15},{0x210D,0x707},{0x210F,0x44200000},{0x2110,0x44200000},
        {0x2111,0xC3B40000},{0x2112,0x43B40000},{0x2113,0xBF800000},{0x2114,0x3F800000},
        {0x2180,0x10910A11},{0x2181,4},{0x2182,0x03FF03FF},{0x2183,0},{0x2184,0},
        {0x2200,0x700263},{0x2201,0x07060706},{0x2202,0xAA000005},{0x2203,0x33},
        {0x2204,0x80000},{0x2205,0x18002},{0x2206,0x43F},{0x2207,0},{0x2208,4},
        {0x2284,0},{0x2292,0},{0x2293,0x10},{0x2300,0x400},{0x2301,0},{0x2302,4},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF},{0x231C,0},{0x2324,2},{0x2325,0}};
    for(const auto& r:required)if(gta5DrawRegister(draw,r[0])!=r[1])return no("indexed-blend-depth-raster-profile");
    // Original target-mode flag intentionally enables format12 while the
    // bound object retains format3 storage (Xenia GetStorageColorFormat).
    if(word(draw.deviceState,12408)!=1 || draw.deviceState[11068] || draw.deviceState[12498] ||
       draw.deviceState[12563])return no("indexed-blend-depth-recording-query-mode");
    const auto viewport=nativeTranslatedViewport(draw);
    const auto scissor=nativeTranslatedScissor(draw);
    if(!viewport || !scissor || *scissor!=std::array<unsigned,4>{0,0,1280,720})return no("indexed-blend-depth-viewport-scissor");
    for(unsigned i=0;i<5;++i) {
        const auto object=word(draw.deviceState,12816+4*i);const auto& s=bound.surfaces[i];
        if(s.object!=object)return no("indexed-blend-depth-surface-binding");
        if(i!=0 && i!=4){if(object)return no("indexed-blend-depth-extra-surface");continue;}
        if(!object || object%4 || object>UINT32_MAX-39 || !s.header || s.header->size()!=40 ||
           word(*s.header,24)!=0x14000500 || word(*s.header,28)!=(i?0x10000u:0x302D0u) ||
           word(*s.header,36)!=0x13FC1678u)return no("indexed-blend-depth-surface-profile");
    }
    if(bound.surfaces[0].object==bound.surfaces[4].object)return no("indexed-blend-depth-surface-alias");
    NativeIndexedBlendDepthPlan p;p.viewport=*viewport;p.depthStencil=*decodeXenosDepthStencilState(0x700263,0x707,0);
    for(unsigned i=0;i<3;++i) {
        const auto sampler=planNativeIndexedBlendDepthTexture(i,textures[i]);
        if(!sampler)return no("indexed-blend-depth-texture-sampler-storage");
        p.textures[i]=*XenosTextureDescription::decode(textures[i].fetch);p.samplers[i]=*sampler;
    }
    if(why)*why="indexed-blend-depth-plan-only";return p;
}
}
