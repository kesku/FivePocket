#pragma once
#include "native_bright_pass_plan.h"
#include "native_linear_sample_target.h"
#include "native_rgb7e3_replacement.h"
#include "native_texture_image.h"

namespace recomp::gpu {
// Fenced float readback of the 320x180 bright pass. Slot1/slot17 share the
// POINT one-texel exposure copy; slot10 samples the 640x360 scene LINEAR with
// its single base level. No clear value, blending or cached target supplies
// output: every pixel is written by the original full rectangle.
inline std::optional<std::vector<float>> renderNativeBrightPass(ProbeDevice& device,
        const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
        const NativeBrightPassPlan& plan,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<std::vector<float>>{};};
    if(!validateNativeBrightPassProgram(program) || draw.vertices.size()!=72 || draw.textures.size()!=3 ||
       plan.target.width!=320 || plan.target.height!=180 || plan.target.format!=3)return no("bright-pass-render-input");
    auto texel=NativeTextureImage::create(device,plan.texelStorage.description,draw.textures[0].source);
    auto scene=NativeTextureImage::create(device,plan.sceneStorage.description,draw.textures[1].source);
    if(!texel || !scene)return no("bright-pass-binary16-import");
    struct Samplers {VkDevice d;VkSampler point{},linear{};
        ~Samplers(){if(point)vkDestroySampler(d,point,nullptr);if(linear)vkDestroySampler(d,linear,nullptr);}} samplers{device.device};
    VkSamplerCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter=info.minFilter=VK_FILTER_NEAREST;info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU=info.addressModeV=info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(device.device,&info,nullptr,&samplers.point)!=VK_SUCCESS)return no("point-sampler");
    info.magFilter=info.minFilter=VK_FILTER_LINEAR;
    if(vkCreateSampler(device.device,&info,nullptr,&samplers.linear)!=VK_SUCCESS)return no("linear-sampler");
    NativeTranslatedPipelineOptions options;
    options.depthStencil=decodeXenosDepthStencilState(0x00700760,0xFFFFFF,0);
    options.externalTextures={{samplers.point,texel->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                              {samplers.linear,scene->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                              {samplers.point,texel->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned value){for(unsigned i=0;i<4;++i)input[at+i]=value>>(24-i*8);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    auto target=NativeLinearSampleTarget::create(device,320,180,1);
    if(!target)return no("bright-pass-target-create");
    const auto rgba=target->draw(program,input,6,plan.viewport,
        {plan.texelStorage.description,plan.sceneStorage.description,plan.texelStorage.description},
        options,NativeTranslatedBlend::Replace,{0,0,0,0},0,0);
    if(!rgba || rgba->size()!=320*180*4)return no("fenced-bright-pass-readback");
    for(std::size_t i=0;i<rgba->size();++i) {
        if(!std::isfinite((*rgba)[i]))return no("nonfinite-bright-pass-output");
        if(i%4==3 && (*rgba)[i]!=0)return no("bright-pass-literal-alpha");
    }
    if(why)*why="fenced-bright-pass-rgba32f";
    return rgba;
}
// Caller holds the exclusive guest/queue gate and invalidates overlapping
// cached views. Success commits only physical7e3 words for the 48 tiles; no
// display, guest texture, query or command-ring completion is invented.
inline bool executeNativeBrightPassDraw(ProbeDevice& device,const NativeTranslatedDrawProgram& program,
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const std::vector<std::uint8_t>& currentVertex,const std::vector<std::uint8_t>& currentPixel,
        bool queriesIdle,NativeEdram32Mutation& mutation,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    const auto plan=planNativeBrightPassDraw(draw,bound,program,currentVertex,currentPixel,false,queriesIdle,why);
    if(!plan)return false;
    const auto rgba=renderNativeBrightPass(device,program,draw,*plan,why);
    if(!rgba)return false;
    if(!recordCompletedNativeRgb7e3Replacement(mutation,plan->target,*rgba,Color7e3Rounding::XeniaShiftRound))
        return no("bright-pass-physical7e3-replacement");
    mutation.commit();if(why)*why="fenced-bright-pass-physical7e3";return true;
}
}
