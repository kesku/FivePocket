#pragma once
#include "native_ordered_fragment7e3_stream.h"

namespace recomp::gpu {
inline constexpr unsigned OrderedMeshMaxTriangles=4096;
inline constexpr unsigned OrderedMeshMaxFragments=2*1280*720;
inline constexpr unsigned OrderedWindowRows=OrderedFragment7e3MaxCapacity/1280;
// Diagnostic/execution building block, NOT guest draw admission. Full-width,
// non-overlapping row bands partition the target exactly. One triangle per
// band guarantees at most width*height single-sample invocations in a stream.
// The unchanged ABI33 overflow/guard/fence checks remain mandatory in Execute.
// Aggregate only in original primitive then pixel order; refuse before growing
// beyond the bounded whole-mesh budget. Caller publishes only a COMPLETE batch.
template<class Execute>
std::optional<NativeOrderedFragment7e3Batch> executeNativeOrderedTriangleWindows(
        std::span<const unsigned> indices,Execute&& execute,const char** why=nullptr,
        unsigned budget=OrderedMeshMaxFragments) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeOrderedFragment7e3Batch>{};};
    if(indices.empty() || indices.size()>3*OrderedMeshMaxTriangles || indices.size()%3 ||
       !budget || budget>OrderedMeshMaxFragments)return no("ordered-windows-extent-or-budget");
    NativeOrderedFragment7e3Batch result{static_cast<unsigned>(indices.size()/3),{}};
    for(unsigned primitive=0;primitive<result.primitiveCount;++primitive) {
        unsigned previous=0;bool first=true;
        for(unsigned row=0;row<720;row+=OrderedWindowRows) {
            const unsigned rows=std::min(OrderedWindowRows,720-row);
            auto batch=execute(indices.subspan(3*primitive,3),row,rows);
            if(!batch || batch->primitiveCount!=1 || batch->fragments.size()>1280*rows)
                return no("ordered-windows-incomplete-band");
            if(batch->fragments.size()>budget-result.fragments.size())
                return no("ordered-windows-mesh-budget");
            for(auto& f:batch->fragments) {
                if(f.primitive!=0 || f.pixel<1280*row || f.pixel>=1280*(row+rows) ||
                   (!first && f.pixel<=previous))return no("ordered-windows-band-order-or-range");
                first=false;previous=f.pixel;f.primitive=primitive;result.fragments.push_back(std::move(f));
            }
        }
    }
    if(why)*why="fenced-ordered-triangle-windows";
    return result;
}
}
