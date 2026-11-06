#pragma once
#include "xenos_color_7e3.h"
#include "xenos_float24_depth.h"
#include <algorithm>

namespace recomp::gpu {
// Finite-input output-merger component, NOT draw admission or a rasterizer.
// Contract: one covered sample; alpha GREATER 0; depth GE + write; no stencil
// test/write; blend 0x07060706; format12's expanded blend intermediates followed
// by format3 storage. Caller supplies ordered fragments and current owners.
// Xenia spirv_shader_translator_rb.cc tests raw alpha BEFORE fixed-point alpha
// clamping, applies separate source/destination terms, and packs each write.
// BSD attribution: docs/licenses/XENIA.txt. Deliberate local restrictions:
// finite inputs, explicit TRUNC20e4 policy. Legacy callers require ideal7e3
// rounding agreement; XeniaShiftRound is an explicit separate packing policy.
// This is NOT a measured Xbox raster, depth-rounding or blend-precision oracle.
struct NativeFragment7e3Depth {
    std::array<float,4> color;
    float depth;
};
enum class NativeFragmentDisposition { AlphaRejected, DepthRejected, Written };
struct NativeFragment7e3Result {
    std::uint32_t color,depthStencil;
    NativeFragmentDisposition disposition;
};
inline std::optional<NativeFragment7e3Result> mergeNativeFragment7e3Depth(
        const NativeFragment7e3Depth& source,std::uint32_t destinationColor,
        std::uint32_t destinationDepthStencil,
        Color7e3Rounding rounding=Color7e3Rounding::RequireNearestEvenAgreement) {
    if(!validColor7e3Rounding(rounding))return std::nullopt;
    for(auto v:source.color)if(!std::isfinite(v))return std::nullopt;
    const auto depth=truncateXenosFloat24Depth(source.depth);
    if(!depth)return std::nullopt;
    NativeFragment7e3Result result{destinationColor,destinationDepthStencil,
        NativeFragmentDisposition::AlphaRejected};
    if(!(source.color[3]>0.f))return result;
    result.disposition=NativeFragmentDisposition::DepthRejected;
    // Unsigned20e4 codes are monotonic. Compare QUANTIZED incoming depth, not
    // raw float32; preserve all stencil bits even when depth is written.
    if(*depth<(destinationDepthStencil>>8))return result;
    const auto destination=unpackColor7e3(destinationColor);
    const float alpha=std::clamp(source.color[3],0.f,1.f);
    volatile float inverse=1.f-alpha;
    std::array<std::uint32_t,4> blended{};
    for(unsigned c=0;c<4;++c) {
        // Separate float32 operations (including alpha*alpha), not FMA.
        volatile float src=(c==3?alpha:source.color[c])*alpha;
        volatile float dst=colorFloat(destination[c])*inverse;
        volatile float sum=src+dst;
        if(!std::isfinite(sum))return std::nullopt;
        blended[c]=colorBits(sum);
    }
    if(!packColor7e3(blended,result.color,rounding))return std::nullopt;
    result.depthStencil=(*depth<<8)|(destinationDepthStencil&255u);
    result.disposition=NativeFragmentDisposition::Written;
    return result;
}
}
