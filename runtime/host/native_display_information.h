#pragma once
#include "guest_native_display.h"
#include <array>

namespace recomp::bootstrap {
// Native fixed logical720p60 SDR profile, not physical Android mode/overscan
// measurements. Public X_DISPLAY_INFO layout is88bytes; actual caller836B9E80
// consumes halfwords at72,74,86. Scaler fields describe configured metadata,
// not proof that a hardware scaler/filter ran. Zero overscan/reserved fields.
inline bool makeNativeDisplayInformation(const NativeDisplayConfiguration& configuration,
        std::array<std::uint8_t,88>& output) noexcept {
    if(!validNativeDisplayConfiguration(configuration) ||
        configuration.dimensions!=std::array<std::uint16_t,4>{1280,720,1280,720})return false;
    std::array<std::uint8_t,88> candidate{};
    const auto word=[&](unsigned at,std::uint32_t value,unsigned bytes){
        for(unsigned i=0;i<bytes;++i)candidate[at+i]=static_cast<std::uint8_t>(value>>((bytes-1-i)*8));
    };
    word(0,1280,2);word(2,720,2);
    // Color/pixel selector0: this native profile's RGB,8:8:8:8 output.
    for(unsigned i=0;i<14;++i)word(8+i*4,configuration.scaler[i],4);
    word(72,1280,2);word(74,720,2);word(76,0x42700000,4);word(86,1280,2);
    output=candidate;return true;
}
}
