#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace recomp::gpu {
inline unsigned roundPositiveFloatEven(float value) {
    const auto floor=static_cast<unsigned>(value);
    const auto part=value-float(floor);
    return floor+(part>.5f || (part==.5f && (floor&1)));
}
// GTA V's original 82347E30 format0/1 clear conversion, exponent0, finite [0,1].
// Format1 calls822DAF30: round linear*1023 BEFORE the piecewise truncation.
// This detail differs from the unrounded reference LinearToPWLGamma in pinned
// Xenia95a5c3ee xenos.cc. Do not use pow/sRGB or convert alpha through gamma.
inline std::optional<std::array<std::uint8_t,4>> packGta5Color8Clear(
        const std::array<float,4>& rgba,unsigned format) {
    if(format>1)return std::nullopt;
    std::array<std::uint8_t,4> result{};
    for(unsigned c=0;c<4;++c) {
        const float f=rgba[c];
        if(!std::isfinite(f) || f<0 || f>1)return std::nullopt;
        if(format==1 && c<3) {
            const volatile float scaled=f*1023.f;
            const auto quantized=roundPositiveFloatEven(scaled);
            if(f<64.f/1023.f)result[c]=quantized;
            else if(f<128.f/1023.f)result[c]=(quantized>>1)+32;
            else if(f<512.f/1023.f)result[c]=(quantized>>2)+64;
            else result[c]=(quantized>>3)+128;
        } else {
            const volatile float scaled=f*255.f;
            result[c]=roundPositiveFloatEven(scaled);
        }
    }
    return result;
}
// Canonical linear values represented by stored PWL gamma bytes. Integer
// specialization of Xenia95a5c3ee PWLGammaToLinear (and Source SDK's Xbox
// conversion); no approximation with a hardware sRGB attachment. This is a
// storage representation helper, not proof of arbitrary gamma blending/resolve.
inline unsigned xenosPwlGammaByteToUnorm10(std::uint8_t gamma) {
    unsigned value=gamma;
    if(gamma>=192){value=unsigned(gamma)*8-1024;value+=value/128;}
    else if(gamma>=96){value=unsigned(gamma)*4-256;value+=value/256;}
    else if(gamma>=64){value=unsigned(gamma)*2-64;value+=value/512;}
    return value;
}
inline float xenosPwlGammaByteToLinear(std::uint8_t gamma) {
    return float(xenosPwlGammaByteToUnorm10(gamma))/1023.f;
}
}
