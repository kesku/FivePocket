#pragma once
#include "native_filter_weights_plan.h"

namespace recomp::gpu {
inline std::optional<Gta5InlineDraw> captureNativeR32ScalarDraw(
        unsigned device,unsigned primitive,unsigned count,unsigned stride,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& data,
        const Gta5TextureFetchReader& submittedFetch) {
    if(primitive!=8 || count!=3 || stride!=24 || !submittedFetch)return std::nullopt;
    return captureGta5InlineDraw(device,primitive,count,stride,{0},heap,data,submittedFetch);
}
struct NativeR32ScalarPlan {
    NativeOffscreenDrawPlan target;
    XenosTextureDescription submitted;
    XenosTexture2DLayout storage;
    std::array<float,4> viewport{};
};
// Observed selectors1/2 use the same original program for64->16,16->4,4->1.
// Match each complete source/destination profile; neither implies generic state.
// storage.description is ONLY a byte-addressing view, never a sampled format.
inline std::optional<NativeR32ScalarPlan> planNativeR32ScalarDraw(
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& currentVertex,
        const std::vector<std::uint8_t>& currentPixel,bool indexed,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeR32ScalarPlan>{};};
    if(!validateNativeR32ScalarProgram(program) || program.vertexIdentity!=currentVertex ||
       program.pixelIdentity!=currentPixel)return no("shader-identity-interface");
    if(indexed || !nativeFilterWeightsFullRectangle(draw))return no("original-position-uv-coverage");
    if(draw.deviceState.size()!=13860)return no("state-extent");
    if(!queriesIdle)return no("query-owner-not-idle");
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("shader-binding");
    const auto scissor=nativeTranslatedScissor(draw);
    const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || !viewport || (*scissor)[0] || (*scissor)[1] ||
       ((*scissor)[2]!=16 && (*scissor)[2]!=4 && (*scissor)[2]!=1) || (*scissor)[3]!=(*scissor)[2])return no("viewport-scissor");
    const auto size=(*scissor)[2];
    const unsigned positiveScale=size==16?0x41000000:size==4?0x40000000:0x3F000000;
    // The captured1x1 result reuses the first word of the scene allocation,
    // not the1440 reduction scratch tile. Size and physical base are coupled.
    const unsigned baseTile=size==1?720:1440;
    const unsigned colorInfo=0x000E0000|baseTile;
    for(const auto& r:NativeFilterWeightsRegisters) {
        unsigned expected=r[1];
        switch(r[0]) {
            case 0x2000:expected=0x00800050;break;
            case 0x2001:expected=colorInfo;break;
            case 0x210F:case 0x2110:case 0x2112:expected=positiveScale;break;
            case 0x2111:expected=positiveScale|0x80000000;break;
            case 0x2180:expected=0x10010401;break;
        }
        if(gta5DrawRegister(draw,r[0])!=expected)return no("raster-profile");
    }
    if(draw.deviceState[11068]&0x10 || word(draw.deviceState,12408))return no("surface-mode");
    for(unsigned i=0;i<5;++i) {
        const auto& s=bound.surfaces[i];
        if(s.object!=word(draw.deviceState,12816+4*i))return no("surface-binding");
        if(i){if(s.object)return no("extra-surface");continue;}
        if(!s.object || s.object%4 || s.object>UINT32_MAX-39 || !s.header || s.header->size()!=40)
            return no("surface-header");
        if(word(*s.header,24)!=0x00800050 || word(*s.header,28)!=colorInfo ||
           word(*s.header,36)!=(size==16?0x003C0078u:size==4?0x000C0018u:0u))return no("surface-format-dimensions");
    }
    if(draw.textures.size()!=1)return no("texture-count");
    const auto& t=draw.textures[0];
    if(t.slot || t.literalZero || t.fetch[0]!=(size==16?0x80804956u:0x80404956u) || (t.fetch[1]&4095)!=0xA4 ||
       t.fetch[2]!=(size==16?0x7E03Fu:size==4?0x1E00Fu:0x6003u) || t.fetch[3]!=0x16D1 || t.fetch[4] || t.fetch[5]!=0x210)
        return no("submitted-point-profile");
    const auto d=XenosTextureDescription::decode(t.fetch);
    const auto storage=d?planXenosFloatStorage(*d):std::nullopt;
    if(!storage || !d->baseAddress || std::uint64_t{d->baseAddress}+storage->sourceBytes>0x20000000ULL ||
       t.source.size()!=storage->sourceBytes)return no("owned-r32-storage");
    if(why)*why="r32-scalar-state-resources-planned-not-executed";
    return NativeR32ScalarPlan{{bound.surfaces[0].object,size,size,80,baseTile,1,14},*d,*storage,*viewport};
}
}
