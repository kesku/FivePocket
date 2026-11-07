#pragma once
#include "gta5_tile_replay.h"

namespace recomp::gpu {
// Read-only scheduling model of original836BE870/836BDE28, for the validated
// Begin/End + paired-phase + indirect-descriptor grammar. It does not parse
// the descriptors' GPU payloads, execute them, mutate worker memory, service
// queries, publish pixels, or retire a callback. Those are separate duties.
struct Gta5TileReplaySchedule {
    enum class Kind { Mask, Indirect };
    struct Event {
        Kind kind;
        std::uint32_t mask=0,bin=0,descriptorWord=0,descriptorAddress=0;
    };
    unsigned beginFlags=0,binCount=0;
    std::array<std::array<unsigned,4>,15> rectangles{};
    std::vector<Event> events;
    std::uint32_t terminalAddress=0;
};
inline std::optional<Gta5TileReplaySchedule> planGta5TileReplaySchedule(
        std::uint32_t start,const Gta5ResourceReader& owned) {
    if(!owned || !start || start%4 || start>UINT32_MAX-255)return std::nullopt;
    const auto header=owned(start,252);
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    if(!header || header->size()!=252 || word(*header,0)!=0x80000000)return std::nullopt;
    Gta5TileReplaySchedule result;
    result.beginFlags=word(*header,4);result.binCount=word(*header,8);
    // Header is 248 bytes after the control word: flags/count + 15 rectangles.
    // Other Begin modes need their original outer-worker side effects audited.
    if(result.beginFlags>3 || !result.binCount || result.binCount>15)return std::nullopt;
    for(unsigned bin=0;bin<result.binCount;++bin)for(unsigned c=0;c<4;++c)
        result.rectangles[bin][c]=word(*header,12+bin*16+c*4);
    std::uint32_t pc=start+252,flags=0,bin=0,mask=0x7FFFFFFF;
    std::optional<std::uint32_t> repeat,phaseStart,phaseEnd;
    bool repeatActive=false,phaseActive=false,repeatFinished=false;
    const auto read=[&](std::uint32_t address,unsigned count)->std::optional<std::vector<std::uint8_t>> {
        if(address%4 || !count || std::uint64_t{address}+count>0x100000000ULL)return std::nullopt;
        auto bytes=owned(address,count);
        return bytes && bytes->size()==count?std::move(bytes):std::nullopt;
    };
    const auto emitMask=[&] {
        const auto next=gta5DriverTileReplayMask(flags,bin);
        if(!next)return false;
        if(*next!=mask) {
            mask=*next;result.events.push_back({Gta5TileReplaySchedule::Kind::Mask,mask,bin,0,0});
        }
        return true;
    };
    // Bounds are host planning limits, not guest completion/fence semantics.
    // Failure discards the entire plan before a consumer may submit anything.
    for(unsigned steps=0;steps<65536 && result.events.size()<8192;++steps) {
        const auto bytes=read(pc,4);if(!bytes)return std::nullopt;
        const auto control=word(*bytes,0),opcode=control>>24;
        if(control==0xC0000000) {
            if(!repeatFinished || repeatActive || phaseActive || flags || bin!=result.binCount)return std::nullopt;
            result.terminalAddress=pc;return result;
        }
        if(opcode==0x81 || opcode==0x8F) {
            if(!repeatActive || pc>UINT32_MAX-8)return std::nullopt;
            const auto descriptor=read(pc,8);if(!descriptor)return std::nullopt;
            result.events.push_back({Gta5TileReplaySchedule::Kind::Indirect,mask,bin,control,word(*descriptor,4)});
            pc+=8;continue;
        }
        if(opcode>=0x83 && opcode<=0x87) {
            if((control&0xFFFFFF) || pc>UINT32_MAX-4)return std::nullopt;
            const auto current=pc;pc+=4;
            switch(opcode) {
            case 0x86:
                if(phaseActive || repeatFinished || (repeat && *repeat!=current))return std::nullopt;
                repeat=current;repeatActive=true;flags|=0x20000000;
                if(!emitMask())return std::nullopt;
                break;
            case 0x87:
                if(!repeatActive || phaseActive || flags!=0x20000000)return std::nullopt;
                ++bin;flags&=~0x20000000u;repeatActive=false;
                if(bin<result.binCount)pc=*repeat;
                else {repeatFinished=true;if(!emitMask())return std::nullopt;}
                break;
            case 0x83:
                if(!repeatActive || (phaseActive && phaseStart!=current))return std::nullopt;
                phaseStart=current;phaseActive=true;flags|=0x40000000;
                if(bin && (result.beginFlags&2))flags|=0x80000000;
                if(!emitMask())return std::nullopt;
                break;
            case 0x84:
                if(!phaseActive || !(flags&0x40000000))return std::nullopt;
                flags&=~0x40000000u;
                if(flags&0x80000000) {
                    if(!phaseEnd)return std::nullopt;
                    pc=*phaseEnd;
                } else if(!emitMask())return std::nullopt;
                break;
            case 0x85:
                if(!phaseActive || (flags&0x40000000))return std::nullopt;
                phaseEnd=current;flags^=0x80000000;
                if(flags&0x80000000)pc=*phaseStart;
                else {phaseActive=false;if(!emitMask())return std::nullopt;}
                break;
            }
            continue;
        }
        // Original default follows a stored link then skips its link word.
        // Require a representable, aligned link in the recorded C..F view;
        // the reader must separately prove ownership (no address masking).
        if(control>0xC0000000 && control<=0xFFFFFFF8 && !(control&3)) {
            pc=control+4;continue;
        }
        // 82 rechecks itself, 88/89/8A/8E/90 need other worker/query behavior,
        // and 8C/8D return to an outer interpreter. None is a completed list.
        return std::nullopt;
    }
    return std::nullopt;
}
}
