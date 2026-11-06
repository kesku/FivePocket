#pragma once
#include "native_color8_clear_execution.h"
#include "native_offscreen_resolve_request.h"
#include "gta5_bound_surface_capture.h"

namespace recomp::gpu {
// The single-sample, clear-only source owns its pixels independently of the
// cached presentation image. This is not an extension of the MSAA gamma path.
struct NativeSingleColor8ResolvePlan {
    NativeOffscreenDrawPlan source;
    NativeOffscreenResolveDestination destination;
};
inline std::optional<NativeSingleColor8ResolvePlan> planNativeSingleColor8Resolve(
        const Gta5Color8ClearPlan& p,bool ownsPresentation,unsigned displayObject,
        bool completedUniformOwner,const std::vector<std::uint8_t>& state,
        const Gta5BoundSurfaceCapture& bindings,const std::vector<std::uint8_t>& completedHeader,
        const XenosTextureDescription& destination,const NativeOffscreenResolveRequest& request,
        const NativeOffscreenResolveArguments& arguments) {
    if(!ownsPresentation || !displayObject || !completedUniformOwner || p.object==displayObject ||
       p.format || p.samples!=1 || p.depth || color8ClearIntersects(p,720,720) ||
       !p.object || p.object%4 || !p.width || !p.height || p.width>1280 || p.height>720 ||
       p.width%80 || p.height%16 || p.pitch<p.width || p.pitch>16383 || p.pitch%80 ||
       p.baseTile>=2048 || state.size()!=13860 || completedHeader.size()!=40)return std::nullopt;
    const auto count=nativeOffscreenTileCount(p.pitch,p.height,0);
    if(!count || *count!=p.tileCount || p.tileCount>2048-p.baseTile)return std::nullopt;
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    bool allCached=true;
    for(unsigned i=0;i<5;++i) {
        const auto& s=bindings.surfaces[i];
        if(s.object!=(i?0:p.object) || s.object!=word(state,12816+4*i) ||
           s.tileCacheObject!=word(state,13104+4*i))return std::nullopt;
        if(s.object && s.object!=s.tileCacheObject)allCached=false;
    }
    if((state[11068]&0x10) || ((state[11068]&0x20) && allCached) ||
       !bindings.surfaces[0].header || *bindings.surfaces[0].header!=completedHeader)return std::nullopt;
    const auto info=word(completedHeader,24),color=word(completedHeader,28),extent=word(completedHeader,36);
    if(info!=word(state,10368) || (info&0x3FFFF)!=p.pitch || color!=p.baseTile ||
       color!=word(state,10372) || (extent>>18)+1!=p.width ||
       ((extent>>3)&0x7FFF)+1!=p.height)return std::nullopt;
    // Original f1 is consumed only for a depth clear. This path admits NO clear,
    // sample selection, mip/face selection or partial source rectangle.
    if(request.flags || request.mip || request.slice)return std::nullopt;
    NativeOffscreenDrawPlan source{p.object,p.width,p.height,p.pitch,p.baseTile,p.tileCount};
    const auto audit=auditNativeOffscreenResolve(source,request,arguments,&destination);
    const auto selected=planNativeOffscreenResolveDestination(destination,0);
    if(!audit.plan || audit.plan->clearAfterCopy || !selected)return std::nullopt;
    return NativeSingleColor8ResolvePlan{source,*selected};
}

// Caller holds ONE exclusive guest gate. The source comes from actual fenced
// clear readback, then agrees with the completed physical owner. The destination
// is copied only after full current-state revalidation; padding is preserved.
template<class Current,class Publish>
bool executeNativeSingleColor8Resolve(const NativeSingleColor8ResolvePlan& plan,
        const std::vector<std::uint8_t>& rgba,const std::vector<std::uint8_t>& before,
        NativeEdram32Mutation& mutation,Current&& current,Publish&& publish,const char** why=nullptr) {
    const auto no=[&](const char* reason){if(why)*why=reason;return false;};
    if(!current())return no("single-color8-resolve-inputs-before");
    const auto& s=plan.source;
    const auto physical=mutation.candidate().readColorWords(s.baseTile,s.pitch,s.width,s.height);
    if(!physical || rgba.size()!=physical->size()*4)return no("single-color8-resolve-physical-owner");
    for(std::size_t i=0;i<physical->size();++i) {
        const auto at=i*4;
        const auto word=std::uint32_t{rgba[at]}|(std::uint32_t{rgba[at+1]}<<8)|
            (std::uint32_t{rgba[at+2]}<<16)|(std::uint32_t{rgba[at+3]}<<24);
        if(word!=(*physical)[i])return no("single-color8-resolve-physical-bytes");
    }
    const auto packed=packNativeOffscreenResolve(s,plan.destination.storage.description,rgba,before);
    if(!packed)return no("single-color8-resolve-packing");
    if(!current())return no("single-color8-resolve-inputs-before-publication");
    if(!publish(*packed))return no("single-color8-resolve-publication");
    mutation.commit();
    if(why)*why="fenced-single-color8-resolve-physical-consumer";
    return true;
}
}
