#pragma once
#include "native_stencil_only_plan.h"
#include "native_geometry_sampler_plan.h"
#include "xenos_alpha_test.h"

namespace recomp::gpu {
// ABI34 has a real fragment stage. Never route it through the no-PS ABI25
// admission, or the RGBA offscreen ABI5 consumer. State/bytes are separate
// evidence from the trusted generated shader identity pack.
inline bool validateNativeSampledStencilProgram(const NativeTranslatedDrawProgram& p) {
    return p.abiVersion==34 && p.directTriangles && !p.indexed && p.stride==36 &&
        p.interpolators==1 && p.textureSlot==1 && nativeTranslatedTextureSlots(p)==std::vector<unsigned>{1} &&
        !p.vertexIdentity.empty() && !p.pixelIdentity.empty() &&
        p.vertexCode.size()>=5 && p.pixelCode.size()>=5 &&
        p.vertexCode.front()==0x07230203 && p.pixelCode.front()==0x07230203 &&
        !p.affinePositionXY && !p.color8SourceClamped && !p.cubeRectanglePwl &&
        !p.explicitLodRectangle && !p.depthOnlyRectangle && !p.rectangleMrt && !p.offsetSample &&
        !p.compositeRectangle && !p.screenSpaceRectangle && !p.screenStrip && !p.worldStrip &&
        !p.fixed16RectangleDepth && !p.fixed16VertexRegisterCount && !p.geometryFixed16GradientDepth &&
        !p.geometryDepth20e4 && !p.geometrySampleDepth && p.geometryColor8Format==UINT32_MAX &&
        !p.geometryPixelEffects && !p.geometryDepthColorWitness && !p.geometryCentroidMask &&
        !p.geometryMrtOutputs && p.geometryParameterRegister==UINT32_MAX &&
        p.geometryMrtFormats==std::array<unsigned,4>{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX} &&
        p.geometryMrtBlendControls==std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001};
}
struct NativeSampledStencilPlan {
    NativeStencilOnlyPlan target;
    XenosTextureDescription texture;
};
inline bool sameNativeSampledStencilInputs(const Gta5InlineDraw& a,const Gta5BoundSurfaceCapture& ab,
        const Gta5InlineDraw& b,const Gta5BoundSurfaceCapture& bb) {
    if(a.primitive!=b.primitive || a.vertexCount!=b.vertexCount || a.stride!=b.stride ||
       a.device!=b.device || a.vertexAddress!=b.vertexAddress || a.vertexShader!=b.vertexShader ||
       a.pixelShader!=b.pixelShader || a.vertices!=b.vertices || a.deviceState!=b.deviceState ||
       a.vertexConstants!=b.vertexConstants || a.pixelConstants!=b.pixelConstants ||
       a.scissorRegisters!=b.scissorRegisters || a.textures.size()!=b.textures.size())return false;
    for(unsigned i=0;i<a.textures.size();++i) {
        const auto& x=a.textures[i];const auto& y=b.textures[i];
        if(x.slot!=y.slot || x.fetch!=y.fetch || x.literalZero!=y.literalZero || x.source!=y.source)return false;
    }
    for(unsigned i=0;i<5;++i) {
        const auto& x=ab.surfaces[i];const auto& y=bb.surfaces[i];
        if(x.object!=y.object || x.header!=y.header || x.tileCacheObject!=y.tileCacheObject ||
           x.tileCacheHeader!=y.tileCacheHeader)return false;
    }
    return true;
}
inline std::optional<NativeSampledStencilPlan> planNativeSampledStencil(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5BoundSurfaceCapture& bindings,const char** why=nullptr) {
    const auto no=[&](const char* reason){if(why)*why=reason;return std::optional<NativeSampledStencilPlan>{};};
    if(!validateNativeSampledStencilProgram(program) || draw.primitive!=4 ||
       draw.vertexCount!=3 || draw.stride!=36 || draw.vertices.size()!=108 ||
       draw.deviceState.size()!=13860 || !draw.vertexShader || !draw.pixelShader)return no("program-profile");
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    // Exact captured single-sample state. RGB7e3 colour is BOUND but never
    // written; D24FS8 low byte is the sole output. Alpha reference is register
    // 0x210E, NOT 0x2107 (a clip-plane value in this capture).
    constexpr unsigned registers[][2]{
        {0x2000,0x14000500},{0x2001,0x000302D0},{0x2002,0x00010000},
        {0x2104,0},{0x210D,0x00101010},{0x210E,0},
        {0x210F,0x44200000},{0x2110,0x44200000},{0x2111,0xC3B40000},{0x2112,0x43B40000},
        {0x2113,0xBF800000},{0x2114,0x3F800000},
        {0x2180,0x10010101},{0x2181,4},{0x2182,0x10001},{0x2183,0},{0x2184,0},
        {0x2200,0x00708771},{0x2201,0x10001},{0x2202,0xAA00000B},{0x2203,0x101C},
        {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},
        {0x2302,4},{0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& r:registers)if(gta5DrawRegister(draw,r[0])!=r[1])return no("register-profile");
    if(draw.deviceState[11068]&0x10)return no("binning");
    const auto scissor=nativeTranslatedScissor(draw);
    const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || !viewport || !(*scissor)[2] || !(*scissor)[3])return no("viewport-scissor");
    // The scissor is a write rectangle, NOT the allocation's height. Preserve
    // all720 rows even when only384 are rasterized.
    for(unsigned i=0;i<5;++i) {
        const auto& b=bindings.surfaces[i];
        if(b.object!=word(draw.deviceState,12816+4*i))return no("binding-shadow");
        if(i>0 && i<4){if(b.object)return no("extra-target");continue;}
        if(!b.object || b.object%4 || b.object>UINT32_MAX-39 || !b.header || b.header->size()!=40)
            return no("surface-header");
        if(word(*b.header,24)!=0x14000500 || word(*b.header,28)!=(i?0x10000u:0x302D0u) ||
           word(*b.header,36)!=0x13FC1678)return no("surface-layout");
    }
    if(bindings.surfaces[0].object==bindings.surfaces[4].object)return no("surface-object-alias");
    if(draw.textures.size()!=1 || draw.textures[0].slot!=1 || draw.textures[0].literalZero)return no("texture-slot");
    const auto& t=draw.textures[0];
    // Tiled RGBA8, BGRA view swizzle (alpha unchanged), clamp XY, point min/mag/mip, no exponent
    // bias and only mip0. Texture address is allocation-owned input, not fixed.
    if(t.fetch[0]!=0x8A004802 || (t.fetch[1]&0xFFF)!=0x86 ||
       t.fetch[2]!=0x0059E4FF || t.fetch[3]!=0xC14 || t.fetch[4] || t.fetch[5]!=0x210)
        return no("texture-profile");
    const auto desc=XenosTextureDescription::decode(t.fetch);
    const auto layout=desc?planXenosTexture2D(*desc):std::nullopt;
    if(!layout || !desc->baseAddress || t.source.size()!=layout->sourceBytes ||
       !planNativeGeometrySampler2D(t.fetch))return no("texture-storage-sampler");
    NativeStencilOnlyPlan target;
    target.colorObject=bindings.surfaces[0].object;target.depthObject=bindings.surfaces[4].object;
    target.width=target.pitch=1280;target.height=720;target.depthBase=0;
    target.minDepth=1;target.maxDepth=0;
    target.state=*decodeXenosDepthStencilState(0x00708771,0x00101010,0x00101010);
    if(why)*why="planned";
    return NativeSampledStencilPlan{target,*desc};
}
}
