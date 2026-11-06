#pragma once
#include "native_color8_clear_target.h"
#include "native_geometry_draw_state.h"
#include "native_geometry_sampler.h"

namespace recomp::gpu {
// Connected resource -> original translated program -> persistent output.
// Caller holds the guest/queue gate, verifies exact shader identities, and
// supplies CURRENT owned surface headers and submitted texture planes. This
// function never manufactures those headers or grants guest ring completion.
inline bool executeNativeGeometryDraw(ProbeDevice& device,NativeColor8ClearTarget& target,
        const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
        const std::vector<std::uint32_t>& indices,const Gta5BoundSurfaceCapture& bindings,
        const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures) {
    const auto state=planNativeGeometryDrawState(draw,program);
    const auto slots=nativeTranslatedTextureSlots(program);
    if(!state || !nativeGammaZeroDepthTargetBindings(draw,target.plan(),bindings) ||
       !draw.vertexCount || draw.vertexCount>65536 || draw.vertices.size()>256*1024 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*program.stride ||
       slots.size()>4 || textures.size()!=slots.size())return false;
    NativeTranslatedPipelineOptions options;options.samples=state->samples;options.minDepth=state->minDepth;options.maxDepth=state->maxDepth;
    options.depthStencil=state->depth;options.sampleShading=true;options.cullMode=state->cull;
    options.frontFace=state->clockwise?VK_FRONT_FACE_CLOCKWISE:VK_FRONT_FACE_COUNTER_CLOCKWISE;
    std::vector<std::unique_ptr<NativeTextureImage>> images;
    std::vector<std::unique_ptr<NativeGeometrySampler>> samplers;
    std::vector<XenosTextureDescription> descriptions;unsigned seen=0;
    for(unsigned i=0;i<slots.size();++i) {
        const auto slot=slots[i];const auto& texture=textures[i].second;
        if(slot>=32 || textures[i].first!=slot || (seen&(1u<<slot)))return false;
        seen|=1u<<slot;
        const auto desc=XenosTextureDescription::decode(texture.fetch);
        if(!desc || !planNativeGeometrySampler2D(texture.fetch))return false;
        images.push_back(NativeTextureImage::createUnpackedMips(device,*desc,texture.base,texture.mips));
        if(!images.back())return false;
        samplers.push_back(NativeGeometrySampler::create(device,*images.back()));if(!samplers.back())return false;
        descriptions.push_back(*desc);
        options.externalTextures.push_back({samplers.back()->sampler(),images.back()->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,std::uint32_t v){for(unsigned c=0;c<4;++c)input[at+c]=v>>(24-8*c);};
    put(0,draw.vertexCount);put(4,static_cast<unsigned>(slots.size()));
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    return target.drawIndexedColor8(program,input,draw.vertexCount,indices,state->viewport,state->scissor,descriptions,options);
}
}
