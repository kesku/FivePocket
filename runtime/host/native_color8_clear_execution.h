#pragma once
#include "gta5_color8_clear_plan.h"
#include "native_edram32_bands.h"

namespace recomp::gpu {
// Presentation ownership and cached-pixel validity are distinct. A disjoint
// offscreen clear does not read the presentation cache; it must neither require
// that cache to be valid nor resurrect it after physical scene rendering.
inline bool nativeColor8ClearOwnerAllowed(const Gta5Color8ClearAudit& audit,
        bool ownsPresentation,unsigned presentationObject) {
    return !audit.issues && ownsPresentation && presentationObject &&
        audit.plan.object!=presentationObject && !color8ClearIntersects(audit.plan,720,720);
}

// Runs under the caller's ONE existing exclusive gate/physical transaction.
// Execute includes actual GPU completion. Current checks re-read the complete
// captured request and headers before and after execution. Failure publishes
// no physical ownership and cannot make an old image usable.
template<class Current,class Execute,class Readback>
bool executeNativeColor8ClearWrite(const Gta5Color8ClearAudit& audit,
        bool ownsPresentation,unsigned presentationObject,NativeEdram32Mutation& mutation,
        Current&& current,Execute&& execute,Readback&& readback,const char** why=nullptr) {
    const auto no=[&](const char* reason){if(why)*why=reason;return false;};
    const auto& p=audit.plan;
    if(!nativeColor8ClearOwnerAllowed(audit,ownsPresentation,presentationObject))return no("color8-clear-owner");
    const auto footprint=[](const auto& s) {
        if(!s.object || s.object%4 || !s.width || !s.height || s.width>1280 || s.height>720 ||
           s.pitch<s.width || s.pitch>16383 || s.pitch%80 || s.baseTile>=2048 ||
           (s.samples!=1 && s.samples!=2 && s.samples!=4))return false;
        const auto tiles=std::uint64_t{s.pitch/80}*(s.samples==4?2u:1u)*
            ((std::uint64_t{s.height}*(s.samples>1?2u:1u)+15)/16);
        return tiles && tiles==s.tileCount && tiles<=2048-s.baseTile;
    };
    if(p.format>1 || !footprint(p))return no("color8-clear-footprint");
    if(p.depth) {
        const auto& d=*p.depth;
        if(d.format!=1 || !footprint(d) || d.object==p.object || d.width!=p.width ||
           d.height!=p.height || d.samples!=p.samples ||
           (d.baseTile<p.baseTile+p.tileCount && p.baseTile<d.baseTile+d.tileCount) ||
           planExactXenosFloat24Clear(d.value.depth,d.value.stencil)!=std::optional{d.value})
            return no("color8-clear-depth-footprint");
    }
    if(!current())return no("color8-clear-inputs-before-gpu");
    auto& candidate=mutation.candidate();
    // Multi-sample and depth views retain their actual native owner, but this
    // helper does not invent a resolved-to-per-sample physical alias mapping.
    // For single-sample colour, fenced storage bytes rejoin the physical owner.
    if(!candidate.invalidate(p.baseTile,p.tileCount) ||
       (p.depth && !candidate.invalidate(p.depth->baseTile,p.depth->tileCount)))
        return no("color8-clear-invalidation");
    if(!execute())return no("color8-clear-gpu");
    if(!current())return no("color8-clear-inputs-after-gpu");
    if(p.samples==1) {
        const auto bytes=readback();
        if(!bytes || bytes->size()!=std::uint64_t{p.width}*p.height*4)return no("color8-clear-readback");
        for(std::size_t i=0;i<bytes->size();++i)
            if((*bytes)[i]!=p.color[i%4])return no("color8-clear-pixel-mismatch");
        if(!candidate.recordCompletedRawColorRows(p.baseTile,p.pitch,p.width,p.height,*bytes,candidate.nextSerial()))
            return no("color8-clear-physical-publication");
    }
    mutation.commit();
    if(why)*why="fenced-color8-clear-physical-disjoint-preserved";
    return true;
}
}
