#pragma once
#include "native_translated_draw.h"
#include "native_vertex_clip_executor.h"
namespace recomp::gpu {
struct NativeGammaFanProgram {
    NativeTranslatedDrawProgram draw;
    NativeClippedFanProgram positions;
    std::vector<std::uint32_t> transferVertex,encodePixel,decodePixel;
};
}
