#pragma once
#include "native_edram32_bands.h"
#include "xenos_color_7e3.h"
#include <cmath>

namespace recomp::gpu {
struct NativeIndexedRgb7e3Readback {
    std::vector<float> rgba;
    std::vector<std::uint32_t> depthStencil;
};
// Replacement only, GEQUAL + depth write, stencil ALWAYS/REPLACE0. Depth is
// TRUNC20e4 before EACH native test, so final RGB7e3 packing cannot be moved
// across a destination-dependent blend. The callback owes fenced execution
// of the admitted original pair, not merely plausible colour/depth values.
template<class Execute>
bool executeNativeIndexedRgb7e3Physical(NativeEdram32Mutation& mutation,Execute&& execute,
        const char** why=nullptr) {
    const auto no=[&](const char* text){if(why)*why=text;return false;};
    auto& owner=mutation.candidate();
    const auto color=owner.readColorWords(720,1280,1280,720);
    const auto depth=owner.readDepthWords(0,1280,1280,720);
    if(!color || !depth)return no("indexed-rgb7e3-physical-source-unknown");
    const auto pixels=color->size();std::vector<float> rgba(pixels*4);
    for(std::size_t i=0;i<pixels;++i) {
        const auto value=unpackColor7e3((*color)[i]);
        for(unsigned c=0;c<4;++c)rgba[4*i+c]=colorFloat(value[c]);
    }
    if(!owner.invalidate(0,1440))return no("indexed-rgb7e3-physical-invalidate");
    const auto result=execute(rgba,*depth);
    if(!result || result->rgba.size()!=rgba.size() || result->depthStencil.size()!=pixels)
        return no("indexed-rgb7e3-fenced-readback");
    std::vector<std::uint8_t> bytes(pixels*4);
    for(std::size_t i=0;i<pixels;++i) {
        const auto before=(*depth)[i],after=result->depthStencil[i],z=after>>8;
        std::array<unsigned,4> value{};
        for(unsigned c=0;c<4;++c)value[c]=colorBits(result->rgba[4*i+c]);
        unsigned packed=0;if(!packColor7e3(value,packed))return no("indexed-rgb7e3-finite-color");
        // Viewport [2^-22,2^-24] bounds code [0x1000,0x400]. Successful
        // writes are monotone GEQUAL and replace stencil with reference0.
        const bool legal=z>=0x400u && z<=0x1000u && z>=(before>>8) && !(after&255);
        if(after!=before && !legal)return no("indexed-rgb7e3-depth-stencil-effect");
        if(packed!=(*color)[i] && !legal)return no("indexed-rgb7e3-color-depth-connection");
        for(unsigned c=0;c<4;++c)bytes[4*i+c]=packed>>(8*c);
    }
    if(!owner.recordCompletedRawColorRows(720,1280,1280,720,bytes,owner.nextSerial()) ||
       !owner.recordCompletedDepthWords(0,1280,1280,720,result->depthStencil,owner.nextSerial()))
        return no("indexed-rgb7e3-physical-publication");
    mutation.commit();if(why)*why="fenced-indexed-rgb7e3-depth20e4-stencil";return true;
}
}
