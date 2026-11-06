#pragma once
#include "native_translated_draw.h"
#include "native_vertex_clip_executor.h"
namespace recomp::gpu {
struct NativeLodRectangleProgram {
    NativeTranslatedDrawProgram draw;
    NativeClippedFanProgram positions;
};
}
