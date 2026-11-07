#pragma once

#include <cstdint>

namespace recomp::vmx {

// VPACK_NORMSHORT4 uses biased float bits, not ordinary SNORM values.
// Xenia 95a5c3ee PACK/UNPACK::EmitSHORT_4 and the corresponding XMM constants.
constexpr std::uint32_t normShort4Component(std::uint32_t bits) {
    constexpr std::uint32_t minimum = 0x403f8001u, maximum = 0x40407fffu;
    if ((bits & 0x80000000u) || bits > 0x7f800000u || bits < minimum)
        bits = minimum;
    else if (bits > maximum)
        bits = maximum;
    return bits & 0xffffu;
}

template<unsigned Mask, unsigned Shift>
inline void packNormShort4(std::uint32_t (&dst)[4], const std::uint32_t (&src)[4]) {
    static_assert(Mask >= 1 && Mask <= 3 && Shift <= 3);
    const auto xy = (normShort4Component(src[3]) << 16) | normShort4Component(src[2]);
    const auto zw = (normShort4Component(src[1]) << 16) | normShort4Component(src[0]);
    if constexpr (Mask == 3 && Shift == 3) {
        dst[0] = xy;
    } else {
        dst[Shift] = zw;
        if constexpr (Mask != 1 && Shift < 3)
            dst[Shift + 1] = xy;
    }
}

inline void unpackNormShort4(std::uint32_t (&dst)[4], const std::uint32_t (&src)[4]) {
    // Snapshot both input words for VD == VB, with host lane order W,Z,Y,X.
    const std::uint32_t fields[4] = {src[1] >> 16, src[1] & 0xffffu,
                                    src[0] >> 16, src[0] & 0xffffu};
    for (unsigned component = 0; component < 4; ++component) {
        const auto bits = fields[component];
        dst[3 - component] = bits == 0x8000u ? 0x7fc00000u :
            0x40400000u + ((bits ^ 0x8000u) - 0x8000u);
    }
}

} // namespace recomp::vmx
