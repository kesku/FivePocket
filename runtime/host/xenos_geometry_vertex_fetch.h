#pragma once
#include "xenos_float_vertex_fetch.h"
#include "xenos_unorm8_vertex_fetch.h"
#include <algorithm>

namespace recomp::gpu {
struct GeometryVertexFetch {VertexFetchLayout layout;std::uint32_t format=0;};
// Explicit geometry profile; legacy float/unorm readers keep their old limits.
// Packed layout/conversion follows Xenia spirv_shader_translator_fetch.cc:
// low-first signed normalized 10/10/10/2 and finite half2/half4. Xenos half exponent31
// is extended range, not IEEE Inf/NaN; reject it until separately implemented.
inline bool decodeGeometryVertexFetch(const std::array<std::uint32_t,3>& words,
        const std::optional<VertexFetchLayout>& previous,GeometryVertexFetch& output) {
    GeometryVertexFetch candidate;candidate.format=(words[1]>>16)&63;
    if(candidate.format==7 || candidate.format==31 || candidate.format==32) {
        const auto modifiers=candidate.format==7?0x1000u:0x3000u;
        if((words[1]&0x3FC0F000)!=modifiers ||
           !decodeVertexFetchLayout(words,previous,candidate.format==31?2:4,candidate.layout))return false;
    }else if(candidate.format==6) {
        Unorm8VertexFetch unorm;if(!decodeUnorm8VertexFetch(words,previous,unorm))return false;
        candidate.layout=unorm.layout;
    }else if(!decodeFloatVertexFetch(words,previous,candidate.layout))return false;
    output=candidate;return true;
}
inline bool readGeometryVertexFetch(const GeometryVertexFetch& fetch,const std::uint8_t* bytes,
        std::size_t size,std::uint32_t index,const KnownVertexVector& prior,KnownVertexVector& output) {
    const auto& plan=fetch.layout;
    const auto format=fetch.format;
    const auto components=format==7 || format==6 || format==32 || format==38?4u:format==31 || format==37?2u:
        format==57?3u:format==36?1u:0u;
    if(!bytes || !size || size>256*1024 || size%4 || index>=0x1000000 || !components ||
       plan.components!=components || !plan.strideWords || plan.strideWords>255 ||
       plan.offsetWords>=0x400000 || plan.fetchSlot>=96 || plan.destinationRegister>=64 ||
       plan.sourceRegister>=64 || plan.sourceComponent>=4 || (prior.known&~15))return false;
    const bool packed=format==7 || format==31 || format==32 || format==6;
    const auto begin=(std::uint64_t{index}*plan.strideWords+plan.offsetWords)*4;
    const auto needed=format==32?8u:packed?4u:components*4;
    if(begin+needed>size)return false;
    // Reuse existing conversion/swizzle on an exact checked element, with
    // addressing already resolved. No unrelated old admission limit is widened.
    auto local=plan;local.offsetWords=0;
    if(format==6)return readUnorm8VertexFetch({local},bytes+begin,needed,0,prior,output);
    if(!packed)return readFloatVertexFetch(local,bytes+begin,needed,0,prior,output);
    std::array<float,4> values{};
    for(unsigned lane=0;lane<components;++lane) {
        const auto* p=bytes+begin+(format==32?lane/2*4:0);
        const auto word=(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
        if(format==7) {
            const auto width=lane==3?2u:10u,shift=lane==3?30u:lane*10;
            const auto raw=(word>>shift)&((1u<<width)-1),sign=1u<<(width-1);
            const auto signedValue=static_cast<int>(raw^sign)-static_cast<int>(sign);
            volatile float normalized=static_cast<float>(signedValue)*(1.f/static_cast<float>(sign-1));
            values[lane]=std::max(-1.f,static_cast<float>(normalized));
        }else {
            const auto half=(word>>((lane%2)*16))&65535,exponent=(half>>10)&31,mantissa=half&1023;
            if(exponent==31)return false;
            // Integer significands and powers of two give an exact float32,
            // including half subnormals (which are normal float32 numbers).
            const float magnitude=std::ldexp(static_cast<float>(exponent?1024+mantissa:mantissa),
                exponent?static_cast<int>(exponent)-25:-24);
            values[lane]=std::copysign(magnitude,half&0x8000?-1.f:1.f);
        }
    }
    return applyVertexFetchSwizzle(plan,values,prior,output);
}
}
