#pragma once
#include "native_filter_weights_plan.h"
#include "native_expanded16_texture_capture.h"
#include "xenos_expanded16_float_texture.h"

namespace recomp::gpu {
// ABI41 capture: the one submitted slot5 EXPAND source of the copy pass.
inline std::optional<Gta5InlineDraw> captureNativeExpanded16CopyDraw(
        unsigned device,unsigned primitive,unsigned count,unsigned stride,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& data,
        const Gta5TextureFetchReader& submittedFetch) {
    if(primitive!=8 || count!=3 || stride!=24 || !submittedFetch)return std::nullopt;
    auto draw=captureGta5InlineDraw(device,primitive,count,stride,{},heap,data);
    const auto fetch=submittedFetch(5);
    auto texture=draw && fetch?captureNativeExpanded16Base(5,*fetch,data):std::nullopt;
    if(!texture)return std::nullopt;
    draw->textures.push_back(std::move(*texture));return draw;
}
struct NativeExpanded16CopyPlan {
    NativeOffscreenDrawPlan target;
    XenosTextureDescription submitted; // Sampler state included.
    XenosTexture2DLayout storage;
    std::array<float,4> viewport{};
};
// PostFX pass71 as issued twice by 8237C8E8 through 82916520: same technique
// handle, pass index, slot5 sampler variable and shader pair; only the source
// texture and bound target change. Each row halves its source EXACTLY (static
// check below); size-dependent words derive from its row, except the observed
// hi-Z pitch (RB_SURFACE_INFO bits18+), which is row data:
//   0 OBSERVED (sj1q6kci, blit return 8237CA80): A 320x180 EXPAND LINEAR ->
//     T1 160x90 k_2_10_10_10_FLOAT, pitch160, hiz160, base768.
//   1 OBSERVED (77745p73, after live T1 resolve): T1 160x90 (0650A000) ->
//     T2 80x45, surface pitch80 with hiz pitch96 (RB_SURFACE_INFO01800050),
//     base780 (RB_COLOR_INFO0003030C = 768 + T1's 12 tiles), dims013C0160.
// No blending, depth or stencil. Exact raster/state matching, not generic admission.
struct NativeExpanded16CopyInstance {unsigned sourceWidth,sourceHeight,width,height,base,hizPitch;};
inline constexpr NativeExpanded16CopyInstance NativeExpanded16CopyInstances[]{
    {320,180,160,90,768,160},{160,90,80,45,780,96}};
constexpr bool nativeExpanded16CopyInstancesHalve() {
    // Both observed hi-Z pitches are the width rounded up to 32 samples.
    for(const auto& i:NativeExpanded16CopyInstances)
        if(!i.width || !i.height || i.sourceWidth!=2*i.width || i.sourceHeight!=2*i.height ||
           i.width%80 || i.sourceWidth%32 || i.base>=2048 || i.hizPitch!=(i.width+31)/32*32)return false;
    return true;
}
static_assert(nativeExpanded16CopyInstancesHalve());
inline bool nativeExpanded16CopyInstanceTarget(const NativeOffscreenDrawPlan& t) {
    return t.format==3 && std::any_of(std::begin(NativeExpanded16CopyInstances),std::end(NativeExpanded16CopyInstances),
        [&](const auto& i){return t.width==i.width && t.height==i.height && t.pitch==i.width && t.baseTile==i.base &&
            t.tileCount==(i.width/80)*((i.height+15)/16);});
}
inline std::optional<NativeExpanded16CopyPlan> planNativeExpanded16CopyDraw(
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& currentVertex,
        const std::vector<std::uint8_t>& currentPixel,bool indexed,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeExpanded16CopyPlan>{};};
    if(!validateNativeExpanded16CopyProgram(program) || program.vertexIdentity!=currentVertex ||
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
    const auto instance=std::find_if(std::begin(NativeExpanded16CopyInstances),std::end(NativeExpanded16CopyInstances),
        [&](const auto& i){return surfaceInfo && (*surfaceInfo&0x3FFF)==i.width;});
    if(instance==std::end(NativeExpanded16CopyInstances))return no("raster-profile");
    const auto& in=*instance;
    for(const auto& r:NativeFilterWeightsRegisters) {
        unsigned expected=r[1];
        switch(r[0]) {
            case 0x2000:expected=(in.hizPitch<<18)|in.width;break; // row hiz pitch, 1x
            case 0x2001:expected=0x00030000u|in.base;break;        // format3, no bias
            case 0x210D:expected=0x00FFFFFE;break;
            // Exact halves of small integers: 80/-45/45 observed, 40/-22.5/22.5 derived.
            case 0x210F:case 0x2110:expected=bits(float(in.width)*.5f);break;
            case 0x2111:expected=bits(float(in.height)*-.5f);break;
            case 0x2112:expected=bits(float(in.height)*.5f);break;
            case 0x2180:expected=0x10010101;break;
            case 0x2200:expected=0x00700770;break;
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
    // Row0's observed sampled fetch82804956/16613F with its pitch and extent
    // derived; clamp, LINEAR filters, sign and endian words stay exact.
    if(t.literalZero || t.fetch[0]!=(0x80004956u|((in.sourceWidth/32)<<22)) || (t.fetch[1]&4095)!=0x5D ||
       t.fetch[2]!=((in.sourceWidth-1)|((in.sourceHeight-1)<<13)) ||
       t.fetch[3]!=0xA80D11 || t.fetch[4]!=3 || t.fetch[5]!=0x210)return no("submitted-linear-profile");
    const auto d=XenosTextureDescription::decode(t.fetch);
    const auto storage=d?planXenosExpanded16FloatStorage(*d):std::nullopt;
    if(!storage || d->width!=in.sourceWidth || d->height!=in.sourceHeight || !d->baseAddress ||
       std::uint64_t{d->baseAddress}+storage->sourceBytes>0x20000000ULL || t.source.size()!=storage->sourceBytes)
        return no("owned-binary16-storage");
    if(why)*why="expanded16-copy-state-resources-planned-not-executed";
    return NativeExpanded16CopyPlan{{bound.surfaces[0].object,in.width,in.height,in.width,in.base,
        (in.width/80)*((in.height+15)/16),3},*d,*storage,*viewport};
}
}
