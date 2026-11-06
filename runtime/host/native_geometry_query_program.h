#pragma once
#include "native_vertex_clip_program.h"
#include "native_translated_draw.h"

namespace recomp::gpu {
inline bool nativeGeometryQueryProgramMatches(const NativeTranslatedDrawProgram& raster,
        const NativeClippedFanProgram& positions) {
    return !raster.vertexIdentity.empty() && !raster.pixelIdentity.empty() &&
        raster.vertexIdentity==positions.vertexIdentity && raster.pixelIdentity==positions.pixelIdentity &&
        raster.stride==positions.stride && !positions.positionCode.empty();
}
// Binding only. Actual position execution and exclusion still precede raster
// writes; missing/ambiguous programs cannot become guessed query results.
inline const NativeClippedFanProgram* selectNativeGeometryQueryProgram(
        const NativeTranslatedDrawProgram& raster,const std::vector<NativeClippedFanProgram>& programs) {
    const NativeClippedFanProgram* selected=nullptr;
    for(const auto& p:programs)if(nativeGeometryQueryProgramMatches(raster,p)) {
        if(selected)return nullptr;
        selected=&p;
    }
    return selected;
}
}
