#pragma once
#include "gta5_fixed16_clear_plan.h"
#include "native_edram32_bands.h"

namespace recomp::gpu {
// Preserve only storage DISJOINT from an already-admitted fixed16 operation.
// Both attachments are invalidated even for a partial raster write. This does
// not reinterpret native SNORM/depth output as Xenos raw colour words.
// Execution must include the real GPU fence AND current-input revalidation.
// Use the caller's existing exclusive-gate mutation; never nest one on the
// temporarily empty global owner or keep it across a guest scheduler yield.
template<class Execute>
bool executeNativeEdram32Fixed16Write(NativeEdram32Mutation& mutation,
        const Gta5Fixed16ClearPlan& plan,Execute&& execute) {
    const auto footprint=[](const auto& p) {
        if(!p.object || p.object%4 || !p.width || !p.height || p.pitch<p.width || p.pitch%80 ||
           (p.samples!=1 && p.samples!=2 && p.samples!=4) || p.baseTile>=2048)return false;
        const auto columns=std::uint64_t{p.pitch/80}*(p.samples==4?2u:1u);
        const auto rows=(std::uint64_t{p.height}*(p.samples>1?2u:1u)+15)/16;
        return columns*rows==p.tileCount && p.tileCount && p.tileCount<=2048-p.baseTile;
    };
    if(plan.format!=4 || !footprint(plan))return false;
    if(plan.depth) {
        const auto& d=*plan.depth;
        if(d.format!=1 || !footprint(d) || d.object==plan.object || d.width!=plan.width ||
           d.height!=plan.height || d.pitch!=plan.pitch || d.samples!=plan.samples ||
           (d.baseTile<plan.baseTile+plan.tileCount && plan.baseTile<d.baseTile+d.tileCount))return false;
    }
    auto& candidate=mutation.candidate();
    if(!candidate.invalidate(plan.baseTile,plan.tileCount) ||
       (plan.depth && !candidate.invalidate(plan.depth->baseTile,plan.depth->tileCount)))return false;
    if(!execute())return false;
    mutation.commit();return true;
}
}
