#pragma once
#include "xenos_indirect_inspection.h"
namespace recomp::gpu {
struct StoppedShaderLoad { std::uint32_t node; Pm4ShaderLoad load; };
struct StoppedDrawInitiator { std::uint32_t node; Pm4DrawInitiator draw; };
struct StoppedGpuWork {
    std::vector<StoppedShaderLoad> shaders;
    std::vector<StoppedDrawInitiator> draws;
};
struct StoppedGpuWorkError { std::uint32_t node = 0, kind = 0; Pm4Error error; };
// Metadata/owned instruction bytes only. Neither draws nor shader compilation
// are executed. The graph was already bounded and captured with writers stopped.
inline bool inspectStoppedGpuWork(const Pm4Snapshot& root, const Pm4IndirectGraph& graph,
        StoppedGpuWork& output, StoppedGpuWorkError& error) {
    try {
        StoppedGpuWork candidate;
        for (std::uint32_t id = 0; id <= graph.nodes.size(); ++id) {
            const auto& snapshot = id ? graph.nodes[id - 1].snapshot : root;
            Pm4ShaderInspection shaders; std::vector<Pm4DrawInitiator> draws; Pm4Error e;
            if (!inspectPm4ShaderLoads(snapshot, shaders, e, 64)) { error = {id,1,e}; return false; }
            if (!inspectPm4DrawInitiators(snapshot, draws, e, 64)) { error = {id,2,e}; return false; }
            if (shaders.loads.size() > 64 - candidate.shaders.size() || draws.size() > 64 - candidate.draws.size()) {
                error = {id,3,{Pm4Failure::PacketLimit,0,0}}; return false;
            }
            for (auto& load : shaders.loads) candidate.shaders.push_back({id,std::move(load)});
            for (auto& draw : draws) candidate.draws.push_back({id,draw});
        }
        output = std::move(candidate); error = {}; return true;
    } catch (const std::bad_alloc&) { error = {0,3,{Pm4Failure::AllocationFailure,0,0}}; return false; }
}
}
