#pragma once
#include "gta5_color8_clear.h"
#include <algorithm>

namespace recomp::gpu {
// Original 82347E30 format4, exponent0 branch (82348040): signed fixed-point
// RG, range [-32,32]. This is not half-float or RGBA8. The original multiplies
// by 1/32, clamps, multiplies by32767, adds32768, rounds nearest-even, converts
// and XORs0x8000. Keep the shifted rounding: direct round(x*32767) is not the
// same float32 computation around every midpoint. R occupies the low16 bits.
// Finite saturation is proven; NaN/Inf and exponent adjustments remain rejected.
inline std::optional<std::uint32_t> packGta5Fixed16Clear(const std::array<float,4>& rgba) {
    for(float f:rgba)if(!std::isfinite(f))return std::nullopt;
    std::uint32_t packed=0;
    for(unsigned c=0;c<2;++c) {
        const volatile float scaled=rgba[c]*(1.f/32.f);
        const float clamped=std::clamp(float(scaled),-1.f,1.f);
        const volatile float product=clamped*32767.f;
        const volatile float shifted=product+32768.f;
        const auto bits=std::uint16_t(roundPositiveFloatEven(shifted)^0x8000u);
        packed|=std::uint32_t{bits}<<(c*16);
    }
    return packed;
}
}
