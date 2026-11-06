#pragma once
#include "gta5_color8_clear_plan.h"
#include "gta5_bound_surface_capture.h"
#include "native_convex_fan.h"
#include "native_mip_texture_capture.h"

namespace recomp::gpu {
// Narrow visible solid or BC3 mip fan: per-sample gamma storage, source*dstAlpha
// + destination RGB, replacement alpha; GEQUAL without depth writes, EQUAL/KEEP
// stencil. The owning target copies current depth/stencil into scratch;
// plan.depth.value is the historical initial clear, not the current contents.
// Owned binding headers, no EDRAM aliases, exact shader identity and complete
// position computation are independent caller obligations. Native sample
// locations are an explicit approximation, not an Xbox conformance claim.
inline bool nativeGammaZeroDepthTargetBindings(const Gta5InlineDraw& draw,
        const Gta5Color8ClearPlan& plan,const Gta5BoundSurfaceCapture& bindings) {
    if(draw.deviceState.size()!=13860 || !nativeTranslatedViewport(draw) ||
       plan.format!=1 || plan.samples!=2 || !plan.depth || plan.depth->format!=1 ||
       plan.depth->samples!=2 || plan.depth->width!=plan.width || plan.depth->height!=plan.height ||
       plan.depth->pitch!=plan.pitch || plan.depth->value!=*planExactXenosFloat24Clear(0,255))return false;
    if(!plan.object || plan.object%4 || !plan.depth->object || plan.depth->object%4 || plan.depth->object==plan.object ||
       !plan.width || !plan.height || plan.width>1280 || plan.height>720 || plan.pitch<plan.width || plan.pitch>16383 || plan.pitch%80 ||
       plan.baseTile>=2048 || plan.depth->baseTile>=2048)return false;
    const auto tiles=(plan.pitch/80)*((plan.height*2+15)/16);
    if(plan.tileCount!=tiles || plan.depth->tileCount!=tiles || tiles>2048-plan.baseTile || tiles>2048-plan.depth->baseTile ||
       (plan.baseTile<plan.depth->baseTile+tiles && plan.depth->baseTile<plan.baseTile+tiles))return false;
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
        (std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(draw.deviceState[11068]&0x10)return false;
    for(unsigned slot=0;slot<5;++slot) {
        const auto& surface=bindings.surfaces[slot];const auto expected=slot==0?plan.object:slot==4?plan.depth->object:0;
        if(surface.object!=expected || word(draw.deviceState,12816+4*slot)!=expected ||
           surface.tileCacheObject || word(draw.deviceState,13104+4*slot))return false;
        if(slot!=0 && slot!=4)continue;
        if(!surface.header || surface.header->size()!=40)return false;
        const auto info=word(*surface.header,24),format=word(*surface.header,28),dimensions=word(*surface.header,36);
        const auto base=slot?plan.depth->baseTile:plan.baseTile;
        if(info!=word(draw.deviceState,10368) || (info&0x3FFFFu)!=(plan.pitch|0x10000) ||
           format!=(base|0x10000) || format!=word(draw.deviceState,slot?10376:10372) ||
           (dimensions>>18)+1!=plan.width || ((dimensions>>3)&0x7FFF)+1!=plan.height)return false;
    }
    const auto scissor=nativeTranslatedScissor(draw);
    if(!scissor || *scissor!=std::array<std::uint32_t,4>{0,0,plan.width,plan.height})return false;
    const auto read=[&](unsigned r){return gta5DrawRegister(draw,r);};
    const auto real=[](std::uint32_t bits){float f;std::memcpy(&f,&bits,4);return f;};
    if(real(*read(0x210F))*2!=plan.width || real(*read(0x2110))*2!=plan.width ||
       real(*read(0x2111))*-2!=plan.height || real(*read(0x2112))*2!=plan.height)return false;
    return true;
}
inline bool nativeGammaFanState(const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5Color8ClearPlan& plan,const Gta5BoundSurfaceCapture& bindings,
        const NativeMipTextureCapture* mipTexture=nullptr) {
    if(program.abiVersion!=7 || !program.color8SourceClamped || program.indexed || program.directTriangles ||
       !draw.textures.empty() ||
       program.stride!=draw.stride || !nativeClippedFanState(draw,program.stride) ||
       !nativeGammaZeroDepthTargetBindings(draw,plan,bindings))return false;
    const auto slots=nativeTranslatedTextureSlots(program);
    if(mipTexture) {
        if(program.interpolators!=2 || slots!=std::vector<unsigned>{7})return false;
        const auto chain=planXenosUnpackedMipChain(mipTexture->fetch);
        const auto sampler=planNativeTextureSampler2D(mipTexture->fetch);
        const auto d=XenosTextureDescription::decode(mipTexture->fetch);
        // Opt-in BC3/unpacked/linear-clamp profile. Vulkan LOD and filter
        // precision are a native policy, not bit-exact Xenos conformance.
        if(!chain || !sampler || !d || d->format!=20 || !d->tiled ||
           sampler->clamp!=std::array<unsigned,2>{2,2} || sampler->magFilter!=1 ||
           sampler->minFilter!=1 || sampler->mipFilter!=1 || sampler->triClamp!=2 ||
           chain->baseBytes!=mipTexture->base.size() || chain->mipBytes!=mipTexture->mips.size() ||
           !chain->mipBytes || std::uint64_t{chain->baseBytes}+chain->mipBytes>64u*1024*1024)return false;
    } else if(program.interpolators!=1 || !slots.empty())return false;
    const auto read=[&](unsigned r){return gta5DrawRegister(draw,r);};
    if(read(0x2180)!=std::optional<std::uint32_t>{mipTexture?0x10110406u:0x10010104u} ||
       read(0x2182)!=std::optional<std::uint32_t>{mipTexture?0x30003u:0x10001u})return false;
    constexpr unsigned required[][2]{{0x2104,15},{0x210C,0xFFFF00},{0x210D,0xFFFF},
        {0x2113,0},{0x2114,0x3F800000},{0x2181,4},
        {0x2200,0x700263},{0x2201,0x1010A},{0x2202,0xAA000007},{0x2203,0x13},
        {0x2301,0x8001},{0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& r:required)if(read(r[0])!=std::optional<std::uint32_t>{r[1]})return false;
    return matchesNativeColorViewportRegisters([&](unsigned r) {
        if(r==0x2000)return std::optional<std::uint32_t>{0x14000500};
        if(r==0x2081)return std::optional<std::uint32_t>{0};
        if(r==0x2082)return std::optional<std::uint32_t>{0x2D00500};
        if(r==0x210F || r==0x2110)return std::optional<std::uint32_t>{0x44200000};
        if(r==0x2111)return std::optional<std::uint32_t>{0xC3B40000};
        if(r==0x2112)return std::optional<std::uint32_t>{0x43B40000};
        if(r==0x2113)return std::optional<std::uint32_t>{0xBF800000};
        if(r==0x2301)return std::optional<std::uint32_t>{0};
        return read(r);
    });
}
}
