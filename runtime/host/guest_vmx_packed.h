#pragma once

#include <cstdint>

namespace recomp::vmx {

// Clamp the positive biased float representation without changing host FP state.
// Matches max(input, minimum) followed by min(result, maximum): unordered inputs
// select the minimum, including signaling NaNs and negative NaNs.
constexpr std::uint32_t clampNormPacked32Component(std::uint32_t bits,
        std::uint32_t minimum, std::uint32_t maximum) {
    if ((bits & 0x80000000u) || bits > 0x7f800000u || bits < minimum)
        return minimum;
    return bits > maximum ? maximum : bits;
}

// VPKD3D128 type2. Xenia 95a5c3ee PACK::EmitUINT_2101010 clamps XYZ to
// 3.0 + [-511,511]*2^-22 and W to 3.0 + [0,3]*2^-22, then retains their
// low bits. The packed word is replicated before the instruction's lane merge.
template<unsigned Mask, unsigned Shift>
inline void packNormPacked32(std::uint32_t (&dst)[4], const std::uint32_t (&src)[4]) {
    static_assert(Mask >= 1 && Mask <= 3 && Shift <= 3);
    std::uint32_t packed = 0;
    for (unsigned component = 0; component < 4; ++component) {
        const auto minimum = component == 3 ? 0x40400000u : 0x403ffe01u;
        const auto maximum = component == 3 ? 0x40400003u : 0x404001ffu;
        const auto mask = component == 3 ? 3u : 1023u;
        packed |= (clampNormPacked32Component(src[3 - component], minimum, maximum)
                & mask) << (component * 10);
    }
    // Compute all components before writing VD, which may also be VB.
    if constexpr (Mask == 3 && Shift == 3) {
        dst[0] = packed;
    } else {
        dst[Shift] = packed;
        if constexpr (Mask != 1 && Shift < 3)
            dst[Shift + 1] = packed;
    }
}

// VUPKD3D128 VPACK_NORMPACKED32. These are biased float bit patterns,
// not normalized [-1,1] values; the guest performs its own scale/subtract.
// Xenia 95a5c3ee, x64_seq_vector.cc UNPACK::EmitUINT_2101010 and
// x64_emitter.cc XMMUnpackUINT_2101010_Overflow specify the -512 qNaN.
// Host vector storage is W,Z,Y,X; the packed input is the low/W word.
inline void unpackNormPacked32(std::uint32_t (&dst)[4], const std::uint32_t (&src)[4]) {
    const std::uint32_t packed = src[0]; // Snapshot for in-place unpacking.
    for (unsigned component = 0; component < 3; ++component) {
        const auto bits = (packed >> (component * 10)) & 0x3ffu;
        dst[3 - component] = bits == 0x200u ? 0x7fc00000u :
                0x40400000u + ((bits ^ 0x200u) - 0x200u);
    }
    dst[0] = 0x3f800000u + (packed >> 30);
}

} // namespace recomp::vmx
