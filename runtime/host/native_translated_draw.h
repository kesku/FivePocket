#pragma once
#include "gta5_draw_state.h"
#include "native_color_viewport.h"
#include "native_affine_position.h"
#include <cmath>
#include <cstring>

namespace recomp::gpu {
struct NativeTranslatedDrawProgram {
    std::vector<std::uint8_t> vertexIdentity,pixelIdentity;
    std::vector<std::uint32_t> vertexCode,pixelCode;
    unsigned stride=0,textureSlot=0;
    bool indexed=false;
    unsigned interpolators=0; // 0 retains the original solid/textured layout.
    std::vector<unsigned> textureSlots{}; // Binding order; empty retains the legacy single slot.
    unsigned abiVersion=4;
    bool directTriangles=false;
    std::optional<NativeAffinePositionXY> affinePositionXY{};
    bool color8SourceClamped=false; // Explicit ABI7 loading output-merger source clamp.
    bool cubeRectanglePwl=false; // Explicit ABI8, separate state/geometry/ownership consumer.
    bool explicitLodRectangle=false; // Explicit ABI9, preserved-target mip generator.
    unsigned geometryColor8Format=UINT32_MAX; // ABI13: 0=linear UNORM, 1=PWL gamma.
    bool geometryDepth20e4=false; // ABI13 TRUNC per sample BEFORE depth testing.
    bool geometrySampleDepth=false; // Explicit NDC sample varying + range push + guarded VS.
    unsigned geometryCentroidMask=0; // Explicit SQ sample-location state, not texture sampling.
    // ABI14 only: single-sample MRT. UINT32_MAX means no bound attachment;
    // output bits describe complete four-lane shader exports, not clear/write coverage.
    std::array<unsigned,4> geometryMrtFormats{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    unsigned geometryMrtOutputs=0;
    // ABI15: explicit predicated pixel/discard and optional unscaled PsParamGen
    // XY. This is not admitted by the normal game's shader-pack loader.
    bool geometryPixelEffects=false;
    unsigned geometryParameterRegister=UINT32_MAX;
    std::array<unsigned,4> geometryMrtBlendControls{0x10001,0x10001,0x10001,0x10001};
    // ABI16: owned pre-draw depth snapshot + proven depth/color implication.
    // No standalone shader flag can establish that implication.
    bool geometryDepthColorWitness=false;
    // ABI17: resource-free coarse gradients, explicit PS depth and one-sample
    // signed fixed16 RG output. ABI18 adds owned textures + predicated discard
    // through geometryPixelEffects. Neither is a legacy color8/MRT program.
    bool geometryFixed16GradientDepth=false;
    // ABI19: nonindexed rectangle expansion, sampled color and explicit depth.
    // No gradients, predicate/discard, indexed input, or implicit owner clear.
    bool fixed16RectangleDepth=false;
    // ABI20: nonindexed rectangle expansion with one sampled color texture and
    // one interpolator. No depth, no PWL, no gradients, no predicate/discard.
    // Its denormalized point-filtered fetch is resolved inside the compiled
    // pixel shader, so no new sampler state is implied. The current owned
    // header and registers identify its target; a tile cache is NOT authority.
    bool compositeRectangle=false;
    // ABI21: a depth/stencil-only rectangle pass. The guest bound NO pixel
    // shader and masked every colour channel, so this program carries an empty
    // pixelIdentity and pixelCode, and no compiled module at all: the draw
    // writes nothing, so it is admitted and completed WITHOUT execution rather
    // than rasterised. See native_inert_draw_plan.h for the proof it requires.
    bool depthOnlyRectangle=false;
    // ABI22: a nonindexed rectangle pass writing TWO colour attachments, with
    // constant texel offsets resolved inside the compiled pixel shader. It
    // carries none of the indexed geometry MRT machinery: no color8 store, no
    // depth20e4, no sample depth. Its targets come from the live registers.
    bool rectangleMrt=false;
    // ABI23: the half-resolution offset-sample downsample. Same nonindexed
    // rectangle list and constant texel offsets as the MRT band, but it writes
    // ONE colour target and samples ONE slot, four times.
    bool offsetSample=false;
    // Explicit captured a0-indexed programs use six GPRs for ABI17 and
    // seven for ABI18 (addressed geometry plus textured pixel effects).
    // Zero retains the earlier two-GPR ABI17 / three-GPR ABI18 contracts.
    unsigned fixed16VertexRegisterCount=0;
    // ABI26: position-only composite rectangle; SQ generates unscaled pixel
    // XY in r0. No interpolators, generated ZW, depth, MSAA or pixel-index gen.
    bool screenSpaceRectangle=false;
    // ABI28: nonindexed strip expanded to triangles, one point depth texture,
    // two smooth interpolators and explicit single-sample20e4 fragment depth.
    // State/target ownership and 7e3 blending remain a separate consumer duty.
    bool screenStrip=false;
    // ABI29: four-vertex perspective strip, four sampled textures, pixel kill,
    // replace7e3 colour and explicit20e4 depth. Separate strict consumer only.
    bool worldStrip=false;
};
inline std::vector<unsigned> nativeTranslatedTextureSlots(const NativeTranslatedDrawProgram& program) {
    if(!program.textureSlots.empty())return program.textureSlots;
    return program.textureSlot==UINT32_MAX?std::vector<unsigned>{}:std::vector<unsigned>{program.textureSlot};
}
// ABI35: ordinary7e3 rectangle, unsigned packed10 source and explicit
// base-level POINT/GET_WEIGHTS arithmetic. Metadata alone cannot admit a draw.
inline bool validateNativeFloatRectangleInterface(const NativeTranslatedDrawProgram& p,
        unsigned abi,unsigned vertexBytes,unsigned pixelBytes,unsigned slot=5,unsigned interpolators=1) {
    return p.abiVersion==abi && p.stride==24 && p.interpolators==interpolators && !p.indexed &&
        p.textureSlot==slot && nativeTranslatedTextureSlots(p)==std::vector<unsigned>{slot} &&
        p.vertexIdentity.size()==vertexBytes && p.pixelIdentity.size()==pixelBytes &&
        p.vertexCode.size()>=5 && p.pixelCode.size()>=5 &&
        p.vertexCode.front()==0x07230203 && p.pixelCode.front()==0x07230203 &&
        !p.screenStrip && !p.worldStrip && !p.directTriangles && !p.affinePositionXY &&
        !p.color8SourceClamped && !p.cubeRectanglePwl && !p.explicitLodRectangle &&
        !p.depthOnlyRectangle && !p.rectangleMrt && !p.offsetSample && !p.compositeRectangle &&
        !p.screenSpaceRectangle && !p.fixed16RectangleDepth && !p.fixed16VertexRegisterCount &&
        !p.geometryFixed16GradientDepth && !p.geometryDepth20e4 && !p.geometrySampleDepth &&
        p.geometryColor8Format==UINT32_MAX && !p.geometryPixelEffects && !p.geometryDepthColorWitness &&
        !p.geometryCentroidMask && !p.geometryMrtOutputs && p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001};
}
inline bool validateNativeFilterWeightsProgram(const NativeTranslatedDrawProgram& p) {
    return validateNativeFloatRectangleInterface(p,35,132,708);
}
// ABI36: separate binary16 LINEAR/max rectangle interface. This validates only
// the resource interface; original shader identity, state, ownership and target
// completion require an independent consumer. No game route is admitted here.
inline bool validateNativeExpanded16MaxProgram(const NativeTranslatedDrawProgram& p) {
    return validateNativeFloatRectangleInterface(p,36,96,120);
}
// ABI37: separate eight-tap POINT/scalar rectangle interface, sampled slot0.
// Shader semantics and the R32 physical output require the dedicated consumer.
inline bool validateNativeExpanded16LumaProgram(const NativeTranslatedDrawProgram& p) {
    return validateNativeFloatRectangleInterface(p,37,96,324,0);
}
// ABI38: separate sixteen-tap numeric R32 POINT/scalar interface. No runtime
// route is implied by this resource contract; state and ownership are separate.
inline bool validateNativeR32ScalarProgram(const NativeTranslatedDrawProgram& p) {
    return validateNativeFloatRectangleInterface(p,38,96,444,0);
}
// ABI39: position-only rectangle, one numeric R32 sample, RGBA exposure result.
// Dead vertex reads are eliminated ONLY by the original shader graph proof.
// This interface alone grants neither runtime admission nor target ownership.
inline bool validateNativeExposureScalarProgram(const NativeTranslatedDrawProgram& p) {
    return validateNativeFloatRectangleInterface(p,39,132,240,0,0);
}
// ABI40: half-resolution bright-pass rectangle with three EXPAND bindings in
// submitted slot order 1,10,17; slot17 is the vertex texel, evaluated by the
// fragment stage from the same fetch. Interface only: state, ownership and
// target completion require the dedicated consumer.
// ABI41: PostFX pass71 copy rectangle, one EXPAND LINEAR slot5 sample exported
// unchanged. Interface only; state/ownership need the dedicated consumer.
inline bool validateNativeExpanded16CopyProgram(const NativeTranslatedDrawProgram& p) {
    return validateNativeFloatRectangleInterface(p,41,96,60,5);
}
// ABI42: PostFX pass81 horizontal blur rectangle, same shared post VS, five
// EXPAND LINEAR slot5 taps weighted by runtime pixel constants. Interface
// only; state/ownership need the dedicated consumer (native_postfx_blur_plan.h).
inline bool validateNativePostfxBlurProgram(const NativeTranslatedDrawProgram& p) {
    return validateNativeFloatRectangleInterface(p,42,96,192,5);
}
// ABI43: PostFX pass82, the exact vertical mirror of ABI42 (taps down one
// column at uv.y+c189.y*k); same interface, state and consumer rows.
inline bool validateNativePostfxVerticalBlurProgram(const NativeTranslatedDrawProgram& p) {
    return validateNativeFloatRectangleInterface(p,43,96,192,5);
}
inline bool validateNativePostfxBlurFamilyProgram(const NativeTranslatedDrawProgram& p) {
    return validateNativePostfxBlurProgram(p) || validateNativePostfxVerticalBlurProgram(p);
}
// ABI44: PostFX pass83 additive upsample rectangle, same shared post VS, ONE
// EXPAND LINEAR slot5 sample exported as (rgb,0). Same 96/60-byte sizes as
// ABI41's copy: only the ABI label and the exact bound identities separate
// them. Interface only; the ONE/ONE add onto the destination's physical words,
// state and ownership need the dedicated consumer (native_postfx_upsample_plan.h).
inline bool validateNativePostfxUpsampleProgram(const NativeTranslatedDrawProgram& p) {
    return validateNativeFloatRectangleInterface(p,44,96,60,5);
}
inline bool validateNativeBrightPassProgram(const NativeTranslatedDrawProgram& p) {
    if(nativeTranslatedTextureSlots(p)!=std::vector<unsigned>{1,10,17})return false;
    auto single=p;single.textureSlots={1};
    return validateNativeFloatRectangleInterface(single,40,132,204,1);
}
inline bool validateNativeScreenStripProgram(const NativeTranslatedDrawProgram& p) {
    return p.abiVersion==28 && p.screenStrip && !p.worldStrip && p.stride==8 && p.interpolators==2 && !p.indexed &&
        p.textureSlot==3 && nativeTranslatedTextureSlots(p)==std::vector<unsigned>{3} &&
        p.vertexIdentity.size()==300 && p.pixelIdentity.size()==492 &&
        p.vertexCode.size()>=5 && p.pixelCode.size()>=5 &&
        p.vertexCode.front()==0x07230203 && p.pixelCode.front()==0x07230203 &&
        !p.directTriangles && !p.affinePositionXY && !p.color8SourceClamped && !p.cubeRectanglePwl &&
        !p.explicitLodRectangle && !p.depthOnlyRectangle && !p.rectangleMrt && !p.offsetSample &&
        !p.compositeRectangle && !p.screenSpaceRectangle && !p.fixed16RectangleDepth &&
        !p.fixed16VertexRegisterCount && !p.geometryFixed16GradientDepth &&
        !p.geometryDepth20e4 && !p.geometrySampleDepth && p.geometryColor8Format==UINT32_MAX &&
        !p.geometryPixelEffects && !p.geometryDepthColorWitness && !p.geometryCentroidMask &&
        !p.geometryMrtOutputs && p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001};
}
inline bool validateNativeWorldStripProgram(const NativeTranslatedDrawProgram& p) {
    return p.abiVersion==29 && p.worldStrip && !p.screenStrip && p.stride==36 && p.interpolators==4 && !p.indexed &&
        p.textureSlot==2 && nativeTranslatedTextureSlots(p)==std::vector<unsigned>{2,6,1,3} &&
        p.vertexIdentity.size()==252 && p.pixelIdentity.size()==912 &&
        p.vertexCode.size()>=5 && p.pixelCode.size()>=5 &&
        p.vertexCode.front()==0x07230203 && p.pixelCode.front()==0x07230203 &&
        !p.directTriangles && !p.affinePositionXY && !p.color8SourceClamped && !p.cubeRectanglePwl &&
        !p.explicitLodRectangle && !p.depthOnlyRectangle && !p.rectangleMrt && !p.offsetSample &&
        !p.compositeRectangle && !p.screenSpaceRectangle && !p.fixed16RectangleDepth &&
        !p.fixed16VertexRegisterCount && !p.geometryFixed16GradientDepth &&
        !p.geometryDepth20e4 && !p.geometrySampleDepth && p.geometryColor8Format==UINT32_MAX &&
        !p.geometryPixelEffects && !p.geometryDepthColorWitness && !p.geometryCentroidMask &&
        !p.geometryMrtOutputs && p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001};
}
// ABI30 is an independent resource-free indexed RGB7e3 interface, not a
// widened MRT/color8 or strip profile. Actual guest ownership/state remains
// the boundary consumer's responsibility; this validates program metadata.
inline bool validateNativeIndexedRgb7e3Program(const NativeTranslatedDrawProgram& p) {
    return p.abiVersion==30 && p.indexed && p.stride==24 && p.interpolators==1 &&
        p.textureSlot==UINT32_MAX && p.textureSlots.empty() &&
        p.vertexIdentity.size()==132 && p.pixelIdentity.size()==360 &&
        p.vertexCode.size()>=5 && p.pixelCode.size()>=5 &&
        p.vertexCode.front()==0x07230203 && p.pixelCode.front()==0x07230203 &&
        !p.screenStrip && !p.worldStrip && !p.directTriangles && !p.affinePositionXY &&
        !p.color8SourceClamped && !p.cubeRectanglePwl && !p.explicitLodRectangle &&
        !p.depthOnlyRectangle && !p.rectangleMrt && !p.offsetSample && !p.compositeRectangle &&
        !p.screenSpaceRectangle && !p.fixed16RectangleDepth && !p.fixed16VertexRegisterCount &&
        !p.geometryFixed16GradientDepth && !p.geometryDepth20e4 && !p.geometrySampleDepth &&
        p.geometryColor8Format==UINT32_MAX && !p.geometryPixelEffects && !p.geometryDepthColorWitness &&
        !p.geometryCentroidMask && !p.geometryMrtOutputs && p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001};
}
inline bool validateNativeScreenSpaceRectangleProgram(const NativeTranslatedDrawProgram& p) {
    const auto slots=nativeTranslatedTextureSlots(p);
    return p.abiVersion==26 && p.screenSpaceRectangle && p.compositeRectangle && !p.indexed &&
        !p.interpolators && p.stride==24 && slots.size()==1 && slots[0]<32 && slots[0]==p.textureSlot &&
        !p.directTriangles && !p.affinePositionXY && !p.color8SourceClamped && !p.cubeRectanglePwl &&
        !p.explicitLodRectangle && !p.depthOnlyRectangle && !p.rectangleMrt && !p.offsetSample &&
        !p.fixed16RectangleDepth && !p.fixed16VertexRegisterCount && !p.geometryFixed16GradientDepth &&
        !p.geometryDepth20e4 && !p.geometrySampleDepth && p.geometryColor8Format==UINT32_MAX &&
        !p.geometryPixelEffects && !p.geometryDepthColorWitness && !p.geometryCentroidMask &&
        !p.geometryMrtOutputs && p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001};
}
// Typed fixed16 ABI shared by state admission and the actual GPU consumers.
// This checks program metadata only, never guest resource ownership or output.
inline bool validateNativeFixed16Program(const NativeTranslatedDrawProgram& p) {
    const bool effects=p.abiVersion==18;
    if(p.fixed16VertexRegisterCount && p.fixed16VertexRegisterCount!=(effects?7u:6u))return false;
    if((p.abiVersion!=17 && !effects) || !p.indexed || !p.geometryFixed16GradientDepth || p.fixed16RectangleDepth ||
       !p.geometryDepth20e4 || p.geometrySampleDepth || p.geometryColor8Format!=UINT32_MAX ||
       p.geometryPixelEffects!=effects || p.geometryCentroidMask || p.geometryMrtOutputs ||
       p.geometryMrtFormats!=std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} ||
       p.geometryMrtBlendControls!=std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001} ||
       p.geometryDepthColorWitness || p.geometryParameterRegister!=UINT32_MAX)return false;
    const auto slots=nativeTranslatedTextureSlots(p);
    if(slots.size()>4 || (!effects && !slots.empty()))return false;
    unsigned seen=0;
    for(auto slot:slots) {
        if(slot>=32 || (seen&(1u<<slot)))return false;
        seen|=1u<<slot;
    }
    return true;
}
inline bool validateNativeFixed16RectangleProgram(const NativeTranslatedDrawProgram& p) {
    if(p.fixed16VertexRegisterCount)return false;
    const auto slots=nativeTranslatedTextureSlots(p);
    return p.abiVersion==19 && p.fixed16RectangleDepth && !p.indexed &&
        !p.geometryFixed16GradientDepth && p.geometryDepth20e4 && !p.geometrySampleDepth &&
        p.geometryColor8Format==UINT32_MAX && !p.geometryPixelEffects && !p.geometryCentroidMask &&
        !p.geometryMrtOutputs && !p.geometryDepthColorWitness && p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001} &&
        !p.directTriangles && !p.affinePositionXY && !p.color8SourceClamped && !p.cubeRectanglePwl &&
        !p.explicitLodRectangle && p.stride && p.stride%4==0 && p.interpolators==1 &&
        slots.size()==1 && slots[0]<32;
}
// Typed ABI21 contract. The absent pixel stage is the defining property, so it
// is required here rather than merely tolerated: a program claiming depth-only
// while carrying pixel code is rejected, as is any colour or texture binding.
inline bool validateNativeDepthOnlyRectangleProgram(const NativeTranslatedDrawProgram& p) {
    // This draw is admitted without execution, so the program carries NO
    // compiled module for either stage: only the identity that selects it.
    if(p.fixed16VertexRegisterCount || !p.pixelIdentity.empty() || !p.pixelCode.empty() ||
       !p.vertexCode.empty())return false;
    return p.abiVersion==21 && p.depthOnlyRectangle && !p.indexed && !p.compositeRectangle && !p.rectangleMrt &&
        !p.fixed16RectangleDepth && !p.geometryFixed16GradientDepth && !p.geometryDepth20e4 &&
        !p.geometrySampleDepth && p.geometryColor8Format==UINT32_MAX && !p.geometryPixelEffects &&
        !p.geometryCentroidMask && !p.geometryMrtOutputs && !p.geometryDepthColorWitness &&
        p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001} &&
        !p.directTriangles && !p.affinePositionXY && !p.color8SourceClamped && !p.cubeRectanglePwl &&
        !p.explicitLodRectangle && p.stride && p.stride%4==0 && p.interpolators==0 &&
        !p.vertexIdentity.empty() &&
        nativeTranslatedTextureSlots(p).empty();
}
// Typed ABI22 contract. Two colour outputs and two interpolators are the
// defining shape, so both are required rather than merely permitted, and every
// geometry/fixed16/composite marker is refused.
inline bool validateNativeRectangleMrtProgram(const NativeTranslatedDrawProgram& p) {
    if(p.fixed16VertexRegisterCount || p.pixelIdentity.empty() || p.pixelCode.empty() ||
       p.vertexIdentity.empty() || p.vertexCode.empty())return false;
    const auto slots=nativeTranslatedTextureSlots(p);
    if(slots.empty() || slots.size()>4)return false;
    unsigned seen=0;
    for(auto slot:slots) {
        if(slot>=32 || (seen&(1u<<slot)))return false;
        seen|=1u<<slot;
    }
    return p.abiVersion==22 && p.rectangleMrt && !p.offsetSample && !p.indexed && !p.depthOnlyRectangle &&
        !p.compositeRectangle && !p.fixed16RectangleDepth && !p.geometryFixed16GradientDepth &&
        !p.geometryDepth20e4 && !p.geometrySampleDepth && p.geometryColor8Format==UINT32_MAX &&
        !p.geometryPixelEffects && !p.geometryCentroidMask && !p.geometryMrtOutputs &&
        !p.geometryDepthColorWitness && p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001} &&
        !p.directTriangles && !p.affinePositionXY && !p.color8SourceClamped && !p.cubeRectanglePwl &&
        !p.explicitLodRectangle && p.stride && p.stride%4==0 && p.interpolators==2;
}
// ABI23/ABI24 offset sample band. One colour target, one sampled slot. The nine
// textures the game leaves bound are NOT all inputs: the pair samples exactly
// the slots this program declares, so the consumer selects them out of the
// bound set rather than requiring the counts to agree.
//
// ABI24 is the tonemap pass. Its DRAW is this band's draw in every respect the
// consumer acts on -- nonindexed rectangle list, stride 24, one live colour
// register, one declared slot sampled at a constant texel offset -- and the
// two differ only in the arithmetic their compiled stages contain (DOT2ADD,
// FRAC, LOG, SIN, COS). It therefore rides this band rather than duplicating a
// plan, a target and a resolve that would be identical; the separate ABI number
// keeps a refusal able to name which translator profile produced the shader.
inline bool validateNativeOffsetSampleProgram(const NativeTranslatedDrawProgram& p) {
    if(p.fixed16VertexRegisterCount || p.pixelIdentity.empty() || p.pixelCode.empty() ||
       p.vertexIdentity.empty() || p.vertexCode.empty())return false;
    const auto slots=nativeTranslatedTextureSlots(p);
    if(slots.size()!=1 || slots[0]>=32)return false;
    return (p.abiVersion==23 || p.abiVersion==24) && p.offsetSample && !p.rectangleMrt && !p.indexed &&
        !p.depthOnlyRectangle && !p.compositeRectangle && !p.fixed16RectangleDepth &&
        !p.geometryFixed16GradientDepth && !p.geometryDepth20e4 && !p.geometrySampleDepth &&
        p.geometryColor8Format==UINT32_MAX && !p.geometryPixelEffects && !p.geometryCentroidMask &&
        !p.geometryMrtOutputs && !p.geometryDepthColorWitness &&
        p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001} &&
        !p.directTriangles && !p.affinePositionXY && !p.color8SourceClamped && !p.cubeRectanglePwl &&
        !p.explicitLodRectangle && p.stride && p.stride%4==0 &&
        // The pyramid's levels come in two shapes: a four-tap pair with two
        // interpolators and an eight-tap pair with four. The tonemap pass adds
        // a third with three, whose first interpolator carries only two lanes.
        // All are this band; what matters is that the count is one the stage
        // interface can actually carry, not which of them it is.
        p.interpolators>=2 && p.interpolators<=4;
}
// ABI25 stencil-only band. The guest bound NO pixel shader, disabled colour and
// depth writes and left stencil enabled, so the stencil buffer is the draw's
// only output. The absent pixel stage is the defining property and is asserted
// rather than assumed: a program carrying pixel code is not this band, however
// its other fields read. The vertex stage exports position and nothing else, so
// there are no interpolators to carry and no texture to sample.
inline bool validateNativeStencilOnlyProgram(const NativeTranslatedDrawProgram& p) {
    if(!p.pixelIdentity.empty() || !p.pixelCode.empty())return false;
    if(p.vertexIdentity.empty() || p.vertexCode.empty())return false;
    if(!nativeTranslatedTextureSlots(p).empty() || p.fixed16VertexRegisterCount)return false;
    return p.abiVersion==25 && p.directTriangles && !p.indexed && !p.offsetSample &&
        !p.rectangleMrt && !p.depthOnlyRectangle && !p.compositeRectangle &&
        !p.fixed16RectangleDepth && !p.geometryFixed16GradientDepth && !p.geometryDepth20e4 &&
        !p.geometrySampleDepth && p.geometryColor8Format==UINT32_MAX && !p.geometryPixelEffects &&
        !p.geometryCentroidMask && !p.geometryMrtOutputs && !p.geometryDepthColorWitness &&
        p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001} &&
        !p.affinePositionXY && !p.color8SourceClamped && !p.cubeRectanglePwl &&
        !p.explicitLodRectangle && p.stride && p.stride%4==0 && p.interpolators==0;
}
// Direct (non-binned) window scissor in the existing 1280x720 viewport.
// Keep the actual rectangle; do not replace clipped UI draws with full screen.
inline std::optional<std::array<std::uint32_t,4>> nativeTranslatedScissor(const Gta5InlineDraw& draw) {
    if(!draw.scissorRegisters)return std::nullopt;
    const auto& r=*draw.scissorRegisters;
    if(r[0] || (r[1]&~0x3FFF3FFFu) || (r[2]&~0x3FFF3FFFu))return std::nullopt;
    const auto x=r[1]&0x3FFF,y=r[1]>>16,right=r[2]&0x3FFF,bottom=r[2]>>16;
    if(x>right || y>bottom || right>1280 || bottom>720)return std::nullopt;
    return std::array<std::uint32_t,4>{x,y,right-x,bottom-y};
}
// Map original signed XY viewport into the padded native 2048x1024 viewport.
// Homogeneous offsets retain W; the documented pixel-center correction is in
// output pixels, independent of the guest viewport's dimensions.
inline std::optional<std::array<float,4>> nativeTranslatedViewport(const Gta5InlineDraw& draw) {
    std::array<float,4> v{};
    for(unsigned i=0;i<4;++i) {
        const auto bits=gta5DrawRegister(draw,0x210F+i);if(!bits)return std::nullopt;
        std::memcpy(&v[i],&*bits,4);if(!std::isfinite(v[i]))return std::nullopt;
    }
    const auto center=gta5DrawRegister(draw,0x2302);
    if(!center || (*center!=4 && *center!=5) || v[0]<=0 || v[2]>=0 ||
       v[1]-v[0]<0 || v[1]+v[0]>1280 || v[3]+v[2]<0 || v[3]-v[2]>720)return std::nullopt;
    const float correction=*center==4?0.5f:0.0f;
    return std::array<float,4>{v[0]/1024.0f,v[2]/512.0f,(v[1]+correction)/1024.0f-1.0f,(v[3]+correction)/512.0f-1.0f};
}
// Actual UI state: quad list, source-alpha RGB, direct source alpha, disabled
// depth/stencil, two interpolators, and either documented pixel-center mode.
// Shader code identity and resources are independent requirements.
inline bool validateNativeTranslatedDraw(const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
                                        const std::vector<std::uint32_t>& indices={}) {
    // Experimental offscreen programs have a separate state/coverage contract.
    if(program.abiVersion!=4 || program.cubeRectanglePwl || program.explicitLodRectangle || program.compositeRectangle ||
       program.depthOnlyRectangle || program.rectangleMrt || program.directTriangles || program.affinePositionXY)return false;
    const auto slots=nativeTranslatedTextureSlots(program);
    const bool textured=!slots.empty();
    if(slots.size()>2 || (textured && slots[0]!=program.textureSlot))return false;
    for(unsigned i=0;i<slots.size();++i)
        if(slots[i]>=32 || (i && slots[i]==slots[0]))return false;
    if(draw.primitive!=(program.indexed?4u:13u) || !draw.vertexCount ||
       (!program.indexed && (draw.vertexCount%4 || !indices.empty())) ||
       (program.indexed && (indices.empty() || indices.size()%3 || indices.size()>65535)) || draw.stride!=program.stride ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride || draw.vertices.size()>256*1024 ||
       draw.textures.size()!=slots.size())return false;
    for(unsigned i=0;i<slots.size();++i)if(draw.textures[i].slot!=slots[i])return false;
    for(const auto i:indices)if(i>=draw.vertexCount)return false;
    const auto read=[&](unsigned r){return gta5DrawRegister(draw,r);};
    const auto center=read(0x2302);
    if(!center || (*center!=4 && *center!=5) || !nativeTranslatedScissor(draw) || !nativeTranslatedViewport(draw))return false;
    // Pixel center and scissor are independently interpreted above and in
    // actual native execution; all other viewport state remains constrained.
    if(!matchesNativeColorViewportRegisters([&](unsigned r){
        if(r==0x2302)return std::optional<std::uint32_t>{4};
        if(r==0x2081)return std::optional<std::uint32_t>{0};
        if(r==0x2082)return std::optional<std::uint32_t>{0x2D00500};
        if(r==0x210F || r==0x2110)return std::optional<std::uint32_t>{0x44200000};
        if(r==0x2111)return std::optional<std::uint32_t>{0xC3B40000};
        if(r==0x2112)return std::optional<std::uint32_t>{0x43B40000};
        return read(r);
    }))return false;
    // Exact identities distinguish the one/two/three/four-interpolator UI layouts.
    const auto parameters=program.interpolators?program.interpolators:(textured?2u:1u);
    if(parameters<1 || parameters>4 || textured!=(parameters>1) || (parameters==4)!=(slots.size()==2))return false;
    const auto control=parameters==4?0x10310304u:parameters==3?0x10210204u:textured?0x10110103u:0x10010103u;
    const auto context=parameters==4?0x3000Fu:parameters==3?0x10007u:textured?0x10003u:1u;
    if(read(0x2180)!=std::optional<std::uint32_t>{control} ||
       read(0x2181)!=std::optional<std::uint32_t>{textured?8u:0u} ||
       read(0x2182)!=std::optional<std::uint32_t>{context})return false;
    constexpr std::array<std::array<std::uint32_t,2>,11> required{{
        {{0x2001,0xC02D0}},{{0x2104,15}},{{0x2183,0}},{{0x2184,0}},{{0x2200,0x700770}},
        {{0x2201,0x10706}},{{0x2202,0xAA000007}},{{0x2203,0x10}},
        {{0x2307,0xFF000}},{{0x2308,0xFF100}},{{0x2312,0xFFFF}}
    }};
    for(const auto& item:required)if(read(item[0])!=std::optional<std::uint32_t>{item[1]})return false;
    return true;
}
}
