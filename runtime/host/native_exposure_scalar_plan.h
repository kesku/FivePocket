#pragma once
#include "native_r32_scalar_plan.h"

namespace recomp::gpu {
struct NativeExposureScalarPlan {
    NativeOffscreenDrawPlan target;
    XenosTextureDescription submitted;
    XenosTexture2DLayout storage;
    std::array<float,4> viewport{};
};
// Position-only ABI39, full1x1 replacement. A separate graph certificate proves
// that the original vertex texture read reaches no export. No fake sample,
// interpolation, previous exposure value or output is supplied here.
inline std::optional<NativeExposureScalarPlan> planNativeExposureScalarDraw(
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& currentVertex,
        const std::vector<std::uint8_t>& currentPixel,bool indexed,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeExposureScalarPlan>{};};
    if(!validateNativeExposureScalarProgram(program) || program.vertexIdentity!=currentVertex ||
       program.pixelIdentity!=currentPixel)return no("shader-identity-interface");
    if(indexed || !nativeFilterWeightsFullRectangle(draw))return no("original-position-coverage");
    if(draw.deviceState.size()!=13860)return no("state-extent");
    if(!queriesIdle)return no("query-owner-not-idle");
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("shader-binding");
    const auto scissor=nativeTranslatedScissor(draw);const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || !viewport || *scissor!=std::array<unsigned,4>{0,0,1,1})return no("viewport-scissor");
    for(const auto& r:NativeFilterWeightsRegisters) {
        unsigned expected=r[1];
        switch(r[0]) {
            case 0x2000:expected=0x00800050;break;
            case 0x2001:expected=0x000702D0;break;
            case 0x210F:case 0x2110:case 0x2112:expected=0x3F000000;break;
            case 0x2111:expected=0xBF000000;break;
            // SQ program control/context: no pixel interpolator is exported.
            case 0x2180:expected=0x10010001;break;
            case 0x2181:case 0x2182:expected=0;break;
        }
        if(gta5DrawRegister(draw,r[0])!=expected)return no("raster-profile");
    }
    if(draw.deviceState[11068]&0x10 || word(draw.deviceState,12408))return no("surface-mode");
    for(unsigned i=0;i<5;++i) {
        const auto& s=bound.surfaces[i];
        if(s.object!=word(draw.deviceState,12816+4*i))return no("surface-binding");
        if(i){if(s.object)return no("extra-surface");continue;}
        if(!s.object || s.object%4 || s.object>UINT32_MAX-39 || !s.header || s.header->size()!=40)return no("surface-header");
        if(word(*s.header,24)!=0x00800050 || word(*s.header,28)!=0x000702D0 || word(*s.header,36))
            return no("surface-format-dimensions");
    }
    if(draw.textures.size()!=1)return no("texture-count");
    const auto& t=draw.textures[0];
    if(t.slot || t.literalZero || t.fetch[0]!=0x80404956 || (t.fetch[1]&4095)!=0xA4 ||
       t.fetch[2] || t.fetch[3]!=0x16D1 || t.fetch[4] || t.fetch[5]!=0x210)return no("submitted-point-profile");
    const auto d=XenosTextureDescription::decode(t.fetch);
    const auto storage=d?planXenosFloatStorage(*d):std::nullopt;
    if(!storage || !d->baseAddress || std::uint64_t{d->baseAddress}+storage->sourceBytes>0x20000000ULL ||
       t.source.size()!=storage->sourceBytes || storage->sourceBytes!=4096)return no("owned-r32-storage");
    if(why)*why="exposure-state-resources-planned-not-executed";
    // 64bpp pitch80 has TWO physical tiles, although only two words are written.
    return NativeExposureScalarPlan{{bound.surfaces[0].object,1,1,80,720,2,7},*d,*storage,*viewport};
}
}
