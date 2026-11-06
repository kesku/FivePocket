#pragma once
#include "native_offscreen_draw_plan.h"
#include "gta5_bound_surface_capture.h"
#include "native_translated_draw.h"

namespace recomp::gpu {
// ABI23: a nonindexed rectangle pass that writes ONE colour target and samples
// ONE slot four times at a constant texel offset.
//
// The captured pass is a half-resolution downsample: a 320x180 target against a
// 640-wide source, RB_COLOR_MASK 0xF, no depth or stencil write. The target
// comes from the REGISTERS, as the ABI22 band's do, not from the tile cache.
//
// Unlike every earlier band, the bound texture set is NOT the input set. The
// game leaves nine textures bound across this part of the frame while the pair
// samples exactly one of them, so the declared slot is SELECTED out of the
// bound set. Requiring the counts to agree, as the MRT plan does, would refuse
// this pass for textures it never reads.
//
// Every predicate names itself on refusal, so a rejection is diagnosable from
// one log line instead of being guessed at.
inline std::optional<NativeOffscreenDrawPlan> planNativeOffsetSampleDraw(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5BoundSurfaceCapture& bindings,const char** why=nullptr) {
    const auto no=[&](const char* reason){if(why)*why=reason;return std::optional<NativeOffscreenDrawPlan>{};};
    if(!validateNativeOffsetSampleProgram(program) || program.stride!=draw.stride || draw.stride!=24 ||
       draw.primitive!=8 || draw.vertexCount<3 || draw.vertexCount>384 || draw.vertexCount%3 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride ||
       draw.deviceState.size()!=13860)return no("program-profile");
    // The one declared slot must be among the bound textures, and it must be a
    // real sampled texture rather than the literal-zero placeholder.
    const auto slots=nativeTranslatedTextureSlots(program);
    if(slots.size()!=1)return no("texture-slots");
    const Gta5InlineDraw::Texture* source=nullptr;
    for(const auto& texture:draw.textures)
        if(texture.slot==slots[0]) {
            if(source)return no("texture-slots"); // The same slot bound twice is ambiguous.
            source=&texture;
        }
    if(!source || source->literalZero)return no("texture-slots");
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    // One colour target, every channel, and no depth or stencil write at all.
    const auto colorMask=gta5DrawRegister(draw,0x2104);
    if(!colorMask || *colorMask!=0x0Fu)return no("colour-mask");
    const auto depthControl=gta5DrawRegister(draw,0x2200);
    if(!depthControl)return no("depth-control-unreadable");
    if((*depthControl)&1u)return no("stencil-enabled");
    if(((*depthControl)>>2)&1u)return no("depth-writes-enabled");
    if(draw.deviceState[11068]&0x10)return no("binning-flag");
    // Colour slots 1-3 carry no attachment, and slot 4 attaches no depth.
    for(unsigned slot=1;slot<5;++slot)
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
    const auto& s=bindings.surfaces[0];
    const auto object=word(draw.deviceState,12816);
    if(!object || object%4 || object>UINT32_MAX-39u || s.object!=object)return no("target-register");
    if(!s.header || s.header->size()!=40)return no("target-header");
    const auto info=word(*s.header,24),color=word(*s.header,28),dimensions=word(*s.header,36);
    if(info!=surfaceInfo)return no("target-surface-info");
    // Base tile is bits 0-10 and the colour format bits 16-19; nothing else may
    // be set. The device recorded target_colour=00070388 for this pass: format
    // 7, k_16_16_16_16_FLOAT, at base tile 904. That is the same element type as
    // its RGBA16F source, which is what a half-resolution downsample of a float
    // buffer must write. Format 0 stays admitted for the ordinary colour case;
    // anything else needs an attachment this band does not create.
    if(color&~0x000F07FFu)return no("target-colour-info");
    const auto base=color&0x7FFu,format=(color>>16)&0xFu;
    // The device recorded target_format 7 for the pyramid's upper levels and 14
    // for the last one: k_32_FLOAT, a single float32 channel, which is what a
    // luminance reduction ends in. Format 0 stays admitted for the ordinary
    // colour case.
    //
    // ABI24 writes a different format entirely -- 4, k_16_16, two channels of
    // fixed point over [-32,32] -- and its compiled pixel stage carries the
    // encode scale for exactly that target. Admitting format 7 or 14 for it
    // would store scaled values into a float attachment, so the two pairs
    // admit disjoint sets and a mismatch names itself here.
    const bool tonemap=program.abiVersion==24;
    if(tonemap?format!=4:(format!=0 && format!=7 && format!=14))
        return no("target-colour-format");
    if((dimensions>>18)+1!=width || ((dimensions>>3)&0x7FFFu)+1!=height)return no("target-dimensions");
    const auto tiles=nativeOffscreenTileCount(pitch,height,format);
    if(!tiles || *tiles>2048-base)return no("edram-wrap");
    // A target that aliases its own tile cache entry would read and write the
    // same storage through two owners.
    if(s.tileCacheObject==object)return no("target-aliases-tile-cache");
    if(why)*why="planned";
    return NativeOffscreenDrawPlan{object,width,height,pitch,base,*tiles,format};
}
}
