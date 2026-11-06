#pragma once
#include "native_translated_draw.h"
#include "native_vertex_clip_executor.h"
namespace recomp::gpu {
struct NativeCubeRectangleProgram {
    NativeTranslatedDrawProgram draw;
    NativeClippedFanProgram positions;
};
}
