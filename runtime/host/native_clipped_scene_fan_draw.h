#pragma once
#include "native_clipped_scene_fan_plan.h"
#include "native_vertex_clip_executor.h"
#include "native_edram32_bands.h"

namespace recomp::gpu {
inline std::optional<unsigned> executeNativeClippedSceneFan(ProbeDevice& device,
        const NativeClippedFanProgram& program,const Gta5InlineDraw& draw,
        const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& vertex,
        const std::vector<std::uint8_t>& pixel,bool queriesIdle,
        NativeEdram32Mutation& physical,const char** why=nullptr,
        NativeSceneFanProfile profile=NativeSceneFanProfile::Textured684) {
    if(!planNativeClippedSceneFan(program,draw,bound,vertex,pixel,queriesIdle,why,profile))return std::nullopt;
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<unsigned>{};};
    auto executor=NativeVertexClipExecutor::create(device,program);
    const auto positions=executor?executor->fanPositions(draw):std::nullopt;
    if(!positions)return no("scene-fan-position-execution");
    const auto excluded=nativeSceneFanOutside(draw,*positions);
    if(!excluded)return no("scene-fan-view-not-excluded");
    // Preserve the same bytes, unknown masks and serials. No shader texture
    // sampling, target creation/write, query result or ring retirement occurs.
    physical.commit();if(why)*why="fenced-scene-fan-excluded-physical-preserved";
    return excluded;
}
}
