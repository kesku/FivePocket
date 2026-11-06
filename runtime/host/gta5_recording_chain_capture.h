#pragma once
#include "gta5_tile_recording_diagnostic.h"
#include <set>

namespace recomp::gpu {
// Read-only snapshot of ORIGINAL82431888's linked recording blocks BEFORE
// 8248B680 seals the current block. Readers must independently prove ownership.
// originalLink is used for comparisons, never to grant address ownership.
// The reader must resolve stored C-view links through a currently owned
// physical allocation and verify its originalLink identity; no masking fallback.
// The unfinished tail excludes undefined slack and the not-yet-written terminator.
struct Gta5RecordingChainCapture {
    enum class Status { Empty, Captured, Unavailable, Malformed, Limit };
    struct Block {std::uint32_t address=0,usedBytes=0;bool tail=false;std::vector<std::uint8_t> bytes;};
    std::array<std::uint32_t,6> descriptor{};
    Status status=Status::Unavailable;
    std::vector<Block> blocks;
};
inline Gta5RecordingChainCapture captureGta5RecordingChain(std::uint32_t device,
        const std::array<std::uint32_t,6>& d,const Gta5ResourceReader& owned) {
    Gta5RecordingChainCapture c;c.descriptor=d;
    if(std::all_of(d.begin(),d.end(),[](auto w){return w==0;})) {
        c.status=Gta5RecordingChainCapture::Status::Empty;return c;
    }
    const auto fail=[&](auto status){c.status=status;return c;};
    const auto link=[](auto a){return Gta5TileRecordingDiagnostic::originalLink(a);};
    const auto word=[](const auto& b,unsigned at) {
        return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
    };
    const auto head=d[0],tail=d[1],capacity=d[2],cursor=d[3],limit=d[4];
    if(!device || !head || !tail || head%128 || tail%128 || d[5]!=device ||
       capacity<128 || capacity>65536 || capacity%128 ||
       std::uint64_t{tail}+capacity>0x100000000ULL ||
       cursor<tail+4 || cursor>std::uint64_t{tail}+capacity-4 || cursor%4 ||
       limit!=tail+capacity-4)
        return fail(Gta5RecordingChainCapture::Status::Malformed);
    if(!owned)return c;
    std::set<unsigned> seen;unsigned address=head;std::size_t bytesCaptured=0;
    for(unsigned count=0;count<64;++count) {
        if(address%128 || std::uint64_t{address}+capacity>0x100000000ULL ||
           !seen.insert(link(address)).second)
            return fail(Gta5RecordingChainCapture::Status::Malformed);
        const bool last=link(address)==link(tail);
        const auto header=owned(address,4);
        if(!header || header->size()!=4)return fail(Gta5RecordingChainCapture::Status::Unavailable);
        const auto usedLink=word(*header,0),baseLink=link(address);
        if(usedLink<baseLink+4 || usedLink%4 ||
           std::uint64_t{usedLink}>std::uint64_t{baseLink}+capacity-4)
            return fail(Gta5RecordingChainCapture::Status::Malformed);
        const auto used=last?cursor-tail:usedLink-baseLink;
        if(last && usedLink>baseLink+used)return fail(Gta5RecordingChainCapture::Status::Malformed);
        const auto bytes=used+(last?0u:4u);
        if(bytesCaptured+bytes>2*1024*1024)return fail(Gta5RecordingChainCapture::Status::Limit);
        auto block=owned(address,bytes);
        if(!block || block->size()!=bytes)return fail(Gta5RecordingChainCapture::Status::Unavailable);
        if(word(*block,0)!=usedLink)return fail(Gta5RecordingChainCapture::Status::Malformed);
        bytesCaptured+=bytes;
        c.blocks.push_back({address,used,last,std::move(*block)});
        if(last){c.status=Gta5RecordingChainCapture::Status::Captured;return c;}
        const auto next=word(c.blocks.back().bytes,used);
        // Actual stored links are C..F addresses; do not mask or reinterpret
        // ordinary opcodes, physical pointers or the sealed-list terminator.
        if(next<=0xC0000000 || next>0xFFFFFF80 || next%128)
            return fail(Gta5RecordingChainCapture::Status::Malformed);
        address=next;
    }
    return fail(Gta5RecordingChainCapture::Status::Limit);
}
}
