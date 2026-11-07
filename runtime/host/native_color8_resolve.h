#pragma once
#include "native_offscreen_resolve_request.h"
#include "native_offscreen_resolve.h"
#include "gta5_color8_clear_plan.h"
#include "gta5_bound_surface_capture.h"
#include <span>

namespace recomp::gpu {
// Explicit native policy: decode stored PWL RGB to canonical UNORM10, average
// every sample, then round to linear UNORM8 (nearest-even). Alpha averages its
// ordinary UNORM8 values. No sRGB or destination PWL encoding. This follows
// the operation order in xenia-canary 3ba9c86 resolve.xesli, not its floating
// point rounding, and is NOT Xbox capture conformance. Master95a5c3ee averages
// encoded gamma bytes and explicitly leaves gamma-correct resolves as a TODO.
// Using integers makes all ties deterministic across host architectures.
inline std::optional<std::vector<std::uint8_t>> resolveNativeColor8LinearAverage(
        unsigned width,unsigned height,unsigned samples,unsigned format,std::span<const std::uint8_t> encoded) {
    if(!width || !height || width>1280 || height>720 || format>1 ||
       (samples!=1 && samples!=2 && samples!=4) ||
       encoded.size()!=std::uint64_t{width}*height*samples*4)return std::nullopt;
    std::vector<std::uint8_t> result(std::size_t{width}*height*4);
    for(std::size_t p=0;p<result.size()/4;++p)for(unsigned c=0;c<4;++c) {
        const bool gamma=format==1 && c<3;unsigned sum=0;
        for(unsigned s=0;s<samples;++s) {
            const auto b=encoded[(p*samples+s)*4+c];sum+=gamma?xenosPwlGammaByteToUnorm10(b):b;
        }
        const unsigned numerator=gamma?sum*255:sum,denominator=gamma?1023*samples:samples;
        const unsigned floor=numerator/denominator,remainder=numerator%denominator;
        result[p*4+c]=floor+(remainder*2>denominator || (remainder*2==denominator && (floor&1)));
    }
    return result;
}

struct NativeColor8ResolveDestination {
    XenosTextureDescription original;
    XenosTexture2DLayout storage;
};
// Storage-only projection of mip0. Original823C2834..823C2940 uses a
// normal 2D copy for cube faces; only ThreeD sets COPY_DEST_INFO.array. A
// nonzero cube face goes through82343AD8. The bounded power-of-two profile
// below was checked against that original routine and its original helpers.
// Stripping sampling sign/dimension here does NOT enable cube/gamma sampling.
// Only this face's byte extent is read/written; other faces are never claimed.
inline std::optional<NativeColor8ResolveDestination> planNativeColor8ResolveFace(
        const XenosTextureDescription& supplied,unsigned faceIndex) {
    const auto decoded=XenosTextureDescription::decode(supplied.fetch);if(!decoded)return std::nullopt;
    const auto& d=*decoded;
    const bool cube=d.dimension==XenosTextureDescription::Dimension::Cube;
    if((!cube && d.dimension!=XenosTextureDescription::Dimension::TwoDOrStacked) || d.stacked ||
       (!cube && faceIndex) || (cube && (faceIndex>=6 || d.width!=d.height || (d.fetch[2]>>26)!=5 ||
        d.width<32 || d.width>512 || (d.width&(d.width-1)) || d.pitchTexels!=d.width)) ||
       d.format!=6 || !d.tiled || d.integerComponents || d.exponentBias ||
       d.mipMin || d.mipMax || d.mipAddress || d.packedMips || (d.fetch[3]&0x80000000u) ||
       (d.sign!=std::array<unsigned,4>{0,0,0,0} && d.sign!=std::array<unsigned,4>{3,3,3,0}) ||
       (d.swizzle!=std::array<unsigned,4>{0,1,2,3} && d.swizzle!=std::array<unsigned,4>{2,1,0,3}))return std::nullopt;
    // Destination number format is UNORM for sign3, just as sign0, in the
    // original823C29E8..823C2A84. Fetch PWL sign is a later sampling operation.
    auto raw=d.fetch;raw[0]&=~0x3FCu;raw[2]&=0x03FFFFFFu;raw[5]=(raw[5]&~0x600u)|0x200u;
    if(cube) {
        // For this profile rowBytes=width*4, paddedHeight=height, and their
        // product is already4KiB aligned. Original82343F28 adds slice*stride.
        const auto stride=std::uint64_t{d.width}*d.height*4;
        const auto address=std::uint64_t{d.baseAddress}+faceIndex*stride;
        if(address+stride>0x100000000ULL)return std::nullopt;
        raw[1]=(raw[1]&0xFFFu)|static_cast<std::uint32_t>(address);
    }
    const auto view=XenosTextureDescription::decode(raw);const auto layout=view?planXenosTexture2D(*view):std::nullopt;
    if(!layout)return std::nullopt;
    return NativeColor8ResolveDestination{d,*layout};
}
inline std::optional<NativeColor8ResolveDestination> planNativeColor8ResolveFace0(const XenosTextureDescription& supplied) {
    return planNativeColor8ResolveFace(supplied,0);
}
inline std::optional<std::vector<std::uint8_t>> packNativeColor8ResolveFace(
        const Gta5Color8ClearPlan& source,const XenosTextureDescription& destination,
        std::span<const std::uint8_t> samples,const std::vector<std::uint8_t>& previous,unsigned faceIndex) {
    const auto d=planNativeColor8ResolveFace(destination,faceIndex);
    if(!d || d->original.width!=source.width || d->original.height!=source.height)return std::nullopt;
    const auto rgba=resolveNativeColor8LinearAverage(source.width,source.height,source.samples,source.format,samples);
    if(!rgba)return std::nullopt;
    return packNativeOffscreenResolve({source.object,source.width,source.height,source.pitch,source.baseTile,source.tileCount},
        d->storage.description,*rgba,previous);
}
inline std::optional<std::vector<std::uint8_t>> packNativeColor8ResolveFace0(
        const Gta5Color8ClearPlan& source,const XenosTextureDescription& destination,
        std::span<const std::uint8_t> samples,const std::vector<std::uint8_t>& previous) {
    return packNativeColor8ResolveFace(source,destination,samples,previous,0);
}
// Shared adapter argument validation, with the source's actual clear packing.
// In particular gamma clear RGB uses the original CPU's 10-bit first rounding,
// not the UNORM packing used by the single-sample format0 offscreen contract.
inline NativeOffscreenResolveAudit auditNativeColor8ResolveArguments(const Gta5Color8ClearPlan& source,
        const NativeOffscreenResolveRequest& request,const NativeOffscreenResolveArguments& arguments,
        const XenosTextureDescription* destination=nullptr) {
    auto shape=request;
    if(destination && planNativeColor8ResolveFace(*destination,request.slice))shape.slice=0;
    auto out=auditNativeOffscreenResolve({source.object,source.width,source.height,source.pitch,source.baseTile,source.tileCount},shape,arguments);
    if(source.format>1 || source.samples!=2) {out.issues|=ResolveFlags;out.plan.reset();}
    if(out.plan && out.plan->clearAfterCopy) {
        std::array<float,4> color{};
        for(unsigned i=0;i<4;++i) {
            const auto* b=arguments.color->data()+4*i;
            const auto bits=(std::uint32_t{b[0]}<<24)|(std::uint32_t{b[1]}<<16)|(std::uint32_t{b[2]}<<8)|b[3];
            std::memcpy(&color[i],&bits,4);
        }
        const auto packed=packGta5Color8Clear(color,source.format);
        if(!packed){out.issues|=ResolveClearColor;out.plan.reset();}else out.plan->clearAfterCopy=*packed;
    }
    return out;
}
// Resource/CPU-shadow validation, separate from lifetime ownership and actual
// completed sample readback. Depth may be NONUNIFORM and must stay untouched.
inline bool nativeColor8ResolveSourceMatches(const Gta5Color8ClearPlan& p,
        const std::vector<std::uint8_t>& state,const Gta5BoundSurfaceCapture& bindings) {
    if(state.size()!=13860 || (state[11068]&0x10) || p.format!=1 || p.samples!=2 || !p.depth ||
       !p.object || p.object%4 || !p.depth->object || p.depth->object%4 || p.depth->object==p.object ||
       !p.width || !p.height || p.width>1280 || p.height>720 || p.pitch<p.width || p.pitch>16383 || p.pitch%80 ||
       p.depth->width!=p.width || p.depth->height!=p.height || p.depth->samples!=2 ||
       p.depth->pitch!=p.pitch || p.depth->format!=1 || p.baseTile>=2048 || p.depth->baseTile>=2048)return false;
    const auto tiles=p.pitch/80*((p.height*2+15)/16);
    if(!tiles || p.tileCount!=tiles || p.depth->tileCount!=tiles || tiles>2048-p.baseTile || tiles>2048-p.depth->baseTile ||
       (p.baseTile<p.depth->baseTile+tiles && p.depth->baseTile<p.baseTile+tiles))return false;
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
        (std::uint32_t{b[at+2]}<<8)|b[at+3];};
    for(unsigned slot=0;slot<5;++slot) {
        const auto& s=bindings.surfaces[slot];const auto expected=slot==0?p.object:slot==4?p.depth->object:0;
        if(s.object!=expected || word(state,12816+4*slot)!=expected || s.tileCacheObject || word(state,13104+4*slot))return false;
        if(!expected)continue;
        if(!s.header || s.header->size()!=40)return false;
        const auto info=word(*s.header,24),format=word(*s.header,28),dimensions=word(*s.header,36);
        if(info!=word(state,10368) || (info&0x3FFFFu)!=(p.pitch|0x10000) ||
           format!=(0x10000|(slot?p.depth->baseTile:p.baseTile)) || format!=word(state,slot?10376:10372) ||
           (dimensions>>18)+1!=p.width || ((dimensions>>3)&0x7FFF)+1!=p.height)return false;
    }
    return true;
}
}
