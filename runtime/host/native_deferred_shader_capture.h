#pragma once
#include "native_deferred_swap_submission.h"
#include "xenos_stopped_shader_capture.h"

namespace recomp::gpu {
struct NativeDeferredShaderCapture {
    std::vector<StoppedShaderLoad> loads;
    std::vector<std::vector<std::uint8_t>> bytes;
};
// Stopped-only prefix source evidence, not a successful full retail graph or
// shader translation. The reader must enforce current allocation ownership;
// callers must join every guest writer. Never read the requested image as code.
inline bool captureNativeDeferredPrefixShaders(const NativeDeferredSwapSubmission& frame,
        const Pm4PhysicalReader& reader,NativeDeferredShaderCapture& output) {
    try {
        Pm4ShaderInspection inspected;Pm4Error error;StoppedGpuWork prefixLoads;
        if(!inspectPm4ShaderLoads(frame.prefix(),inspected,error,8))return false;
        const auto image=frame.swap().command().identity().imagePhysical;
        for(auto& load:inspected.loads){
            const auto bytes=std::uint64_t{load.instructionWords}*4;
            if(!load.immediate && load.physicalAddress<std::uint64_t{image}+NativeSwapImagePlan::Capacity &&
                image<std::uint64_t{load.physicalAddress}+bytes)return false;
            prefixLoads.shaders.push_back({1,std::move(load)});
        }
        NativeDeferredShaderCapture candidate;
        if(!captureStoppedShaderBytes(prefixLoads,reader,candidate.bytes))return false;
        candidate.loads=std::move(prefixLoads.shaders);output=std::move(candidate);return true;
    }catch(...){return false;}
}
}
