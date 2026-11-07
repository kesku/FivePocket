#pragma once
#include <cmath>
#include <cstdint>
#include <optional>

namespace recomp::gpu {
// Xenos D24FS8's unsigned20e4 value: exponent bias15, no Inf/NaN encodings,
// denormals in units2^-34, normal values in[2^-14,2). Independent mathematical
// expression of the format described by pinned Xenia95a5c3ee xenos.cc.
inline std::optional<float> decodeXenosFloat24(std::uint32_t encoded) {
    if(encoded>0xFFFFFF)return std::nullopt;
    const auto exponent=encoded>>20,mantissa=encoded&0xFFFFF;
    return exponent?std::ldexp(float(0x100000+mantissa),int(exponent)-35):
        std::ldexp(float(mantissa),-34);
}
// Exact representation only. Do NOT select a hardware depth-rounding mode or
// confuse native D32 storage with general D24FS8 raster/depth-test equivalence.
inline std::optional<std::uint32_t> encodeExactXenosFloat24(float value) {
    if(!std::isfinite(value) || value<0 || value>=2)return std::nullopt;
    if(value==0)return 0;
    if(value<0x1p-14f) {
        const double scaled=std::ldexp(double(value),34);
        if(scaled!=std::floor(scaled))return std::nullopt;
        return static_cast<std::uint32_t>(scaled);
    }
    int binaryExponent=0;const double fraction=std::frexp(double(value),&binaryExponent);
    const double mantissa=std::ldexp(fraction,21)-0x100000;
    if(mantissa!=std::floor(mantissa))return std::nullopt;
    return (std::uint32_t(binaryExponent+14)<<20)|static_cast<std::uint32_t>(mantissa);
}
struct XenosFloat24Clear {
    std::uint32_t encoded=0,stencil=0;
    float depth=0;
    bool operator==(const XenosFloat24Clear&)const=default;
};
inline std::optional<XenosFloat24Clear> planExactXenosFloat24Clear(double depth,std::uint32_t stencil) {
    if(!std::isfinite(depth) || depth<0 || depth>1 || stencil>255)return std::nullopt;
    const float single=static_cast<float>(depth);
    if(double(single)!=depth)return std::nullopt; // Original stfs rounding not inferred.
    const auto encoded=encodeExactXenosFloat24(single);
    if(!encoded)return std::nullopt;
    return XenosFloat24Clear{*encoded,stencil,*decodeXenosFloat24(*encoded)};
}
}
