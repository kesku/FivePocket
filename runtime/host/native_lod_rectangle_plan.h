#pragma once
#include "native_offscreen_draw_plan.h"
#include "native_mip_texture_capture.h"
#include "native_rectangle_list.h"
#include <bit>
namespace recomp::gpu {
struct NativeLodRectanglePlan {
    NativeOffscreenDrawPlan target;
    unsigned outputMip=0,width=0,height=0;
};
// Explicit ABI9/10/11 spherical mip-generation rectangle. This is an input/state
// contract for the independently identity-matched original shader pair, not a
// general rectangle or explicit-LOD shader admission. The target MUST already
// be valid; this planner does not establish that lifetime/ownership condition.
inline std::optional<NativeLodRectanglePlan> planNativeLodRectangleDraw(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5BoundSurfaceCapture& bindings,const NativeMipTextureCapture& texture) {
    if((program.abiVersion!=9 && program.abiVersion!=10 && program.abiVersion!=11) || !program.explicitLodRectangle || program.cubeRectanglePwl || program.indexed ||
       program.directTriangles || program.affinePositionXY || program.color8SourceClamped || program.interpolators!=1 ||
       program.stride!=24 || draw.stride!=24 || draw.primitive!=8 || draw.vertexCount!=3 || draw.vertices.size()!=72 ||
       draw.deviceState.size()!=13860 || !draw.textures.empty() || nativeTranslatedTextureSlots(program)!=std::vector<unsigned>{5} ||
       program.textureSlot!=5)return std::nullopt;
    const auto d=XenosTextureDescription::decode(texture.fetch);const auto chain=planXenosUnpackedMipChain(texture.fetch);
    const auto sampler=planNativeTextureSampler2D(texture.fetch);
    if(!d || !chain || !sampler || d->format!=6 || !d->tiled || d->packedMips || d->width<128 || d->width>1024 ||
       !std::has_single_bit(d->width) || d->height!=d->width/2 || d->pitchTexels!=d->width || d->mipMax!=3 ||
       d->sign!=std::array<unsigned,4>{0,0,0,0} || sampler->clamp!=std::array<unsigned,2>{2,2} ||
       sampler->minFilter!=1 || sampler->magFilter!=1 || sampler->mipFilter!=1 ||
       texture.base.size()!=chain->baseBytes || texture.mips.size()!=chain->mipBytes)return std::nullopt;
    const float mipFloat=std::bit_cast<float>(draw.pixelConstants[229*4]);
    if(mipFloat!=1 && mipFloat!=2 && mipFloat!=3)return std::nullopt;
    const unsigned mip=static_cast<unsigned>(mipFloat),width=d->width>>mip,height=d->height>>mip;
    // The recovered 25-tap kernel has only been replayed for this original
    // next-loop extent. Do not generalize its numeric domain from ABI9 cases.
    if(program.abiVersion==11 && (d->width!=512 || mip!=3))return std::nullopt;
    // Original82369A08..A78 sets (mip,2*mip-1,1,2^mip/baseWidth).
    // ABI9 reads x/w. ABI10 also reads z and has a DIFFERENT literal bank for
    // its nine-sample predicated kernel; never validate it against ABI9's bank.
    if(width<16 || height<8 || draw.pixelConstants[229*4+3]!=std::bit_cast<unsigned>(1.f/width) ||
       draw.vertexConstants[251*4]!=0xBF800000 || draw.vertexConstants[251*4+1]!=0x40000000)return std::nullopt;
    if(program.abiVersion==9) {
        constexpr std::array<unsigned,8> constants{0x3F400000,0x3E800000,0x3F000000,0xBF000000,
            0x40800000,0x41000000,0,0xC0000000};
        if(draw.pixelConstants[253*4]!=0x3F800000)return std::nullopt;
        for(unsigned i=0;i<8;++i)if(draw.pixelConstants[254*4+i]!=constants[i])return std::nullopt;
    } else if(program.abiVersion==10) {
        constexpr unsigned constants[][2]{{229*4+2,0x3F800000},{252*4,0x3F800000},
            {252*4+1,0x40800000},{252*4+2,0x3DE38E39},
            {253*4,0xBF000000},{253*4+1,0xC0000000},{253*4+2,0x40800000},{253*4+3,0x41000000},
            {254*4,0x3F400000},{254*4+1,0x3E800000},{254*4+2,0x3F000000},{254*4+3,0x40800000},
            {255*4,0x3F000000},{255*4+1,0xC0000000},{255*4+2,0xBF800000},{255*4+3,0}};
        for(const auto& c:constants)if(draw.pixelConstants[c[0]]!=c[1])return std::nullopt;
    } else {
        constexpr unsigned constants[]{0x3E800000,0x3F400000,0xBF800000,0x3D23D70A,
            0x3F800000,0x40800000,0x41000000,0x3F000000,
            0,0xBF000000,0xC0000000,0x40800000,
            0x3F000000,0xC0800000,0xC0000000,0,
            0x41000000,0x3E800000,0x3F000000,0xC0000000};
        if(draw.pixelConstants[229*4+2]!=0x3F800000)return std::nullopt;
        for(unsigned i=0;i<20;++i)if(draw.pixelConstants[251*4+i]!=constants[i])return std::nullopt;
    }
    const auto word=[](const auto& b,unsigned at){return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];};
    const float fraction=1.f/(1u<<mip);
    const std::array<std::array<float,4>,3> fields{{{0,1,0,0},{fraction,1,1,0},{0,1-fraction,0,1}}};
    for(unsigned i=0;i<3;++i)for(unsigned c=0;c<4;++c)
        if(std::bit_cast<float>(word(draw.vertices,i*24+(c<2?c*4:16+(c-2)*4)))!=fields[i][c])return std::nullopt;
    // The unused word12 can contain NaN; it is deliberately not inspected.
    const auto read=[&](unsigned r){return gta5DrawRegister(draw,r);};
    const auto& s=bindings.surfaces[0];
    if(!s.object || s.object%4 || s.object>UINT32_MAX-39 || !s.header || s.header->size()!=40 ||
       s.tileCacheObject || (draw.deviceState[11068]&0x10))return std::nullopt;
    for(unsigned i=0;i<5;++i) {
        const auto& b=bindings.surfaces[i];
        if(b.object!=word(draw.deviceState,12816+4*i) || b.tileCacheObject!=word(draw.deviceState,13104+4*i) ||
           b.tileCacheObject || (i>0 && i<4 && b.object))return std::nullopt;
    }
    const auto info=word(*s.header,24),color=word(*s.header,28),dimensions=word(*s.header,36),pitch=info&0x3FFF;
    const auto scissor=nativeTranslatedScissor(draw);
    if(!scissor || (*scissor)!=std::array<unsigned,4>{0,0,d->width,d->height} || !nativeTranslatedViewport(draw) ||
       read(0x2000)!=info || read(0x2001)!=color || (info&0x3C000) || (color&~0x7FFu) || !pitch || pitch%80 ||
       pitch<d->width || (dimensions>>18)+1!=d->width || ((dimensions>>3)&0x7FFF)+1!=d->height)return std::nullopt;
    const std::array<float,4> expectedViewport{float(d->width)/2,float(d->width)/2,-float(d->height)/2,float(d->height)/2};
    for(unsigned i=0;i<4;++i)if(read(0x210F+i)!=std::bit_cast<unsigned>(expectedViewport[i]))return std::nullopt;
    if(!matchesNativeColorViewportRegisters([&](unsigned r)->std::optional<unsigned> {
        if(r==0x2000)return 0x14000500;if(r==0x2081)return 0;if(r==0x2082)return 0x2D00500;
        if(r==0x210F || r==0x2110)return 0x44200000;if(r==0x2111)return 0xC3B40000;if(r==0x2112)return 0x43B40000;
        return read(r);
    }))return std::nullopt;
    // SQ_PROGRAM_CNTL's PS register extent changes with the original kernel:
    // variants1/3/5 have 3/10/21 registers. Keep other bits exact too.
    if(read(0x2180)!=(program.abiVersion==9?0x10010201u:program.abiVersion==10?0x10010901u:0x10011401u))return std::nullopt;
    constexpr unsigned required[][2]{{0x2104,15},{0x2181,4},{0x2182,0x10001},
        {0x2183,0},{0x2184,0},{0x2200,0x700770},{0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x10},
        {0x2302,4},{0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& r:required)if(read(r[0])!=r[1])return std::nullopt;
    const unsigned tiles=pitch/80*((d->height+15)/16);if(tiles>2048-color)return std::nullopt;
    return NativeLodRectanglePlan{{s.object,d->width,d->height,pitch,color,tiles},mip,width,height};
}
inline bool nativeLodRectanglePositions(const Gta5InlineDraw& draw,const NativeLodRectanglePlan& plan,
        std::span<const std::array<float,4>> positions) {
    if(plan.outputMip<1 || plan.outputMip>3 || positions.size()!=3 ||
       !nativeTopLeftRectangleList(draw,positions,plan.width,plan.height))return false;
    const float fraction=1.f/(1u<<plan.outputMip);
    const std::array<std::array<float,4>,3> expected{{{-1,1,0,1},{2*fraction-1,1,0,1},{-1,1-2*fraction,0,1}}};
    return std::equal(positions.begin(),positions.end(),expected.begin());
}
}
