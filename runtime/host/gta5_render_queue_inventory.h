#pragma once
#include "gta5_inline_draw.h"
#include <limits>
#include <ostream>
#include <string>

namespace recomp::gpu {
// Diagnostic, not a command processor. Original 823B5BC8 advances an encoded
// handle BEFORE calling a handler, then reloads a mutable global afterward.
// Thus these are potential linear fallthrough records observed at capture time,
// not proof of future execution, immutable entry contents, or runtime support.
struct Gta5QueueRecord {
    std::uint32_t handle=0,bankPointer=0,bankBase=0,address=0,word=0,advance=0;
    unsigned handlerSlot=0;
    std::optional<std::uint32_t> handler;
    std::optional<std::vector<std::uint8_t>> payload;
};
struct Gta5QueueInventory {
    std::uint32_t root=0,entryHandle=0;
    std::vector<Gta5QueueRecord> records;
    const char* stop="record-limit";
    unsigned unavailablePayloads=0,unavailableHandlers=0,payloadBytes=0;
};
inline Gta5QueueInventory captureGta5RenderQueue(std::uint32_t root,std::uint32_t entry,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics) {
    constexpr unsigned MaxRecords=128,MaxPayloadBytes=256*1024;
    Gta5QueueInventory out;out.root=root;out.entryHandle=entry;
    const auto read=[&](std::uint64_t address,unsigned size)
            ->std::optional<std::vector<std::uint8_t>> {
        if(!address || address%4 || !size || address>UINT32_MAX ||
           size-1>UINT32_MAX-address)return std::nullopt;
        const auto& reader=address<0x20000000?heap:graphics;
        if(!reader)return std::nullopt;
        auto bytes=reader(static_cast<std::uint32_t>(address),size);
        return bytes && bytes->size()==size?bytes:std::nullopt;
    };
    const auto word=[](const auto& b) {
        return (std::uint32_t{b[0]}<<24)|(std::uint32_t{b[1]}<<16)|
            (std::uint32_t{b[2]}<<8)|b[3];
    };
    if(!root || root%4){out.stop="invalid-root";return out;}
    auto handle=entry;
    for(unsigned i=0;i<MaxRecords;++i) {
        const auto bank=handle>>24;
        // Original sign-extends this byte. Negative-bank layout is not proved;
        // explicitly leave it uninventoried rather than treating it as unsigned.
        if(bank>=128){out.stop="negative-bank-uninventoried";break;}
        const auto bankAt=std::uint64_t{root}+1032+20*bank;
        const auto bankBytes=read(bankAt,4);
        if(!bankBytes){out.stop="bank-pointer-unavailable";break;}
        const auto bankBase=word(*bankBytes);
        if(!bankBase){out.stop="null-bank";break;}
        const auto recordAt=std::uint64_t{bankBase}+(handle&0xFFFFFF);
        const auto header=read(recordAt,4);
        if(!header){out.stop="record-header-unavailable";break;}
        Gta5QueueRecord record;
        record.handle=handle;record.bankPointer=static_cast<std::uint32_t>(bankAt);
        record.bankBase=bankBase;record.address=static_cast<std::uint32_t>(recordAt);
        record.word=word(*header);record.advance=((record.word>>20)&0xFFF)*4;
        record.handlerSlot=(record.word>>12)&0xFF;
        const auto handler=read(std::uint64_t{root}+8+4*record.handlerSlot,4);
        if(handler)record.handler=word(*handler);else ++out.unavailableHandlers;
        // Zero advance is possible with a terminator or a handler that changes
        // the head. A missing body never erases independently available metadata.
        const auto bytes=std::max(4u,record.advance);
        const bool fits=bytes<=MaxPayloadBytes-out.payloadBytes;
        if(fits)record.payload=bytes==4?header:read(recordAt,bytes);
        if(record.payload)out.payloadBytes+=record.payload->size();else ++out.unavailablePayloads;
        out.records.push_back(std::move(record));
        if(!fits){out.stop="payload-budget";break;}
        if(out.records.back().handlerSlot==2){out.stop="linear-terminator";break;}
        if(!out.records.back().advance){out.stop="zero-advance-unknown-control-flow";break;}
        const auto next=std::uint64_t{handle}+out.records.back().advance;
        if(next>UINT32_MAX){out.stop="handle-wrap-uninventoried";break;}
        handle=static_cast<std::uint32_t>(next);
    }
    return out;
}

// Persist the bounded inventory even when the original exception came from a
// submission/drain rather than a draw, resolve or clear. This metadata never
// claims a shader capture or that these potential records actually executed.
template<class SavePayload>
bool writeGta5RenderQueueInventory(const Gta5QueueInventory& q,std::ostream& meta,
        const SavePayload& save) {
    bool ok=true;
    meta<<"{\"version\":1,\"root\":"<<q.root<<",\"entry_handle\":"<<q.entryHandle
        <<",\"snapshot\":\"renderer-exception-unwind\",\"execution_proven\":false"
        <<",\"interpretation\":\"potential-linear-fallthrough-at-capture-time\",\"stop\":\""<<q.stop
        <<"\",\"unavailable_payloads\":"<<q.unavailablePayloads<<",\"unavailable_handlers\":"<<q.unavailableHandlers
        <<",\"payload_bytes\":"<<q.payloadBytes<<",\"records\":[";
    for(unsigned i=0;i<q.records.size();++i) {
        const auto& r=q.records[i];if(i)meta<<',';
        meta<<"{\"handle\":"<<r.handle<<",\"bank_pointer\":"<<r.bankPointer<<",\"bank_base\":"<<r.bankBase
            <<",\"address\":"<<r.address<<",\"word\":"<<r.word<<",\"advance\":"<<r.advance
            <<",\"handler_slot\":"<<r.handlerSlot<<",\"handler\":";
        if(r.handler)meta<<*r.handler;else meta<<"null";
        meta<<",\"payload_bytes\":"<<(r.payload?r.payload->size():0)<<'}';
        if(r.payload)ok=save("queue-record-"+std::to_string(i)+".bin",*r.payload) && ok;
    }
    meta<<"]}";return bool(meta) && ok;
}
}
