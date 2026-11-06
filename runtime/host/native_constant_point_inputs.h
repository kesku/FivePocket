#pragma once
#include "xenos_copy_shader.h"
#include <cstddef>
#include <limits>
#include <type_traits>

namespace recomp::gpu {
struct alignas(16) NativeConstantPointInputs {
    std::array<float,4> position{}, parameter{};
};
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(std::is_standard_layout_v<NativeConstantPointInputs>);
static_assert(sizeof(NativeConstantPointInputs) == 32 && offsetof(NativeConstantPointInputs,parameter) == 16);
// std430 push-constant layout for our generic native shader pair. This only
// packs a validated literal-zero/one domain; not a guest constant-bank upload,
// shader validator, title raster-state check or GPU submission permission.
inline bool packNativeConstantPointInputs(const ConstantShaderPair& pair, NativeConstantPointInputs& output) noexcept {
    NativeConstantPointInputs candidate;
    for (std::size_t i = 0; i < 4; ++i) {
        if (pair.position[i] > 1 || pair.parameter[i] > 1) return false;
        candidate.position[i] = pair.position[i] ? 1.0f : 0.0f;
        candidate.parameter[i] = pair.parameter[i] ? 1.0f : 0.0f;
    }
    output = candidate; return true;
}
}
