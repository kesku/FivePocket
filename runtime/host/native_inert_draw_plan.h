#pragma once
#include "native_translated_draw.h"

namespace recomp::gpu {
// A rectangle pass that provably writes NOTHING.
//
// The game issues this draw in the same post-resolve window the ABI20 composite
// draw occupies, and it is not a draw whose output we are unable to reproduce:
// it is a draw that has no output at all. The guest bound no pixel shader, then
// disabled every path by which a draw could write:
//
//   RB_COLOR_MASK       == 0  no colour channel is written
//   RB_DEPTHCONTROL.2   == 0  depth writes are disabled
//   RB_DEPTHCONTROL.0   == 0  stencil is disabled
//   no pixel shader           no fragment can export colour, depth or memory
//   VS allocations 1 and 2    position and parameters only; NO memory export
//
// The last of those is established offline, by the analysis that produced the
// ABI21 program: a vertex program that allocated a memory export (kind 3) is
// never emitted as depth-only, so an identity match here carries that proof.
//
// Rasterising this draw would therefore write nothing on this device exactly as
// it wrote nothing on the original, so completing it without executing it is an
// equivalence rather than a shortcut, and it cannot disturb any bound target.
// This admits ONLY that: it never reports a target, never claims presentation,
// and never stands in for a draw that does have output.
//
// The draw's VERTEX BYTES are deliberately not part of this proof, and are not
// required to be readable. The device has never made them readable for this
// draw: every observed run reports `RENDER_STOPPED_INPUT ... captured=0` for
// its buffer. That changes nothing, because geometry cannot make a draw with no
// colour, depth, stencil or memory write produce output. This plan therefore
// reads the device state and the draw's shape, and never the vertices.
//
// Every predicate names itself on refusal so a rejection is diagnosable without
// guessing which one fired.
inline bool planNativeInertRectangleDraw(const Gta5InlineDraw& draw,
        const NativeTranslatedDrawProgram& program,const char** why=nullptr) {
    const auto no=[&](const char* reason){if(why)*why=reason;return false;};
    if(!validateNativeDepthOnlyRectangleProgram(program))return no("program-contract");
    // Rectangle lists expand three original vertices into two triangles; the
    // count must be a whole number of rectangles and the stride must be the
    // one the analysed program was compiled for.
    if(draw.primitive!=8 || !draw.vertexCount || draw.vertexCount%3 ||
       draw.stride!=program.stride)return no("rectangle-shape");
    if(!draw.textures.empty())return no("unexpected-textures");
    if(draw.deviceState.size()!=13860)return no("device-state-extent");
    const auto colorMask=gta5DrawRegister(draw,0x2104);
    if(!colorMask)return no("colour-mask-unreadable");
    if(*colorMask)return no("colour-writes-enabled");
    const auto depthControl=gta5DrawRegister(draw,0x2200);
    if(!depthControl)return no("depth-control-unreadable");
    if((*depthControl)&1u)return no("stencil-enabled");
    if(((*depthControl)>>2)&1u)return no("depth-writes-enabled");
    // A draw that writes nothing still has to be a coherent draw; a malformed
    // viewport or scissor means the captured state is not the one analysed.
    if(!nativeTranslatedViewport(draw))return no("viewport");
    if(!nativeTranslatedScissor(draw))return no("scissor");
    if(why)*why="inert";
    return true;
}
}
