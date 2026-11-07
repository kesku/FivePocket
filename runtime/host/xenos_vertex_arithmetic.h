#pragma once
#include "xenos_float_vertex_fetch.h"
#include "xenos_unorm8_vertex_fetch.h"
#include "xenos_geometry_vertex_fetch.h"
#include <bitset>
#include <cfenv>
#include <limits>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace recomp::gpu {
struct VertexArithmeticInputs {
    const std::uint8_t* vertices=nullptr;
    std::size_t vertexBytes=0;
    std::uint32_t fetchSlot=0;
    std::array<std::array<std::uint32_t,4>,256> constantBits{};
    std::array<std::uint8_t,256> constantKnown{};
    bool packedGeometry=false; // Opt-in owned256KiB geometry; not draw admission.
};
enum class VertexArithmeticFailure : unsigned {
    None, InvalidInput, ControlLimit, UnsupportedControl, ExecutionRange,
    Allocation, ExecutionLimit, Fetch, FetchLimit, Index, Source, Alu,
    Destination, Arithmetic, MissingExports
};
struct VertexArithmeticError {VertexArithmeticFailure failure=VertexArithmeticFailure::None;std::uint32_t instruction=0;};
struct VertexArithmeticResult {
    std::array<KnownVertexVector,64> exports{};
    std::bitset<64> exported;
    std::bitset<256> constantsRead;
    std::array<std::uint32_t,256> executedAddresses{};
    unsigned controlCount=0,executedCount=0,fetchCount=0;
};
inline bool regularVertexFloatBits(std::uint32_t bits) noexcept {
    bits&=0x7FFFFFFF;return !bits || (bits>=0x00800000 && bits<0x7F800000);
}
inline std::uint32_t vertexFloatBits(float value) noexcept {
    std::uint32_t bits=0;std::memcpy(&bits,&value,4);return bits;
}
inline bool vertexArithmeticEnvironmentSupported() noexcept {
#if defined(__FAST_MATH__)
    return false;
#elif defined(__aarch64__) || defined(__arm64__)
    std::uint64_t control=0;__asm__ volatile("mrs %0, fpcr" : "=r"(control));
    // FZ, FZ16, alternate handling and flush-input modes must not turn a
    // rejected subnormal result into an apparently valid zero before checking.
    return !(control&((std::uint64_t{1}<<24)|(std::uint64_t{1}<<19)|3)) && std::fegetround()==FE_TONEAREST;
#elif defined(__x86_64__) || defined(_M_X64)
    return !(_mm_getcsr()&((1u<<15)|(1u<<6))) && std::fegetround()==FE_TONEAREST;
#else
    return false; // No untested floating-point control-register assumption.
#endif
}
// A pure bounded CPU preflight over caller-owned bytes, not GPU execution or a
// hardware float/raster proof. One explicit input slot, unchanged initial r0.x
// auto-index, float/unorm8 fetches and straight-line MAX-copy/MUL/unfused MAD.
// Reject all raw nonfinite/subnormal float inputs, unknown operands and non-RNE
// rounding. Multiplication by either signed zero produces +0 by explicit policy.
// Volatile intermediate storage prevents contraction of MAD into FMA.
//
// ALLOC presence/kind/repetition is checked, but its encoded size is metadata,
// NOT a proven hardware export extent (same distinction as the Python dataflow
// analyzer). Output slots are limited to position62/parameters0..15. This does
// not establish shader linkage, register side effects or full command admission.
// Inputs must remain valid/unmodified for this synchronous call. No callbacks,
// allocations or caller-visible writes occur until the complete result succeeds.
inline bool evaluateVertexArithmetic(const std::uint8_t* bytes,std::size_t size,
        const VertexArithmeticInputs& inputs,std::uint32_t index,
        VertexArithmeticResult& output,VertexArithmeticError& error) noexcept {
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
    const auto fail=[&](VertexArithmeticFailure why,unsigned at=0){error={why,at};return false;};
    if(!bytes || !size || size%12 || size>4096 || !inputs.vertices || !inputs.vertexBytes ||
       inputs.vertexBytes>(inputs.packedGeometry?256u*1024:4096u) || inputs.vertexBytes%4 || inputs.fetchSlot>=96 || index>=0x1000000 ||
       !vertexArithmeticEnvironmentSupported())return fail(VertexArithmeticFailure::InvalidInput);
    const auto word=[&](unsigned i){const auto* p=bytes+i*4;
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];};
    struct Control {unsigned op=0,first=0,count=0,sequence=0;};
    std::array<Control,128> controls{};unsigned count=0;
    const auto extent=static_cast<unsigned>(size/12);auto body=extent;bool ended=false;
    for(unsigned pair=0;pair<body && !ended;++pair){
        const auto a=word(pair*3),b=word(pair*3+1),c=word(pair*3+2);
        const std::array<std::uint64_t,2> packed{{a|(std::uint64_t{b&65535}<<32),(b>>16)|(std::uint64_t{c}<<16)}};
        for(auto cf:packed){
            if(count==controls.size())return fail(VertexArithmeticFailure::ControlLimit,count);
            Control control;control.op=static_cast<unsigned>(cf>>44);
            if(!control.op){if(cf)return fail(VertexArithmeticFailure::UnsupportedControl,count);}
            else if(control.op==12){
                if(cf&~((std::uint64_t{15}<<44)|(std::uint64_t{7}<<40)|7))return fail(VertexArithmeticFailure::Allocation,count);
                control.first=(cf>>41)&3;control.count=(cf&7)+1;
                if(control.first!=1 && control.first!=2)return fail(VertexArithmeticFailure::Allocation,count);
            }else if(control.op==1 || control.op==2){
                if(cf&~((std::uint64_t{15}<<44)|(std::uint64_t{1}<<43)|(std::uint64_t{1}<<41)|((std::uint64_t{1}<<34)-1)))
                    return fail(VertexArithmeticFailure::UnsupportedControl,count);
                control.first=cf&4095;control.count=(cf>>12)&7;control.sequence=(cf>>16)&4095;
                if(!control.count || control.count>6 || control.first+control.count>extent ||
                    (control.sequence>>(control.count*2)))return fail(VertexArithmeticFailure::ExecutionRange,count);
                if(control.first<body)body=control.first;ended=control.op==2;
            }else return fail(VertexArithmeticFailure::UnsupportedControl,count);
            controls[count++]=control;if(ended)break;
        }
    }
    if(!ended || (count+1)/2>body)return fail(VertexArithmeticFailure::ExecutionRange);
    // Tokens preserve operand identity, including saved full-fetch index and
    // simultaneous swizzled copies. Arithmetic tokens are unique per instruction
    // execution/lane; equivalent separately calculated expressions may reject.
    struct Value {float value=0;unsigned token=0;bool known=false;};
    using Vector=std::array<Value,4>;
    std::array<Vector,64> regs{},exports{};
    for(unsigned r=0;r<64;++r)for(unsigned lane=0;lane<4;++lane)regs[r][lane].token=3+r*4+lane;
    regs[0][0].known=true;regs[0][0].value=static_cast<float>(index);
    VertexArithmeticResult candidate;candidate.controlCount=count;
    unsigned allocations=0;std::optional<VertexFetchLayout> previous;
    for(unsigned controlIndex=0;controlIndex<count;++controlIndex){
        const auto& control=controls[controlIndex];
        if(control.op==12){
            const auto bit=1u<<control.first;
            if(allocations&bit)return fail(VertexArithmeticFailure::Allocation,controlIndex);
            allocations|=bit;
        }
        if(control.op!=1 && control.op!=2)continue;
        for(unsigned at=control.first;at<control.first+control.count;++at){
            if(candidate.executedCount==256)return fail(VertexArithmeticFailure::ExecutionLimit,at);
            const auto a=word(at*3),b=word(at*3+1),c=word(at*3+2);
            if(control.sequence&(1u<<((at-control.first)*2))){
                if(candidate.fetchCount==32)return fail(VertexArithmeticFailure::FetchLimit,at);
                const auto format=(b>>16)&63;
                const bool packed=format==6 || (inputs.packedGeometry && (format==7 || format==31 || format==32));
                VertexFetchLayout layout;Unorm8VertexFetch unorm;GeometryVertexFetch geometry;
                if(inputs.packedGeometry){if(!decodeGeometryVertexFetch({a,b,c},previous,geometry))return fail(VertexArithmeticFailure::Fetch,at);layout=geometry.layout;}
                else if(packed){if(!decodeUnorm8VertexFetch({a,b,c},previous,unorm))return fail(VertexArithmeticFailure::Fetch,at);layout=unorm.layout;}
                else if(!decodeFloatVertexFetch({a,b,c},previous,layout))return fail(VertexArithmeticFailure::Fetch,at);
                if(layout.fetchSlot!=inputs.fetchSlot)return fail(VertexArithmeticFailure::Fetch,at);
                if(!layout.mini){
                    const auto source=regs[layout.sourceRegister][layout.sourceComponent];
                    if(!source.known || source.token!=3)return fail(VertexArithmeticFailure::Index,at);
                    previous=layout; // Every accepted full fetch has the same saved initial index.
                }
                const auto start=(std::uint64_t{index}*layout.strideWords+layout.offsetWords)*4;
                const auto sourceWords=packed?(format==32?2u:1u):layout.components;
                if(start+sourceWords*4>inputs.vertexBytes)return fail(VertexArithmeticFailure::Source,at);
                if(!packed)for(unsigned lane=0;lane<sourceWords;++lane){
                    const auto* p=inputs.vertices+start+lane*4;
                    const auto bits=(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
                    if(!regularVertexFloatBits(bits))return fail(VertexArithmeticFailure::Source,at);
                }
                KnownVertexVector prior,read;
                auto& target=regs[layout.destinationRegister];
                for(unsigned lane=0;lane<4;++lane)if(target[lane].known){prior.known|=1u<<lane;prior.values[lane]=target[lane].value;}
                const bool okay=inputs.packedGeometry?readGeometryVertexFetch(geometry,inputs.vertices,inputs.vertexBytes,index,prior,read):
                    packed?readUnorm8VertexFetch(unorm,inputs.vertices,inputs.vertexBytes,index,prior,read):
                    readFloatVertexFetch(layout,inputs.vertices,inputs.vertexBytes,index,prior,read);
                if(!okay)return fail(VertexArithmeticFailure::Source,at);
                for(unsigned lane=0;lane<4;++lane){const auto swizzle=layout.swizzle[lane];
                    if(swizzle==7)continue;
                    target[lane]={read.values[lane],swizzle<4?1283+candidate.fetchCount*4+swizzle:1u+swizzle-4,true};
                }
                ++candidate.fetchCount;
            }else{
                const auto op=(c>>24)&31;
                if((op!=1 && op!=2 && op!=11) || a>>26!=50 || (a&((1u<<6)|(1u<<7)|(3u<<24))) || b>>24)
                    return fail(VertexArithmeticFailure::Alu,at);
                const bool exported=a&(1u<<15),relativeScalar=a&(1u<<14);
                const auto dest=a&63,vector=(a>>16)&15,scalar=(a>>20)&15;
                const auto vectorResult=exported?vector&~scalar:vector,scalarResult=exported?scalar&~vector:scalar;
                const auto zero=exported && relativeScalar?15&~(vector|scalar):0,one=exported?vector&scalar:0;
                if(scalarResult)return fail(VertexArithmeticFailure::Alu,at);
                if(!exported && relativeScalar)return fail(VertexArithmeticFailure::Destination,at);
                if(exported && (!(allocations&(dest==62?2u:4u)) || (dest!=62 && dest>=16)))
                    return fail(VertexArithmeticFailure::Destination,at);
                auto& target=exported?exports[dest]:regs[dest];auto value=target;
                for(unsigned lane=0;lane<4;++lane){
                    if(vectorResult&(1u<<lane)){
                        std::array<Value,3> operands{};
                        for(unsigned i=0;i<(op==11?3u:2u);++i){
                            const auto shift=16-i*8,source=(c>>shift)&255,swizzle=(b>>shift)&255;
                            const auto component=((swizzle>>(lane*2))+lane)&3;
                            if(c&(1u<<(31-i))){
                                if(source>=64)return fail(VertexArithmeticFailure::Source,at);
                                operands[i]=regs[source][component];
                            }else{
                                if((inputs.constantKnown[source]&~15) || !(inputs.constantKnown[source]&(1u<<component)))
                                    return fail(VertexArithmeticFailure::Source,at);
                                const auto bits=inputs.constantBits[source][component];
                                if(!regularVertexFloatBits(bits))return fail(VertexArithmeticFailure::Source,at);
                                float f=0;std::memcpy(&f,&bits,4);
                                operands[i]={f,259+source*4+component,true};candidate.constantsRead.set(source);
                            }
                            if(!operands[i].known || !regularVertexFloatBits(vertexFloatBits(operands[i].value)))
                                return fail(VertexArithmeticFailure::Source,at);
                        }
                        if(op==2){
                            if(operands[0].token!=operands[1].token)return fail(VertexArithmeticFailure::Alu,at);
                            value[lane]=operands[0];
                        }else{
                            volatile float product=operands[0].value==0 || operands[1].value==0?0:operands[0].value*operands[1].value;
                            if(!regularVertexFloatBits(vertexFloatBits(product)))return fail(VertexArithmeticFailure::Arithmetic,at);
                            volatile float sum=op==11?product+operands[2].value:product;
                            if(!regularVertexFloatBits(vertexFloatBits(sum)))return fail(VertexArithmeticFailure::Arithmetic,at);
                            value[lane]={sum,4096+candidate.executedCount*4+lane,true};
                        }
                    }
                    if(zero&(1u<<lane))value[lane]={0,1,true};if(one&(1u<<lane))value[lane]={1,2,true};
                }
                target=value;if(exported)candidate.exported.set(dest);
            }
            candidate.executedAddresses[candidate.executedCount++]=at;
        }
    }
    if(candidate.exported.none())return fail(VertexArithmeticFailure::MissingExports);
    for(unsigned slot=0;slot<64;++slot)if(candidate.exported[slot])for(unsigned lane=0;lane<4;++lane)
        if(exports[slot][lane].known){candidate.exports[slot].values[lane]=exports[slot][lane].value;candidate.exports[slot].known|=1u<<lane;}
    output=candidate;error={};return true;
}
}
