#pragma once
#include "gta5_fixed16_clear_plan.h"
#include "gta5_bound_surface_capture.h"
#include "native_offscreen_resolve_request.h"
#include <bit>

namespace recomp::gpu {
// Native fixed16 producer -> binary16 texture conversion. The pinned ReX
// resolve_full_16bpp SPIR-V decodes signed RG16 with a float32 multiplication
// by32/32767, clamps the lower end to-32, then packs the R channel to half.
// Vulkan permits implementation-defined PackHalf2x16 rounding. On Thor/Adreno740
// the reference operation matches RTZ for all65536 inputs (RNE differs30718),
// with identical float32 scale results. Use that explicitly measured policy;
// this is not a claim of independently measured Xbox resolve bit fidelity.
// Every nonzero input is a normal half in[2^-10,32], so no NaN/Inf/subnormal
// policy is needed. Do NOT use the PPC VMX converter (that operation is RTZ).
inline float nativeFixed16ResolveValue(std::uint16_t encoded) {
    const auto signedValue=encoded<32768?int(encoded):int(encoded)-65536;
    const volatile float scaled=float(signedValue)*(32.0f/32767.0f);
    return std::max(float(scaled),-32.0f);
}
inline std::uint16_t nativeFixed16ResolveHalfRne(std::uint16_t encoded) {
    const auto bits=std::bit_cast<std::uint32_t>(nativeFixed16ResolveValue(encoded));
    const auto magnitude=bits&0x7FFFFFFFu;
    if(!magnitude)return 0;
    const auto rounded=(magnitude-0x38000000u+0xFFFu+((magnitude>>13)&1u))>>13;
    return static_cast<std::uint16_t>(((bits>>16)&0x8000u)|rounded);
}
inline std::uint16_t nativeFixed16ResolveHalfRtz(std::uint16_t encoded) {
    const auto bits=std::bit_cast<std::uint32_t>(nativeFixed16ResolveValue(encoded));
    const auto magnitude=bits&0x7FFFFFFFu;
    return magnitude?static_cast<std::uint16_t>(((bits>>16)&0x8000u)|((magnitude-0x38000000u)>>13)):0;
}
struct NativeFixed16ResolvePlan {
    XenosTexture2DLayout storage;
    unsigned width=0,height=0;
};
inline bool nativeFixed16ResolveSourceMatches(const Gta5Fixed16ClearPlan& p,
        const std::vector<std::uint8_t>& state,const Gta5BoundSurfaceCapture& bindings) {
    if(state.size()!=13860 || p.format!=4 || p.samples!=1)return false;
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    // Same fully inactive tiling domain as the completed fixed16 producer.
    // Original82476080 keeps cache pointers after ending tiling. Nonzero cache
    // pointers alone neither authorize cached work nor invalidate a live source.
    if(state[11068] || word(state,11060) || word(state,11064) || word(state,13088) ||
       word(state,13092) || word(state,13100))return false;
    for(unsigned slot=0;slot<5;++slot) {
        const auto& s=bindings.surfaces[slot];
        const auto expected=slot==0?p.object:slot==4 && p.depth?p.depth->object:0;
        if(s.object!=expected || word(state,12816+slot*4)!=expected ||
           s.tileCacheObject!=word(state,13104+slot*4) || (expected && expected==s.tileCacheObject))return false;
    }
    const auto& s=bindings.surfaces[0];
    if(!s.header || s.header->size()!=40)return false;
    const auto info=word(*s.header,24),color=word(*s.header,28),dimensions=word(*s.header,36);
    return info==word(state,10368) && (info&0x3FFFFu)==p.pitch && color==(p.baseTile|0x40000u) &&
        color==word(state,10372) && (dimensions>>18)+1==p.width && ((dimensions>>3)&0x7FFF)+1==p.height;
}
// Original82388A38 ABI, source0/default single-sample copy, mip0/slice0 and
// no clears or offsets. Explicit full rectangle or the original null default.
// f1 is unused without the depth-clear flag; the actual call supplies0, not1.
inline std::optional<NativeFixed16ResolvePlan> planNativeFixed16Resolve(
        const Gta5Fixed16ClearPlan& p,const NativeOffscreenResolveRequest& r,
        const NativeOffscreenResolveArguments& a,const XenosTextureDescription& destination) {
    const auto storage=planXenosTexture2D(destination);
    if(!storage || storage->description.format!=30 || p.format!=4 || p.samples!=1 ||
       !p.object || p.object%4 || !p.width || !p.height || p.width>1280 || p.height>720 ||
       p.pitch<p.width || p.pitch>16383 || p.pitch%80 || p.baseTile>=2048 ||
       p.tileCount!=(p.pitch/80)*((p.height+15)/16) || p.tileCount>2048-p.baseTile ||
       r.flags || r.mip || r.slice || storage->description.width!=p.width || storage->description.height!=p.height)
        return std::nullopt;
    const auto zero=[](const auto& b,unsigned size) {
        return b && b->size()==size && std::all_of(b->begin(),b->end(),[](auto v){return !v;});
    };
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    if(!r.stack || (r.stack&3) || r.stack>UINT32_MAX-103 || !zero(a.stack92,4) || !zero(a.stack100,4) ||
       (r.destinationPoint && ((r.destinationPoint&3) || !zero(a.point,8))))return std::nullopt;
    if(r.rectangle && ((r.rectangle&3) || !a.rectangle || a.rectangle->size()!=16 ||
       word(*a.rectangle,0) || word(*a.rectangle,4) || word(*a.rectangle,8)!=p.width || word(*a.rectangle,12)!=p.height))
        return std::nullopt;
    return NativeFixed16ResolvePlan{*storage,p.width,p.height};
}
// Source bytes are the real completed native RG16_SNORM image readback,
// little-endian R,G per pixel. Destination swizzle does not alter stored R.
// Only owned previous storage is modified; tiled padding remains unchanged.
inline std::optional<std::vector<std::uint8_t>> packNativeFixed16Resolve(
        const NativeFixed16ResolvePlan& p,const std::vector<std::uint8_t>& encodedRG,
        const std::vector<std::uint8_t>& previous) {
    const auto actual=planXenosTexture2D(p.storage.description);
    if(!actual || actual->description.format!=30 || p.width!=actual->description.width ||
       p.height!=actual->description.height || p.storage.sourceBytes!=actual->sourceBytes ||
       p.storage.linearBytes!=actual->linearBytes || p.storage.bytesPerTexel!=2 ||
       encodedRG.size()!=std::uint64_t{p.width}*p.height*4 || previous.size()!=actual->sourceBytes)return std::nullopt;
    std::vector<std::uint8_t> linear(actual->linearBytes);
    for(std::size_t i=0;i<linear.size()/2;++i) {
        const auto half=nativeFixed16ResolveHalfRtz(std::uint16_t(encodedRG[i*4])|(std::uint16_t(encodedRG[i*4+1])<<8));
        linear[i*2]=half&255;linear[i*2+1]=half>>8;
    }
    return storeXenosTexture2D(actual->description,linear,previous);
}
}
