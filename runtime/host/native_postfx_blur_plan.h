#pragma once
#include "native_expanded16_copy_plan.h"

namespace recomp::gpu {
// ABI42 capture: the submitted contract is exactly ABI41's (one slot5 EXPAND
// source, primitive8, three stride24 vertices), so the capture is shared.
inline std::optional<Gta5InlineDraw> captureNativePostfxBlurDraw(
        unsigned device,unsigned primitive,unsigned count,unsigned stride,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& data,
        const Gta5TextureFetchReader& submittedFetch) {
    return captureNativeExpanded16CopyDraw(device,primitive,count,stride,heap,data,submittedFetch);
}
struct NativePostfxBlurPlan {
    NativeOffscreenDrawPlan target;
    XenosTextureDescription submitted; // Sampler state included.
    XenosTexture2DLayout storage;
    std::array<float,4> viewport{};
};
// PostFX pass81: five horizontal LINEAR taps of one slot5 EXPAND source at
// uv.x+c189.x*k, weighted by c254/c255 (runtime pixel constants, read by the
// translated module like ABI40/41). Source and target are the SAME size.
// Size-dependent words derive from the row; the observed hi-Z pitch
// (RB_SURFACE_INFO bits18+) is row data, as in ABI41:
//   0 OBSERVED (zepsefhf, after the live T2 resolve): T2 80x45 EXPAND (06546000,
//     tiled pitch96) LINEAR -> T3 80x45 k_2_10_10_10_FLOAT, surface pitch80 with
//     hiz pitch96 (RB_SURFACE_INFO01800050), base780 (RB_COLOR_INFO0003030C),
//     dims013C0160, scissor 80x45.
// Pass82 (ABI43, vertical mirror; 6ehacmfz) plans on the same row: T3 80x45
//   (06552000) -> T2 object10271250 at base780, every checked word identical.
//   1 OBSERVED (cnb086dj, pass81 after the live T1 re-resolve): T1 160x90 (0650A000,
//     tiled pitch160) LINEAR -> T4 object10271210 160x90, RB_SURFACE_INFO028000A0,
//     base768, dims027C02C8, scissor 160x90, c189.x=1/160. Pass82 at this size
//     plans only if its state matches this row exactly (not yet captured).
//   2 OBSERVED (18rl488a, pass81 after the live A 320x180 flags0 resolve): A texture
//     320x180 (0641A000, tiled pitch320) LINEAR -> A object10271110 320x180,
//     RB_SURFACE_INFO05000140, base720 (RB_COLOR_INFO000302D0), dims04FC0598,
//     scissor 320x180. Pass82 at this size plans only on an exact state match.
// Any further instance is NOT admitted until captured; each becomes one more
// row here and in NativeLinearSampleTarget's ABI42 check.
// Band constants that differ from ABI41 (observed, not derived):
//   RB_STENCILREFMASK(0x210D)=0x00FFFF00 (ref 0; ABI41 captured ref 0xFE),
//   SQ_PROGRAM_CNTL(0x2180)=0x10010501 (six PS GPRs; ABI41 0x10010101).
// No blending, depth or stencil. Exact raster/state matching, not generic admission.
inline constexpr std::uint32_t NativePostfxBlurDepthControl=0x00700770;
inline constexpr std::uint32_t NativePostfxBlurStencilRefMask=0x00FFFF00;
inline constexpr std::uint32_t NativePostfxBlurProgramControl=0x10010501;
struct NativePostfxBlurInstance {unsigned sourceWidth,sourceHeight,width,height,base,hizPitch;};
inline constexpr NativePostfxBlurInstance NativePostfxBlurInstances[]{
    {80,45,80,45,780,96},{160,90,160,90,768,160},{320,180,320,180,720,320}};
// Tiled source pitch and hi-Z pitch: the width padded to 32 (observed 96 for 80).
constexpr unsigned nativePostfxBlurPaddedPitch(unsigned width){return (width+31)/32*32;}
constexpr bool nativePostfxBlurInstancesExact() {
    for(const auto& i:NativePostfxBlurInstances)
        if(!i.width || !i.height || i.sourceWidth!=i.width || i.sourceHeight!=i.height ||
           i.width%80 || i.width>1280 || i.height>720 || i.base>=2048 ||
           i.base+(i.width/80)*((i.height+15)/16)>2048 || i.hizPitch!=nativePostfxBlurPaddedPitch(i.width))return false;
    return nativePostfxBlurPaddedPitch(80)==96;
}
static_assert(nativePostfxBlurInstancesExact());
inline bool nativePostfxBlurInstanceTarget(const NativeOffscreenDrawPlan& t) {
    return t.format==3 && std::any_of(std::begin(NativePostfxBlurInstances),std::end(NativePostfxBlurInstances),
        [&](const auto& i){return t.width==i.width && t.height==i.height && t.pitch==i.width && t.baseTile==i.base &&
            t.tileCount==(i.width/80)*((i.height+15)/16);});
}
inline std::optional<NativePostfxBlurPlan> planNativePostfxBlurDraw(
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& currentVertex,
        const std::vector<std::uint8_t>& currentPixel,bool indexed,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativePostfxBlurPlan>{};};
    if(!validateNativePostfxBlurFamilyProgram(program) || program.vertexIdentity!=currentVertex ||
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
    const auto instance=std::find_if(std::begin(NativePostfxBlurInstances),std::end(NativePostfxBlurInstances),
        [&](const auto& i){return surfaceInfo && (*surfaceInfo&0x3FFF)==i.width;});
    if(instance==std::end(NativePostfxBlurInstances))return no("raster-profile");
    const auto& in=*instance;
    for(const auto& r:NativeFilterWeightsRegisters) {
        unsigned expected=r[1];
        switch(r[0]) {
            case 0x2000:expected=(in.hizPitch<<18)|in.width;break; // row hiz pitch, 1x
            case 0x2001:expected=0x00030000u|in.base;break;        // format3, no bias
            case 0x210D:expected=NativePostfxBlurStencilRefMask;break;
            // Exact halves of small integers: 40/-22.5/22.5 observed.
            case 0x210F:case 0x2110:expected=bits(float(in.width)*.5f);break;
            case 0x2111:expected=bits(float(in.height)*-.5f);break;
            case 0x2112:expected=bits(float(in.height)*.5f);break;
            case 0x2180:expected=NativePostfxBlurProgramControl;break;
            case 0x2200:expected=NativePostfxBlurDepthControl;break;
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
    // Row0's observed sampled fetch80C04956/0654605D/0005804F: tiled pitch
    // (padded width) and extent derived; clamp, LINEAR filters, sign and endian
    // words stay exact (identical to ABI41's).
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
    if(why)*why="postfx-blur-state-resources-planned-not-executed";
    return NativePostfxBlurPlan{{bound.surfaces[0].object,in.width,in.height,in.width,in.base,
        (in.width/80)*((in.height+15)/16),3},*d,*storage,*viewport};
}
}
