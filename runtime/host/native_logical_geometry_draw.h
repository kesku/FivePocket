#pragma once
#include "native_logical_geometry_state.h"
#include "native_logical_tile_target.h"
#include "native_geometry_sampler.h"

namespace recomp::gpu {
// Runtime-capable scene producer, shared by the connected capture replay.
// One explicitly selected bin, never CPU-record-order -> GPU-order inference.
// Caller serializes the queue and proves program/resource/recording ownership.
// The target fences its actual writes. This API does NOT retire guest workers,
// publish resolved pixels or authorize a ring receipt/presentation by itself.
inline bool executeNativeLogicalGeometryDraw(ProbeDevice& device,NativeLogicalTileTarget& target,
        const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
        const std::vector<std::uint32_t>& indices,const Gta5ClearCapture& surfaces,
        unsigned tile,const std::array<unsigned,4>& recordedScissor,
        const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures,bool* visibleSamples=nullptr,
        std::uint32_t* fragmentDiagnostics=nullptr) {
    const auto input=prepareNativeLogicalGeometry(program,draw,indices,surfaces,recordedScissor,textures);
    if(!input || !device.vertexStoresAndAtomicsEnabled || !device.fragmentStoresAndAtomicsEnabled ||
       !device.independentBlendEnabled || (input->anisotropy && !device.samplerAnisotropyEnabled))return false;
    if((input->state.visibilityControl&1) && !visibleSamples)return false;
    const auto& state=input->state;
    NativeTranslatedPipelineOptions options;options.samples=1;options.colorAttachmentCount=4;
    options.depthStencil=state.depth;options.minDepth=0;options.maxDepth=1;
    options.colorWriteMasks=state.colorWriteMasks;options.cullMode=VK_CULL_MODE_BACK_BIT;
    options.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;
    struct SampledResources {
        std::vector<std::unique_ptr<NativeTextureImage>> images;
        std::vector<std::unique_ptr<NativeGeometrySampler>> samplers;
    };
    auto resources=std::make_shared<SampledResources>();
    auto& images=resources->images;auto& samplers=resources->samplers;
    for(unsigned i=0;i<textures.size();++i) {
        const auto& t=textures[input->textureSourceIndices[i]].second;
        images.push_back(NativeTextureImage::createUnpackedMips(device,input->descriptions[i],t.base,t.mips));
        if(!images.back())return false;
        samplers.push_back(NativeGeometrySampler::create(device,*images.back()));if(!samplers.back())return false;
        options.externalTextures.push_back({samplers.back()->sampler(),images.back()->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    // On timeout the target retains BOTH its pipeline and these external
    // images/samplers until queue retirement. Sampler/image destructors do not
    // wait by themselves; returning false must not free an in-flight binding.
    return target.drawCapturedIndexed(surfaces,tile,program,input->payload,draw.vertexCount,indices,
        state.viewport,state.scissor,input->descriptions,options,std::move(resources),visibleSamples,fragmentDiagnostics);
}
}
