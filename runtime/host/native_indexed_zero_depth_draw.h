#pragma once
#include "native_indexed_zero_depth_plan.h"
#include "native_indexed_zero_depth_physical.h"
#include "native_vertex_clip_executor.h"
#include "native_linear_sample_target.h"
#include "native_texture_image.h"

namespace recomp::gpu {
struct NativeIndexedZeroDepthProgram {
    NativeTranslatedDrawProgram draw;
    NativeClippedFanProgram positions;
};
// Exact consumer; live admission also requires exclusive engine/resource owners.
// Native clipping handles the captured positive/negative W triangles. Original
// geometry and PS run; a failed exclusion is never converted into completion.
inline bool executeNativeIndexedZeroDepthDraw(ProbeDevice& device,const NativeIndexedZeroDepthProgram& program,
        const Gta5InlineDraw& draw,const std::vector<std::uint32_t>& indices,
        const Gta5BoundSurfaceCapture& bound,const NativeIndexedZeroDepthTextures& captured,
        const std::vector<std::uint8_t>& actualVertex,const std::vector<std::uint8_t>& actualPixel,
        bool queriesIdle,NativeEdram32Mutation& mutation,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    if(program.positions.stride!=20 || program.positions.vertexIdentity!=program.draw.vertexIdentity ||
       program.positions.pixelIdentity!=program.draw.pixelIdentity)return no("indexed-zero-depth-position-program");
    const auto plan=planNativeIndexedZeroDepth(program.draw,draw,indices,bound,captured,
        actualVertex,actualPixel,queriesIdle,why);
    if(!plan)return false;
    auto executor=NativeVertexClipExecutor::create(device,program.positions);
    if(!executor)return no("indexed-zero-depth-position-executor");
    const auto positions=executor->indexedPositions(draw);
    if(!positions || positions->size()!=draw.vertexCount)return no("indexed-zero-depth-position-readback");
    for(const auto& p:*positions) {
        for(auto v:p)if(!std::isfinite(v))return no("indexed-zero-depth-nonfinite-position");
        if(p[3]==0)return no("indexed-zero-depth-zero-w-unqualified");
    }
    std::array<std::unique_ptr<NativeTextureImage>,4> images;
    struct Samplers {VkDevice d;std::array<VkSampler,4> handles{};
        ~Samplers(){for(auto s:handles)if(s)vkDestroySampler(d,s,nullptr);}} samplers{device.device};
    NativeTranslatedPipelineOptions options;options.depthStencil=plan->depthStencil;
    options.minDepth=options.maxDepth=0; // Actual original viewport, not guessed depth.
    for(unsigned i=0;i<4;++i) {
        images[i]=NativeTextureImage::createUnpackedMips(device,plan->textures[i],captured[i].base,captured[i].mips);
        const auto& s=plan->samplers[i];
        if(!images[i] || images[i]->levelCount()!=s.maxLevel+1 || images[i]->layerCount()!=1 ||
           ((s.minFilter || s.magFilter) && !images[i]->supportsLinearFiltering()))return no("indexed-zero-depth-texture-import");
        VkSamplerCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.minFilter=s.minFilter?VK_FILTER_LINEAR:VK_FILTER_NEAREST;ci.magFilter=s.magFilter?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        ci.mipmapMode=s.mipFilter?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
        constexpr VkSamplerAddressMode clamps[]{VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        if(s.clamp[0]>2 || s.clamp[1]>2)return no("indexed-zero-depth-sampler-clamp");
        ci.addressModeU=clamps[s.clamp[0]];ci.addressModeV=clamps[s.clamp[1]];ci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod=float(s.maxLevel);
        if(vkCreateSampler(device.device,&ci,nullptr,&samplers.handles[i])!=VK_SUCCESS)return no("indexed-zero-depth-sampler-create");
        options.externalTextures.push_back({samplers.handles[i],images[i]->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned value){for(unsigned c=0;c<4;++c)input[at+c]=value>>(24-8*c);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    const char* failure=nullptr;
    const bool complete=executeNativeIndexedZeroDepthPhysical(mutation,[&](const auto& rgba,const auto& depth)
            ->std::optional<NativeIndexedZeroDepthReadback> {
        auto target=NativeLinearSampleTarget::create(device,1280,720,1);
        if(!target){failure="indexed-zero-depth-target-create";return std::nullopt;}
        if(!target->loadSingleSampleContents(rgba,depth)){failure="indexed-zero-depth-owner-import";return std::nullopt;}
        const std::vector<XenosTextureDescription> descriptions(plan->textures.begin(),plan->textures.end());
        const auto colors=target->draw(program.draw,input,draw.vertexCount,plan->viewport,descriptions,options,
            NativeTranslatedBlend::Replace,{0,0,0,0},0,0,true,indices,true,std::array<unsigned,4>{0,0,1280,720});
        if(!colors){failure=target->lastDrawFailure();return std::nullopt;}
        const auto depths=target->readSingleSampleDepthWords();
        if(!depths){failure="indexed-zero-depth-readback";return std::nullopt;}
        return NativeIndexedZeroDepthReadback{*colors,*depths};
    },why);
    if(!complete && failure && why)*why=failure;
    return complete;
}
}
