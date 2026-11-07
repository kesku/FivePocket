#pragma once
#include "xenos_vertex_fetch_layout.h"
#include <cstddef>

namespace recomp::gpu {
struct Unorm8VertexFetch { VertexFetchLayout layout; };
// Explicit unsigned normalized 8_8_8_8 only. Signed/integer/repeating-fraction,
// exponent, rounding and reserved fields reject. Default float-only decode is
// unchanged. No GPU resource binding or full shader execution is performed.
inline bool decodeUnorm8VertexFetch(const std::array<std::uint32_t,3>& words,
        const std::optional<VertexFetchLayout>& previousFull, Unorm8VertexFetch& output) {
    if (((words[1]>>16)&63)!=6 || (words[1]&0x3FC0F000)) return false;
    Unorm8VertexFetch candidate;
    if (!decodeVertexFetchLayout(words,previousFull,4,candidate.layout)) return false;
    output=candidate; return true;
}
inline bool readUnorm8VertexFetch(const Unorm8VertexFetch& fetch, const std::uint8_t* bytes,
        std::size_t size, std::uint32_t index, const KnownVertexVector& prior,
        KnownVertexVector& output) {
    const auto& plan=fetch.layout;
    if (!bytes || !size || size>4096 || size%4 || index>=0x1000000 ||
        plan.components!=4 || !plan.strideWords || plan.strideWords>255 ||
        plan.offsetWords>=0x400000 || plan.fetchSlot>=96 || plan.destinationRegister>=64 ||
        plan.sourceRegister>=64 || plan.sourceComponent>=4 || (prior.known&~15)) return false;
    const std::uint64_t begin=(std::uint64_t{index}*plan.strideWords+plan.offsetWords)*4;
    if (begin+4>size) return false;
    const auto* p=bytes+begin;
    const std::uint32_t word=(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|
        (std::uint32_t{p[2]}<<8)|p[3];
    std::array<float,4> components{};
    // Explicit float32 reciprocal-multiply policy (pinned Xenia's conversion),
    // not a claim of retail precision/rounding for every normalized value.
    for(unsigned i=0;i<4;++i) components[i]=static_cast<float>((word>>(i*8))&255)*(1.0f/255.0f);
    return applyVertexFetchSwizzle(plan,components,prior,output);
}
}
