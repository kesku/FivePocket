#pragma once
#include <array>
#include <cstdint>

namespace recomp::bootstrap {
// Observed event-1 payload: four BE uint16 dimensions, then a 56-byte scaler
// descriptor. Metadata only; accepting it executes no scaler or XAM callbacks.
struct NativeDisplayConfiguration {
    std::array<std::uint16_t, 4> dimensions{}; // Buffer w/h, display w/h.
    std::array<std::uint32_t, 14> scaler{}; // Rect, output w/h, two filter descriptors.
};
inline bool validNativeDisplayConfiguration(const NativeDisplayConfiguration& c) {
    for (auto dimension : c.dimensions) if (!dimension || dimension > 4096) return false;
    const auto& p = c.scaler;
    if (p[0] >= p[2] || p[1] >= p[3] || p[2] > c.dimensions[0] || p[3] > c.dimensions[1] ||
        p[4] != c.dimensions[2] || p[5] != c.dimensions[3] || p[6] > 7 || p[10] > 7) return false;
    for (auto i : {7U, 8U, 9U, 11U, 12U, 13U}) {
        if ((p[i] & 0x7F800000U) == 0x7F800000U) return false; // IEEE binary32 finite.
    }
    return true;
}
}
