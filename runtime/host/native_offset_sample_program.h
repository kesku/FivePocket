#pragma once
#include "native_translated_draw.h"
#include "native_vertex_clip_program.h"
namespace recomp::gpu {
// ABI23 offset sample band. `positions` is the position-only compute
// translation of the same captured pair, used to prove rectangle coverage on
// the GPU before the colour draw is admitted; it shares the draw's identities
// and stride, exactly as the composite and MRT bands do.
struct NativeOffsetSampleProgram {
    NativeTranslatedDrawProgram draw;
    NativeClippedFanProgram positions;
};
}
