#pragma once
#include "native_color_resolved_memory.h"
#include "native_gpu_output_publication.h"
namespace recomp::gpu {
// Bind publication to one immutable captured request and CURRENT complete
// allocation mappings. The caller MUST retain real lifecycle ownership and
// park all guest writers through this call, with actual GPU fences/visibility
// and every command's backend semantic obligation already satisfied. This is
// not enabled for live game submission by adding this helper. A stopped copy,
// model timeline, output bytes or mapping metadata is not such a certificate.
// All admission/current-source/readback checks precede the first store. No
// allocation, callback or fallible step follows the first copy publication.
inline bool publishCapturedNativeColorOutputs(const NativeColorBatchCapture& capture,
        const NativeColorResolvedMemory& memory,const NativeTiledCopyPublication& copy,
        const std::uint8_t* events,std::size_t eventBytes) noexcept {
    if(!copy.matchesPlan(capture.copy().copy) || !matchesNativeColorBatchMemory(capture,memory))return false;
    const auto span=[](const auto& mapping){return NativeOutputSpan{mapping.range.address,mapping.bytes(),mapping.range.bytes};};
    const std::array<NativeOutputWord,2> outputs{{
        {16,span(memory.outputs[1])},{20,span(memory.outputs[2])}}};
    return publishNativeGpuOutputs(capture.events().staging,events,eventBytes,outputs.data(),outputs.size(),
        span(memory.outputs[3]),capture.controls().previousWord,capture.controls().nextWord,&copy,span(memory.outputs[0]));
}
}
