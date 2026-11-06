#pragma once
#include "native_edram32_bands.h"
#include "xenos_color_7e3.h"

namespace recomp::gpu {
// Completed full-RGBA replacement into ordinary format3. This is ONLY the
// fenced-output -> physical-storage boundary, not shader/raster/state/query
// admission. Caller must prove single-sample full coverage, replacement
// blending and all-channel writes before supplying completed floating output.
// No display, ring, guest texture publication, or invented padding contents.
inline bool recordCompletedNativeRgb7e3Replacement(NativeEdram32Mutation& mutation,
        const NativeOffscreenDrawPlan& plan,const std::vector<float>& rgba,
        Color7e3Rounding rounding=Color7e3Rounding::RequireNearestEvenAgreement) {
    const auto tileCount=std::uint64_t{plan.pitch/80}*((std::uint64_t{plan.height}+15)/16);
    if(!validColor7e3Rounding(rounding) || !plan.object || plan.format!=3 ||
       !plan.width || !plan.height || plan.width>plan.pitch || !plan.pitch || plan.pitch%80 ||
       plan.baseTile>=2048 || !tileCount || tileCount>2048-plan.baseTile ||
       plan.tileCount!=tileCount || rgba.size()!=std::uint64_t{plan.width}*plan.height*4)
        return false;
    std::vector<std::uint8_t> bytes(rgba.size());
    // Validate and encode the ENTIRE output before mutating even the candidate.
    // Partial final rows retain both bytes and known/unknown provenance.
    for(std::size_t i=0;i<rgba.size();i+=4) {
        std::array<std::uint32_t,4> bits{};
        for(unsigned c=0;c<4;++c)bits[c]=colorBits(rgba[i+c]);
        unsigned word=0;if(!packColor7e3(bits,word,rounding))return false;
        for(unsigned c=0;c<4;++c)bytes[i+c]=word>>(c*8);
    }
    auto& owner=mutation.candidate();
    return owner.recordCompletedRawColorRows(plan.baseTile,plan.pitch,plan.width,plan.height,
                                             bytes,owner.nextSerial());
}
// Completed ONE/ONE additive blend (RB_BLENDCONTROL 0x01010101: colour and
// alpha src ONE, dst ONE, ADD) into ordinary format3: per channel
// unpack7e3(destination)+source in ONE fp32 add, then packColor7e3 (which
// saturates to the 7e3/alpha ranges). Same boundary contract as the
// replacement above; the caller additionally proves the blend words. EVERY
// destination word of the written rectangle must already be KNOWN (partially
// known final tiles included) or nothing happens. Rows/columns outside the
// rectangle keep their words and known/unknown provenance. Non-finite sums
// refuse. The whole result is encoded before the candidate is touched.
inline bool recordCompletedNativeRgb7e3AdditiveBlend(NativeEdram32Mutation& mutation,
        const NativeOffscreenDrawPlan& plan,const std::vector<float>& rgba,
        Color7e3Rounding rounding,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    const auto tileCount=std::uint64_t{plan.pitch/80}*((std::uint64_t{plan.height}+15)/16);
    if(!validColor7e3Rounding(rounding) || !plan.object || plan.format!=3 ||
       !plan.width || !plan.height || plan.width>plan.pitch || !plan.pitch || plan.pitch%80 ||
       plan.baseTile>=2048 || !tileCount || tileCount>2048-plan.baseTile ||
       plan.tileCount!=tileCount || rgba.size()!=std::uint64_t{plan.width}*plan.height*4)
        return no("additive-blend-input");
    auto& owner=mutation.candidate();
    const auto destination=owner.readKnownColorRows(plan.baseTile,plan.pitch,plan.width,plan.height);
    if(!destination || destination->size()*4!=rgba.size())return no("destination-colour-unknown");
    std::vector<std::uint8_t> bytes(rgba.size());
    for(std::size_t i=0;i<destination->size();++i) {
        const auto before=unpackColor7e3((*destination)[i]);
        std::array<std::uint32_t,4> bits{};
        for(unsigned c=0;c<4;++c) {
            const float sum=colorFloat(before[c])+rgba[i*4+c];
            if(!std::isfinite(sum))return no("nonfinite-additive-blend");
            bits[c]=colorBits(sum);
        }
        unsigned word=0;if(!packColor7e3(bits,word,rounding))return no("additive-blend-pack");
        for(unsigned c=0;c<4;++c)bytes[i*4+c]=word>>(c*8);
    }
    if(!owner.recordCompletedRawColorRows(plan.baseTile,plan.pitch,plan.width,plan.height,bytes,owner.nextSerial()))
        return no("additive-blend-record");
    return true;
}
}
