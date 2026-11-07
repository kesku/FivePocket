// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(APPLE_EXACT_BGRA_FORCE_SCALAR)
#include <arm_neon.h>
#define APPLE_EXACT_BGRA_NEON_ENABLED 1
#else
#define APPLE_EXACT_BGRA_NEON_ENABLED 0
#endif

namespace recomp::apple {

namespace detail {
// Callers still own the lifetime and readable extent of both borrowed spans.
// Reject empty, partial-pixel, unequal, null and arithmetically wrapping spans
// before any dereference. No allocation or conversion scratch buffer is used.
inline bool validExactColorSpans(const std::uint8_t* bgra, std::size_t bgraBytes,
                                const std::uint8_t* rgba, std::size_t rgbaBytes) noexcept {
    if (!bgra || !rgba || !bgraBytes || bgraBytes != rgbaBytes || bgraBytes % 4 ||
        bgraBytes > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        return false;
    const auto last = bgraBytes - 1;
    const auto limit = std::numeric_limits<std::uintptr_t>::max();
    return last <= limit - reinterpret_cast<std::uintptr_t>(bgra) &&
           last <= limit - reinterpret_cast<std::uintptr_t>(rgba);
}
} // namespace detail

inline constexpr bool exactBgraCompareUsesNeon = APPLE_EXACT_BGRA_NEON_ENABLED;

// Reference/fallback: BGRA destination must match the RGBA source at every
// pixel, including alpha. This intentionally retains the installed guard's
// byte-wise semantics rather than accepting a checksum or sampled subset.
inline bool exactBgraEqualsRgbaScalar(const std::uint8_t* bgra, std::size_t bgraBytes,
                                    const std::uint8_t* rgba, std::size_t rgbaBytes) noexcept {
    if (!detail::validExactColorSpans(bgra, bgraBytes, rgba, rgbaBytes)) return false;
    for (std::size_t i = 0; i < bgraBytes; i += 4)
        if (bgra[i] != rgba[i + 2] || bgra[i + 1] != rgba[i + 1] ||
            bgra[i + 2] != rgba[i] || bgra[i + 3] != rgba[i + 3]) return false;
    return true;
}

inline bool exactBgraEqualsRgba(const std::uint8_t* bgra, std::size_t bgraBytes,
                              const std::uint8_t* rgba, std::size_t rgbaBytes) noexcept {
    if (!detail::validExactColorSpans(bgra, bgraBytes, rgba, rgbaBytes)) return false;
    std::size_t i = 0;
#if APPLE_EXACT_BGRA_NEON_ENABLED
    constexpr std::uint8_t lanes[16] = {2, 1, 0, 3, 6, 5, 4, 7,
                                        10, 9, 8, 11, 14, 13, 12, 15};
    const auto permutation = vld1q_u8(lanes);
    // Unaligned loads are supported. Subtraction avoids i+16 overflow; only
    // complete chunks are loaded, so a span ending at a guard page is valid.
    for (; bgraBytes - i >= 16; i += 16) {
        const auto expected = vqtbl1q_u8(vld1q_u8(rgba + i), permutation);
        const auto mismatch = veorq_u8(vld1q_u8(bgra + i), expected);
        if (vmaxvq_u8(mismatch) != 0) return false;
    }
#endif
    for (; i < bgraBytes; i += 4)
        if (bgra[i] != rgba[i + 2] || bgra[i + 1] != rgba[i + 1] ||
            bgra[i + 2] != rgba[i] || bgra[i + 3] != rgba[i + 3]) return false;
    return true;
}

} // namespace recomp::apple

#undef APPLE_EXACT_BGRA_NEON_ENABLED
