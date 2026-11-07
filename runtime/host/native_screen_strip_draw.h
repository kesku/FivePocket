#pragma once
#include "native_screen_strip_physical.h"
#include "native_vertex_clip_executor.h"
#include "native_linear_sample_target.h"
#include "native_texture_image.h"

namespace recomp::gpu {
struct NativeScreenStripProgram {
    NativeTranslatedDrawProgram draw;
    NativeClippedFanProgram positions;
};
// Exact ABI28 consumer. Caller holds the live execution/queue gate, has
// captured owned shader/resource bytes and invalidated overlapping native
// views. No display, guest-texture or ring completion is produced here.
inline bool executeNativeScreenStripDraw(ProbeDevice& device,const NativeScreenStripProgram& program,
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,bool queriesIdle,
        NativeEdram32Mutation& mutation,const char** why=nullptr) {
    const auto no=[&](const char* text){if(why)*why=text;return false;};
    if(!validateNativeScreenStripProgram(program.draw) || program.positions.stride!=8 ||
       program.positions.vertexIdentity!=program.draw.vertexIdentity ||
       program.positions.pixelIdentity!=program.draw.pixelIdentity)return no("program-interface");
    const NativeScreenStripIdentity identity{program.draw.vertexIdentity,program.draw.pixelIdentity};
    const auto plan=planNativeScreenStrip(draw,bound,identity,program.draw.vertexIdentity,
        program.draw.pixelIdentity,false,queriesIdle,why);
    if(!plan)return false;
    auto executor=NativeVertexClipExecutor::create(device,program.positions);
    if(!executor)return no("position-executor");
    const auto positions=executor->stripPositions(draw);
    if(!positions || !nativeScreenStripSingleCoverage(*positions))return no("position-grid-proof");
    const auto& captured=draw.textures[plan->textureIndex];
    auto texture=NativeTextureImage::createDepth24Float(device,plan->texture,captured.source);
    if(!texture)return no("depth-texture-upload");
    struct Sampler {VkDevice device;VkSampler handle{};~Sampler(){if(handle)vkDestroySampler(device,handle,nullptr);}} sampler{device.device};
    VkSamplerCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    s.minFilter=s.magFilter=VK_FILTER_NEAREST;s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU=s.addressModeV=s.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(device.device,&s,nullptr,&sampler.handle)!=VK_SUCCESS)return no("depth-texture-sampler");
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned value){for(unsigned c=0;c<4;++c)input[at+c]=value>>(24-8*c);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+i*4,draw.vertexConstants[i]);put(4112+i*4,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    NativeTranslatedPipelineOptions options;options.depthStencil=plan->depthStencil;
    options.colorWriteMasks={7,0,0,0};
    options.externalTextures={{sampler.handle,texture->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const char* gpuFailure=nullptr;
    const bool complete=executeNativeScreenStripPhysical(mutation,*plan,*positions,
        [&](const auto& rgba,const auto& depth)->std::optional<NativeScreenStripReadback> {
            auto target=NativeLinearSampleTarget::create(device,plan->width,plan->height,1);
            if(!target){gpuFailure="linear-target-create";return std::nullopt;}
            if(!target->loadSingleSampleContents(rgba,depth)){gpuFailure="physical-import";return std::nullopt;}
            auto output=target->draw(program.draw,input,plan->expandedVertices,plan->viewport,{plan->texture},options,
                NativeTranslatedBlend::ScreenStripSourceOne,{0,0,0,0},0,0,true,{},true);
            if(!output){gpuFailure="strip-draw-finite-readback";return std::nullopt;}
            auto afterDepth=target->readSingleSampleDepthWords();
            if(!afterDepth){gpuFailure="depth-stencil-readback";return std::nullopt;}
            return NativeScreenStripReadback{std::move(*output),std::move(*afterDepth)};
        },why);
    if(!complete && gpuFailure && why)*why=gpuFailure;
    return complete;
}
}
