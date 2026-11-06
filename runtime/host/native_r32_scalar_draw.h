#pragma once
#include "native_r32_scalar_plan.h"
#include "native_linear_sample_target.h"
#include "native_edram32_bands.h"
#include "native_texture_image.h"

namespace recomp::gpu {
// Exclusive gate only. Fenced scalar output replaces only the planned raw words;
// the rest of the physical tile/padded parent remains untouched.
inline bool executeNativeR32ScalarDraw(ProbeDevice& device,const NativeTranslatedDrawProgram& program,
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const std::vector<std::uint8_t>& currentVertex,const std::vector<std::uint8_t>& currentPixel,
        bool queriesIdle,NativeEdram32Mutation& mutation,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    const auto plan=planNativeR32ScalarDraw(draw,bound,program,currentVertex,currentPixel,false,queriesIdle,why);
    if(!plan)return false;
    // Numeric format36, NOT plan->storage.description (raw RGBA8 addressing).
    auto image=NativeTextureImage::create(device,plan->submitted,draw.textures[0].source);
    if(!image)return no("r32-point-import");
    struct Sampler{VkDevice d;VkSampler s{};~Sampler(){if(s)vkDestroySampler(d,s,nullptr);}} sampler{device.device};
    VkSamplerCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter=info.minFilter=VK_FILTER_NEAREST;info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU=info.addressModeV=info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(device.device,&info,nullptr,&sampler.s)!=VK_SUCCESS)return no("point-sampler");
    NativeTranslatedPipelineOptions options;options.depthStencil=decodeXenosDepthStencilState(0x00700760,0xFFFFFF,0);
    options.externalTextures={{sampler.s,image->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned v){for(unsigned i=0;i<4;++i)input[at+i]=v>>(24-i*8);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+i*4,draw.vertexConstants[i]);put(4112+i*4,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    const auto width=plan->target.width,height=plan->target.height;
    auto target=NativeLinearSampleTarget::create(device,width,height,1);if(!target)return no("float-target-create");
    const auto rgba=target->draw(program,input,6,plan->viewport,{plan->submitted},options,
        NativeTranslatedBlend::Replace,{0,0,0,0},0,0);
    if(!rgba || rgba->size()!=width*height*4)return no("fenced-scalar-readback");
    std::vector<std::uint8_t> bytes(width*height*4);
    for(unsigned i=0;i<width*height;++i) {
        const float scalar=(*rgba)[i*4];
        if(!std::isfinite(scalar))return no("nonfinite-scalar-output");
        for(unsigned c=1;c<4;++c)if((*rgba)[i*4+c]!=scalar)return no("non-scalar-output");
        std::uint32_t bits;std::memcpy(&bits,&scalar,4);
        for(unsigned c=0;c<4;++c)bytes[i*4+c]=bits>>(c*8);
    }
    if(!recordCompletedNativeRaw32Draw(mutation,plan->target,bytes))return no("physical-r32-replacement");
    mutation.commit();if(why)*why="fenced-r32-scalar-physical-r32";return true;
}
}
