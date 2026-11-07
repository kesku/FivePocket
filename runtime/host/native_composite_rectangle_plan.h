#pragma once
#include "native_offscreen_draw_plan.h"
#include "native_rectangle_list.h"
namespace recomp::gpu {
// ABI20: the full-screen composite band. One float4 interpolator, one sampled
// 2D color texture, nonindexed rectangle_list expansion, no depth attachment,
// no PWL, no gradients, no predicate/discard.
//
// sub_823A64D8 builds a current-stack header via 82370910 and binds it through
// 823AB770. The Sept21 live capture confirms base0/1280x32, not the unrelated
// cached base384/1280x384. Require the CURRENT owned header and register proof.
// Caller supplies a heap/current-worker-stack reader under the guest gate;
// neither a cached header nor an arbitrary raw-memory read is an alternative.
// This plans one colour band only, not depth restoration or EDRAM ownership.
inline std::optional<NativeOffscreenDrawPlan> planNativeCompositeRectangleDraw(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
    const Gta5BoundSurfaceCapture& bindings,const char** why=nullptr) {
    const auto no=[&](const char* reason){if(why)*why=reason;return std::optional<NativeOffscreenDrawPlan>{};};
    const bool screen=program.abiVersion==26;
    if((screen && !validateNativeScreenSpaceRectangleProgram(program)) ||
       (!screen && (program.abiVersion!=20 || program.screenSpaceRectangle)) ||
       !program.compositeRectangle || program.cubeRectanglePwl ||
       program.explicitLodRectangle || program.fixed16RectangleDepth || program.indexed ||
       program.directTriangles || program.affinePositionXY || program.color8SourceClamped ||
       program.geometryPixelEffects || program.geometryFixed16GradientDepth ||
       program.interpolators!=(screen?0u:1u) || program.stride!=draw.stride || draw.stride!=24 ||
       draw.primitive!=8 || draw.vertexCount<3 || draw.vertexCount>384 || draw.vertexCount%3 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride ||
       draw.deviceState.size()!=13860 || draw.textures.size()!=1)return no("program-profile");
    const auto slots=nativeTranslatedTextureSlots(program);const auto& t=draw.textures[0];
    if(slots.size()!=1 || slots[0]!=program.textureSlot || slots[0]!=t.slot || slots[0]>=32 ||
       t.literalZero)return no("texture-slots");
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    const auto& s=bindings.surfaces[0];
    if(!s.object || s.object%4 || s.object>UINT32_MAX-39u ||
       s.object!=word(draw.deviceState,12816))return no("current-target-object");
    if(!s.header || s.header->size()!=40)return no("current-target-header");
    // Additional COLOUR slots (1-3) must be genuinely unbound: this profile
    // writes one attachment. The original loop binds the PREVIOUS band's
    // depth header on later iterations. Its ALWAYS/no-write depth test is
    // checked below; this colour draw never imports or modifies that depth.
    for(unsigned i=1;i<4;++i) {
        const auto& bound=bindings.surfaces[i];
        if(bound.object!=word(draw.deviceState,12816+4*i) || bound.object)return no("other-colour-slot-bound");
    }
    const auto& depth=bindings.surfaces[4];
    if(depth.object!=word(draw.deviceState,12832))return no("depth-binding-mismatch");
    for(unsigned i=0;i<5;++i)
        if(bindings.surfaces[i].tileCacheObject!=word(draw.deviceState,13104+4*i))return no("cache-snapshot-mismatch");
    const auto flags=draw.deviceState[11068];
    if(flags&0x30)return no("tiled-mode");
    const auto info=word(*s.header,24),color=word(*s.header,28);
    // sub_823AB3D8 copies the bound header's +28 to this register. Comparing
    // pitch alone admitted the captured base0 write against cached base384.
    if(gta5DrawRegister(draw,0x2001)!=color)return no("colour-register-header-mismatch");
    // RB_SURFACE_INFO pitch14 at0, MSAA2 at16; RB_COLOR_INFO base11, format4
    // at16. The current header must corroborate the live surface register.
    const auto pitch=info&0x3FFFu;
    if(info!=word(draw.deviceState,10368) || (info&0x3C000u) || (color&~0x7FFu) ||
       !pitch || pitch%80)return no("surface-info");
    const auto scissor=nativeTranslatedScissor(draw);
    const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || !viewport || (*scissor)[0] || (*scissor)[1] || !(*scissor)[2] ||
       !(*scissor)[3])return no("scissor-viewport");
    const auto width=(*scissor)[2],height=(*scissor)[3];
    if(pitch<width)return no("pitch-lt-width");
    const auto dimensions=word(*s.header,36);
    if((dimensions>>18)+1!=width || ((dimensions>>3)&0x7FFFu)+1!=height)return no("header-dimensions");
    const auto tiles=(pitch/80)*((height+15)/16);
    if(tiles>2048-color)return no("edram-wrap");
    // The original 823A64D8 calls enable(1), write(0), compare(7).823CC8F8
    // enables depth only with a bound depth object.823BB650 derives HiZ bit0
    // from that enable and device11070 bit5. First/second live captures
    // corroborate these TWO states. ALWAYS cannot reject a fragment; writes
    // and stencil are disabled, so neither depth contents nor HiZ can affect
    // this output. Do not generalise this to other compares or side effects.
    const auto dc=gta5DrawRegister(draw,0x2200),hiz=gta5DrawRegister(draw,0x2203);
    //82370998 encodes the HiZ address in bits17..31 of header+32.823AB070
    // binds it and, when unbinding depth, preserves this address while clearing
    // enable bits. It advances after strip2; it is NOT a fixed raster flag.
    constexpr std::uint32_t HizAddressMask=0xFFFE0000u;
    if(screen) {
        // Captured8291C770: position-only VS, generated XY in pixel r0,
        // no depth binding/test/write, no stencil or HiZ. This is a separate
        // ABI, not a relaxation of ABI20's paired previous-band depth state.
        if(depth.object || dc!=0x700770u || hiz!=0x10u)return no("screen-depth-state");
        if(gta5DrawRegister(draw,0x2180)!=0x10050100u ||
           gta5DrawRegister(draw,0x2181)!=2u || gta5DrawRegister(draw,0x2182)!=0u ||
           gta5DrawRegister(draw,0x2202)!=0xAA000005u)return no("screen-parameter-state");
    } else if(dc==0x700770u) {
        if(!hiz || (*hiz&~HizAddressMask)!=0x714u)return no("depth-hiz-pair");
    } else if(dc==0x700772u) {
        if(!hiz || (*hiz&~HizAddressMask)!=0x715u ||
           !(draw.deviceState[11070]&0x20u))return no("depth-hiz-pair");
        // Exact original previous-band binding, owned by the caller's reader.
        // A cached/unreadable/mismatched header cannot justify this profile.
        if(!depth.object || depth.object%4 || depth.object>UINT32_MAX-39u ||
           !depth.header || depth.header->size()!=40)return no("always-depth-header");
        const auto di=word(*depth.header,28);
        const auto headerHiz=word(*depth.header,32);
        if(word(*depth.header,24)!=info || word(*depth.header,36)!=dimensions ||
           (headerHiz&~HizAddressMask)!=0x11u ||
           (headerHiz&HizAddressMask)!=(*hiz&HizAddressMask) ||
           gta5DrawRegister(draw,0x2002)!=di || (di&~0x7FFu)!=0x10000u ||
           (di&0x7FFu)+tiles!=color)return no("always-depth-previous-band");
    } else return no("depth-not-colour-only");
    // Exact captured ancillary state: full RGBA replacement, no culling,
    // homogeneous clip transform, no blend. Depth was independently proved
    // colour-only above, not inferred from a readable binding alone.
    constexpr unsigned required[][2]{
        {0x2104,15},{0x2183,0},{0x2184,0},{0x2201,0x10001},
        {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},{0x2302,4},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& r:required)if(gta5DrawRegister(draw,r[0])!=r[1])return no("raster-output-state");
    if(!screen && (gta5DrawRegister(draw,0x2180)!=0x10010000u ||
       gta5DrawRegister(draw,0x2181)!=4u || gta5DrawRegister(draw,0x2182)!=0x10001u ||
       gta5DrawRegister(draw,0x2202)!=0xAA000007u))return no("raster-output-state");
    return NativeOffscreenDrawPlan{s.object,width,height,pitch,color,tiles};
}
// Geometry comes from the original translated vertex program, not raw vertex
// guesses. Live823A64D8 covers32 rows; authored partial-tail tests cover16. Only a
// hole-free, full-width prefix of WHOLE physical tile rows can be published.
inline std::optional<unsigned> nativeCompositeCompletedRows(const Gta5InlineDraw& draw,
        const NativeOffscreenDrawPlan& plan,std::span<const std::array<float,4>> positions) {
    const auto coverage=nativeRectangleListCoverage(draw,positions);
    if(plan.format || plan.width!=plan.pitch || plan.width%80 || !coverage ||
       coverage->left || coverage->top || coverage->right!=plan.width ||
       !coverage->bottom || coverage->bottom>plan.height || coverage->bottom%16 ||
       coverage->area!=std::uint64_t{plan.width}*coverage->bottom)return std::nullopt;
    return coverage->bottom;
}
}
