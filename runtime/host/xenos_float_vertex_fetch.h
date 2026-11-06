#pragma once
#include "xenos_vertex_fetch_layout.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace recomp::gpu {
using FloatVertexFetch = VertexFetchLayout;
// Fixed bounded float32-only decode; does not execute control flow or a fetch.
// Previous FULL operands are inherited by mini, including source/index and
// stride, not its own encoded placeholders. Based on pinned Xenia
// ParseVertexFetchInstruction and XenosRecomp shader_code.h. No hardware cache
// timing, index rounding, negative offsets, packed formats or predicates.
inline bool decodeFloatVertexFetch(const std::array<std::uint32_t,3>& words,
        const std::optional<FloatVertexFetch>& previousFull, FloatVertexFetch& output) {
    const auto b=words[1];
    if ((b&0x3FC0F000)!=0x3000) return false;
    const auto format=(b>>16)&63;
    const std::uint32_t components=format==36?1:format==37?2:format==38?4:format==57?3:0;
    return decodeVertexFetchLayout(words,previousFull,components,output);
}
// CPU experiment over an owned exact BE buffer. Caller supplies the explicit
// auto-index for a full fetch; for mini it MUST retain that full fetch's saved
// index, not re-read an overwritten temporary register. No console index-input
// setup or full shader execution is asserted. Unknown KEEP lanes stay unknown.
inline bool readFloatVertexFetch(const FloatVertexFetch& plan, const std::uint8_t* bytes,
        std::size_t size, std::uint32_t index, const KnownVertexVector& prior,
        KnownVertexVector& output) {
    if (!bytes || !size || size>4096 || size%4 || index>=0x1000000 ||
        !plan.components || plan.components>4 || !plan.strideWords || plan.strideWords>255 ||
        plan.offsetWords>=0x400000 || plan.fetchSlot>=96 || plan.destinationRegister>=64 ||
        (prior.known&~15)) return false;
    const std::uint64_t begin=(std::uint64_t{index}*plan.strideWords+plan.offsetWords)*4;
    if (begin+plan.components*4>size) return false;
    std::array<float,4> source{};
    for (unsigned i=0;i<plan.components;++i) {
        const auto* p=bytes+begin+i*4;
        const std::uint32_t word=(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|
            (std::uint32_t{p[2]}<<8)|p[3];
        std::memcpy(&source[i],&word,4);
        if (!std::isfinite(source[i])) return false;
    }
    return applyVertexFetchSwizzle(plan,source,prior,output);
}
}
