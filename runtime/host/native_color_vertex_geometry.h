#pragma once
#include "native_color_chain_inputs.h"
#include "native_color_chain_program.h"
#include "xenos_vertex_arithmetic.h"

namespace recomp::gpu {
struct NativeColorVertexGeometry {
    std::array<std::uint32_t,4> uniformTintBits{};
    std::array<unsigned,4> executedInstructions{},fetches{};
};
// Complete CPU preflight of all four vertices for the narrow mapped color
// profile. Original guest exports must form the exact W1/Z0 clip rectangle,
// XY (or XYXY) UV corners and bounded uniform tint. It is not enough that each
// generated VS invocation can individually return an outside-clip position.
//
// This establishes the bounded arithmetic/geometry contract, NOT hardware ALLOC
// extent, arbitrary clipping, vertex/pixel linkage, sampling/blending fidelity,
// remaining fixed-function state, cache effects or ownership. Inputs remain
// borrowed; the capture caller keeps them alive and unchanged. No GPU/guest work.
inline bool planNativeColorVertexGeometryInputs(const NativeColorShaderIdentity& shader,
        const VertexArithmeticInputs& input,unsigned role,NativeColorVertexGeometry& output) noexcept {
    if(role>1 || shader.stage || !input.vertices || input.vertexBytes!=144 || input.fetchSlot!=95)return false;
    NativeColorVertexGeometry candidate;
    for(unsigned i=0;i<4;++i){
        VertexArithmeticResult result;VertexArithmeticError error;
        if(!evaluateVertexArithmetic(shader.bigEndianCode.data(),shader.bigEndianCode.size(),input,i,result,error) ||
            result.exported.count()!=3 || !result.exported[0] || !result.exported[1] || !result.exported[62] ||
            result.exports[62].known!=15 || result.exports[1].known!=15 ||
            (result.exports[0].known&(role?3:15))!=(role?3:15))return false;
        const std::array<std::uint32_t,4> position{{i/2?0x3F800000u:0xBF800000u,i%2?0x3F800000u:0xBF800000u,0,0x3F800000}};
        const std::array<std::uint32_t,4> uv{{i/2?0x3F800000u:0,i%2?0:0x3F800000u,i/2?0x3F800000u:0,i%2?0:0x3F800000u}};
        for(unsigned lane=0;lane<4;++lane){
            if(vertexFloatBits(result.exports[62].values[lane])!=position[lane] ||
                (lane<(role?2u:4u) && vertexFloatBits(result.exports[0].values[lane])!=uv[lane]))return false;
            const auto tint=result.exports[1].values[lane];
            if(std::abs(tint)>16)return false;
            const auto bits=vertexFloatBits(tint);
            if(i && bits!=candidate.uniformTintBits[lane])return false;
            candidate.uniformTintBits[lane]=bits;
        }
        candidate.executedInstructions[i]=result.executedCount;candidate.fetches[i]=result.fetchCount;
    }
    output=candidate;return true;
}
inline bool planNativeColorVertexGeometry(const NativeColorShaderIdentity& shader,
        const NativeColorPairBindings& pair,unsigned role,NativeColorVertexGeometry& output) noexcept {
    if(role>1 || shader.stage || !color_chain_inputs::validVertex(pair.vertices,95,144) ||
        pair.vertexConstants.size()!=(role?4u:5u))return false;
    VertexArithmeticInputs input;input.vertices=pair.vertices.source.bytes;input.vertexBytes=144;input.fetchSlot=95;
    for(const auto& c:pair.vertexConstants){
        if((c.index<8 || c.index>11) && (role || c.index!=36))return false;
        if(input.constantKnown[c.index])return false;
        for(unsigned lane=0;lane<4;++lane){
            if(c.words[lane].knownMask!=0xFFFFFFFF)return false;
            input.constantBits[c.index][lane]=c.words[lane].bits;
        }
        input.constantKnown[c.index]=15;
    }
    return planNativeColorVertexGeometryInputs(shader,input,role,output);
}
}
