#pragma once
#include <cstdint>

namespace recomp::gpu {
// Literal/RMW bit knowledge only. This does not implement register side effects.
struct StoppedRegisterKnowledge {
    std::uint32_t value = 0, known = 0;
    void assign(std::uint32_t v) noexcept { value = v; known = 0xFFFFFFFF; }
    void modify(std::uint32_t andMask, std::uint32_t orMask) noexcept {
        known |= ~andMask | orMask; value = ((value & andMask) | orMask) & known;
    }
    bool equals(std::uint32_t expected) const noexcept { return known == 0xFFFFFFFF && value == expected; }
};
}
