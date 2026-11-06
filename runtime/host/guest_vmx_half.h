#pragma once

#include <cstdint>

namespace recomp::vmx {

// Integer implementation of the F16C round-toward-zero behavior used by
// Xenia's FLOAT16 pack backend. Independent of the host FP rounding/flush mode.
constexpr std::uint16_t floatToHalfRtz(std::uint32_t bits) {
    const auto sign = (bits >> 16) & 0x8000u;
    const auto exponent = (bits >> 23) & 0xffu;
    const auto fraction = bits & 0x7fffffu;
    if (exponent == 255)
        return static_cast<std::uint16_t>(sign | 0x7c00u |
                (fraction ? (0x200u | (fraction >> 13)) : 0u));
    if (exponent >= 143)
        return static_cast<std::uint16_t>(sign | 0x7bffu);
    if (exponent >= 113)
        return static_cast<std::uint16_t>(sign | ((exponent - 112) << 10) |
                (fraction >> 13));
    if (exponent >= 103)
        return static_cast<std::uint16_t>(sign |
                ((0x800000u | fraction) >> (126 - exponent)));
    return static_cast<std::uint16_t>(sign);
}

constexpr std::uint32_t halfToFloat(std::uint16_t bits) {
    const auto sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16;
    auto exponent = static_cast<std::uint32_t>((bits >> 10) & 31u);
    auto fraction = static_cast<std::uint32_t>(bits & 0x3ffu);
    if (exponent == 31)
        return sign | 0x7f800000u | (fraction << 13) |
                (fraction ? 0x400000u : 0u);
    if (exponent)
        return sign | ((exponent + 112) << 23) | (fraction << 13);
    if (!fraction)
        return sign;
    exponent = 113;
    while (!(fraction & 0x400u)) {
        fraction <<= 1;
        --exponent;
    }
    return sign | (exponent << 23) | ((fraction & 0x3ffu) << 13);
}

// PPCVRegister host storage is W,Z,Y,X. Snapshot both packed words before
// changing the destination, including when source and destination alias.
template<unsigned Components, unsigned Mask, unsigned Shift>
inline void packHalf(std::uint32_t (&dst)[4], const std::uint32_t (&src)[4]) {
    static_assert(Components == 2 || Components == 4);
    static_assert(Mask >= 1 && Mask <= 3 && Shift <= 3);
    const std::uint32_t xy = (std::uint32_t(floatToHalfRtz(src[3])) << 16) |
            floatToHalfRtz(src[2]);
    const std::uint32_t zw = (std::uint32_t(floatToHalfRtz(src[1])) << 16) |
            floatToHalfRtz(src[0]);
    const std::uint32_t low = Components == 4 ? zw : xy;
    const std::uint32_t high = Components == 4 ? xy : 0;
    if constexpr (Mask == 3 && Shift == 3) {
        dst[0] = high;
    } else {
        dst[Shift] = low;
        if constexpr (Mask != 1 && Shift < 3)
            dst[Shift + 1] = high;
    }
}

template<unsigned Components>
inline void unpackHalf(std::uint32_t (&dst)[4], const std::uint32_t (&src)[4]) {
    static_assert(Components == 2 || Components == 4);
    const auto xy = src[Components == 4 ? 1 : 0];
    const auto zw = src[0];
    dst[3] = halfToFloat(static_cast<std::uint16_t>(xy >> 16));
    dst[2] = halfToFloat(static_cast<std::uint16_t>(xy));
    dst[1] = Components == 4 ? halfToFloat(static_cast<std::uint16_t>(zw >> 16)) : 0;
    dst[0] = Components == 4 ? halfToFloat(static_cast<std::uint16_t>(zw)) : 0x3f800000u;
}

} // namespace recomp::vmx
