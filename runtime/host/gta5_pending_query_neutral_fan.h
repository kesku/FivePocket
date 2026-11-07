#pragma once
#include "gta5_inline_draw.h"
#include "gta5_stopped_query_evidence.h"

namespace recomp::gpu {
// Current read-only guards around ONE synchronous zero-contribution draw.
// Unlike a CP owner, this neither consumes an event nor publishes counters.
// The caller parks guest writers continuously; retaining this after releasing
// that gate would NOT preserve allocation lifetime or command ownership.
struct Gta5PendingQueryNeutralFan {
    Gta5StoppedQueryEvidence query;
    std::uint32_t firstWord=0;
    std::vector<std::uint8_t> commands;
    static std::optional<Gta5PendingQueryNeutralFan> capture(const Gta5InlineDraw& draw,
            const Gta5StoppedQueryEvidence& q,const Gta5ResourceReader& graphics) {
        if(!graphics || draw.deviceState.size()!=13860 || q.device!=draw.device || q.sampleBranch!=1 ||
           q.source.size()!=168 || q.query.size()!=156 || q.samples.size()!=64 ||
           q.query[16]!=2 || q.query[20]!=0 || !q.sampleAddress || q.sampleAddress%32)
            return std::nullopt;
        const auto word=[](const auto& b,unsigned at) {
            return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
        };
        if(word(q.query,0)!=draw.device || word(q.query,4)!=9 || word(q.query,24) ||
           word(q.query,28)!=q.sampleAddress || word(q.query,148)!=1 || word(q.query,152)!=1 ||
           word(q.source,120)!=q.queryObject)return std::nullopt;
        const auto cursor=word(draw.deviceState,48);
        if(cursor<52 || cursor%4 || cursor>UINT32_MAX-3 || cursor>word(draw.deviceState,56))return std::nullopt;
        const auto first=cursor-48;
        auto bytes=graphics(first,52);if(!bytes || bytes->size()!=52)return std::nullopt;
        // Xenia command_processor.cc uses RB_SAMPLE_COUNT_ADDR for ZPD21,
        // and separate VIZ_QUERY begin/end packets. Preserve both raw events.
        // The encoded physical destination is range-checked, NOT masked into
        // an alias or promoted to a write lease by this read-only observer.
        constexpr unsigned expected[]={0x2208,6,0x12000,0x20000,0,0x2325,0,
            0xC0005B00,21,0x2293,0x15,0xC0002300,10};
        for(unsigned i=0;i<13;++i)if(i!=6 && word(*bytes,4*i)!=expected[i])return std::nullopt;
        const auto address=word(*bytes,24);
        if(address<32 || address%32 || std::uint64_t{address}+32>0x20000000)return std::nullopt;
        return Gta5PendingQueryNeutralFan{q,first,std::move(*bytes)};
    }
    bool unchanged(const Gta5InlineDraw& draw,const Gta5ResourceReader& image,
            const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics) const {
        if(!image || !heap || !graphics)return false;
        const auto equal=[](const auto& reader,unsigned a,const auto& expected) {
            auto b=reader(a,expected.size());return b && *b==expected;
        };
        return equal(heap,draw.device,draw.deviceState) && equal(image,query.sourceObject,query.source) &&
            equal(heap,query.queryObject,query.query) && equal(graphics,query.sampleAddress,query.samples) &&
            equal(graphics,firstWord,commands) && equal(graphics,draw.vertexAddress,draw.vertices);
    }
};
}
