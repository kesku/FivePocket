#pragma once
#include "native_offscreen_draw_plan.h"
#include "gta5_bound_surface_capture.h"
#include "native_translated_draw.h"

namespace recomp::gpu {
// ABI22: a nonindexed rectangle pass that writes TWO colour targets.
//
// Unlike the ABI20 composite, this draw is NOT issued in the post-resolve
// window: its colour registers are live and disagree with the tile cache, so
// the targets come from the REGISTERS, which is where the game aimed them.
// Both must be genuinely bound, with readable headers, and the remaining
// colour slots and the depth slot must be genuinely unbound, because this pass
// writes exactly two attachments and no depth.
//
// The pass writes colour only: RB_COLOR_MASK must enable both targets' channels
// and RB_DEPTHCONTROL must have depth writes and stencil disabled. Extents come
// from owned surface state, never from a shader identity or a draw index.
//
// Every predicate names itself on refusal, so a rejection is diagnosable from
// one log line instead of being guessed at.
struct NativeRectangleMrtPlan {
    std::array<NativeOffscreenDrawPlan,2> targets{};
    // Xenos colour render-target format, from bits 16-19 of the surface
    // header's colour word. The same field the fixed16 plan reads, where 4 is
    // k_16_16. These targets are floating point, so an 8-bit attachment would
    // silently quantise them; the consumer must create the matching image.
    std::array<unsigned,2> formats{};
    bool operator==(const NativeRectangleMrtPlan&) const = default;
};
// Only the formats this pass actually uses are mapped. Anything else is
// refused rather than approximated with a wider or narrower attachment.
inline constexpr bool nativeRectangleMrtFormatSupported(unsigned format) {
    return format==7 || format==14;
}
inline std::optional<NativeRectangleMrtPlan> planNativeRectangleMrtDraw(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5BoundSurfaceCapture& bindings,const char** why=nullptr) {
    const auto no=[&](const char* reason){if(why)*why=reason;return std::optional<NativeRectangleMrtPlan>{};};
    if(!validateNativeRectangleMrtProgram(program) || program.stride!=draw.stride || draw.stride!=24 ||
       draw.primitive!=8 || draw.vertexCount<3 || draw.vertexCount>384 || draw.vertexCount%3 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride ||
       draw.deviceState.size()!=13860)return no("program-profile");
    const auto slots=nativeTranslatedTextureSlots(program);
    if(slots.size()!=draw.textures.size() || slots.empty())return no("texture-slots");
    for(std::size_t i=0;i<slots.size();++i)
        if(slots[i]!=draw.textures[i].slot || slots[i]>=32 || draw.textures[i].literalZero)return no("texture-slots");
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    // Colour writes for BOTH targets, and no depth or stencil write at all.
    const auto colorMask=gta5DrawRegister(draw,0x2104);
    if(!colorMask || *colorMask!=0xFFu)return no("colour-mask");
    const auto depthControl=gta5DrawRegister(draw,0x2200);
    if(!depthControl)return no("depth-control-unreadable");
    if((*depthControl)&1u)return no("stencil-enabled");
    if(((*depthControl)>>2)&1u)return no("depth-writes-enabled");
    if(draw.deviceState[11068]&0x10)return no("binning-flag");
    // Slots 2 and 3 carry no attachment, and slot 4 attaches no depth.
    for(unsigned slot=2;slot<5;++slot)
        if(bindings.surfaces[slot].object || word(draw.deviceState,12816+4*slot))return no("extra-slot-bound");
    const auto scissor=nativeTranslatedScissor(draw);
    const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || !viewport || (*scissor)[0] || (*scissor)[1] || !(*scissor)[2] ||
       !(*scissor)[3])return no("scissor-viewport");
    const auto width=(*scissor)[2],height=(*scissor)[3];
    const auto surfaceInfo=word(draw.deviceState,10368);
    const auto pitch=surfaceInfo&0x3FFFu;
    if(surfaceInfo&0x3C000u)return no("surface-samples"); // Binned/MSAA is not this pass.
    if(!pitch || pitch%80 || pitch<width)return no("surface-pitch");
    NativeRectangleMrtPlan plan{};
    for(unsigned slot=0;slot<2;++slot) {
        const auto& s=bindings.surfaces[slot];
        // The register names the target and must agree with the binding, which
        // must carry a readable header. A stack-backed header is not evidence
        // of an unusable register; the composite now requires its current one too.
        const auto object=word(draw.deviceState,12816+4*slot);
        if(!object || object%4 || object>UINT32_MAX-39u || s.object!=object)return no("target-register");
        if(!s.header || s.header->size()!=40)return no("target-header");
        const auto info=word(*s.header,24),color=word(*s.header,28),dimensions=word(*s.header,36);
        if(info!=surfaceInfo)return no("target-surface-info");
        // Base tile is bits 0-10 and the colour format bits 16-19; nothing else
        // may be set. The composite band required the whole word below 0x800
        // because its target is k_8_8_8_8; these targets are not.
        if(color&~0x000F07FFu)return no("target-colour-info");
        const auto base=color&0x7FFu,format=(color>>16)&0xFu;
        if(!nativeRectangleMrtFormatSupported(format))return no("target-colour-format");
        if((dimensions>>18)+1!=width || ((dimensions>>3)&0x7FFFu)+1!=height)return no("target-dimensions");
        const auto tiles=nativeOffscreenTileCount(pitch,height,format);
        if(!tiles || *tiles>2048-base)return no("edram-wrap");
        plan.targets[slot]=NativeOffscreenDrawPlan{object,width,height,pitch,base,*tiles,format};
        plan.formats[slot]=format;
    }
    // Two attachments that share EDRAM would alias; the pass writes both.
    const auto& a=plan.targets[0];const auto& b=plan.targets[1];
    if(a.object==b.object)return no("targets-identical");
    if(a.baseTile<b.baseTile+b.tileCount && b.baseTile<a.baseTile+a.tileCount)return no("targets-overlap");
    if(why)*why="planned";
    return plan;
}
}
