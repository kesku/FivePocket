#pragma once
#include "gta5_clear_capture.h"
#include <cstdint>
#include <span>

namespace recomp::gpu {
// An asynchronous MMIO write accepts a queue entry, NOT its completion. This
// bounded original-EndTiling capture retains the actual queued ring/IB bytes
// while the caller holds one exclusive CPU-recording scope. It NEVER writes
// guest memory, RPTR, events or fences. Sealed copies are not allocation leases.
class Gta5NativeTileSubmissionCapture final {
public:
    enum class Phase {Recording,Sealed,Failed};
    struct Indirect {unsigned physical=0;std::vector<std::uint8_t> bytes;};
    struct Submission {
        unsigned before=0,after=0;
        std::vector<std::uint32_t> ringWords;
        std::vector<Indirect> indirects;
    };
    static std::optional<Gta5NativeTileSubmissionCapture> begin(unsigned device,
            const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics) {
        if(!heap || !graphics || !device || device%4)return std::nullopt;
        const auto state=heap(device,15308);
        if(!state || state->size()!=15308)return std::nullopt;
        Gta5NativeTileSubmissionCapture c;c.device_=device;
        c.ring_=word(*state,15300);c.mask_=word(*state,15304);
        const auto fence=word(*state,11024);c.last_=word(*state,11088);
        if(!c.mask_ || c.mask_>0x3FFF || (c.mask_&(c.mask_+1)) || c.last_>c.mask_ ||
           c.ring_<0xA0000000 || c.ring_%4 || std::uint64_t{c.ring_}+(c.mask_+1)*4>0xC0000000 ||
           fence<0xA0000000 || fence%4 || std::uint64_t{fence}+64>0xC0000000)return std::nullopt;
        c.readAddress_=fence+60;
        const auto read=graphics(c.readAddress_,4),ring=graphics(c.ring_,(c.mask_+1)*4);
        if(!read || read->size()!=4 || !ring || ring->size()!=(c.mask_+1)*4)return std::nullopt;
        c.entryRead_=word(*read,0);
        // Existing in-flight work needs its own consumer first. The observed
        // EndTiling entry is empty (read=write=73), not state-batch receipt34.
        if(c.entryRead_!=c.last_)return std::nullopt;
        if(c.readAddress_<std::uint64_t{c.ring_}+(c.mask_+1)*4 && c.ring_<std::uint64_t{c.readAddress_}+4)return std::nullopt;
        return c;
    }
    bool capture(unsigned writeWord,const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics) {
        if(phase_!=Phase::Recording)return false;
        phase_=Phase::Failed;
        if(submissions_.size()>=8 || writeWord>mask_ || !unchangedIdentity(writeWord,heap,graphics))return false;
        const auto count=(writeWord-last_)&mask_,occupied=(last_-entryRead_)&mask_;
        if(!count || count>64 || count>mask_-occupied)return false;
        const auto ring=graphics(ring_,(mask_+1)*4);if(!ring || ring->size()!=(mask_+1)*4)return false;
        Submission s;s.before=last_;s.after=writeWord;
        for(unsigned i=0;i<count;++i)s.ringWords.push_back(word(*ring,((last_+i)&mask_)*4));
        for(unsigned i=0;i<count;) {
            const auto h=s.ringWords[i++];
            if(h==0xC0006200) {if(i==count)return false;++i;continue;}
            if(h!=0xC0013F00 || count-i<2)return false;
            const auto physical=s.ringWords[i++],words=s.ringWords[i++];
            if(physical%4 || !words || words>16384 || std::uint64_t{physical}+words*4>0x20000000)return false;
            // No command storage may alias this ring or its completion word.
            const auto address=std::uint64_t{physical}+0xA0000000;
            if((address<std::uint64_t{ring_}+(mask_+1)*4 && ring_<address+words*4) ||
               (address<std::uint64_t{readAddress_}+4 && readAddress_<address+words*4))return false;
            const auto bytes=graphics(physical,words*4);
            if(!bytes || bytes->size()!=words*4)return false;
            s.indirects.push_back({physical,*bytes});
        }
        if(s.indirects.empty())return false;
        submissions_.push_back(std::move(s));last_=writeWord;phase_=Phase::Recording;return true;
    }
    bool seal(const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics) {
        if(phase_!=Phase::Recording)return false;
        phase_=Phase::Failed;
        if(!retainedBytesMatch(heap,graphics))return false;
        phase_=Phase::Sealed;return true;
    }
    bool matchesCurrent(const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics)const {
        return phase_==Phase::Sealed && retainedBytesMatch(heap,graphics);
    }
    Phase phase()const noexcept{return phase_;}
    unsigned device()const noexcept{return device_;}
    unsigned ringVirtual()const noexcept{return ring_;}
    unsigned ringBytes()const noexcept{return (mask_+1)*4;}
    unsigned readVirtual()const noexcept{return readAddress_;}
    unsigned entryReadWord()const noexcept{return entryRead_;}
    unsigned lastWriteWord()const noexcept{return last_;}
    const auto& submissions()const noexcept{return submissions_;}
private:
    bool retainedBytesMatch(const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics)const {
        if(submissions_.empty() || !unchangedIdentity(last_,heap,graphics))return false;
        const auto ring=graphics(ring_,(mask_+1)*4);if(!ring || ring->size()!=(mask_+1)*4)return false;
        for(const auto& s:submissions_) {
            for(unsigned i=0;i<s.ringWords.size();++i)if(word(*ring,((s.before+i)&mask_)*4)!=s.ringWords[i])return false;
            for(const auto& ib:s.indirects) {
                const auto bytes=graphics(ib.physical,ib.bytes.size());if(!bytes || *bytes!=ib.bytes)return false;
            }
        }
        return true;
    }
    static unsigned word(std::span<const std::uint8_t> b,unsigned at) {
        return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
    }
    bool unchangedIdentity(unsigned write,const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics)const {
        if(!heap || !graphics)return false;
        const auto state=heap(device_,15308),read=graphics(readAddress_,4);
        return state && state->size()==15308 && read && read->size()==4 &&
            word(*state,15300)==ring_ && word(*state,15304)==mask_ &&
            std::uint64_t{word(*state,11024)}+60==readAddress_ && word(*state,11088)==write && word(*read,0)==entryRead_;
    }
    Gta5NativeTileSubmissionCapture()=default;
    Phase phase_=Phase::Recording;
    unsigned device_=0,ring_=0,mask_=0,readAddress_=0,entryRead_=0,last_=0;
    std::vector<Submission> submissions_;
};
}
