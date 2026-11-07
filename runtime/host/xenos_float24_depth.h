#pragma once
#include "xenos_float24_clear.h"

namespace recomp::gpu {
// Explicit native truncation policy for already-clamped depth. This matches
// the truncating20e4 representation in the pinned ReX/Xenia reference; it is
// NOT evidence of Xbox raster/interpolation or hardware rounding conformance.
// Separate from exact clears: representable values stay unchanged; other
// finite[0,1] values round toward zero. Never silently clamp invalid inputs.
inline std::optional<std::uint32_t> truncateXenosFloat24Depth(float value) {
    if(!std::isfinite(value) || value<0 || value>1)return std::nullopt;
    if(value<0x1p-14f)return static_cast<std::uint32_t>(std::floor(std::ldexp(double(value),34)));
    int exponent=0;const double fraction=std::frexp(double(value),&exponent);
    const auto mantissa=static_cast<std::uint32_t>(std::floor(std::ldexp(fraction,21)))-0x100000;
    return (std::uint32_t(exponent+14)<<20)|mantissa;
}
}
