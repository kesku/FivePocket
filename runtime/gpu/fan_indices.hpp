// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fan_indices_candidate {
enum class StartupPolicy { Disabled, Enabled, Invalid };
// Caller reads startup environment once. This experimental candidate defaults on.
inline StartupPolicy parseStartupPolicy(const char* value) noexcept {
    if (!value) return StartupPolicy::Enabled;
    const std::string_view text(value);
    if (text == "0") return StartupPolicy::Disabled;
    if (text == "1") return StartupPolicy::Enabled;
    return StartupPolicy::Invalid;
}
inline bool eligible(StartupPolicy policy, unsigned primitive,
                     bool semanticIndexed, std::uint32_t vertexCount) noexcept {
    return policy == StartupPolicy::Enabled && primitive == 5 && !semanticIndexed &&
           vertexCount >= 3 && vertexCount <= 64;
}
// Compare without overflow; caller separately requires successful constants allocation.
inline bool fitsRemaining(std::size_t baselineBytes, std::size_t extraBytes,
                          std::size_t capacity, std::size_t offset) noexcept {
    if (offset > capacity) return false;
    const auto remaining = capacity - offset;
    if (baselineBytes > remaining) return false;
    return extraBytes <= remaining - baselineBytes;
}
struct Result {
    bool expanded = false;
    std::size_t indexCount = 0;
};
// Output capacity is measured in uint32 indices. Rejection never touches output.
// FIRST provoking vertex: Vulkan fan triangle j = (j+1, j+2, 0).
inline Result write(StartupPolicy policy, unsigned primitive, bool semanticIndexed,
                    std::uint32_t vertexCount, std::uint32_t* output,
                    std::size_t capacity) noexcept {
    if (!eligible(policy, primitive, semanticIndexed, vertexCount)) return {};
    const std::size_t count = std::size_t(vertexCount - 2) * 3;
    if (!output || capacity < count) return {};
    for (std::uint32_t j = 0; j < vertexCount - 2; ++j) {
        output[3 * j] = j + 1;
        output[3 * j + 1] = j + 2;
        output[3 * j + 2] = 0;
    }
    return {true, count};
}
} // namespace fan_indices_candidate
