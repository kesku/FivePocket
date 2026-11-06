#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace recomp::bootstrap {
// Convert a declared native compatibility profile from XEX's packed version
// to XboxKrnlVersion's big-endian major/minor/build/QFE export. This describes
// the guest ABI profile, not the Android kernel or a complete Xbox kernel.
inline std::optional<std::array<std::uint8_t, 8>> guestKernelVersionBytes(
        std::uint32_t packed) noexcept {
    if ((packed >> 28) != 2) return std::nullopt;
    const std::uint16_t major = packed >> 28;
    const std::uint16_t minor = (packed >> 24) & 15;
    const std::uint16_t build = (packed >> 8) & 0xFFFF;
    const std::uint16_t qfe = packed & 0xFF;
    return std::array<std::uint8_t, 8>{
        std::uint8_t(major >> 8), std::uint8_t(major),
        std::uint8_t(minor >> 8), std::uint8_t(minor),
        std::uint8_t(build >> 8), std::uint8_t(build),
        std::uint8_t(qfe >> 8), std::uint8_t(qfe)};
}
}
