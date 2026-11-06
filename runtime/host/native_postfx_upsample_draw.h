#pragma once
#include "native_postfx_upsample_plan.h"
#include "native_linear_sample_target.h"
#include "native_rgb7e3_replacement.h"
#include "native_texture_image.h"

namespace recomp::gpu {
// Fenced float readback of the SOURCE TERM of one admitted pass83 row
// (160x90): one LINEAR sample of the owned 80x45 binary16 source per pixel,
// exported as (rgb,0). Rendered with Replace into a scratch float target (no
// clear value, load or cached target is consumed); the captured ONE/ONE add
// onto the destination's physical 7e3 words is recordCompletedNativeRgb7e3AdditiveBlend.
inline std::optional<std::vector<float>> renderNativePostfxUpsample(ProbeDevice& device,
        const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
        const NativePostfxUpsamplePlan& plan,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<std::vector<float>>{};};
    if(!validateNativePostfxUpsampleProgram(program) || draw.vertices.size()!=72 || draw.textures.size()!=1 ||
       !nativePostfxUpsampleInstanceTarget(plan.target))return no("postfx-upsample-render-input");
    const unsigned width=plan.target.width,height=plan.target.height;
    auto image=NativeTextureImage::create(device,plan.storage.description,draw.textures[0].source);
    if(!image)return no("postfx-upsample-binary16-import");
    struct Sampler {VkDevice d;VkSampler s{};~Sampler(){if(s)vkDestroySampler(d,s,nullptr);}} sampler{device.device};
    VkSamplerCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter=info.minFilter=VK_FILTER_LINEAR;info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU=info.addressModeV=info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(device.device,&info,nullptr,&sampler.s)!=VK_SUCCESS)return no("linear-sampler");
    NativeTranslatedPipelineOptions options;
    options.depthStencil=decodeXenosDepthStencilState(NativePostfxUpsampleDepthControl,NativePostfxUpsampleStencilRefMask,0);
    options.externalTextures={{sampler.s,image->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned value){for(unsigned i=0;i<4;++i)input[at+i]=value>>(24-i*8);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    auto target=NativeLinearSampleTarget::create(device,width,height,1);
    if(!target)return no("postfx-upsample-target-create");
    const auto rgba=target->draw(program,input,6,plan.viewport,{plan.storage.description},options,
        NativeTranslatedBlend::Replace,{0,0,0,0},0,0);
    if(!rgba || rgba->size()!=std::size_t{width}*height*4)return no("fenced-postfx-upsample-readback");
    for(const auto v:*rgba)if(!std::isfinite(v))return no("nonfinite-postfx-upsample-output");
    // The original shader exports the literal alpha 0 (the ONE/ONE add then
    // leaves destination alpha unchanged); anything else is not its output.
    for(std::size_t i=3;i<rgba->size();i+=4)if((*rgba)[i]!=0)return no("postfx-upsample-alpha-not-zero");
    if(why)*why="fenced-postfx-upsample-rgba32f";
    return rgba;
}
// Caller holds the exclusive guest/queue gate and invalidates overlapping
// cached views. The row's whole destination rectangle (160x90 at base768)
// must already be KNOWN physical words, checked before any GPU submission;
// otherwise nothing is rendered ("destination-colour-unknown"). Success
// replaces exactly those words with the fp32 ONE/ONE sum and commits; the
// last tile row's rows 90..95 keep their words and known/unknown provenance.
inline bool executeNativePostfxUpsampleDraw(ProbeDevice& device,const NativeTranslatedDrawProgram& program,
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const std::vector<std::uint8_t>& currentVertex,const std::vector<std::uint8_t>& currentPixel,
        bool queriesIdle,NativeEdram32Mutation& mutation,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    const auto plan=planNativePostfxUpsampleDraw(draw,bound,program,currentVertex,currentPixel,false,queriesIdle,why);
    if(!plan)return false;
    const auto& t=plan->target;
    if(!mutation.candidate().readKnownColorRows(t.baseTile,t.pitch,t.width,t.height))return no("destination-colour-unknown");
    const auto rgba=renderNativePostfxUpsample(device,program,draw,*plan,why);
    if(!rgba)return false;
    if(!recordCompletedNativeRgb7e3AdditiveBlend(mutation,t,*rgba,Color7e3Rounding::XeniaShiftRound,why))return false;
    mutation.commit();if(why)*why="fenced-postfx-upsample-additive-physical7e3";return true;
}
}
