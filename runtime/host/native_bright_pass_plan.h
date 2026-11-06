#pragma once
#include "native_filter_weights_plan.h"
#include "native_expanded16_texture_capture.h"
#include "xenos_expanded16_float_texture.h"

namespace recomp::gpu {
// ABI40 capture: the three submitted EXPAND textures the original pair reads.
// Slot1 and slot17 are the same one-texel exposure copy; slot10 is the
// 640x360 half-resolution scene. Owned storage evidence only, never admission.
inline std::optional<Gta5InlineDraw> captureNativeBrightPassDraw(
        unsigned device,unsigned primitive,unsigned count,unsigned stride,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& data,
        const Gta5TextureFetchReader& submittedFetch) {
    if(primitive!=8 || count!=3 || stride!=24 || !submittedFetch)return std::nullopt;
    auto draw=captureGta5InlineDraw(device,primitive,count,stride,{},heap,data);
    if(!draw)return std::nullopt;
    for(const unsigned slot:{1u,10u,17u}) {
        const auto fetch=submittedFetch(slot);
        auto texture=fetch?captureNativeExpanded16Base(slot,*fetch,data):std::nullopt;
        if(!texture)return std::nullopt;
        draw->textures.push_back(std::move(*texture));
    }
    return draw;
}
inline bool nativeBrightPassTexelFetch(const std::array<std::uint32_t,6>& f) {
    return f[0]==0x80404956 && (f[1]&4095)==0x5D && f[2]==0 && f[3]==0xD11 && !f[4] && f[5]==0x210;
}
inline bool nativeBrightPassSceneFetch(const std::array<std::uint32_t,6>& f) {
    return f[0]==0x85004956 && (f[1]&4095)==0x5D && f[2]==0x2CE27F && f[3]==0x280D11 && f[4]==3 && f[5]==0x210;
}
struct NativeBrightPassPlan {
    NativeOffscreenDrawPlan target;
    XenosTextureDescription texel,scene; // Submitted words, sampler state included.
    XenosTexture2DLayout texelStorage,sceneStorage;
    std::array<float,4> viewport{};
};
// Observed half-resolution bright pass: EXPAND 640x360 bilinear scene and a
// one-texel EXPAND exposure copy -> 320x180 k_2_10_10_10_FLOAT, pitch320,
// base720, no blending. Exact raster/state matching, not generic admission.
inline std::optional<NativeBrightPassPlan> planNativeBrightPassDraw(
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& currentVertex,
        const std::vector<std::uint8_t>& currentPixel,bool indexed,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeBrightPassPlan>{};};
    if(!validateNativeBrightPassProgram(program) || program.vertexIdentity!=currentVertex ||
       program.pixelIdentity!=currentPixel)return no("shader-identity-interface");
    if(indexed || !nativeFilterWeightsFullRectangle(draw))return no("original-position-uv-coverage");
    if(draw.deviceState.size()!=13860)return no("state-extent");
    if(!queriesIdle)return no("query-owner-not-idle");
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("shader-binding");
    for(const auto& r:NativeFilterWeightsRegisters) {
        unsigned expected=r[1];
        switch(r[0]) {
            case 0x2000:expected=0x05000140;break;
            case 0x210F:case 0x2110:expected=0x43200000;break;
            case 0x2111:expected=0xC2B40000;break;
            case 0x2112:expected=0x42B40000;break;
            case 0x2180:expected=0x10010201;break;
        }
        if(gta5DrawRegister(draw,r[0])!=expected)return no("raster-profile");
    }
    if(draw.deviceState[11068]&0x10 || word(draw.deviceState,12408))return no("surface-mode");
    const auto scissor=nativeTranslatedScissor(draw);const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || *scissor!=std::array<unsigned,4>{0,0,320,180} || !viewport)return no("viewport-scissor");
    for(unsigned i=0;i<5;++i) {
        const auto& s=bound.surfaces[i];
        if(s.object!=word(draw.deviceState,12816+4*i))return no("surface-binding");
        if(i){if(s.object)return no("extra-surface");continue;}
        if(!s.object || s.object%4 || s.object>UINT32_MAX-39 || !s.header || s.header->size()!=40)
            return no("surface-header");
        if(word(*s.header,24)!=0x05000140 || word(*s.header,28)!=0x302D0)return no("surface-format");
        const auto dimensions=word(*s.header,36);
        if((dimensions>>18)+1!=320 || ((dimensions>>3)&0x7FFF)+1!=180)return no("surface-dimensions");
    }
    if(draw.textures.size()!=3 || draw.textures[0].slot!=1 || draw.textures[1].slot!=10 || draw.textures[2].slot!=17)
        return no("texture-slots");
    const auto& t1=draw.textures[0];const auto& t10=draw.textures[1];const auto& t17=draw.textures[2];
    if(t1.literalZero || t10.literalZero || t17.literalZero || !nativeBrightPassTexelFetch(t1.fetch) ||
       t17.fetch!=t1.fetch || t17.source!=t1.source || !nativeBrightPassSceneFetch(t10.fetch))
        return no("submitted-sampling-profile");
    const auto texel=XenosTextureDescription::decode(t1.fetch),scene=XenosTextureDescription::decode(t10.fetch);
    const auto texelStorage=texel?planXenosExpanded16FloatStorage(*texel):std::nullopt;
    const auto sceneStorage=scene?planXenosExpanded16FloatStorage(*scene):std::nullopt;
    if(!texelStorage || !sceneStorage || texel->width!=1 || texel->height!=1 || scene->width!=640 || scene->height!=360 ||
       !texel->baseAddress || !scene->baseAddress ||
       std::uint64_t{texel->baseAddress}+texelStorage->sourceBytes>0x20000000ULL ||
       std::uint64_t{scene->baseAddress}+sceneStorage->sourceBytes>0x20000000ULL ||
       t1.source.size()!=texelStorage->sourceBytes || t10.source.size()!=sceneStorage->sourceBytes)
        return no("owned-binary16-storage");
    if(why)*why="bright-pass-state-resources-planned-not-executed";
    return NativeBrightPassPlan{{bound.surfaces[0].object,320,180,320,720,48,3},
        *texel,*scene,*texelStorage,*sceneStorage,*viewport};
}
}
