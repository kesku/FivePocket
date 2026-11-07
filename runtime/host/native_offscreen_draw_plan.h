#pragma once
#include "native_translated_draw.h"
#include "gta5_bound_surface_capture.h"

namespace recomp::gpu {
struct NativeOffscreenDrawPlan {
    std::uint32_t object=0,width=0,height=0,pitch=0,baseTile=0,tileCount=0;
    // Xenos colour render-target format. 0 is k_8_8_8_8, which every band that
    // predates the offset sample writes, so the default keeps them unchanged.
    // 7 is k_16_16_16_16_FLOAT and needs a wider attachment and readback.
    unsigned format=0;
    bool operator==(const NativeOffscreenDrawPlan&) const = default;
};
// Bytes per texel for the formats an offscreen attachment can carry.
// 0 is k_8_8_8_8 and 14 is k_32_FLOAT, both four bytes; 7 is
// k_16_16_16_16_FLOAT at eight. The downsample pyramid ends in 14, a single
// float32 channel, after reducing through 7.
inline constexpr unsigned nativeOffscreenTexelBytes(unsigned format) {
    // k_16_16_16_16_FLOAT is four binary16. k_32_FLOAT is one binary32 and
    // k_16_16 two 16-bit fixed-point channels, so both are four bytes, as the
    // ordinary colour case is.
    return format==7?8u:4u;
}
// Single-sample physical footprint, including pitch and padded final rows.
// A physical tile contains 5120 bytes, not 1280 pixels of every format.
// The pinned Xenia render_target_cache.cc likewise doubles 32bpp tile lengths
// for 64bpp surfaces. This establishes capacity/overlap, NOT an exact 64bpp
// colour-alias permutation (which the reference itself calls unproven).
inline constexpr std::optional<unsigned> nativeOffscreenTileCount(
        unsigned pitch,unsigned height,unsigned format) {
    if(!pitch || pitch%80 || !height ||
       (format!=0 && format!=4 && format!=7 && format!=14))return std::nullopt;
    const auto count=std::uint64_t{pitch/80}*((std::uint64_t{height}+15)/16)*(format==7?2u:1u);
    if(!count || count>2048)return std::nullopt;
    return static_cast<unsigned>(count);
}
// First direct RGBA8 offscreen profile: one UV interpolator, full replacement,
// no depth/stencil, no MSAA or binning. Dimensions/base come from owned state,
// not shader identity or a loading-stage number. This proves the defined region
// only; the caller must prevent EDRAM aliases and validate every later resolve.
inline std::optional<NativeOffscreenDrawPlan> planNativeOffscreenDraw(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5BoundSurfaceCapture& bindings) {
    if(program.abiVersion!=5 || program.cubeRectanglePwl || program.explicitLodRectangle || !program.directTriangles || program.indexed ||
       program.interpolators!=1 || !program.affinePositionXY || program.stride!=draw.stride ||
       draw.deviceState.size()!=13860 || draw.vertices.size()>256*1024 ||
       !nativeAffineFullRectangle(draw,*program.affinePositionXY))return std::nullopt;
    const auto slots=nativeTranslatedTextureSlots(program);
    if(slots.size()!=1 || slots[0]>=32 || slots[0]!=program.textureSlot ||
       draw.textures.size()!=1 || draw.textures[0].slot!=slots[0])return std::nullopt;
    const auto word=[](const auto& data,unsigned at) {
        return (std::uint32_t{data[at]}<<24)|(std::uint32_t{data[at+1]}<<16)|
            (std::uint32_t{data[at+2]}<<8)|data[at+3];
    };
    const auto& surface=bindings.surfaces[0];
    if(!surface.object || surface.object>UINT32_MAX-31u || !surface.header || surface.header->size()!=32 ||
       surface.object!=word(draw.deviceState,12816))return std::nullopt;
    for(unsigned i=0;i<5;++i) {
        const auto& bound=bindings.surfaces[i];
        if(bound.object!=word(draw.deviceState,12816+4*i) ||
           bound.tileCacheObject!=word(draw.deviceState,13104+4*i) || (i && bound.object))return std::nullopt;
    }
    const auto flags=draw.deviceState[11068];
    if((flags&0x10) || ((flags&0x20) && surface.object==surface.tileCacheObject))return std::nullopt;
    const auto info=word(*surface.header,24),color=word(*surface.header,28);
    // RB_SURFACE_INFO: pitch14, MSAA2 at16. RB_COLOR_INFO: base11 and
    // format4 at16, exponent6 at20. Format0 is RGBA8 UNORM. Pinned Xenia
    // 95a5c3ee registers.h/xenos.h; 32bpp EDRAM tiles are80x16 pixels.
    const auto pitch=info&0x3FFFu;
    if(info!=word(draw.deviceState,10368) || color!=word(draw.deviceState,10372) ||
       (info&0x3C000u) || (color&~0x7FFu) || !pitch || pitch%80)return std::nullopt;
    const auto scissor=nativeTranslatedScissor(draw);
    const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || !viewport || (*scissor)[0] || (*scissor)[1] || !(*scissor)[2] || !(*scissor)[3])return std::nullopt;
    const auto width=(*scissor)[2],height=(*scissor)[3];
    if(pitch<width)return std::nullopt;
    const auto read=[&](unsigned r){return gta5DrawRegister(draw,r);};
    const auto floatValue=[](std::uint32_t bits){float f;std::memcpy(&f,&bits,4);return f;};
    if(floatValue(*read(0x210F))*2!=width || floatValue(*read(0x2110))*2!=width ||
       floatValue(*read(0x2111))*-2!=height || floatValue(*read(0x2112))*2!=height)return std::nullopt;
    // Reuse only the fixed ancillary rasterization checks; the substituted
    // surface/viewport/scissor words have been independently interpreted above.
    if(!matchesNativeColorViewportRegisters([&](unsigned r) {
        if(r==0x2000)return std::optional<std::uint32_t>{0x14000500};
        if(r==0x2081)return std::optional<std::uint32_t>{0};
        if(r==0x2082)return std::optional<std::uint32_t>{0x2D00500};
        if(r==0x210F || r==0x2110)return std::optional<std::uint32_t>{0x44200000};
        if(r==0x2111)return std::optional<std::uint32_t>{0xC3B40000};
        if(r==0x2112)return std::optional<std::uint32_t>{0x43B40000};
        if(r==0x2302)return std::optional<std::uint32_t>{4};
        return read(r);
    }))return std::nullopt;
    constexpr unsigned required[][2]{
        {0x2104,15},{0x2180,0x10010100},{0x2181,4},{0x2182,0x10001},{0x2183,0},{0x2184,0},
        {0x2200,0x700770},{0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x10},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& r:required)if(read(r[0])!=std::optional<std::uint32_t>{r[1]})return std::nullopt;
    const auto tiles=(pitch/80)*((height+15)/16);
    if(tiles>2048-color)return std::nullopt; // Wrapped/aliased EDRAM unsupported.
    return NativeOffscreenDrawPlan{surface.object,width,height,pitch,color,tiles};
}
}
