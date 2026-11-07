#pragma once
#include "gta5_inline_draw.h"

namespace recomp::gpu {
// Diagnostic-only ownership capture. A stopped vertex-only draw still has
// real vertices/constants worth preserving. Unlike an execution profile,
// diagnostics do not require a pixel stage. All allocation, size and vertex
// stage checks remain in the underlying capture. This grants no draw admission.
inline std::optional<Gta5InlineDraw> captureGta5StoppedInlineDraw(
        std::uint32_t device,unsigned primitive,unsigned vertexCount,unsigned stride,
        const std::vector<unsigned>& textureSlots,const Gta5ResourceReader& heap,
        const Gta5ResourceReader& data,const Gta5TextureFetchReader& submittedFetch={}) {
    return captureGta5InlineDraw(device,primitive,vertexCount,stride,
        textureSlots,heap,data,submittedFetch,true);
}
}
