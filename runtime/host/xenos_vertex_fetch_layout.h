#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace recomp::gpu {
// Addressing and swizzle only: the caller must independently validate the
// format/modifier bits and apply the corresponding data conversion.
struct VertexFetchLayout {
    std::uint32_t sourceRegister=0, sourceComponent=0, destinationRegister=0, fetchSlot=0;
    std::uint32_t strideWords=0, offsetWords=0, components=0;
    std::array<std::uint8_t,4> swizzle{};
    bool mini=false;
};
struct KnownVertexVector {
    std::array<float,4> values{};
    std::uint8_t known=0;
};
inline bool decodeVertexFetchLayout(const std::array<std::uint32_t,3>& words,
        const std::optional<VertexFetchLayout>& previousFull, std::uint32_t components,
        VertexFetchLayout& output) {
    const auto a=words[0], b=words[1], c=words[2];
    if ((a&31) || !(a&(1<<19)) || (a&(1<<18)) || (b&(1u<<31)) || (c>>31) ||
            !components || components>4) return false;
    VertexFetchLayout candidate;
    candidate.mini=bool(b&(1u<<30));
    candidate.offsetWords=(c>>8)&0x7FFFFF;
    if (candidate.offsetWords&0x400000) return false;
    if (candidate.mini) {
        if (!previousFull || previousFull->mini || previousFull->fetchSlot>=96 ||
            !previousFull->strideWords || previousFull->strideWords>255 ||
            previousFull->sourceRegister>=64 || previousFull->sourceComponent>=4) return false;
        candidate.sourceRegister=previousFull->sourceRegister;
        candidate.sourceComponent=previousFull->sourceComponent;
        candidate.fetchSlot=previousFull->fetchSlot;
        candidate.strideWords=previousFull->strideWords;
    } else {
        const auto select=(a>>25)&3;
        if ((a&(1<<11)) || select==3 || !(c&255)) return false;
        candidate.sourceRegister=(a>>5)&63; candidate.sourceComponent=a>>30;
        candidate.fetchSlot=((a>>20)&31)*3+select; candidate.strideWords=c&255;
    }
    candidate.components=components; candidate.destinationRegister=(a>>12)&63;
    for (unsigned i=0;i<4;++i) {
        const auto v=(b>>(i*3))&7;
        if (v==6 || (v<4 && v>=components)) return false;
        candidate.swizzle[i]=static_cast<std::uint8_t>(v);
    }
    output=candidate; return true;
}
inline bool applyVertexFetchSwizzle(const VertexFetchLayout& plan,
        const std::array<float,4>& source, const KnownVertexVector& prior,
        KnownVertexVector& output) {
    if (!plan.components || plan.components>4 || (prior.known&~15)) return false;
    auto candidate=prior;
    for (unsigned i=0;i<4;++i) {
        const auto s=plan.swizzle[i];
        if (s==6 || s>7 || (s<4 && s>=plan.components)) return false;
        if (s==7) {
            if ((candidate.known&(1<<i)) && !std::isfinite(candidate.values[i])) return false;
            continue;
        }
        if (s<4 && !std::isfinite(source[s])) return false;
        candidate.values[i]=s<4?source[s]:static_cast<float>(s-4);
        candidate.known|=static_cast<std::uint8_t>(1<<i);
    }
    output=candidate; return true;
}
}
