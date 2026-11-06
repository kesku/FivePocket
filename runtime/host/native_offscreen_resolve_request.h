#pragma once
#include "native_offscreen_resolve.h"
#include <cmath>

namespace recomp::gpu {
// 82388A38 adapter arguments, not the rearranged 823C2738 register ABI.
struct NativeOffscreenResolveRequest {
    std::uint32_t stack=0,flags=0,rectangle=0,destinationPoint=0,mip=0,slice=0,clearColor=0;
};
struct NativeOffscreenResolveArguments {
    std::optional<std::vector<std::uint8_t>> rectangle,point,color,stack92,stack100;
};
inline NativeOffscreenResolveArguments captureNativeOffscreenResolveArguments(
        const NativeOffscreenResolveRequest& r,const Gta5ResourceReader& reader) {
    NativeOffscreenResolveArguments out;
    if(!reader)return out;
    const auto read=[&](std::uint64_t address,unsigned size,unsigned alignment) {
        if(!address || address>UINT32_MAX-(size-1) || address%alignment)
            return std::optional<std::vector<std::uint8_t>>{};
        return reader(static_cast<std::uint32_t>(address),size);
    };
    // Collect every available argument even when an earlier one is unsupported.
    out.rectangle=read(r.rectangle,16,4);out.point=read(r.destinationPoint,8,4);
    // Original vector load aligns down to 16. Reject unaligned pointers instead
    // of silently consuming different bytes than the supplied argument.
    out.color=read(r.clearColor,16,16);
    out.stack92=read(std::uint64_t{r.stack}+92,4,4);
    out.stack100=read(std::uint64_t{r.stack}+100,4,4);
    return out;
}
inline std::optional<std::vector<std::uint8_t>> captureNativeResolveTextureHeader(
        std::uint32_t object,const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics) {
    if(!object || (object&3) || object>UINT32_MAX-51)return std::nullopt;
    const auto& reader=object<0x20000000?heap:graphics;
    if(!reader)return std::nullopt;
    const auto bytes=reader(object,52);
    return bytes && bytes->size()==52?bytes:std::nullopt;
}
enum NativeOffscreenResolveIssue : std::uint32_t {
    ResolveFlags=1u<<0,ResolveExtent=1u<<1,ResolveRectangle=1u<<2,
    ResolveDestinationPoint=1u<<3,ResolveSubresource=1u<<4,
    ResolveStack92=1u<<5,ResolveStack100=1u<<6,ResolveClearColor=1u<<7
};
struct NativeOffscreenResolvePlan {
    // Original format0 conversion: clamp, multiply by255, round nearest-even.
    // Packed first so Vulkan's float-to-UNORM conversion cannot change a tie.
    std::optional<std::array<std::uint8_t,4>> clearAfterCopy;
    unsigned width=0,height=0; // Validated top-left copy region, not target resize.
};
struct NativeOffscreenResolveAudit {
    std::uint32_t issues=0;
    std::optional<NativeOffscreenResolvePlan> plan;
};
inline NativeOffscreenResolveAudit auditNativeOffscreenResolveImpl(
        const NativeOffscreenDrawPlan& source,const NativeOffscreenResolveRequest& r,
        const NativeOffscreenResolveArguments& a,const XenosTextureDescription* destination,bool allowMipRegion) {
    NativeOffscreenResolveAudit out;NativeOffscreenResolvePlan plan;
    plan.width=source.width;plan.height=source.height;
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    const auto sized=[](const auto& b,unsigned n){return b && b->size()==n;};
    const auto zero=[&](const auto& b,unsigned n) {
        return sized(b,n) && std::all_of(b->begin(),b->end(),[](auto v){return !v;});
    };
    // Color0, default single-sample copy, optionally clear that color source.
    // 823C2C30 uses f1 ONLY with flag0x200 (depth clear), which is not admitted.
    if(r.flags&~0x100u)out.issues|=ResolveFlags;
    if(!source.width || !source.height || source.width>1280 || source.height>720)
        out.issues|=ResolveExtent;
    if(r.rectangle) {
        if((r.rectangle&3) || !sized(a.rectangle,16))out.issues|=ResolveRectangle;
        else {
            plan.width=word(*a.rectangle,8);plan.height=word(*a.rectangle,12);
            if(word(*a.rectangle,0)!=0 || word(*a.rectangle,4)!=0 || !plan.width || !plan.height ||
               plan.width>source.width || plan.height>source.height ||
               (!allowMipRegion && (plan.width!=source.width || plan.height!=source.height)))out.issues|=ResolveRectangle;
        }
    }
    if(r.destinationPoint && ((r.destinationPoint&3) || !zero(a.point,8)))
        out.issues|=ResolveDestinationPoint;
    const auto subresource=destination?planNativeOffscreenResolveDestination(*destination,r.mip):std::nullopt;
    if(r.slice || (r.mip && !subresource) || (destination && !subresource))out.issues|=ResolveSubresource;
    if(subresource && (subresource->storage.description.width!=plan.width ||
        subresource->storage.description.height!=plan.height))out.issues|=ResolveExtent;
    if(allowMipRegion && !destination)out.issues|=ResolveSubresource;
    if(!r.stack || r.stack>UINT32_MAX-95 || (r.stack&3) || !zero(a.stack92,4))out.issues|=ResolveStack92;
    if(!r.stack || r.stack>UINT32_MAX-103 || (r.stack&3) || !zero(a.stack100,4))out.issues|=ResolveStack100;
    if(r.flags&0x100) {
        // Region clear hardware rounds to eight pixels (823C2C68). Keep this
        // explicit path restricted to exact aligned extents until padding and
        // neighboring ownership for rounded clears are separately validated.
        if(allowMipRegion && (plan.width%8 || plan.height%8))out.issues|=ResolveClearColor;
        if(!r.clearColor || (r.clearColor&15) || !sized(a.color,16))out.issues|=ResolveClearColor;
        else {
            std::array<std::uint8_t,4> packed{};
            for(unsigned c=0;c<4;++c) {
                const auto bits=word(*a.color,4*c);float f;std::memcpy(&f,&bits,4);
                // Conservative finite [0,1] profile; do not guess NaN policy.
                if(!std::isfinite(f) || f<0 || f>1){out.issues|=ResolveClearColor;continue;}
                const volatile float scaled=f*255.f;
                const auto floor=static_cast<unsigned>(scaled);
                const float fraction=scaled-float(floor);
                packed[c]=floor+(fraction>.5f || (fraction==.5f && (floor&1)));
            }
            if(!(out.issues&ResolveClearColor))plan.clearAfterCopy=packed;
        }
    }
    if(!out.issues)out.plan=plan;
    return out;
}
inline NativeOffscreenResolveAudit auditNativeOffscreenResolve(
        const NativeOffscreenDrawPlan& source,const NativeOffscreenResolveRequest& r,
        const NativeOffscreenResolveArguments& a,const XenosTextureDescription* destination=nullptr) {
    return auditNativeOffscreenResolveImpl(source,r,a,destination,false);
}
// Separate opt-in. Legacy full-source audit above remains strict. No nonzero
// source origin or destination point, scaling, partial destination or copy with
// unavailable source state is admitted by this argument planner.
inline NativeOffscreenResolveAudit auditNativeOffscreenMipRegionResolve(
        const NativeOffscreenDrawPlan& source,const NativeOffscreenResolveRequest& r,
        const NativeOffscreenResolveArguments& a,const XenosTextureDescription& destination) {
    return auditNativeOffscreenResolveImpl(source,r,a,&destination,true);
}
}
