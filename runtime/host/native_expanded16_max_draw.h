#pragma once
#include "native_expanded16_max_plan.h"
#include "native_linear_sample_target.h"
#include "native_rgb7e3_replacement.h"
#include "native_texture_image.h"

namespace recomp::gpu {
// Execute only under the exclusive guest/queue gate. Original source pixels
// remain untouched; only fenced, finite complete output may replace physical
// colour words. No display, ring, query completion or texture publication.
inline bool executeNativeExpanded16MaxDraw(ProbeDevice& device,const NativeTranslatedDrawProgram& program,
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const std::vector<std::uint8_t>& currentVertex,const std::vector<std::uint8_t>& currentPixel,
        bool queriesIdle,NativeEdram32Mutation& mutation,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    const auto plan=planNativeExpanded16MaxDraw(draw,bound,program,currentVertex,currentPixel,false,queriesIdle,why);
    if(!plan)return false;
    auto image=NativeTextureImage::create(device,plan->storage.description,draw.textures[0].source);
    if(!image || !image->supportsLinearFiltering())return no("binary16-linear-import");
    struct Sampler{VkDevice d;VkSampler s{};~Sampler(){if(s)vkDestroySampler(d,s,nullptr);}} sampler{device.device};
    VkSamplerCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter=info.minFilter=VK_FILTER_LINEAR;info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU=info.addressModeV=info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(device.device,&info,nullptr,&sampler.s)!=VK_SUCCESS)return no("linear-sampler");
    NativeTranslatedPipelineOptions options;options.depthStencil=decodeXenosDepthStencilState(0x00700760,0xFFFFFF,0);
    options.externalTextures={{sampler.s,image->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned v){for(unsigned i=0;i<4;++i)input[at+i]=v>>(24-i*8);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+i*4,draw.vertexConstants[i]);put(4112+i*4,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    auto target=NativeLinearSampleTarget::create(device,320,180,1);if(!target)return no("float-target-create");
    const auto rgba=target->draw(program,input,6,plan->viewport,{plan->storage.description},options,
        NativeTranslatedBlend::Replace,{0,0,0,0},0,0);
    if(!rgba)return no("fenced-max-reduce-readback");
    if(!recordCompletedNativeRgb7e3Replacement(mutation,plan->target,*rgba,Color7e3Rounding::XeniaShiftRound))
        return no("physical7e3-max-replacement");
    mutation.commit();if(why)*why="fenced-expanded16-max-physical7e3";return true;
}
}
