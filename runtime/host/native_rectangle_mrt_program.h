#pragma once
#include "native_translated_draw.h"
#include "native_vertex_clip_program.h"
namespace recomp::gpu {
// ABI22 rectangle MRT band. `positions` is the position-only compute
// translation of the same captured pair, used to prove rectangle coverage on
// the GPU before the two-attachment colour draw is admitted; it shares the
// draw's identities and stride.
struct NativeRectangleMrtProgram {
    NativeTranslatedDrawProgram draw;
    NativeClippedFanProgram positions;
};
}
