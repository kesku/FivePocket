#pragma once
#include "native_color_chain_inputs.h"
#include "native_color_chain_program.h"
#include "xenos_copy_shader.h"
#include "xenos_draw_command_plan.h"

namespace recomp::gpu {
struct NativeDepthClearStateRequest {
    unsigned drawOffset=0,width=0,height=0,samples=0,sampleMask=0,stencilReference=0;
    unsigned interpolatorControl=0;
};
// Bounded pretransformed, unclipped full-origin depth/stencil request. Exact
// known state only; the active pixel shader must be side-effect-free oC0=r0.
// Its lack of kill/depth/memory export avoids assuming arbitrary PS behavior in
// depth-only mode. This does not claim Xbox sample positions, hierarchical Z,
// cache/register effects, or the native D32->reference UNORM24 storage is retail.
inline bool planNativeDepthClearState(const DrawCommandPlan& plan,unsigned index,
        NativeDepthClearStateRequest& output) noexcept {
    if(index>=plan.draws.size() || plan.shaders.size()>8)return false;
    const auto& draw=plan.draws[index];
    if(!draw.offsetWords || (draw.opcode!=0x22 && draw.opcode!=0x36) || draw.indexCount!=3 || draw.primitive!=8 ||
        evaluateDrawBinPredicate(draw.predicate.state,draw.predicate.predicated)!=DrawBinDecision::Pass ||
        draw.shaders[1]>=plan.shaders.size())return false;
    const auto& pixel=plan.shaders[draw.shaders[1]];
    if(pixel.stage!=1 || pixel.offsetWords>=draw.offsetWords)return false;
    CopyShaderProof proof;CopyShaderError error;
    if(!analyzeCopyShader(pixel.bigEndianCode.data(),pixel.bigEndianCode.size(),CopyShaderStage::Pixel,proof,error) ||
        proof.exported.count()!=1 || !proof.exported[0] || proof.initialComponentsRead.count()!=4)return false;
    for(unsigned lane=0;lane<4;++lane)
        if(proof.exports[0][lane]!=CopyShaderProof::Initial+lane || !proof.initialComponentsRead[lane])return false;
    constexpr std::array<std::array<std::uint32_t,2>,28> required{{
        {{0x2000,0x0A020280}},{{0x2002,0x2D0}},{{0x200E,0}},{{0x200F,0x20002000}},
        {{0x2080,0}},{{0x2081,0}},{{0x2082,0x20002000}},
        {{0x2100,0xFFFF}},{{0x2101,0}},{{0x2102,0}},{{0x210D,0xFF0000}},
        {{0x2180,0x10010001}},{{0x2181,0}},{{0x2182,0}},{{0x2183,0}},{{0x2184,0}},
        {{0x2200,0x8777}},{{0x2203,0}},{{0x2204,0x10000}},{{0x2205,0x10000}},{{0x2206,0x300}},{{0x2208,5}},
        {{0x2284,0}},{{0x2292,0}},{{0x2293,0}},{{0x2301,0xC003}},{{0x2302,4}},{{0x2312,0xFFFF}}
    }};
    for(const auto& pair:required){
        const auto it=draw.registers.find(pair[0]);
        if(it==draw.registers.end())return false;
        // SQ_INTERPOLATOR_CNTL: the later clear retains 0x10003 from color.
        // Only these two exact states are admitted. The separately required
        // whole-depth geometry proves uniform color/depth, and this pixel
        // shader has no kill/depth/memory effect. No general interpolation
        // equivalence is assumed (Xenia95a5c3ee registers.h).
        if(pair[0]==0x2182){
            if(!it->second.bits.equals(0) && !it->second.bits.equals(0x10003))return false;
        }else if(!it->second.bits.equals(pair[1]))return false;
    }
    output={draw.offsetWords,640,360,4,15,0,draw.registers.at(0x2182).bits.value};return true;
}
struct NativeDepthClearGeometry {
    unsigned depthBits=0,executedInstructions=0;
    std::array<std::uint32_t,4> uniformColorBits{};
};
// Exact shader-derived two-fetch layout and full TL/TR/BR rectangle. Compare
// original integer words before float conversion: no host FTZ/NaN dependence.
// All three positions use the one canonical depth producing 0xC0000000 with
// stencil 0 under the separately tested reference packing. No CPU pixel upload.
// Source is borrowed and must be pinned/unchanged throughout this synchronous
// preflight. This proof neither owns guest allocation nor executes a clear.
inline bool planNativeDepthClearGeometry(const NativeColorShaderIdentity& shader,
        const NativeColorVertexBinding& vertex,NativeDepthClearGeometry& output) noexcept {
    if(shader.stage || !color_chain_inputs::validVertex(vertex,0,84))return false;
    FloatCopyShaderProof proof;CopyShaderError error;
    if(!analyzeFloatCopyShader(shader.bigEndianCode.data(),shader.bigEndianCode.size(),proof,error) ||
        proof.fetchCount!=2 || proof.exported.count()!=2 || !proof.exported[0] || !proof.exported[62] ||
        proof.initialComponentsRead.count()!=1 || !proof.initialComponentsRead[0])return false;
    const CopyShaderProof::Vector position{{CopyShaderProof::Fetch,CopyShaderProof::Fetch+1,CopyShaderProof::Fetch+2,CopyShaderProof::One}};
    const CopyShaderProof::Vector color{{CopyShaderProof::Fetch+4,CopyShaderProof::Fetch+5,CopyShaderProof::Fetch+6,CopyShaderProof::Fetch+7}};
    if(proof.exports[62]!=position || proof.exports[0]!=color)return false;
    for(unsigned i=0;i<2;++i){
        const auto& f=proof.fetches[i];const auto& layout=f.layout;
        if(f.indexExpression!=CopyShaderProof::Initial || layout.fetchSlot || layout.strideWords!=7 ||
            layout.offsetWords!=(i?3u:0u) || layout.components!=(i?4u:3u) || layout.mini!=(i!=0))return false;
    }
    NativeDepthClearGeometry candidate;candidate.depthBits=0x3F400001;candidate.executedInstructions=proof.executedCount;
    const auto word=[&](unsigned at){const auto* b=vertex.source.bytes+at;
        return (std::uint32_t{b[0]}<<24)|(std::uint32_t{b[1]}<<16)|(std::uint32_t{b[2]}<<8)|b[3];};
    for(unsigned i=0;i<3;++i){
        // -0.5, 639.5 and 359.5 are exact normal IEEE754 encodings.
        if(word(i*28)!=(i?0x441FE000u:0xBF000000u) || word(i*28+4)!=(i==2?0x43B3C000u:0xBF000000u) ||
            word(i*28+8)!=candidate.depthBits)return false;
        for(unsigned lane=0;lane<4;++lane){
            const auto bits=word(i*28+12+lane*4),abs=bits&0x7FFFFFFF;
            if(abs>0x41800000 || (abs && abs<0x00800000) || bits==0x80000000 ||
                (i && bits!=candidate.uniformColorBits[lane]))return false;
            candidate.uniformColorBits[lane]=bits;
        }
    }
    output=candidate;return true;
}
}
