#pragma once
#include "gta5_clear_capture.h"
#include "native_edram32_bands.h"
#include <algorithm>
#include <cstring>

namespace recomp::gpu {
// Original823D89A8 clips to viewport/scissor, then8237B378 selects8701
// (stencil ALWAYS/REPLACE, depth disabled) and FF0000|reference. Xenia's
// RB_DEPTHCONTROL / RB_STENCILREFMASK definitions confirm these fields.
// A0 additionally updates RB_HIZCONTROL, not depth samples. This physical
// emulator has no HiZ cache; do not represent that hint by clearing depth.
// Narrow non-binned D24FS8 single-sample profile. No GPU/query/ring receipt.
struct NativeStencilClearPlan {
    unsigned object=0,width=0,height=0,pitch=0,baseTile=0,tileCount=0;
    unsigned left=0,top=0,right=0,bottom=0,stencil=0;
};
inline std::optional<NativeStencilClearPlan> planNativeStencilClear(
        const Gta5ClearCapture& c,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeStencilClearPlan>{};};
    if(c.call.flags!=0xA0 || c.call.stencil>255)return no("stencil-clear-flags");
    if(!c.state || c.state->size()!=13864 || !c.rectangle || c.rectangle->size()!=16)
        return no("stencil-clear-input");
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto& state=*c.state;bool allCached=true;
    for(unsigned i=0;i<5;++i) {
        const auto object=word(state,12816+4*i),cache=word(state,13104+4*i);
        if(object!=c.surfaces[i].object || cache!=c.surfaces[i].tileCache)return no("stencil-clear-binding");
        if(object && object!=cache)allCached=false;
    }
    if((state[11068]&0x10) || ((state[11068]&0x20) && allCached))return no("stencil-clear-binned");
    const auto& d=c.surfaces[4];
    if(!d.object || d.object%4 || d.object>UINT32_MAX-39 || !d.header || d.header->size()!=40)
        return no("stencil-clear-depth-header");
    const auto surface=word(*d.header,24),info=word(*d.header,28),dimensions=word(*d.header,36);
    if(surface!=word(state,10368) || info!=word(state,10376))return no("stencil-clear-shadow");
    const unsigned width=(dimensions>>18)+1,height=((dimensions>>3)&0x7FFF)+1,pitch=surface&0x3FFF;
    // The connected evidence owns this full depth band; wrapping, MSAA and
    // other dimensions remain deferred, not inferred from a matching object.
    if(width!=1280 || height!=720 || pitch!=1280 || (surface&0x3C000) || info!=0x10000)
        return no("stencil-clear-surface");
    if(word(state,13024)!=0 || word(state,13028)!=0 || word(state,13032)!=0x44A00000 ||
       word(state,13036)!=0x44340000)return no("stencil-clear-viewport");
    const auto signedWord=[&](const auto& b,unsigned at) {
        const auto u=word(b,at);return u<0x80000000u?std::int64_t(u):std::int64_t(u)-0x100000000LL;
    };
    auto l=std::max<std::int64_t>(0,signedWord(*c.rectangle,0));
    auto t=std::max<std::int64_t>(0,signedWord(*c.rectangle,4));
    auto r=std::min<std::int64_t>(width,signedWord(*c.rectangle,8));
    auto b=std::min<std::int64_t>(height,signedWord(*c.rectangle,12));
    if(word(state,12288)) {
        l=std::max<std::int64_t>(l,signedWord(state,13052));t=std::max<std::int64_t>(t,signedWord(state,13056));
        r=std::min<std::int64_t>(r,signedWord(state,13060));b=std::min<std::int64_t>(b,signedWord(state,13064));
    }
    if(l>=r || t>=b)return no("stencil-clear-empty");
    if(why)*why="planned";
    return NativeStencilClearPlan{d.object,width,height,pitch,0,720,unsigned(l),unsigned(t),unsigned(r),unsigned(b),c.call.stencil};
}
inline bool sameNativeStencilClearInputs(const Gta5ClearCapture& a,const Gta5ClearCapture& b) {
    const auto& x=a.call;const auto& y=b.call;
    if(x.device!=y.device || x.flags!=y.flags || x.rectangle!=y.rectangle || x.color!=y.color ||
       x.stencil!=y.stencil || x.depthBits!=y.depthBits || a.state!=b.state ||
       a.rectangle!=b.rectangle || a.color!=b.color)return false;
    for(unsigned i=0;i<5;++i)if(a.surfaces[i].object!=b.surfaces[i].object ||
        a.surfaces[i].tileCache!=b.surfaces[i].tileCache || a.surfaces[i].header!=b.surfaces[i].header)return false;
    return true;
}
template<class Current>
bool executeNativeStencilClear(const Gta5ClearCapture& c,NativeEdram32Mutation& mutation,
        Current&& current,const char** why=nullptr) {
    const auto p=planNativeStencilClear(c,why);if(!p)return false;
    const auto completed=executeNativeEdram32Stencil(mutation,p->baseTile,p->pitch,p->width,p->height,
        [&](const std::vector<std::uint32_t>& before)->std::optional<std::vector<std::uint32_t>> {
            auto after=before;
            for(unsigned y=p->top;y<p->bottom;++y)for(unsigned x=p->left;x<p->right;++x) {
                auto& w=after[std::size_t{y}*p->width+x];w=(w&0xFFFFFF00u)|p->stencil;
            }
            if(!current())return std::nullopt;
            return after;
        });
    if(why)*why=completed?"completed":"stencil-clear-owner-or-stale";
    return completed;
}
}
