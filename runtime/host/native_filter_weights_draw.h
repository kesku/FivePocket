#pragma once
#include "native_filter_weights_plan.h"
#include "native_linear_sample_target.h"
#include "native_rgb7e3_replacement.h"
#include "native_texture_image.h"

namespace recomp::gpu {
// Caller holds the exclusive guest/queue gate, supplies current owned shader
// bytes and current query status, and invalidates overlapping cached views.
// The compiler must certify unmodified position/UV fetches; the planner proves
// full coverage from CURRENT vertices. No native target cache supplies bytes.
// Success commits only physical7e3 words. No display, guest-texture, query or
// command-ring completion is invented. Failed transactions remain unpublished.
inline bool executeNativeFilterWeightsDraw(ProbeDevice& device,const NativeTranslatedDrawProgram& program,
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const std::vector<std::uint8_t>& currentVertex,const std::vector<std::uint8_t>& currentPixel,
        bool queriesIdle,NativeEdram32Mutation& mutation,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    if(!validateNativeFilterWeightsProgram(program))return no("program-interface");
    const NativeFilterWeightsIdentity identity{program.vertexIdentity,program.pixelIdentity};
    const auto plan=planNativeFilterWeightsDraw(draw,bound,identity,currentVertex,currentPixel,false,queriesIdle,why);
    if(!plan)return false;
    const auto& captured=draw.textures[plan->textureIndex];
    auto image=NativeTextureImage::createRaw10Unsigned(device,plan->texture,captured.source);
    if(!image)return no("packed10-image-import");
    struct Sampler {VkDevice d;VkSampler s{};~Sampler(){if(s)vkDestroySampler(d,s,nullptr);}} sampler{device.device};
    VkSamplerCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter=info.minFilter=VK_FILTER_NEAREST;info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU=info.addressModeV=info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(device.device,&info,nullptr,&sampler.s)!=VK_SUCCESS)return no("point-sampler");
    NativeTranslatedPipelineOptions options;
    options.depthStencil=decodeXenosDepthStencilState(0x00700760,0xFFFFFF,0);
    options.externalTextures={{sampler.s,image->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned value){for(unsigned i=0;i<4;++i)input[at+i]=value>>(24-i*8);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    auto target=NativeLinearSampleTarget::create(device,plan->target.width,plan->target.height,1);
    if(!target)return no("linear-target-create");
    const auto rgba=target->draw(program,input,6,plan->viewport,{plan->texture},options,
        NativeTranslatedBlend::Replace,{0,0,0,0},0,0);
    if(!rgba)return no("fenced-rectangle-finite-readback");
    if(!recordCompletedNativeRgb7e3Replacement(mutation,plan->target,*rgba,Color7e3Rounding::XeniaShiftRound))
        return no("physical7e3-replacement");
    mutation.commit();if(why)*why="fenced-filter-weights-physical7e3";return true;
}
}
