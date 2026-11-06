#pragma once
#include "native_edram32_bands.h"
#include "xenos_color_7e3.h"
#include <span>

namespace recomp::gpu {
struct NativeSingleCoverage7e3Readback {
    std::vector<float> coverageRgba,rgba;
    std::vector<std::uint32_t> depthStencil;
};
inline bool validateNativeSingleCoverage7e3(std::span<const float> counts,
        std::span<const std::uint32_t> depth,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    if(depth.size()!=1280*720 || counts.size()!=depth.size()*4)
        return no("single-coverage7e3-count-extent");
    for(std::size_t i=0;i<depth.size();++i) {
        const float count=counts[4*i];
        if(count!=0.f && count!=1.f)return no("single-coverage7e3-overlap-or-invalid-count");
        for(unsigned c=1;c<4;++c)if(counts[4*i+c]!=count)
            return no("single-coverage7e3-count-channel-disagreement");
        if(count && ((depth[i]>>8)!=0 || (depth[i]&7)!=7))
            return no("single-coverage7e3-coverage-depth-stencil-connection");
    }
    return true;
}
// Xenia marks both 7e3 formats' alpha as fixed-point: it clamps source alpha
// BEFORE using it as a blend value/factor. A floating target is equivalent
// only in this finite [0,1] source domain. Validate CURRENT original-PS output,
// not a few reference pixels or an assumption about captured constants.
inline bool validateNativeSingleCoverage7e3Source(std::span<const float> counts,
        std::span<const float> source,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    if(counts.size()!=1280*720*4 || source.size()!=counts.size())
        return no("single-coverage7e3-source-extent");
    for(std::size_t i=0;i<source.size();i+=4) {
        const float count=counts[i];
        if(count!=0.f && count!=1.f)return no("single-coverage7e3-source-coverage-invalid");
        for(unsigned c=0;c<4;++c) {
            if(counts[i+c]!=count)return no("single-coverage7e3-source-coverage-invalid");
            if(!std::isfinite(source[i+c]))return no("single-coverage7e3-source-nonfinite");
            if(!count && source[i+c]!=0.f)return no("single-coverage7e3-source-coverage-disagreement");
        }
        if(source[i+3]<0.f || source[i+3]>1.f)return no("single-coverage7e3-source-alpha-range");
    }
    return true;
}
// Destination blending is safe to pack once ONLY after a fenced coverage
// witness proves <=1 contributing fragment at EVERY sample. The executor owes
// identical original VS/indices/raster/scissor/depth/stencil, no PS discard or
// depth export, single sampling, a fenced current-source finite alpha [0,1]
// proof, and separately fenced original blended colour. This
// is not permission to replace the game's PS with the coverage witness.
template<class Execute>
bool executeNativeSingleCoverage7e3Physical(NativeEdram32Mutation& mutation,Execute&& execute,
        const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    auto& owner=mutation.candidate();
    const auto color=owner.readColorWords(720,1280,1280,720),depth=owner.readDepthWords(0,1280,1280,720);
    if(!color || !depth)return no("single-coverage7e3-current-owner-unknown");
    const auto pixels=color->size();std::vector<float> rgba(pixels*4);
    for(std::size_t i=0;i<pixels;++i){const auto v=unpackColor7e3((*color)[i]);
        for(unsigned c=0;c<4;++c)rgba[4*i+c]=colorFloat(v[c]);}
    if(!owner.invalidate(720,720))return no("single-coverage7e3-invalidate");
    const auto result=execute(rgba,*depth);
    if(!result || result->rgba.size()!=rgba.size() || result->coverageRgba.size()!=rgba.size() ||
       result->depthStencil!=*depth)return no("single-coverage7e3-fenced-readback");
    if(!validateNativeSingleCoverage7e3(result->coverageRgba,*depth,why))return false;
    std::vector<std::uint8_t> bytes(pixels*4);
    for(std::size_t i=0;i<pixels;++i) {
        const float count=result->coverageRgba[4*i];
        std::array<unsigned,4> v{};for(unsigned c=0;c<4;++c)v[c]=colorBits(result->rgba[4*i+c]);
        unsigned packed=0;if(!packColor7e3(v,packed))return no("single-coverage7e3-finite-color");
        if(!count && packed!=(*color)[i])return no("single-coverage7e3-uncovered-color-change");
        for(unsigned c=0;c<4;++c)bytes[4*i+c]=packed>>(8*c);
    }
    if(!owner.recordCompletedRawColorRows(720,1280,1280,720,bytes,owner.nextSerial()))
        return no("single-coverage7e3-publication");
    mutation.commit();if(why)*why="fenced-single-coverage7e3-depth-stencil-preserved";return true;
}
}
