#pragma once
#include "native_indexed_view_exclusion.h"
#include "native_logical_geometry_state.h"
#include "native_vertex_clip_executor.h"
#include "native_geometry_query_program.h"

namespace recomp::gpu {
// Only a sufficient EMPTY view-volume result is implemented here. A draw
// intersecting the view needs a real early/Hi-Z provider; it remains unknown.
// Unlike a Vulkan final-sample count, geometric exclusion precedes pixel KILL,
// stencil and depth replacement. No guessed visible result is returned.
inline std::optional<NativeIndexedViewExclusion> measureNativeGeometryViewExclusion(
        NativeVertexClipExecutor& executor,const NativeTranslatedDrawProgram& program,
        const Gta5InlineDraw& draw,const std::vector<std::uint32_t>& indices,
        const Gta5ClearCapture& surfaces,const std::array<unsigned,4>& rectangle,
        const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures) {
    if(!nativeGeometryQueryProgramMatches(program,executor.program()) ||
       !prepareNativeLogicalGeometry(program,draw,indices,surfaces,rectangle,textures))return std::nullopt;
    const auto positions=executor.indexedPositions(draw);
    return positions?nativeIndexedViewExclusion(draw,*positions,indices,rectangle):std::nullopt;
}
}
