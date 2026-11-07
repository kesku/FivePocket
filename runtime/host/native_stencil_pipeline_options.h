#pragma once
#include "native_stencil_only_plan.h"
#include "native_translated_resources.h"

namespace recomp::gpu {
// The dispatcher and connected GPU tests share this exact state handoff.
// A pipeline configuration is not proof of current surface/EDRAM ownership.
inline NativeTranslatedPipelineOptions nativeStencilPipelineOptions(const NativeStencilOnlyPlan& plan) {
    NativeTranslatedPipelineOptions options;
    options.samples=1;options.colorAttachmentCount=4;options.depthStencil=plan.state;
    options.minDepth=plan.minDepth;options.maxDepth=plan.maxDepth;
    options.colorWriteMasks={0,0,0,0};
    options.cullMode=(plan.cull&1u?VK_CULL_MODE_FRONT_BIT:0u)|
        (plan.cull&2u?VK_CULL_MODE_BACK_BIT:0u);
    options.frontFace=plan.clockwise?VK_FRONT_FACE_CLOCKWISE:VK_FRONT_FACE_COUNTER_CLOCKWISE;
    return options;
}
inline NativeTranslatedPipelineOptions nativeDirectStencilPipelineOptions(const NativeStencilOnlyPlan& plan) {
    auto options=nativeStencilPipelineOptions(plan);
    options.colorAttachmentCount=0;
    return options;
}
}
