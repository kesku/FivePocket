#pragma once
#include <algorithm>
#include "gta5_fixed16_clear_plan.h"
#include "gta5_bound_surface_capture.h"

namespace recomp::gpu {
// Retag an already completed native image, never create or initialize one.
// Only a color-object alias of exactly the same physical layout is supported;
// the depth object and its full current header must remain the same. The caller
// separately proves current old headers, exclusive ownership and draw state.
inline std::optional<Gta5Fixed16ClearPlan> planNativeFixed16ColorAlias(
        const Gta5Fixed16ClearPlan& source,const std::vector<std::uint8_t>& sourceHeader,
        const std::vector<std::uint8_t>& depthHeader,const Gta5BoundSurfaceCapture& bindings) {
    const auto& color=bindings.surfaces[0];const auto& depth=bindings.surfaces[4];
    if(!source.object || !source.depth || source.format!=4 || source.samples!=1 ||
       !color.object || color.object%4 || color.object==source.depth->object || !color.header ||
       sourceHeader.size()!=40 || color.header->size()!=40 || depthHeader.size()!=40 ||
       depth.object!=source.depth->object || depth.header!=std::optional{depthHeader})return std::nullopt;
    for(unsigned slot=1;slot<4;++slot)if(bindings.surfaces[slot].object)return std::nullopt;
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
        (std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if((word(sourceHeader,24)&0x3FFFFu)!=source.pitch ||
       word(sourceHeader,28)!=(0x40000u|source.baseTile) ||
       (word(sourceHeader,36)>>18)+1!=source.width || ((word(sourceHeader,36)>>3)&0x7FFF)+1!=source.height ||
       !std::equal(sourceHeader.begin()+24,sourceHeader.end(),color.header->begin()+24))return std::nullopt;
    auto result=source;result.object=color.object;return result;
}
inline bool nativeFixed16SameStorageRetag(const Gta5Fixed16ClearPlan& before,const Gta5Fixed16ClearPlan& after) {
    if(!before.depth || !after.object || after.object%4 || after.object==before.depth->object)return false;
    auto expected=before;expected.object=after.object;
    return expected==after;
}
}
