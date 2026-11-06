#pragma once
#include "gta5_clear_capture.h"
#include "gta5_color8_clear.h"
#include "xenos_float24_clear.h"
#include <algorithm>
#include <cstring>

namespace recomp::gpu {
struct Gta5Depth24ClearPlan {
    std::uint32_t object=0,width=0,height=0,pitch=0,baseTile=0,tileCount=0,samples=0,format=0;
    XenosFloat24Clear value;
    bool operator==(const Gta5Depth24ClearPlan&) const = default;
};
struct Gta5Color8ClearPlan {
    std::uint32_t object=0,width=0,height=0,pitch=0,baseTile=0,tileCount=0,samples=0,format=0;
    std::array<std::uint8_t,4> color{};
    std::optional<Gta5Depth24ClearPlan> depth;
    bool operator==(const Gta5Color8ClearPlan&) const = default;
};
inline bool color8ClearIntersects(const Gta5Color8ClearPlan& p,unsigned base,unsigned tiles) {
    const auto overlap=[&](unsigned b,unsigned n){return n && tiles &&
        std::uint64_t{b}<std::uint64_t{base}+tiles && std::uint64_t{base}<std::uint64_t{b}+n;};
    return overlap(p.baseTile,p.tileCount) || (p.depth && overlap(p.depth->baseTile,p.depth->tileCount));
}
inline bool color8ClearPlansOverlap(const Gta5Color8ClearPlan& a,const Gta5Color8ClearPlan& b) {
    return color8ClearIntersects(a,b.baseTile,b.tileCount) ||
        (b.depth && color8ClearIntersects(a,b.depth->baseTile,b.depth->tileCount));
}
enum Gta5Color8ClearIssue : std::uint32_t {
    ClearStateMissing=1,ClearFlagsUnsupported=2,ClearBinned=4,ClearBindingMismatch=8,
    ClearMultipleTargets=16,ClearDepthStencilUnsupported=32,ClearHeaderMissing=64,
    ClearSurfaceUnsupported=128,ClearColorUnsupported=256,ClearRectangleMissing=512,
    ClearViewportUnsupported=1024,ClearPartial=2048,ClearEdramRange=4096,
    ClearDepthHeaderMissing=8192,ClearDepthSurfaceUnsupported=16384,
    ClearDepthValueUnsupported=32768,ClearDepthExtentMismatch=65536,ClearDepthColorAlias=131072
};
struct Gta5Color8ClearAudit {
    Gta5Color8ClearPlan plan;
    std::uint32_t issues=0;
    std::uint32_t effectiveFlags=0;
};
namespace detail {
// Shared 32-bit color-surface geometry/binding/depth contract. Storage format
// admission and clear encoding are typed policies; callers must not mask away
// a failed color8 audit to authorize another format. This does not admit draws,
// resolve conversions, 64-bit color surfaces or GPU completion.
template<class Audit,class SurfaceAllowed,class PackColor>
inline Audit auditGta5Clear32(const Gta5ClearCapture& c,
        const SurfaceAllowed& surfaceAllowed,const PackColor& packColor) {
    Audit out;auto& p=out.plan;out.effectiveFlags=c.call.flags;
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto real=[](std::uint32_t bits){float f;std::memcpy(&f,&bits,4);return f;};
    if(!(c.call.flags&1) || (c.call.flags&~0x3Fu))out.issues|=ClearFlagsUnsupported;
    const bool state=c.state && c.state->size()==13864;
    if(!state)out.issues|=ClearStateMissing;
    else {
        const auto& b=*c.state;
        // Original823D89EC ignores depth/stencil only when the depth binding is
        // actually null. A missing header is not evidence of a null binding.
        if(!word(b,12832))out.effectiveFlags&=~0x30u;
        if(out.effectiveFlags&0x30)out.issues|=ClearDepthStencilUnsupported;
        bool allCached=true;
        for(unsigned i=0;i<5;++i) {
            const auto object=word(b,12816+4*i),cache=word(b,13104+4*i);
            if(object!=c.surfaces[i].object || cache!=c.surfaces[i].tileCache)out.issues|=ClearBindingMismatch;
            if(object && object!=cache)allCached=false;
            if(i>0 && i<4 && (c.call.flags&(1u<<i)) && object)out.issues|=ClearMultipleTargets;
        }
        if((b[11068]&0x10) || ((b[11068]&0x20) && allCached))out.issues|=ClearBinned;
    }
    const auto& s=c.surfaces[0];p.object=s.object;
    if(!s.object || s.object%4 || s.object>UINT32_MAX-39 || !s.header || s.header->size()!=40)
        out.issues|=ClearHeaderMissing;
    else {
        const auto info=word(*s.header,24),color=word(*s.header,28),dimensions=word(*s.header,36);
        const auto mode=(info>>16)&3;
        p.width=(dimensions>>18)+1;p.height=((dimensions>>3)&0x7FFF)+1;
        p.pitch=info&0x3FFF;p.baseTile=color&0x7FF;p.format=(color>>16)&15;
        p.samples=mode<3?1u<<mode:0;
        if((info&0xC000) || mode>2 || !surfaceAllowed(color) ||
           !p.pitch || p.pitch%80 || p.pitch<p.width || p.width>1280 || p.height>720)
            out.issues|=ClearSurfaceUnsupported;
        // HiZ pitch in upper surface bits is not color sample count. Full
        // color clear does not consume HiZ. Header and CPU shadow must agree.
        if(state && (info!=word(*c.state,10368) || color!=word(*c.state,10372)))out.issues|=ClearBindingMismatch;
        const auto pitchSamples=p.pitch*(mode==2?2u:1u),heightSamples=p.height*(mode?2u:1u);
        p.tileCount=((pitchSamples+79)/80)*((heightSamples+15)/16);
        if(!p.tileCount || p.tileCount>2048-p.baseTile)out.issues|=ClearEdramRange;
    }
    if(!c.color || c.color->size()!=16)out.issues|=ClearColorUnsupported;
    else {
        std::array<float,4> color;
        for(unsigned i=0;i<4;++i)color[i]=real(word(*c.color,4*i));
        const auto packed=packColor(color,p.format);
        if(!packed)out.issues|=ClearColorUnsupported;else p.color=*packed;
    }
    if(!c.rectangle || c.rectangle->size()!=16)out.issues|=ClearRectangleMissing;
    if(state && c.rectangle && c.rectangle->size()==16) {
        std::array<int,4> clipped{},viewport{};bool valid=true;
        for(unsigned i=0;i<4;++i) {
            const auto f=real(word(*c.state,13024+4*i));
            if(!std::isfinite(f) || f<-8192 || f>8192){valid=false;break;}
            viewport[i]=static_cast<int>(f);clipped[i]=static_cast<std::int32_t>(word(*c.rectangle,4*i));
        }
        if(!valid)out.issues|=ClearViewportUnsupported;
        else {
            viewport[2]+=viewport[0];viewport[3]+=viewport[1];
            for(unsigned i=0;i<4;++i)clipped[i]=i<2?std::max(clipped[i],viewport[i]):std::min(clipped[i],viewport[i]);
            if(word(*c.state,12288))for(unsigned i=0;i<4;++i) {
                const auto scissor=static_cast<std::int32_t>(word(*c.state,13052+4*i));
                clipped[i]=i<2?std::max(clipped[i],scissor):std::min(clipped[i],scissor);
            }
            if(clipped!=std::array<int,4>{0,0,int(p.width),int(p.height)})out.issues|=ClearPartial;
        }
    }
    if(state && (out.effectiveFlags&0x30)) {
        const auto& depth=c.surfaces[4];const auto before=out.issues;
        Gta5Depth24ClearPlan d;d.object=depth.object;
        // Only a full initialization of BOTH aspects is presently executable.
        // A depth-only/stencil-only request must preserve the other aspect.
        if((out.effectiveFlags&0x30)!=0x30)out.issues|=ClearDepthStencilUnsupported;
        if(!d.object || d.object%4 || d.object>UINT32_MAX-39 || !depth.header || depth.header->size()!=40)
            out.issues|=ClearDepthHeaderMissing;
        else {
            const auto info=word(*depth.header,24),depthInfo=word(*depth.header,28),dimensions=word(*depth.header,36);
            const auto mode=(info>>16)&3;d.samples=mode<3?1u<<mode:0;
            d.width=(dimensions>>18)+1;d.height=((dimensions>>3)&0x7FFF)+1;
            d.pitch=info&0x3FFF;d.baseTile=depthInfo&0x7FF;d.format=(depthInfo>>16)&1;
            d.tileCount=((d.pitch*(mode==2?2u:1u)+79)/80)*((d.height*(mode?2u:1u)+15)/16);
            if((info&0xC000) || mode>2 || (depthInfo&~0x107FFu) || d.format!=1 || !d.pitch || d.pitch%80 ||
                d.pitch<d.width || !d.tileCount || d.tileCount>2048-d.baseTile ||
                info!=word(*c.state,10368) || depthInfo!=word(*c.state,10376))out.issues|=ClearDepthSurfaceUnsupported;
            if(d.width!=p.width || d.height!=p.height || d.samples!=p.samples)out.issues|=ClearDepthExtentMismatch;
            if(d.object==p.object || (d.baseTile<p.baseTile+p.tileCount && p.baseTile<d.baseTile+d.tileCount))
                out.issues|=ClearDepthColorAlias;
        }
        double requestedDepth;std::memcpy(&requestedDepth,&c.call.depthBits,8);
        const auto value=planExactXenosFloat24Clear(requestedDepth,c.call.stencil);
        if(!value)out.issues|=ClearDepthValueUnsupported;else d.value=*value;
        if(out.issues==before && (out.effectiveFlags&0x30)==0x30) {
            p.depth=d;out.issues&=~ClearDepthStencilUnsupported;
        }
    }
    return out;
}
} // namespace detail
// Existing color8 admission is unchanged. Gamma draws still require explicit
// linear blending/storage conversion, not hardware sRGB.
inline Gta5Color8ClearAudit auditGta5Color8Clear(const Gta5ClearCapture& c) {
    return detail::auditGta5Clear32<Gta5Color8ClearAudit>(c,
        [](unsigned color){return !(color&~0x000107FFu);},
        [](const auto& color,unsigned format){return packGta5Color8Clear(color,format);});
}
}
