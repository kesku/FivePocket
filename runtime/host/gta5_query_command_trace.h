#pragma once
#include "gta5_stopped_query_evidence.h"
#include <algorithm>

namespace recomp::gpu {
// READ-ONLY diagnostic spanning original BEGIN and a later refused draw.
// This is NOT a lease, generation proof, parser, execution or retirement receipt.
// Keep unavailable/changed evidence explicit; never recover ownership by masking.
struct Gta5QueryCommandTrace {
    std::uint32_t source=0,query=0,device=0,link=0,firstWord=0,lastWord=0;
    std::vector<std::uint8_t> deviceBefore,queryBefore,deviceAfter,queryAfter;
    std::vector<std::uint8_t> beginCommands,pendingCommands;
    std::vector<std::uint8_t> deviceAtEnd,queryAtEnd,commandsThroughEnd;
    const char* endStatus="original-end-not-observed";
    const char* status="begin-return-unavailable";
    static std::uint32_t word(const std::vector<std::uint8_t>& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    }
    static std::optional<std::vector<std::uint8_t>> exact(const Gta5ResourceReader& read,
            unsigned address,unsigned bytes) {
        if(!read || !address || address%4 || address>UINT32_MAX-(bytes-1))return std::nullopt;
        auto result=read(address,bytes);if(result && result->size()!=bytes)result.reset();return result;
    }
    static std::optional<Gta5QueryCommandTrace> begin(unsigned source,unsigned query,unsigned link,
            const Gta5ResourceReader& image,const Gta5ResourceReader& heap) noexcept {
        try {
            if(source!=0x83817028 || (link!=0x82364A34 && link!=0x82364A74))return std::nullopt;
            const auto src=exact(image,source,168),q=exact(heap,query,156);
            if(!src || !q || word(*src,120)!=query || word(*q,4)!=9)return std::nullopt;
            const auto device=word(*q,0);const auto d=exact(heap,device,13860);
            if(!d)return std::nullopt;
            const auto cursor=word(*d,48);
            if(!cursor || cursor%4 || cursor>UINT32_MAX-36 || cursor>word(*d,56))return std::nullopt;
            Gta5QueryCommandTrace result;result.source=source;result.query=query;
            result.device=device;result.link=link;result.firstWord=cursor+4;
            result.deviceBefore=*d;result.queryBefore=*q;return result;
        } catch(...) {return std::nullopt;}
    }
    void returned(const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics) noexcept {
        try {
            auto d=exact(heap,device,13860),q=exact(heap,query,156);
            if(!d || !q)return;
            deviceAfter=std::move(*d);queryAfter=std::move(*q);
            if(word(deviceAfter,48)!=firstWord+32 || queryAfter[16]!=2 ||
               word(queryAfter,0)!=device || word(queryAfter,4)!=9) {
                status="begin-return-shape-changed";return;
            }
            auto commands=exact(graphics,firstWord,36);
            if(!commands){status="begin-command-storage-unavailable";return;}
            beginCommands=std::move(*commands);status="begin-bytes-captured";
        } catch(...) {status="begin-capture-error";}
    }
    void stopped(const Gta5StoppedQueryEvidence& q,const std::vector<std::uint8_t>& state,
            const Gta5ResourceReader& graphics) noexcept {
        try {
            pendingCommands.clear();lastWord=0;
            if(q.device!=device || q.sourceObject!=source || q.queryObject!=query ||
               q.sampleBranch!=1 || q.query!=queryAfter || state.size()!=13860 || beginCommands.size()!=36) {
                status="stopped-query-owner-or-state-changed";return;
            }
            lastWord=word(state,48);
            const auto length=std::uint64_t{lastWord}+4-firstWord;
            if(lastWord<firstWord+32 || lastWord%4 || length>65536) {
                status="pending-command-range-unavailable";return;
            }
            auto bytes=exact(graphics,firstWord,static_cast<unsigned>(length));
            if(!bytes){status="pending-command-storage-unavailable";return;}
            if(!std::equal(beginCommands.begin(),beginCommands.end(),bytes->begin())) {
                status="begin-command-prefix-changed";return;
            }
            pendingCommands=std::move(*bytes);status="owned-pending-prefix-captured";
        } catch(...) {status="pending-capture-error";}
    }
    void ended(const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics) noexcept {
        try {
            deviceAtEnd.clear();queryAtEnd.clear();commandsThroughEnd.clear();
            auto d=exact(heap,device,13860),q=exact(heap,query,156);
            if(!d || !q){endStatus="original-end-storage-unavailable";return;}
            if(word(*q,0)!=device || word(*q,4)!=9 || (*q)[16]!=1 || !((*q)[20]&0x40)) {
                endStatus="original-end-state-unmatched";return;
            }
            const auto cursor=word(*d,48);const auto length=std::uint64_t{cursor}+4-firstWord;
            if(beginCommands.size()!=36 || cursor%4 || cursor<firstWord+32 || length>65536) {
                endStatus="original-end-command-range-unavailable";return;
            }
            auto commands=exact(graphics,firstWord,static_cast<unsigned>(length));
            if(!commands || !std::equal(beginCommands.begin(),beginCommands.end(),commands->begin())) {
                endStatus="original-end-prefix-unavailable-or-changed";return;
            }
            deviceAtEnd=std::move(*d);queryAtEnd=std::move(*q);commandsThroughEnd=std::move(*commands);
            endStatus="original-end-bytes-captured-not-retired";
        } catch(...) {endStatus="original-end-capture-error";}
    }
};
// A diagnostic COPY, not a live memory owner. The retained copy can outlive
// active tracking/unbind and must be reset explicitly for the next run.
inline bool retainGta5QueryEndDiagnostic(const Gta5QueryCommandTrace& trace,
        std::optional<Gta5QueryCommandTrace>& retained) noexcept {
    if(trace.commandsThroughEnd.empty())return false;
    try {retained=trace;return true;}catch(...) {retained.reset();return false;}
}
template<class Save>
bool writeGta5QueryCommandTrace(const Gta5QueryCommandTrace& t,std::ostream& meta,const Save& save) {
    bool ok=true;
    const auto put=[&](const char* n,const auto& b){if(!b.empty())ok=save(n,b)&&ok;};
    put("query-command-device-before.bin",t.deviceBefore);put("query-command-object-before.bin",t.queryBefore);
    put("query-command-device-after.bin",t.deviceAfter);put("query-command-object-after.bin",t.queryAfter);
    put("query-command-begin.bin",t.beginCommands);put("query-command-pending.bin",t.pendingCommands);
    put("query-command-device-at-end.bin",t.deviceAtEnd);put("query-command-object-at-end.bin",t.queryAtEnd);
    put("query-command-through-end.bin",t.commandsThroughEnd);
    meta<<"{\"version\":1,\"capture_only\":true,\"execution_authority\":false,\"retirement_proven\":false"
        <<",\"allocation_generation_proven\":false,\"device\":"<<t.device<<",\"source\":"<<t.source
        <<",\"query\":"<<t.query<<",\"begin_link\":"<<t.link<<",\"first_word\":"<<t.firstWord
        <<",\"last_word_inclusive\":"<<t.lastWord<<",\"pending_bytes\":"<<t.pendingCommands.size()
        <<",\"status\":\""<<t.status<<"\",\"end_status\":\""<<t.endStatus<<"\",\"through_end_bytes\":"<<t.commandsThroughEnd.size()<<'}';
    return ok && bool(meta);
}
}
