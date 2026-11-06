#pragma once
#include "native_gamma_fan_state.h"
#include "native_axis_quad.h"
#include "native_texture_sampler.h"

namespace recomp::gpu {
// One textured quad-list depth writer on a target originating at zero/FF.
// Replace RGB/alpha, GREATER depth, stencil disabled,
// CCW/back culling, center interpolation, no shader depth export or kill.
// Coverage and target-content policy remain caller obligations. The legacy
// scratch consumer requires uniform depth and rejects positive underflow;
// the explicit sample-depth consumer loads current persistent depth instead.
inline bool nativeGammaQuadState(const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5Color8ClearPlan& plan,const Gta5BoundSurfaceCapture& bindings) {
    if(program.abiVersion!=6 || program.color8SourceClamped || program.indexed || program.directTriangles || program.affinePositionXY ||
       program.interpolators!=2 || program.stride!=8 || program.textureSlot!=3 ||
       nativeTranslatedTextureSlots(program)!=std::vector<unsigned>{3} ||
       !nativeQuadPositionState(draw,program.stride) || !nativeGammaZeroDepthTargetBindings(draw,plan,bindings) ||
       draw.textures.size()!=1 || draw.textures[0].slot!=3 || draw.textures[0].literalZero)return false;
    const auto& texture=draw.textures[0];
    const auto description=XenosTextureDescription::decode(texture.fetch);
    const auto layout=description?planXenosTexture2D(*description):std::nullopt;
    const auto sampler=planNativeTextureSampler2D(texture.fetch);
    if(!description || !layout || !sampler || description->format!=18 || description->mipMin || description->mipMax ||
       sampler->magFilter!=1 || sampler->minFilter!=1 || sampler->mipFilter || sampler->clamp!=std::array<unsigned,2>{2,2} ||
       texture.source.size()!=layout->sourceBytes)return false;
    // 2181: SC_SAMPLE_CNTL=1 (centers only); 2182's sample-pattern bits
    // therefore do not request centroid qualifiers. Interpolators are smooth.
    // RB_DEPTHCONTROL below disables stencil entirely. The original draw can
    // change its front reference byte (210D) without affecting any comparison
    // or write. Keep all mask/reserved bits exact; do NOT admit stencil enable.
    const auto frontStencil=gta5DrawRegister(draw,0x210D);
    if(!frontStencil || (*frontStencil&0xFFFFFF00u)!=0xFFFF00u)return false;
    constexpr unsigned required[][2]{{0x200E,0},{0x200F,0x20002000},{0x2100,0xFFFFFF},{0x2101,0},{0x2102,0},
        {0x2104,15},{0x210C,0xFFFF00},{0x2113,0x3F800000},{0x2114,0},
        {0x2180,0x10110401},{0x2181,4},{0x2182,0x30003},
        {0x2200,0x700746},{0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x13},
        {0x2209,0x10001},{0x220A,0x10001},{0x220B,0x10001},
        {0x2301,0x8001},{0x2302,4},{0x2303,0x40000000},{0x2304,0x3F800000},{0x2305,0x40000000},{0x2306,0x3F800000},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& r:required)if(gta5DrawRegister(draw,r[0])!=std::optional<std::uint32_t>{r[1]})return false;
    return true;
}
}
