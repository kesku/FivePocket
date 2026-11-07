// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "present_fast.h"
#include <cstdlib>
#include <stdexcept>
#include <string_view>
namespace recomp::gpu::scanout_fusion {
enum class Outcome { NotApplicable, Success, Failure };
inline int policy(const char* value) noexcept {
    if(!value || std::string_view(value)=="0")return 0;
    if(std::string_view(value)=="1")return 1;
    return -1;
}
inline bool enabled(){
    static const bool value=[] {
        const int p=policy(std::getenv("RECOMP_IOS_SCANOUT_FUSION"));
        if(p<0)throw std::invalid_argument("RECOMP_IOS_SCANOUT_FUSION must be 0 or 1");
        return p==1;
    }();
    return value;
}
inline Outcome map(const NativeScalerDisplayState& scaler,const NativeDisplayGammaRamp* ramp,
                   const std::uint8_t* input,std::size_t bytes,unsigned width,unsigned height,
                   std::vector<std::uint8_t>& output) {
    if(!ramp)return Outcome::NotApplicable;
    using C=NativeIdentityScalerCandidate;
    constexpr std::size_t PixelBytes=std::size_t{C::Width}*C::Height*4;
    if(!scaler.configured() || !input || bytes!=PixelBytes || width!=C::Width || height!=C::Height ||
       reinterpret_cast<std::uintptr_t>(input)>std::numeric_limits<std::uintptr_t>::max()-bytes)return Outcome::Failure;
    const auto src=reinterpret_cast<std::uintptr_t>(input);
    const auto dst=reinterpret_cast<std::uintptr_t>(output.data());
    const auto capacity=output.capacity();
    // Any alias of allocated output storage can be invalidated by resize; leave original path untouched.
    if(capacity && (dst>std::numeric_limits<std::uintptr_t>::max()-capacity ||
                    (src<dst+capacity && dst<src+bytes)))return Outcome::NotApplicable;
    std::array<std::array<std::uint8_t,256>,3> lut{};
    for(unsigned c=0;c<3;++c)
        for(unsigned v=0;v<256;++v)lut[c][v]=static_cast<std::uint8_t>((ramp->sample(v,c)*255u+511u)/1023u);
    try{output.resize(PixelBytes);}catch(...){return Outcome::Failure;}
    auto* const outBytes=output.data();
    for(std::size_t i=0;i<bytes;i+=4){
        std::array<std::uint8_t,4> pixel;
        std::memcpy(pixel.data(),input+i,4);
        pixel[0]=lut[0][pixel[0]];pixel[1]=lut[1][pixel[1]];pixel[2]=lut[2][pixel[2]];
        std::memcpy(outBytes+i,pixel.data(),4);
    }
    return Outcome::Success;
}
}
