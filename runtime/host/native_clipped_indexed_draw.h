#pragma once
#include "native_clipped_indexed_plan.h"
#include "native_vertex_clip_executor.h"
#include "native_edram32_bands.h"

namespace recomp::gpu {
inline std::optional<NativeIndexedViewExclusion> executeNativeClippedIndexed(
        ProbeDevice& device,const NativeClippedFanProgram& program,const Gta5InlineDraw& draw,
        const std::vector<std::uint32_t>& indices,const Gta5BoundSurfaceCapture& bound,
        const std::vector<std::uint8_t>& vertex,const std::vector<std::uint8_t>& pixel,
        bool queriesIdle,NativeEdram32Mutation& physical,const char** why=nullptr) {
    if(!planNativeClippedIndexed(program,draw,indices,bound,vertex,pixel,queriesIdle,why))return std::nullopt;
    const auto no=[&](const char* value){if(why)*why=value;return std::optional<NativeIndexedViewExclusion>{};};
    auto executor=NativeVertexClipExecutor::create(device,program);
    const auto positions=executor?executor->indexedPositions(draw):std::nullopt;
    if(!positions)return no("clipped-indexed-position-execution");
    const auto proof=nativeIndexedViewExclusion(draw,*positions,indices,{0,0,1280,720});
    if(!proof)return no("clipped-indexed-view-not-excluded");
    // The transaction took ownership on entry. Return those SAME bytes and
    // validity masks, unchanged, only after fenced current-position exclusion.
    // No target is read, initialized, sampled or written. No new content serial.
    physical.commit();
    if(why)*why="fenced-indexed-view-excluded-physical-preserved";
    return proof;
}
}
