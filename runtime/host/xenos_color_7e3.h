#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

// Explicit finite-input reference policy, not a retail pixel/ROP oracle.
// RGB decode: unsigned7e3, bias3, no Inf/NaN codes. Encode: saturating nearest
// even, accepted only where pinned Xenia's SPIR-V shift/round algorithm agrees.
// Alpha: clamp -> separate float32 *3 -> +0.5 -> truncate (Xenia Vulkan policy).
namespace recomp::gpu {
// XeniaShiftRound follows PreClampedFloat32To7e3 in the pinned Xenia
// spirv_shader_translator_rb.cc (BSD attribution: docs/licenses/XENIA.txt).
// It deliberately preserves the source's discarded sticky bits. This is an
// explicit emulator policy, NOT a claim of Xbox hardware rounding equivalence.
enum class Color7e3Rounding { RequireNearestEvenAgreement, XeniaShiftRound };
inline bool validColor7e3Rounding(Color7e3Rounding p) {
    return p==Color7e3Rounding::RequireNearestEvenAgreement || p==Color7e3Rounding::XeniaShiftRound;
}
inline float colorFloat(std::uint32_t bits){float v=0;std::memcpy(&v,&bits,4);return v;}
inline std::uint32_t colorBits(float v){std::uint32_t b=0;std::memcpy(&b,&v,4);return b;}
inline bool unpack7e3(std::uint32_t code,std::uint32_t& out){
    if(code>1023)return false;
    const unsigned e=code>>7,m=code&127;
    const float value=std::ldexp(static_cast<float>(e?128+m:m),e?static_cast<int>(e)-10:-9);
    out=colorBits(value);return true;
}
inline bool pack7e3Finite(std::uint32_t bits,std::uint32_t& out,Color7e3Rounding policy){
    if(!validColor7e3Rounding(policy))return false;
    if((bits&0x7FFFFFFFu)>=0x7F800000)return false;
    if(bits&0x80000000u){out=0;return true;}
    if(bits>=0x41FF0000){out=1023;return true;}
    std::uint32_t biased=0;
    if(bits<0x3E800000){
        const unsigned shift=125-(bits>>23);
        biased=(0x800000|(bits&0x7FFFFF))>>(shift<24?shift:24);
    }else biased=bits-(124u<<23);
    const auto candidate=((biased+0x7FFF+((biased>>16)&1))>>16)&1023;
    if(bits<0x3E800000 && policy==Color7e3Rounding::RequireNearestEvenAgreement){
        // Independent exact nearest-even check. The source algorithm drops
        // sticky bits before rounding denormal7e3; ambiguous boundaries reject.
        const double scaled=static_cast<double>(colorFloat(bits))*512.0;
        const auto lower=static_cast<unsigned>(std::floor(scaled));
        const double remainder=scaled-lower;
        const unsigned nearest=lower+(remainder>.5 || (remainder==.5 && (lower&1)));
        if(candidate!=nearest)return false;
    }
    out=candidate;return true;
}
inline bool pack7e3Checked(std::uint32_t bits,std::uint32_t& out){
    return pack7e3Finite(bits,out,Color7e3Rounding::RequireNearestEvenAgreement);
}
inline bool packColor7e3(const std::array<std::uint32_t,4>& rgba,std::uint32_t& out,
        Color7e3Rounding policy=Color7e3Rounding::RequireNearestEvenAgreement){
    std::array<std::uint32_t,3> rgb{};
    for(unsigned i=0;i<3;++i)if(!pack7e3Finite(rgba[i],rgb[i],policy))return false;
    if((rgba[3]&0x7FFFFFFFu)>=0x7F800000)return false;
    float alpha=colorFloat(rgba[3]);if(alpha<0)alpha=0;if(alpha>1)alpha=1;
    volatile float scaled=alpha*3.0f;
    volatile float shifted=scaled+.5f; // Require two rounded operations, no FMA.
    const unsigned a=static_cast<unsigned>(shifted);
    out=rgb[0]|(rgb[1]<<10)|(rgb[2]<<20)|(a<<30);return true;
}
inline std::array<std::uint32_t,4> unpackColor7e3(std::uint32_t word){
    std::array<std::uint32_t,4> out{};
    for(unsigned i=0;i<3;++i)unpack7e3((word>>(i*10))&1023,out[i]);
    const float a=static_cast<float>(word>>30)*(1.0f/3.0f);out[3]=colorBits(a);return out;
}
}
