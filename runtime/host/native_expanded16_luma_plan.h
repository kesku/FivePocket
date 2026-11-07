#pragma once
#include "native_expanded16_max_plan.h"

namespace recomp::gpu {
inline std::optional<Gta5InlineDraw> captureNativeExpanded16LumaDraw(
        unsigned device,unsigned primitive,unsigned count,unsigned stride,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& data,
        const Gta5TextureFetchReader& submittedFetch) {
    if(primitive!=8 || count!=3 || stride!=24 || !submittedFetch)return std::nullopt;
    auto draw=captureGta5InlineDraw(device,primitive,count,stride,{},heap,data);
    const auto fetch=submittedFetch(0);
    if(!draw || !fetch)return std::nullopt;
    auto source=captureNativeExpanded16Base(0,*fetch,data);
    if(!source)return std::nullopt;
    draw->textures.push_back(std::move(*source));return draw;
}
struct NativeExpanded16LumaPlan {
    NativeOffscreenDrawPlan target;
    XenosTextureDescription submitted;
    XenosTexture2DLayout storage;
    std::array<float,4> viewport{};
};
// Observed selector0:320x180 EXPAND point source ->64x64 R32_FLOAT, pitch80.
// Exact raster/state matching, not a generic shader or texture admission.
inline std::optional<NativeExpanded16LumaPlan> planNativeExpanded16LumaDraw(
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& currentVertex,
        const std::vector<std::uint8_t>& currentPixel,bool indexed,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeExpanded16LumaPlan>{};};
    if(!validateNativeExpanded16LumaProgram(program) || program.vertexIdentity!=currentVertex ||
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
            case 0x2000:expected=0x01000050;break;
            case 0x2001:expected=0x000E05A0;break;
            case 0x210F:case 0x2110:case 0x2112:expected=0x42000000;break;
            case 0x2111:expected=0xC2000000;break;
            case 0x2180:expected=0x10010A01;break;
        }
        if(gta5DrawRegister(draw,r[0])!=expected)return no("raster-profile");
    }
    if(draw.deviceState[11068]&0x10 || word(draw.deviceState,12408))return no("surface-mode");
    const auto scissor=nativeTranslatedScissor(draw);
    const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || *scissor!=std::array<unsigned,4>{0,0,64,64} || !viewport)return no("viewport-scissor");
    for(unsigned i=0;i<5;++i) {
        const auto& s=bound.surfaces[i];
        if(s.object!=word(draw.deviceState,12816+4*i))return no("surface-binding");
        if(i){if(s.object)return no("extra-surface");continue;}
        if(!s.object || s.object%4 || s.object>UINT32_MAX-39 || !s.header || s.header->size()!=40)
            return no("surface-header");
        if(word(*s.header,24)!=0x01000050 || word(*s.header,28)!=0x000E05A0 ||
           word(*s.header,36)!=0x00FC01F8)return no("surface-format-dimensions");
    }
    if(draw.textures.size()!=1)return no("texture-count");
    const auto& t=draw.textures[0];
    if(t.slot || t.literalZero || t.fetch[0]!=0x82804956 || (t.fetch[1]&4095)!=0x5D ||
       t.fetch[2]!=0x16613F || t.fetch[3]!=0xD11 || t.fetch[4] || t.fetch[5]!=0x210)
        return no("submitted-point-profile");
    const auto d=XenosTextureDescription::decode(t.fetch);
    const auto storage=d?planXenosExpanded16FloatStorage(*d):std::nullopt;
    if(!storage || !d->baseAddress || std::uint64_t{d->baseAddress}+storage->sourceBytes>0x20000000ULL ||
       t.source.size()!=storage->sourceBytes)return no("owned-binary16-storage");
    if(why)*why="luma-reduce-state-resources-planned-not-executed";
    return NativeExpanded16LumaPlan{{bound.surfaces[0].object,64,64,80,1440,4,14},*d,*storage,*viewport};
}
}
