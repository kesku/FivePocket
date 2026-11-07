#pragma once
#include "native_fixed16_clear_target.h"
#include "native_fixed16_draw_state.h"

namespace recomp::gpu {
// Same marshalling/admission for private connection tests and the live caller.
// The caller owns the gate, exact effective shader identities and CURRENT guest
// headers/constants/indices. This does not confer guest ownership or retire RPTR.
// A correctly scissored-out draw is valid; no nonzero-fragment requirement exists.
inline bool executeNativeFixed16GeometryDraw(NativeFixed16ClearTarget& target,
        const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
        const std::vector<std::uint32_t>& indices,const Gta5BoundSurfaceCapture& bindings,
        const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures={}) {
    const auto state=planNativeFixed16DrawState(draw,program,target.plan(),bindings);
    const auto slots=nativeTranslatedTextureSlots(program);
    if(!target.valid() || !state || indices.empty() || indices.size()>65535 || indices.size()%3 ||
       textures.size()!=slots.size() ||
       std::any_of(indices.begin(),indices.end(),[&](auto i){return i>=draw.vertexCount;}))return false;
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,std::uint32_t v){for(unsigned c=0;c<4;++c)input[at+c]=v>>(24-8*c);};
    put(0,draw.vertexCount);put(4,static_cast<unsigned>(slots.size()));
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    NativeTranslatedPipelineOptions options;options.samples=1;
    options.minDepth=state->minDepth;options.maxDepth=state->maxDepth;options.depthStencil=state->depth;
    options.cullMode=state->cull;options.frontFace=state->clockwise?VK_FRONT_FACE_CLOCKWISE:VK_FRONT_FACE_COUNTER_CLOCKWISE;
    return target.drawIndexedFixed16(program,input,draw.vertexCount,indices,state->viewport,state->scissor,options,textures);
}
inline bool executeNativeFixed16RectangleDraw(NativeFixed16ClearTarget& target,
        const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
        const Gta5BoundSurfaceCapture& bindings,
        const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures) {
    const auto state=planNativeFixed16RectangleState(draw,program,target.plan(),bindings);
    const auto slots=nativeTranslatedTextureSlots(program);
    if(!target.valid() || !state || textures.size()!=slots.size())return false;
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,std::uint32_t v){for(unsigned c=0;c<4;++c)input[at+c]=v>>(24-8*c);};
    put(0,draw.vertexCount);put(4,static_cast<unsigned>(slots.size()));
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    NativeTranslatedPipelineOptions options;options.samples=1;
    options.minDepth=state->minDepth;options.maxDepth=state->maxDepth;options.depthStencil=state->depth;
    options.cullMode=state->cull;options.frontFace=state->clockwise?VK_FRONT_FACE_CLOCKWISE:VK_FRONT_FACE_COUNTER_CLOCKWISE;
    return target.drawRectangleFixed16(program,input,draw.vertexCount,state->viewport,state->scissor,options,textures);
}
}
