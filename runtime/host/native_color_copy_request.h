#pragma once
#include "xenos_copy_shader.h"
#include "xenos_color_copy.h"
#include "xenos_draw_command_plan.h"
#include <algorithm>

namespace recomp::gpu {
struct NativeColorCopyRequest {
    ColorCopyPlan copy;
    std::uint32_t drawOffset=0,vertexPhysicalAddress=0;
    std::array<std::uint8_t,24> vertexBytes{};
};
// Narrow full-origin color-copy geometry/storage request. The copy VS must
// prove exactly one slot0 float2 auto-index fetch, stride2/offset0, exported as
// (x,y,1,1), with no color/memory export or other input. Three exact TL/TR/BR
// source pairs establish the entire 1280x720 rectangle without float arithmetic.
// Native bit patterns are IEEE754 normal values; subnormals/nonfinite/outside
// rectangles cannot pass by host/GPU flush-to-zero behavior.
//
// This still does NOT implement the copy-mode hardware path, full raster state,
// WAIT/cache/register effects, owner/alias checks or guest writes. It checks the
// input geometry and storage profile consumed by the bounded native copy helper.
// Reader must pin ownership and park all guest writers throughout admission.
inline bool planNativeColorCopyRequest(const DrawCommandPlan& plan,unsigned drawIndex,
        const Pm4PhysicalReader& reader,NativeColorCopyRequest& output) noexcept {
    if (!reader || drawIndex>=plan.draws.size() || plan.shaders.size()>8) return false;
    const auto& draw=plan.draws[drawIndex];
    if (draw.indexCount!=3 || draw.primitive!=8 || (draw.opcode!=0x22 && draw.opcode!=0x36) ||
        evaluateDrawBinPredicate(draw.predicate.state,draw.predicate.predicated)!=DrawBinDecision::Pass ||
        draw.shaders[0]>=plan.shaders.size()) return false;
    const auto& shader=plan.shaders[draw.shaders[0]];
    if (shader.stage!=0 || shader.offsetWords>=draw.offsetWords) return false;
    FloatCopyShaderProof proof;CopyShaderError error;
    if (!analyzeFloatCopyShader(shader.bigEndianCode.data(),shader.bigEndianCode.size(),proof,error) ||
        proof.fetchCount!=1 || proof.exported.count()!=1 || !proof.exported[62] ||
        proof.initialComponentsRead.count()!=1 || !proof.initialComponentsRead[0]) return false;
    const auto& fetch=proof.fetches[0];const auto& layout=fetch.layout;
    const CopyShaderProof::Vector position{{CopyShaderProof::Fetch,CopyShaderProof::Fetch+1,CopyShaderProof::One,CopyShaderProof::One}};
    if (proof.exports[62]!=position || fetch.indexExpression!=CopyShaderProof::Initial || layout.fetchSlot ||
        layout.mini || layout.components!=2 || layout.strideWords!=2 || layout.offsetWords) return false;
    const auto required=[&](unsigned index,unsigned expected) {
        const auto it=draw.registers.find(index);return it!=draw.registers.end() && it->second.bits.equals(expected);
    };
    for (const auto& pair:std::array<std::array<unsigned,2>,9>{{
        {{0x2208,6}},{{0x2081,0}},{{0x2082,0x2D00500}},{{0x2301,0}},
        {{0x2000,0x14000500}},{{0x2001,0xC02D0}},{{0x2318,0x100000}},
        {{0x231A,0x2D00500}},{{0x231B,0x1000300}}}})
        if (!required(pair[0],pair[1])) return false;
    const auto destination=draw.registers.find(0x2319),vertex=draw.registers.find(0x4800);
    if (destination==draw.registers.end() || destination->second.bits.known!=0xFFFFFFFF ||
        vertex==draw.registers.end() || vertex->second.bits.known!=0xFFFFFFFF ||
        (vertex->second.bits.value&3)!=3 || !required(0x4801,0x1000001A)) return false;
    NativeColorCopyRequest candidate;candidate.drawOffset=draw.offsetWords;
    candidate.vertexPhysicalAddress=vertex->second.bits.value&~3u;
    if (std::uint64_t{candidate.vertexPhysicalAddress}+24>0x20000000 ||
        !planColorCopy({0x14000500,0xC02D0,1280,720,false},0x100000,destination->second.bits.value,
            0x2D00500,0x1000300,candidate.copy)) return false;
    try {
        const auto source=reader(candidate.vertexPhysicalAddress,24);
        if (!source || source->size()!=24) return false;
        // -0.5, 1279.5 and 719.5, respectively. Exact IEEE754 encodings.
        constexpr std::array<std::uint32_t,6> expected{{0xBF000000,0xBF000000,0x449FF000,0xBF000000,0x449FF000,0x4433E000}};
        for (unsigned i=0;i<6;++i) for (unsigned b=0;b<4;++b)
            if ((*source)[i*4+b]!=static_cast<std::uint8_t>(expected[i]>>(24-b*8))) return false;
        std::copy(source->begin(),source->end(),candidate.vertexBytes.begin());
        output=candidate;return true;
    } catch (...) {return false;}
}
}
