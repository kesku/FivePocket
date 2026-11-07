#pragma once
#include "native_ordered_fragment7e3_physical.h"
#include <span>

namespace recomp::gpu {
// Explicit private/production-candidate wire contract, not legacy ABI32 draw
// admission. Header8 words: four guards, count, error bits, two guards.
// Each10-word record: primitive, pixel, RGBA4, depth, encoded depth, native
// FragCoord depth (diagnostic), guard. Append order may be arbitrary; primitive
// order must NOT be inferred from atomics. Capacity is bounded and overflow
// fails the ENTIRE draw. Caller still owes current-input and successful-fence
// evidence; static guard words are not a substitute for either.
inline constexpr unsigned OrderedFragment7e3Guard=0x5A5A5A5A;
inline constexpr unsigned OrderedFragment7e3MaxCapacity=65536;
inline std::optional<NativeOrderedFragment7e3Batch> decodeNativeOrderedFragment7e3Stream(
        std::span<const std::uint32_t> words,unsigned capacity,unsigned primitiveCount) {
    if(!capacity || capacity>OrderedFragment7e3MaxCapacity || !primitiveCount || primitiveCount>65535 ||
       words.size()!=8+std::size_t{capacity}*10)return std::nullopt;
    for(unsigned i:{0u,1u,2u,3u,6u,7u})if(words[i]!=OrderedFragment7e3Guard)return std::nullopt;
    if(words[4]>capacity || words[5])return std::nullopt;
    NativeOrderedFragment7e3Batch result{primitiveCount,{}};result.fragments.reserve(words[4]);
    for(unsigned i=0;i<capacity;++i) {
        const auto r=words.subspan(8+std::size_t{i}*10,10);
        if(i>=words[4]) {
            for(auto value:r)if(value!=OrderedFragment7e3Guard)return std::nullopt;
            continue;
        }
        if(r[9]!=OrderedFragment7e3Guard || r[0]>=primitiveCount || r[1]>=1280*720)return std::nullopt;
        NativeFragment7e3Depth sample{{colorFloat(r[2]),colorFloat(r[3]),colorFloat(r[4]),colorFloat(r[5])},colorFloat(r[6])};
        for(auto v:sample.color)if(!std::isfinite(v))return std::nullopt;
        const auto encoded=truncateXenosFloat24Depth(sample.depth);
        const float nativeDepth=colorFloat(r[8]);
        if(!encoded || *encoded!=r[7] || !std::isfinite(nativeDepth) ||
           nativeDepth<0 || nativeDepth>1 || std::abs(nativeDepth-sample.depth)>0x1p-20f)return std::nullopt;
        result.fragments.push_back({r[0],r[1],sample});
    }
    const auto key=[](const auto& f){return std::uint64_t{f.primitive}*(1280*720)+f.pixel;};
    std::sort(result.fragments.begin(),result.fragments.end(),[&](const auto& a,const auto& b){return key(a)<key(b);});
    for(std::size_t i=1;i<result.fragments.size();++i)
        if(key(result.fragments[i-1])==key(result.fragments[i]))return std::nullopt;
    return result;
}
}
