#pragma once
#include "native_translated_draw.h"
#include "native_vertex_clip_program.h"
namespace recomp::gpu {
// ABI20 composite band or separately admitted ABI26 screen-coordinate rectangle.
// `positions` is the position-only compute translation of
// the same captured pair, used to prove rectangle coverage on the GPU before
// the colour draw is admitted; it shares the draw's identities and stride.
struct NativeCompositeRectangleProgram {
    NativeTranslatedDrawProgram draw;
    NativeClippedFanProgram positions;
};
}
