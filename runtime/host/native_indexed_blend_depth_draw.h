#pragma once
#include "native_indexed_blend_depth_plan.h"
#include "native_single_coverage7e3_physical.h"
#include "native_vertex_clip_executor.h"
#include "native_linear_sample_target.h"
#include "native_texture_image.h"

namespace recomp::gpu {
struct NativeIndexedBlendDepthProgram {
    NativeTranslatedDrawProgram draw;
    NativeClippedFanProgram positions;
    // Engine-owned vec4(1) fragment helper, not original game colour. Its
    // compiler/identity receipt must be bound alongside the original modules.
    std::vector<std::uint32_t> coverageCode;
};
// An exact state/pair consumer. The coverage pass uses the same original VS,
// geometry, raster and current depth/stencil as colour, and refuses overlap.
// Source-only original PS verifies fixed-point alpha's pre-blend domain. No
// publication happens until coverage, source, and blend passes have fenced.
inline bool executeNativeIndexedBlendDepthDraw(ProbeDevice& device,const NativeIndexedBlendDepthProgram& program,
        const Gta5InlineDraw& draw,const std::vector<std::uint32_t>& indices,
        const Gta5BoundSurfaceCapture& bound,const NativeIndexedBlendDepthTextures& captured,
        const std::vector<std::uint8_t>& actualVertex,const std::vector<std::uint8_t>& actualPixel,
        bool queriesIdle,NativeEdram32Mutation& mutation,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    if(program.positions.stride!=28 || program.positions.vertexIdentity!=program.draw.vertexIdentity ||
       program.positions.pixelIdentity!=program.draw.pixelIdentity || program.coverageCode.size()<5 ||
       program.coverageCode.size()>65536 || program.coverageCode.front()!=0x07230203)
        return no("indexed-blend-depth-position-coverage-program");
    const auto plan=planNativeIndexedBlendDepth(program.draw,draw,indices,bound,captured,
        actualVertex,actualPixel,queriesIdle,why);
    if(!plan)return false;
    auto executor=NativeVertexClipExecutor::create(device,program.positions);
    if(!executor)return no("indexed-blend-depth-position-executor");
    const auto positions=executor->indexedPositions(draw);
    if(!positions || positions->size()!=draw.vertexCount)return no("indexed-blend-depth-position-readback");
    for(const auto& p:*positions) {
        for(auto v:p)if(!std::isfinite(v))return no("indexed-blend-depth-nonfinite-position");
        // Actual viewport maps NDC1 to depth0. A future changed transform
        // does not inherit this capture's depth or single-coverage proof.
        if(p[3]==0 || p[2]!=p[3])return no("indexed-blend-depth-depth-domain");
    }
    std::array<std::unique_ptr<NativeTextureImage>,3> images;
    struct Samplers {VkDevice d;std::array<VkSampler,3> handles{};
        ~Samplers(){for(auto s:handles)if(s)vkDestroySampler(d,s,nullptr);}} samplers{device.device};
    NativeTranslatedPipelineOptions options;options.depthStencil=plan->depthStencil;
    // ABI12 has one attachment. Keep its full-write contract, including the
    // unused mask slots; the exact guest COLOR_MASK=15 is checked by the plan.
    // This exact original VS exports the SAME expression to clip Z and W,
    // verified above against CURRENT fenced positions; the admitted guest
    // viewport maps NDC1 to0 and the PS has no depth export. Canonicalize that
    // constant result in the host viewport. A reverse1..0 viewport produced
    // small positive raster depths on Thor, incorrectly passing GE against
    // positive20e4 subnormals. Do not relax the coverage/depth guard or change
    // clip coordinates: XY/clip geometry and every nonconstant-depth path stay
    // unchanged. This is not a global viewport rule.
    options.minDepth=0;options.maxDepth=0;
    options.cullMode=VK_CULL_MODE_BACK_BIT;options.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;
    for(unsigned i=0;i<3;++i) {
        images[i]=NativeTextureImage::createUnpackedMips(device,plan->textures[i],captured[i].base,captured[i].mips);
        const auto& s=plan->samplers[i];
        if(!images[i] || images[i]->levelCount()!=s.maxLevel+1 || images[i]->layerCount()!=1 ||
           ((s.minFilter || s.magFilter) && !images[i]->supportsLinearFiltering()))return no("indexed-blend-depth-texture-import");
        VkSamplerCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.minFilter=s.minFilter?VK_FILTER_LINEAR:VK_FILTER_NEAREST;ci.magFilter=s.magFilter?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        ci.mipmapMode=s.mipFilter?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
        constexpr VkSamplerAddressMode clamps[]{VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        if(s.clamp[0]>2 || s.clamp[1]>2)return no("indexed-blend-depth-sampler-clamp");
        ci.addressModeU=clamps[s.clamp[0]];ci.addressModeV=clamps[s.clamp[1]];ci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod=float(s.maxLevel);
        if(vkCreateSampler(device.device,&ci,nullptr,&samplers.handles[i])!=VK_SUCCESS)return no("indexed-blend-depth-sampler-create");
        options.externalTextures.push_back({samplers.handles[i],images[i]->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned value){for(unsigned c=0;c<4;++c)input[at+c]=value>>(24-8*c);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    const char* failure=nullptr;
    const bool complete=executeNativeSingleCoverage7e3Physical(mutation,[&](const auto& rgba,const auto& depth)
            ->std::optional<NativeSingleCoverage7e3Readback> {
        auto target=NativeLinearSampleTarget::create(device,1280,720,1);
        if(!target){failure="indexed-blend-depth-target-create";return std::nullopt;}
        const std::vector<XenosTextureDescription> descriptions(plan->textures.begin(),plan->textures.end());
        auto counter=program.draw;counter.pixelCode=program.coverageCode;counter.pixelIdentity.clear();
        if(!target->loadSingleSampleContents(std::vector<float>(rgba.size(),0),depth)) {
            failure="indexed-blend-depth-coverage-owner-import";return std::nullopt;
        }
        options.guestBlendControl=0x01010101; // ONE+ONE in RGB and alpha.
        auto coverage=target->draw(counter,input,draw.vertexCount,plan->viewport,descriptions,options,
            NativeTranslatedBlend::GuestRegister,{0,0,0,0},0,0,true,indices,true,std::array<unsigned,4>{0,0,1280,720});
        if(!coverage){failure=target->lastDrawFailure();return std::nullopt;}
        const auto counterDepth=target->readSingleSampleDepthWords();
        if(!counterDepth || *counterDepth!=depth){failure="indexed-blend-depth-coverage-depth-changed";return std::nullopt;}
        if(!validateNativeSingleCoverage7e3(*coverage,depth,&failure))return std::nullopt;
        if(!target->loadSingleSampleContents(std::vector<float>(rgba.size(),0),depth)) {
            failure="indexed-blend-depth-source-owner-import";return std::nullopt;
        }
        options.guestBlendControl.reset();
        const auto source=target->draw(program.draw,input,draw.vertexCount,plan->viewport,descriptions,options,
            NativeTranslatedBlend::Replace,{0,0,0,0},0,0,true,indices,true,std::array<unsigned,4>{0,0,1280,720});
        if(!source){failure=target->lastDrawFailure();return std::nullopt;}
        const auto sourceDepth=target->readSingleSampleDepthWords();
        if(!sourceDepth || *sourceDepth!=depth){failure="indexed-blend-depth-source-depth-changed";return std::nullopt;}
        if(!validateNativeSingleCoverage7e3Source(*coverage,*source,&failure))return std::nullopt;
        if(!target->loadSingleSampleContents(rgba,depth)){failure="indexed-blend-depth-color-owner-import";return std::nullopt;}
        options.guestBlendControl=0x07060706; // Original independent RGB/alpha.
        auto colors=target->draw(program.draw,input,draw.vertexCount,plan->viewport,descriptions,options,
            NativeTranslatedBlend::GuestRegister,{0,0,0,0},0,0,true,indices,true,std::array<unsigned,4>{0,0,1280,720});
        if(!colors){failure=target->lastDrawFailure();return std::nullopt;}
        auto depths=target->readSingleSampleDepthWords();
        if(!depths){failure="indexed-blend-depth-readback";return std::nullopt;}
        return NativeSingleCoverage7e3Readback{std::move(*coverage),std::move(*colors),std::move(*depths)};
    },why);
    if(!complete && failure && why)*why=failure;
    return complete;
}
}
