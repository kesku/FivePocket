#pragma once
#include "native_postfx_blur_plan.h"

namespace recomp::gpu {
// ABI44 capture: the submitted contract is exactly ABI41's (one slot5 EXPAND
// source, primitive8, three stride24 vertices), so the capture is shared.
inline std::optional<Gta5InlineDraw> captureNativePostfxUpsampleDraw(
        unsigned device,unsigned primitive,unsigned count,unsigned stride,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& data,
        const Gta5TextureFetchReader& submittedFetch) {
    return captureNativeExpanded16CopyDraw(device,primitive,count,stride,heap,data,submittedFetch);
}
struct NativePostfxUpsamplePlan {
    NativeOffscreenDrawPlan target;
    XenosTextureDescription submitted; // Sampler state included.
    XenosTexture2DLayout storage;
    std::array<float,4> viewport{};
};
// PostFX pass83: ONE LINEAR sample of one slot5 EXPAND source at the
// interpolated UV, exported as (rgb, literal 0), ADDED to the target:
// RB_BLENDCONTROL0..3 = 0x01010101 (colour and alpha: src ONE, dst ONE, ADD),
// so target = target + (sample.rgb, 0). The source is HALF the target in each
// axis (an upsample; ABI41's rows halve). Size-dependent words derive from the
// row; the observed hi-Z pitch (RB_SURFACE_INFO bits18+) is row data, as in ABI41:
//   0 OBSERVED (8hvjz4qw, after the live pass82 and its T2 resolve): T2 80x45
//     EXPAND (06546000, tiled pitch96) LINEAR -> T1 object102711D0 (ABI41
//     row0's target) 160x90 k_2_10_10_10_FLOAT, RB_SURFACE_INFO028000A0
//     (pitch160, hiz160), RB_COLOR_INFO00030300 (base768), dims027C02C8,
//     scissor 160x90, viewport 80/-45/45.
// Band words that differ from ABI41 row0 (observed, not derived):
//   RB_STENCILREFMASK(0x210D)=0x00FFFF00 (ref 0, as ABI42),
//   RB_BLENDCONTROL0..3 (0x2201,0x2209,0x220A,0x220B)=0x01010101 (ABI41/42: 0x00010001).
// SQ_PROGRAM_CNTL(0x2180)=0x10010101 and RB_DEPTHCONTROL=0x00700770 equal ABI41's;
// every other NativeFilterWeightsRegisters word equals the table.
// Later pass83 instances (T4 160x90 -> A 320x180) are NOT admitted until
// captured; each becomes one more row here and in NativeLinearSampleTarget's
// ABI44 check. Exact raster/state matching, not generic admission.
inline constexpr std::uint32_t NativePostfxUpsampleDepthControl=0x00700770;
inline constexpr std::uint32_t NativePostfxUpsampleStencilRefMask=0x00FFFF00;
inline constexpr std::uint32_t NativePostfxUpsampleProgramControl=0x10010101;
inline constexpr std::uint32_t NativePostfxUpsampleBlendControl=0x01010101;
// Xenos RB_BLENDCONTROL fields: colour src[0:4] fn[5:7] dst[8:12], alpha
// src[16:20] fn[21:23] dst[24:28]; blend factor1 = ONE, fn0 = ADD.
static_assert((NativePostfxUpsampleBlendControl&31)==1 && ((NativePostfxUpsampleBlendControl>>5)&7)==0 &&
    ((NativePostfxUpsampleBlendControl>>8)&31)==1 && ((NativePostfxUpsampleBlendControl>>16)&31)==1 &&
    ((NativePostfxUpsampleBlendControl>>21)&7)==0 && ((NativePostfxUpsampleBlendControl>>24)&31)==1 &&
    (NativePostfxUpsampleBlendControl&0xE0E0E0E0u)==0);
struct NativePostfxUpsampleInstance {unsigned sourceWidth,sourceHeight,width,height,base,hizPitch;};
inline constexpr NativePostfxUpsampleInstance NativePostfxUpsampleInstances[]{
    {80,45,160,90,768,160},{160,90,320,180,720,320}};
// Row1 OBSERVED (1if67slu, after live pass81/82 at 160x90 and T1's clearing
// resolve): T1 160x90 (0650A000, pitch160) -> A object10271110 320x180 base720
// (RB_SURFACE_INFO05000140, dims04FC0598); A's EDRAM holds the bright pass words.
constexpr bool nativePostfxUpsampleInstancesDouble() {
    // Source tiled pitch and hi-Z pitch: the width padded to 32 (ABI41/42 rule).
    for(const auto& i:NativePostfxUpsampleInstances)
        if(!i.sourceWidth || !i.sourceHeight || i.width!=2*i.sourceWidth || i.height!=2*i.sourceHeight ||
           i.width%80 || i.width>1280 || i.height>720 || i.base>=2048 ||
           i.base+(i.width/80)*((i.height+15)/16)>2048 || i.hizPitch!=nativePostfxBlurPaddedPitch(i.width))return false;
    return nativePostfxBlurPaddedPitch(160)==160;
}
static_assert(nativePostfxUpsampleInstancesDouble());
inline bool nativePostfxUpsampleInstanceTarget(const NativeOffscreenDrawPlan& t) {
    return t.format==3 && std::any_of(std::begin(NativePostfxUpsampleInstances),std::end(NativePostfxUpsampleInstances),
        [&](const auto& i){return t.width==i.width && t.height==i.height && t.pitch==i.width && t.baseTile==i.base &&
            t.tileCount==(i.width/80)*((i.height+15)/16);});
}
inline std::optional<NativePostfxUpsamplePlan> planNativePostfxUpsampleDraw(
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& currentVertex,
        const std::vector<std::uint8_t>& currentPixel,bool indexed,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativePostfxUpsamplePlan>{};};
    if(!validateNativePostfxUpsampleProgram(program) || program.vertexIdentity!=currentVertex ||
       program.pixelIdentity!=currentPixel)return no("shader-identity-interface");
    if(indexed || !nativeFilterWeightsFullRectangle(draw))return no("original-position-uv-coverage");
    if(draw.deviceState.size()!=13860)return no("state-extent");
    if(!queriesIdle)return no("query-owner-not-idle");
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto bits=[](float v){std::uint32_t b=0;std::memcpy(&b,&v,4);return b;};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("shader-binding");
    // The row is chosen by RB_SURFACE_INFO's pitch alone; its height, base,
    // viewport, scissor, surface header and source texture must then all agree.
    const auto surfaceInfo=gta5DrawRegister(draw,0x2000);
    const auto instance=std::find_if(std::begin(NativePostfxUpsampleInstances),std::end(NativePostfxUpsampleInstances),
        [&](const auto& i){return surfaceInfo && (*surfaceInfo&0x3FFF)==i.width;});
    if(instance==std::end(NativePostfxUpsampleInstances))return no("raster-profile");
    const auto& in=*instance;
    for(const auto& r:NativeFilterWeightsRegisters) {
        unsigned expected=r[1];
        switch(r[0]) {
            case 0x2000:expected=(in.hizPitch<<18)|in.width;break; // row hiz pitch, 1x
            case 0x2001:expected=0x00030000u|in.base;break;        // format3, no bias
            case 0x210D:expected=NativePostfxUpsampleStencilRefMask;break;
            // Exact halves of small integers: 80/-45/45 observed.
            case 0x210F:case 0x2110:expected=bits(float(in.width)*.5f);break;
            case 0x2111:expected=bits(float(in.height)*-.5f);break;
            case 0x2112:expected=bits(float(in.height)*.5f);break;
            case 0x2180:expected=NativePostfxUpsampleProgramControl;break;
            case 0x2200:expected=NativePostfxUpsampleDepthControl;break;
            // All four render-target blend controls are checked words, bound or not.
            case 0x2201:case 0x2209:case 0x220A:case 0x220B:
                if(gta5DrawRegister(draw,r[0])!=NativePostfxUpsampleBlendControl)return no("additive-blend-profile");
                continue;
        }
        if(gta5DrawRegister(draw,r[0])!=expected)return no("raster-profile");
    }
    if(draw.deviceState[11068]&0x10 || word(draw.deviceState,12408))return no("surface-mode");
    const auto scissor=nativeTranslatedScissor(draw);const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || *scissor!=std::array<unsigned,4>{0,0,in.width,in.height} || !viewport)return no("viewport-scissor");
    for(unsigned i=0;i<5;++i) {
        const auto& s=bound.surfaces[i];
        if(s.object!=word(draw.deviceState,12816+4*i))return no("surface-binding");
        if(i){if(s.object)return no("extra-surface");continue;}
        if(!s.object || s.object%4 || s.object>UINT32_MAX-39 || !s.header || s.header->size()!=40)
            return no("surface-header");
        if(word(*s.header,24)!=((in.hizPitch<<18)|in.width) || word(*s.header,28)!=(0x30000u|in.base))return no("surface-format");
        const auto dimensions=word(*s.header,36);
        if((dimensions>>18)+1!=in.width || ((dimensions>>3)&0x7FFF)+1!=in.height)return no("surface-dimensions");
    }
    if(draw.textures.size()!=1 || draw.textures[0].slot!=5)return no("texture-slot");
    const auto& t=draw.textures[0];
    // Row0's observed sampled fetch80C04956/0654605D/0005804F (the live T2
    // texture, as pass81 samples it): tiled pitch (padded source width) and
    // extent derived; clamp, LINEAR filters, sign and endian words stay exact.
    const unsigned sourcePitch=nativePostfxBlurPaddedPitch(in.sourceWidth);
    if(t.literalZero || t.fetch[0]!=(0x80004956u|((sourcePitch/32)<<22)) || (t.fetch[1]&4095)!=0x5D ||
       t.fetch[2]!=((in.sourceWidth-1)|((in.sourceHeight-1)<<13)) ||
       t.fetch[3]!=0xA80D11 || t.fetch[4]!=3 || t.fetch[5]!=0x210)return no("submitted-linear-profile");
    const auto d=XenosTextureDescription::decode(t.fetch);
    const auto storage=d?planXenosExpanded16FloatStorage(*d):std::nullopt;
    if(!storage || d->width!=in.sourceWidth || d->height!=in.sourceHeight || d->pitchTexels!=sourcePitch ||
       !d->baseAddress || std::uint64_t{d->baseAddress}+storage->sourceBytes>0x20000000ULL ||
       t.source.size()!=storage->sourceBytes)
        return no("owned-binary16-storage");
    if(why)*why="postfx-upsample-state-resources-planned-not-executed";
    return NativePostfxUpsamplePlan{{bound.surfaces[0].object,in.width,in.height,in.width,in.base,
        (in.width/80)*((in.height+15)/16),3},*d,*storage,*viewport};
}
}
