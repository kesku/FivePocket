#pragma once
#include "native_ordered_fragment7e3_stream.h"

namespace recomp::gpu {
// A larger mesh does not change ABI33's four-primitive shader contract or its
// fixed stream capacity. Execute each original index triplet with local ID0,
// fence/validate it, then remap to guest order. The caller publishes the paired
// physical owners ONLY after every batch succeeds. No partial draw receipts.
// Only shaders without guest primitive-ID/attachment feedback may use this.
template<class Execute>
std::optional<NativeOrderedFragment7e3Batch> executeNativeOrderedTriangleBatches(
        std::span<const unsigned> indices,Execute&& execute,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeOrderedFragment7e3Batch>{};};
    if(indices.empty() || indices.size()>36 || indices.size()%3)return no("ordered-triangles-index-extent");
    NativeOrderedFragment7e3Batch result{static_cast<unsigned>(indices.size()/3),{}};
    for(unsigned primitive=0;primitive<result.primitiveCount;++primitive) {
        auto batch=execute(indices.subspan(3*primitive,3));
        if(!batch || batch->primitiveCount!=1 || batch->fragments.size()>OrderedFragment7e3MaxCapacity)
            return no("ordered-triangles-incomplete-batch");
        unsigned previous=0;bool first=true;
        for(auto& f:batch->fragments) {
            if(f.primitive!=0 || f.pixel>=1280*720 || (!first && f.pixel<=previous))
                return no("ordered-triangles-batch-order");
            first=false;previous=f.pixel;f.primitive=primitive;result.fragments.push_back(std::move(f));
        }
    }
    if(why)*why="fenced-ordered-triangle-batches";
    return result;
}
}
