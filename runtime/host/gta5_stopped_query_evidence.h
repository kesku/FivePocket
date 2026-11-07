#pragma once
#include "gta5_stopped_surface_evidence.h"
#include <ostream>

namespace recomp::gpu {
struct Gta5StoppedQueryEvidence {
    std::uint32_t device=0,sourceObject=0,sampleBranch=0,queryObject=0,sampleAddress=0;
    std::vector<std::uint8_t> source,query,samples;
    const char* status="source-unavailable";
};
// Failed-draw diagnostic ONLY, inside the existing exclusive executor gate.
// Original823649C8 keeps its conditional sample-query branch in r20 and source
// object in r21. Direct caller823BD370 does not modify those nonvolatiles before
// calling823BD360. Require the observed original stack chain before assigning
// those meanings. Source is mutable TITLE IMAGE, not heap/initial image bytes.
// No execution, pointer masking, query completion or guest writes here.
inline std::optional<Gta5StoppedQueryEvidence> captureGta5StoppedQueryEvidence(
        std::uint32_t device,std::uint32_t r20,std::uint32_t r21,
        const Gta5StoppedSurfaceEvidence& caller,const Gta5ResourceReader& image,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics) noexcept {
    try {
        if(!device || device%4 || r20>1 || r21!=0x83817028 ||
           caller.link!=0x823BD3A0 || caller.frames.size()<2 ||
           caller.frames[0].savedLink!=0x82364C30 || caller.frames[1].savedLink!=0x8234F018)
            return std::nullopt;
        Gta5StoppedQueryEvidence result;result.device=device;result.sourceObject=r21;result.sampleBranch=r20;
        const auto exact=[](const Gta5ResourceReader& reader,std::uint32_t at,unsigned size) {
            if(!reader || !at || at%4 || at>UINT32_MAX-(size-1))return std::optional<std::vector<std::uint8_t>>{};
            auto b=reader(at,size);if(b && b->size()!=size)b.reset();return b;
        };
        auto source=exact(image,r21,168);if(!source)return result;result.source=std::move(*source);
        const auto word=[](const auto& b,unsigned at) {
            return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
                (std::uint32_t{b[at+2]}<<8)|b[at+3];
        };
        result.queryObject=word(result.source,120);
        result.status="query-unavailable";
        auto query=exact(heap,result.queryObject,156);if(!query)return result;result.query=std::move(*query);
        if(word(result.query,0)!=device || word(result.query,4)!=9) {
            result.status="query-owner-or-type-unmatched";return result;
        }
        if(word(result.query,148)!=1 || word(result.query,152)!=1) {
            result.status="multi-slot-query-uninventoried";return result;
        }
        result.sampleAddress=word(result.query,28);
        result.status="sample-storage-unavailable";
        auto samples=exact(graphics,result.sampleAddress,64);if(!samples)return result;
        result.samples=std::move(*samples);result.status="owned-single-slot-bytes";return result;
    } catch(...) {return std::nullopt;}
}
template<class Save>
bool writeGta5StoppedQueryEvidence(const Gta5StoppedQueryEvidence& q,std::ostream& meta,const Save& save) {
    bool ok=true;
    if(!q.source.empty())ok=save("query-source-object.bin",q.source)&&ok;
    if(!q.query.empty())ok=save("sample-query-object.bin",q.query)&&ok;
    if(!q.samples.empty())ok=save("sample-query-slot.bin",q.samples)&&ok;
    meta<<"{\"version\":1,\"capture_only\":true,\"execution_authority\":false,\"completion_proven\":false"
        <<",\"device\":"<<q.device<<",\"source_object\":"<<q.sourceObject<<",\"sample_branch_r20\":"<<q.sampleBranch
        <<",\"query_object\":"<<q.queryObject<<",\"sample_address\":"<<q.sampleAddress
        <<",\"source_bytes\":"<<q.source.size()<<",\"query_bytes\":"<<q.query.size()<<",\"sample_bytes\":"<<q.samples.size()
        <<",\"status\":\""<<q.status<<"\"}";
    return bool(meta)&&ok;
}
}
