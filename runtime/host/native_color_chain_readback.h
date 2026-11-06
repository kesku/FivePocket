#pragma once
#include "native_color_chain_profile.h"
#include "xenos_color_7e3.h"
#include "xenos_color_copy.h"
#include <algorithm>

namespace recomp::gpu {
inline bool nativeColorChainHalfBits(std::uint16_t half,std::uint32_t& bits) noexcept {
    const unsigned exponent=(half>>10)&31,mantissa=half&1023;
    if(exponent==31 || (exponent==0 && mantissa))return false;
    bits=((half&0x8000u)<<16)|(exponent?((exponent+112)<<23)|(mantissa<<13):0);
    return true;
}
// Internal consistency audit of ACTUAL native half -> EDRAM -> tiled bytes.
// No expected scene pixels are supplied. This is the explicit bounded reference
// conversion policy, not proof of retail ROP fidelity, draw execution or a
// fence. Caller supplies stable, host-visible buffers AFTER the real fence.
inline bool auditNativeColorChainReadback(const std::uint8_t* half,std::size_t halfBytes,
        const std::uint8_t* edram,std::size_t edramBytes,const std::uint8_t* tiled,std::size_t tiledBytes) noexcept {
    using P=NativeColorChainProfile<true,true,true>;
    if(!half || !edram || !tiled || halfBytes!=P::OutputBytes || edramBytes!=P::BackingBytes || tiledBytes!=P::CopyBytes)return false;
    const auto guarded=[](const std::uint8_t* data,std::size_t bytes){
        for(unsigned i=0;i<16;++i)if(data[i]!=0x5A || data[bytes-16+i]!=0x5A)return false;return true;
    };
    if(!guarded(half,halfBytes) || !guarded(edram,edramBytes) || !guarded(tiled,tiledBytes))return false;
    const auto canary=[](std::uint8_t value){return value==0x5A;};
    if(!std::all_of(edram+16,edram+16+3686400,canary) ||
        !std::all_of(edram+16+7372800,edram+edramBytes-16,canary))return false;
    const AliasView view{0x14000500,0xC02D0,1280,720,false};
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){
        const auto* pixel=half+16+(y*1280+x)*8;std::array<std::uint32_t,4> rgba{};
        for(unsigned c=0;c<4;++c)
            if(!nativeColorChainHalfBits(static_cast<std::uint16_t>(pixel[c*2]|(unsigned(pixel[c*2+1])<<8)),rgba[c]))return false;
        std::uint32_t packed=0,from=0,to=0;
        if(!packColor7e3(rgba,packed) || !aliasSampleOffset(view,x,y,0,from) || !colorCopyOffset(1280,720,x,y,to))return false;
        const auto copied=copyColorWord(packed,true);
        for(unsigned c=0;c<4;++c)
            if(edram[16+from+c]!=static_cast<std::uint8_t>(packed>>(c*8)) ||
               tiled[16+to+c]!=static_cast<std::uint8_t>(copied>>(c*8)))return false;
    }
    // The final macro-tile row has exactly 16 rows of untouched padding.
    for(unsigned y=720;y<736;++y)for(unsigned x=0;x<1280;++x){
        std::uint32_t to=0;if(!colorCopyOffset(1280,736,x,y,to))return false;
        for(unsigned c=0;c<4;++c)if(tiled[16+to+c]!=0x5A)return false;
    }
    return true;
}
}
