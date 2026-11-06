#pragma once
#include "native_expanded16_copy_plan.h"
#include "native_linear_sample_target.h"
#include "native_rgb7e3_replacement.h"
#include "native_texture_image.h"

namespace recomp::gpu {
// Fenced float readback of one admitted pass71 row (160x90 or 80x45): one
// LINEAR sample of the owned binary16 source per pixel, no clear value,
// blending or cached target.
inline std::optional<std::vector<float>> renderNativeExpanded16Copy(ProbeDevice& device,
        const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
        const NativeExpanded16CopyPlan& plan,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<std::vector<float>>{};};
    if(!validateNativeExpanded16CopyProgram(program) || draw.vertices.size()!=72 || draw.textures.size()!=1 ||
       !nativeExpanded16CopyInstanceTarget(plan.target))return no("expanded16-copy-render-input");
    const unsigned width=plan.target.width,height=plan.target.height;
    auto image=NativeTextureImage::create(device,plan.storage.description,draw.textures[0].source);
    if(!image)return no("expanded16-copy-binary16-import");
    struct Sampler {VkDevice d;VkSampler s{};~Sampler(){if(s)vkDestroySampler(d,s,nullptr);}} sampler{device.device};
    VkSamplerCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter=info.minFilter=VK_FILTER_LINEAR;info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU=info.addressModeV=info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(device.device,&info,nullptr,&sampler.s)!=VK_SUCCESS)return no("linear-sampler");
    NativeTranslatedPipelineOptions options;
    options.depthStencil=decodeXenosDepthStencilState(0x00700770,0xFFFFFE,0);
    options.externalTextures={{sampler.s,image->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned value){for(unsigned i=0;i<4;++i)input[at+i]=value>>(24-i*8);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    auto target=NativeLinearSampleTarget::create(device,width,height,1);
    if(!target)return no("expanded16-copy-target-create");
    const auto rgba=target->draw(program,input,6,plan.viewport,{plan.storage.description},options,
        NativeTranslatedBlend::Replace,{0,0,0,0},0,0);
    if(!rgba || rgba->size()!=std::size_t{width}*height*4)return no("fenced-expanded16-copy-readback");
    for(const auto v:*rgba)if(!std::isfinite(v))return no("nonfinite-expanded16-copy-output");
    if(why)*why="fenced-expanded16-copy-rgba32f";
    return rgba;
}
// Caller holds the exclusive guest/queue gate and invalidates overlapping
// cached views. Success commits only physical7e3 words for the row's tiles.
inline bool executeNativeExpanded16CopyDraw(ProbeDevice& device,const NativeTranslatedDrawProgram& program,
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const std::vector<std::uint8_t>& currentVertex,const std::vector<std::uint8_t>& currentPixel,
        bool queriesIdle,NativeEdram32Mutation& mutation,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    const auto plan=planNativeExpanded16CopyDraw(draw,bound,program,currentVertex,currentPixel,false,queriesIdle,why);
    if(!plan)return false;
    const auto rgba=renderNativeExpanded16Copy(device,program,draw,*plan,why);
    if(!rgba)return false;
    if(!recordCompletedNativeRgb7e3Replacement(mutation,plan->target,*rgba,Color7e3Rounding::XeniaShiftRound))
        return no("expanded16-copy-physical7e3-replacement");
    mutation.commit();if(why)*why="fenced-expanded16-copy-physical7e3";return true;
}
}
