#pragma once
#include "native_offscreen_draw_plan.h"
#include "native_texture_sampler.h"
#include "native_rectangle_list.h"
namespace recomp::gpu {
// ABI8, one float4 interpolator, six-faced base texture, post-filter PWL RGB,
// complete replacement into single-sample RGBA8. Bound depth is disabled and
// not attached/read by Vulkan. Alias invalidation still belongs to the owner.
inline std::optional<NativeOffscreenDrawPlan> planNativeCubeRectangleDraw(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5BoundSurfaceCapture& bindings) {
    if(program.abiVersion!=8 || !program.cubeRectanglePwl || program.explicitLodRectangle || program.indexed || program.directTriangles ||
       program.affinePositionXY || program.color8SourceClamped || program.interpolators!=1 ||
       program.stride!=draw.stride || draw.stride!=24 || draw.primitive!=8 || draw.vertexCount<3 ||
       draw.vertexCount>384 || draw.vertexCount%3 || draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride ||
       draw.deviceState.size()!=13860 || draw.textures.size()!=1)return std::nullopt;
    const auto slots=nativeTranslatedTextureSlots(program);const auto& t=draw.textures[0];
    if(slots.size()!=1 || slots[0]!=program.textureSlot || slots[0]!=t.slot || slots[0]>=32 || t.literalZero)return std::nullopt;
    const auto d=XenosTextureDescription::decode(t.fetch);const auto sampler=planNativeTextureSamplerCubeBase(t.fetch);
    if(!d || !sampler || sampler->minFilter!=1 || d->sign!=std::array<unsigned,4>{3,3,3,0} ||
       t.source.size()!=std::uint64_t{d->width}*d->height*4*6)return std::nullopt;
    // The admitted original VS exports float2 at offsets16/20 and a shared
    // hemisphere sign at8. Its PS constructs (2u,2v,(1-u*u-v*v)*sign), whose
    // length never vanishes for sign=+/-1. Validate only FETCHED fields: the
    // unused padding word12 in the actual stream contains NaN and is not read.
    if(draw.pixelConstants[255*4]!=0x3FC00000 || draw.pixelConstants[255*4+1]!=0x3F800000)return std::nullopt;
    const auto inputFloat=[&](unsigned at){unsigned bits=0;for(unsigned b=0;b<4;++b)bits=(bits<<8)|draw.vertices[at+b];
        float f;std::memcpy(&f,&bits,4);return f;};
    for(unsigned i=0;i<draw.vertexCount;++i) {
        const auto sign=inputFloat(i*24+8);
        if((sign!=1 && sign!=-1) || sign!=inputFloat((i/3)*72+8))return std::nullopt;
        for(unsigned at:{16u,20u}) {const auto uv=inputFloat(i*24+at);if(!std::isfinite(uv) || std::abs(uv)>1)return std::nullopt;}
    }
    const auto read=[&](unsigned r){return gta5DrawRegister(draw,r);};
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto& s=bindings.surfaces[0];
    if(!s.object || s.object%4 || s.object>UINT32_MAX-39 || !s.header || s.header->size()!=40 ||
       s.tileCacheObject || (draw.deviceState[11068]&0x10))return std::nullopt;
    for(unsigned i=0;i<5;++i) {
        const auto& bound=bindings.surfaces[i];
        if(bound.object!=word(draw.deviceState,12816+4*i) || bound.tileCacheObject!=word(draw.deviceState,13104+4*i) ||
           bound.tileCacheObject || (i>0 && i<4 && bound.object))return std::nullopt;
    }
    const auto info=word(*s.header,24),color=word(*s.header,28),dimensions=word(*s.header,36),pitch=info&0x3FFF;
    const auto scissor=nativeTranslatedScissor(draw);const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || !viewport || (*scissor)[0] || (*scissor)[1] || !(*scissor)[2] || !(*scissor)[3] ||
       read(0x2000)!=info || read(0x2001)!=color || (info&0x3C000) || (color&~0x7FFu) || !pitch || pitch%80 ||
       pitch<(*scissor)[2] || (dimensions>>18)+1!=(*scissor)[2] || ((dimensions>>3)&0x7FFF)+1!=(*scissor)[3])return std::nullopt;
    if(!matchesNativeColorViewportRegisters([&](unsigned r)->std::optional<unsigned> {
        if(r==0x2000)return 0x14000500;
        if(r==0x2081)return 0;if(r==0x2082)return 0x2D00500;
        if(r==0x210F || r==0x2110)return 0x44200000;
        if(r==0x2111)return 0xC3B40000;if(r==0x2112)return 0x43B40000;
        return read(r);
    }))return std::nullopt;
    constexpr unsigned required[][2]{{0x2104,15},{0x2180,0x10010101},{0x2181,4},{0x2182,0x10001},
        {0x2183,0},{0x2184,0},{0x2200,0x700770},{0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x10},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& r:required)if(read(r[0])!=r[1])return std::nullopt;
    const auto tiles=pitch/80*(((*scissor)[3]+15)/16);
    if(tiles>2048-color)return std::nullopt;
    return NativeOffscreenDrawPlan{s.object,(*scissor)[2],(*scissor)[3],pitch,color,tiles};
}
}
