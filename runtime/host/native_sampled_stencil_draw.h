#pragma once
#include "native_direct_depth_target.h"
#include "native_geometry_sampler.h"
#include "native_edram32_bands.h"

namespace recomp::gpu {
// Completed physical producer -> real original sampled shader -> stencil-only
// physical publication. No implicit clear, guest query or ring retirement.
template<class Current>
inline bool executeNativeSampledStencil(ProbeDevice& gpu,const NativeTranslatedDrawProgram& program,
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bindings,
        const std::vector<std::uint8_t>& vertexIdentity,const std::vector<std::uint8_t>& pixelIdentity,
        NativeEdram32Mutation& mutation,Current&& current,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    if(program.vertexIdentity!=vertexIdentity || program.pixelIdentity!=pixelIdentity)return no("shader-identity");
    const auto plan=planNativeSampledStencil(draw,program,bindings,why);if(!plan)return false;
    if(!current())return no("stale-inputs");
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned v){for(unsigned b=0;b<4;++b)input[at+b]=v>>(24-8*b);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::memcpy(input.data()+8208,draw.vertices.data(),draw.vertices.size());
    auto image=NativeTextureImage::create(gpu,plan->texture,draw.textures[0].source);
    if(!image)return no("texture-import");
    auto sampler=NativeGeometrySampler::create(gpu,*image);if(!sampler)return no("texture-sampler");
    const VkDescriptorImageInfo descriptor{sampler->sampler(),image->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const auto viewport=nativeTranslatedViewport(draw);const auto scissor=nativeTranslatedScissor(draw);
    const char* stage="physical-depth-incomplete";
    const auto& p=plan->target;
    const bool result=executeNativeEdram32Stencil(mutation,p.depthBase,p.pitch,p.width,p.height,
        [&](const auto& before)->std::optional<std::vector<unsigned>> {
            stage="depth-import";auto target=NativeDirectDepthTarget::create(gpu,p,before);if(!target)return std::nullopt;
            stage="sampled-stencil-draw";
            if(!target->drawSampled(program,input,draw.vertexCount,*viewport,*scissor,plan->texture,descriptor))return std::nullopt;
            stage="depth-readback";auto after=target->readWords();if(!after)return std::nullopt;
            stage="stale-inputs";if(!current())return std::nullopt;
            stage="completed";return after;
        });
    if(why)*why=stage;
    return result;
}
}
