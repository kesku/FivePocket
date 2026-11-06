#pragma once
#include "native_query_neutral_fan_plan.h"
#include "native_vertex_clip_executor.h"

namespace recomp::gpu {
inline std::optional<NativeQueryNeutralFanProof> executeNativeQueryNeutralFan(
        ProbeDevice& device,const NativeClippedFanProgram& program,const Gta5InlineDraw& draw,
        const std::vector<std::uint8_t>& vertex,const std::vector<std::uint8_t>& pixel,
        const char** why=nullptr) {
    if(!planNativeQueryNeutralFan(program,draw,vertex,pixel,why))return std::nullopt;
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeQueryNeutralFanProof>{};};
    auto executor=NativeVertexClipExecutor::create(device,program);
    const auto positions=executor?executor->fanPositions(draw):std::nullopt;
    if(!positions)return no("neutral-fan-gpu-positions");
    // Require positive W and both independent separations. No final-samples
    // count, depth-only rejection, degenerate geometry or saved positions.
    const auto plane=nativeCommonOutsideXYPlane(*positions);
    if(!plane || !nativeCommonOutsideScissorHalfPlane(draw,*positions))return no("neutral-fan-not-excluded");
    if(why)*why="neutral-fan-draw-only-query-events-pending";
    return NativeQueryNeutralFanProof{*plane,draw.vertexCount};
}
}
