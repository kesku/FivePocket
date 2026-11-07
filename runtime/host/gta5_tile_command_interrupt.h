#pragma once
#include "xenos_scratch_writeback.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace recomp::gpu {
class Gta5TileSceneContinuation;
// Original 82307960 -> 82386EE0 emits this command-only CPU2 interrupt before
// the tiled scene worker runs. It is NOT a swap/scaler request and cannot be
// admitted by relaxing those wrappers' context or completion checks.
// Parsing proves syntax only. Ownership, preceding GPU ordering, real guest
// IRQ dispatch/queue ingress and scratch acknowledgement remain obligations.
class Gta5TileCommandInterrupt final {
public:
    static constexpr unsigned Words=35,Bytes=Words*4,Callback=0x836BE1C8;
    static constexpr unsigned CpuMask=4,CpuIndex=2,RequiredScratchSlots=0x31;
    enum class Kind { ScratchWrite,WaitScratch,Interrupt,CallbackAcknowledge };
    struct Operation {
        Kind kind;std::uint32_t wordOffset,target,value;
        bool operator==(const Operation&) const=default;
    };
    static std::optional<Gta5TileCommandInterrupt> parse(std::uint32_t childPhysical,
            std::span<const std::uint8_t> bytes) noexcept {
        return parseProfile(childPhysical,bytes,false);
    }
    bool predicated() const noexcept {return predicated_;}
private:
    friend class Gta5TileSceneContinuation;
    static std::optional<Gta5TileCommandInterrupt> parseProfile(std::uint32_t childPhysical,
            std::span<const std::uint8_t> bytes,bool predicated) noexcept {
        if(bytes.size()!=Bytes || (childPhysical&3) ||
           std::uint64_t{childPhysical}+Bytes>0x20000000)return std::nullopt;
        const auto word=[&](unsigned i) {
            const auto* p=bytes.data()+i*4;
            return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|
                (std::uint32_t{p[2]}<<8)|p[3];
        };
        const auto context=word(2),rawScratch=word(7);
        if(word(0)!=0x1057C || word(1)!=Callback || (context&3) ||
           context<0xC0000004 || std::uint64_t{context}+256>0xE0000000ULL ||
           (rawScratch&3)!=2)return std::nullopt;
        const auto scratch=rawScratch&~3u;
        const auto contextPhysical=context-0xC0000000;
        const auto overlaps=[](unsigned a,unsigned n,unsigned b,unsigned m) {
            return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
        };
        if(!scratch || std::uint64_t{scratch}+32>0x20000000 ||
           overlaps(scratch,32,childPhysical,Bytes) || overlaps(contextPhysical,4,scratch,32) ||
           overlaps(contextPhysical,4,childPhysical,Bytes))return std::nullopt;
        Gta5TileCommandInterrupt result;result.scratch_=scratch;result.context_=context;result.predicated_=predicated;
        unsigned cursor=3,count=0;
        const auto add=[&](Kind kind,unsigned at,unsigned target,unsigned value) {
            result.operations_[count++]={kind,at,target,value};
        };
        add(Kind::ScratchWrite,0,4,Callback);add(Kind::ScratchWrite,0,5,context);
        const auto pair=[&](Kind kind,unsigned header,unsigned target,unsigned value) {
            if(cursor+2>Words || word(cursor)!=header || word(cursor+1)!=value)return false;
            add(kind,cursor,target,value);cursor+=2;return true;
        };
        const auto wait=[&](unsigned slot,unsigned value) {
            if(cursor+6>Words || word(cursor)!=(0xC0043C00u|unsigned(predicated)) || word(cursor+1)!=0x13 ||
               word(cursor+2)!=scratch+slot*4+2 || word(cursor+3)!=value ||
               word(cursor+4)!=UINT32_MAX || word(cursor+5)!=0x100)return false;
            add(Kind::WaitScratch,cursor,slot,value);cursor+=6;return true;
        };
        if(!pair(Kind::ScratchWrite,0x578,0,CpuMask) || !wait(0,CpuMask) ||
           !wait(5,context) || !wait(4,Callback) || !pair(Kind::Interrupt,0xC0005400u|unsigned(predicated),CpuMask,CpuMask) ||
           !pair(Kind::CallbackAcknowledge,0x45E,0x45E,CpuMask) || !wait(0,0) ||
           !pair(Kind::ScratchWrite,0x57C,4,0x0BADF00D) || cursor!=Words ||
           count!=result.operations_.size())return std::nullopt;
        return result;
    }
public:
    std::uint32_t scratchPhysical() const noexcept {return scratch_;}
    std::uint32_t callbackAddress() const noexcept {return Callback;}
    std::uint32_t callbackContext() const noexcept {return context_;}
    const std::array<Operation,10>& operations() const noexcept {return operations_;}
    bool scratchStateMatches(std::uint32_t base,std::uint32_t mask) const noexcept {
        return base==scratch_ && scratchWritebackBigEndianProfile(mask) &&
            (mask&RequiredScratchSlots)==RequiredScratchSlots;
    }
    bool operator==(const Gta5TileCommandInterrupt&) const=default;
private:
    std::uint32_t scratch_=0,context_=0;
    bool predicated_=false;
    std::array<Operation,10> operations_{};
};

// Orders obligations; supplies no GPU operation or guest acknowledgement.
// The IRQ caller must execute and validate the actual guest interrupt before
// reporting guestReturned. Completion here means this small wrapper only;
// it does not mean its queued scene worker, render targets, or fences retired.
class Gta5TileCommandInterruptSequence final {
public:
    using Plan=Gta5TileCommandInterrupt;
    using Action=std::function<bool(const Plan::Operation&)>;
    enum class Phase { Ready,AwaitingGuest,GuestRunning,GuestReturned,Complete,Failed };
    explicit Gta5TileCommandInterruptSequence(const Plan& plan):plan_(plan) {}
    bool prefix(unsigned base,unsigned mask,const Action& action) {
        if(phase_!=Phase::Ready || plan_.predicated() || !action || !plan_.scratchStateMatches(base,mask))return false;
        if(!apply(0,6,action))return false;
        phase_=Phase::AwaitingGuest;return true;
    }
    bool enterGuest() noexcept {
        if(phase_!=Phase::AwaitingGuest)return false;
        phase_=Phase::GuestRunning;return true;
    }
    bool guestReturned() noexcept {
        if(phase_!=Phase::GuestRunning)return false;
        phase_=Phase::GuestReturned;next_=7;return true;
    }
    bool acknowledge(const Action& action) {
        if(phase_!=Phase::GuestReturned || !action)return false;
        if(!apply(7,10,action))return false;
        phase_=Phase::Complete;return true;
    }
    void fail() noexcept {phase_=Phase::Failed;}
    Phase phase() const noexcept {return phase_;}
    unsigned nextOperation() const noexcept {return next_;}
private:
    bool apply(unsigned first,unsigned end,const Action& action) {
        phase_=Phase::Failed;
        for(unsigned i=first;i<end;++i) {
            next_=i;if(!action(plan_.operations()[i]))return false;next_=i+1;
        }
        return true;
    }
    Plan plan_;Phase phase_=Phase::Ready;unsigned next_=0;
};
}
