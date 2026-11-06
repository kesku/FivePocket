#pragma once
#include "gta5_fixed16_clear_plan.h"
#include "gta5_bound_surface_capture.h"
#include "native_translated_draw.h"
#include "xenos_depth_stencil_state.h"

namespace recomp::gpu {
struct NativeFixed16DrawState {
    std::array<float,4> viewport{};
    std::array<unsigned,4> scissor{};
    XenosDepthStencilState depth;
    float minDepth=1,maxDepth=0;
    unsigned cull=2;
    bool clockwise=false;
};
// Observed single-sample fixed16 draw with explicit PS depth. The original
// shaders and ordered constants are independent requirements. Polygon offset
// is enabled in SC_MODE, but the PS supplies depth itself; pinned translator
// ROV_DepthStencilTest applies raster polygon offset ONLY without depth export.
// Do not apply that bias a second time to the shader's gradient-derived depth.
inline std::optional<NativeFixed16DrawState> planNativeFixed16DrawStateForTopology(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5Fixed16ClearPlan& plan,const Gta5BoundSurfaceCapture& bindings,bool rectangle) {
    if(!(rectangle?validateNativeFixed16RectangleProgram(program):validateNativeFixed16Program(program)) ||
       draw.primitive!=(rectangle?8u:4u) || (rectangle && draw.vertexCount%3) || draw.stride!=program.stride ||
       !draw.vertexCount || draw.vertexCount>65536 || draw.vertices.size()>256*1024 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride || !draw.textures.empty() ||
       draw.deviceState.size()!=13860 || plan.format!=4 || plan.samples!=1 || !plan.depth ||
       plan.depth->format!=1 || plan.depth->samples!=1 || plan.depth->width!=plan.width ||
       plan.depth->height!=plan.height || plan.depth->pitch!=plan.pitch)return std::nullopt;
    if(!plan.object || plan.object%4 || !plan.depth->object || plan.depth->object%4 || plan.depth->object==plan.object ||
       !plan.width || !plan.height || plan.width>1280 || plan.height>720 || plan.pitch<plan.width || plan.pitch>16383 ||
       plan.pitch%80 || plan.baseTile>=2048 || plan.depth->baseTile>=2048)return std::nullopt;
    const auto tiles=(plan.pitch/80)*((plan.height+15)/16);
    if(plan.tileCount!=tiles || plan.depth->tileCount!=tiles || tiles>2048-plan.baseTile || tiles>2048-plan.depth->baseTile ||
       (plan.baseTile<plan.depth->baseTile+tiles && plan.depth->baseTile<plan.baseTile+tiles))return std::nullopt;
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
        (std::uint32_t{b[at+2]}<<8)|b[at+3];};
    // Original82476080 clears the active tiling roots/flags, but deliberately
    // retains device[13104..13120] cache objects. Original823B9B58 chooses raw
    // versus recorded emission via device[11068].bit0, not pointer presence.
    // Recognize ONLY the fully inactive captured domain; do not dereference or
    // reuse retained tile owners, and do not weaken the live pending-work gate.
    if(draw.deviceState[11068] || word(draw.deviceState,11060) || word(draw.deviceState,11064) ||
       word(draw.deviceState,13088) || word(draw.deviceState,13092) || word(draw.deviceState,13100))return std::nullopt;
    for(unsigned slot=0;slot<5;++slot) {
        const auto& surface=bindings.surfaces[slot];const auto expected=slot==0?plan.object:slot==4?plan.depth->object:0;
        if(surface.object!=expected || word(draw.deviceState,12816+4*slot)!=expected ||
           surface.tileCacheObject!=word(draw.deviceState,13104+4*slot) ||
           (expected && surface.tileCacheObject==expected))return std::nullopt;
        if(slot!=0 && slot!=4)continue;
        if(!surface.header || surface.header->size()!=40)return std::nullopt;
        const auto info=word(*surface.header,24),format=word(*surface.header,28),dimensions=word(*surface.header,36);
        const auto wanted=(slot?plan.depth->baseTile|0x10000u:plan.baseTile|0x40000u);
        if(info!=word(draw.deviceState,10368) || (info&0x3FFFFu)!=plan.pitch || format!=wanted ||
           format!=word(draw.deviceState,slot?10376:10372) || (dimensions>>18)+1!=plan.width ||
           ((dimensions>>3)&0x7FFF)+1!=plan.height)return std::nullopt;
    }
    const auto viewport=nativeTranslatedViewport(draw);
    const auto scissor=nativeTranslatedScissor(draw);
    if(!viewport || !scissor || *scissor!=std::array<unsigned,4>{0,0,plan.width,plan.height})return std::nullopt;
    const auto r=[&](unsigned reg){return gta5DrawRegister(draw,reg);};
    const auto real=[](std::uint32_t bits){float f;std::memcpy(&f,&bits,4);return f;};
    if(real(*r(0x210F))*2!=plan.width || real(*r(0x2110))*2!=plan.width ||
       real(*r(0x2111))*-2!=plan.height || real(*r(0x2112))*2!=plan.height)return std::nullopt;
    // Actual resource-free and alpha-tested pairs use different GPR/export
    // counts and interpolator masks. The captured ABI18 pair exports two
    // interpolators; do not silently reuse ABI17's one-interpolator state.
    const bool effects=program.abiVersion==18;
    // SQ_PROGRAM_CNTL.VS_NUM_REG is count minus one. Captured a0-indexed
    // ABI17 uses r0..r5; its PS/export/resource bits match the earlier ABI17.
    // The addressed textured ABI18 capture uses r0..r6 with the same PS/export
    // bits as earlier ABI18. Require the explicit program contract; do not
    // infer support for arbitrary GPR counts from an otherwise similar state.
    const auto programControl=effects?
        (program.fixed16VertexRegisterCount==7?0x18110206u:0x18110202u):
        (program.fixed16VertexRegisterCount==6?0x18010105u:0x18010101u);
    if(r(0x2180)!=programControl ||
       r(0x2182)!=(effects?0x30003u:0x10001u) || program.interpolators!=(effects?2u:1u))return std::nullopt;
    constexpr unsigned required[][2]{
        {0x2003,0x300},{0x200E,0},{0x200F,0x20002000},
        {0x2100,0xFFFFFF},{0x2101,0},{0x2102,0},{0x2104,15},{0x210C,0xFFFF00},{0x210D,0xFFFFFF},
        {0x2113,0xBF800000},{0x2114,0x3F800000},
        {0x2181,4},{0x2183,0},{0x2184,0},
        {0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x11},{0x2204,0x80000},
        {0x2205,0x19802},{0x2206,0x43F},{0x2207,0},{0x2208,4},{0x2209,0x10001},{0x220A,0x10001},{0x220B,0x10001},
        {0x2300,0x400},{0x2301,0},{0x2302,4},{0x2303,0x40000000},{0x2304,0x3F800000},
        {0x2305,0x40000000},{0x2306,0x3F800000},{0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& expected:required)if(r(expected[0])!=expected[1])return std::nullopt;
    // The captured rectangle replaces depth (ALWAYS); indexed geometry still
    // requires GREATER_OR_EQUAL. Neither contract accepts arbitrary depth state.
    if(r(0x2200)!=(rectangle?0x700776u:0x700766u))return std::nullopt;
    const auto depth=decodeXenosDepthStencilState(*r(0x2200),*r(0x210D),*r(0x210C));
    if(!depth || depth->stencilEnable || !depth->depthEnable || !depth->depthWrite || depth->depthCompare!=(rectangle?7u:6u))return std::nullopt;
    // ReX draw_util::IsPrimitivePolygonal explicitly excludes rectangle lists;
    // its Vulkan pipeline applies PA_SU_SC_MODE face culling only to polygonal
    // primitives. Our expanded rectangle triangles must not inherit the guest
    // triangle BACK cull bit. Indexed triangles retain their existing culling.
    return NativeFixed16DrawState{*viewport,*scissor,*depth,1,0,rectangle?0u:2u,false};
}
inline std::optional<NativeFixed16DrawState> planNativeFixed16DrawState(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5Fixed16ClearPlan& plan,const Gta5BoundSurfaceCapture& bindings) {
    return planNativeFixed16DrawStateForTopology(draw,program,plan,bindings,false);
}
inline std::optional<NativeFixed16DrawState> planNativeFixed16RectangleState(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5Fixed16ClearPlan& plan,const Gta5BoundSurfaceCapture& bindings) {
    return planNativeFixed16DrawStateForTopology(draw,program,plan,bindings,true);
}
}
