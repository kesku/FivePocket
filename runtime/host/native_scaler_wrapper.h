#pragma once
#include "native_scaler_command.h"
#include "xenos_scratch_writeback.h"

namespace recomp::gpu {
// An owned request plan, NOT an executable command or a completion receipt.
// The observed wrapper requires a real guest command-interrupt callback before
// its scaler update. Matching memory waits are obligations, never permission
// to manufacture their expected values. Retail scanout timing is not modeled.
class NativeScalerWrapper final {
public:
    static constexpr std::uint32_t Words=145,Bytes=Words*4,CommandOffsetWords=69;
    static constexpr std::uint32_t RequiredScratchSlots=0x33;
    enum class Kind { BinMaskLow,ScratchWrite,WaitScratch,WaitIdle,Interrupt,
        CallbackAcknowledge,ScalerUpdateLock,ScalerEnable,WaitScalerUpdate,
        FlipControl,MasterUpdateLock,IdentityScaler,InvalidateState,DiagnosticNop,LiteralEvent };
    struct Operation {
        Kind kind;
        std::uint32_t wordOffset,target,value;
        // target: scratch slot, register, CPU mask or physical event address,
        // according to kind. All waits use equality. Scratch/event byte order2.
    };
    static std::optional<NativeScalerWrapper> parse(const NativeScalerCommand& command,
            std::uint32_t childPhysical,std::uint32_t commandPhysical,
            const std::uint8_t* bytes,std::size_t count) noexcept {
        if (!bytes || count!=Bytes || (childPhysical&3) ||
            std::uint64_t{childPhysical}+Bytes>0x20000000 ||
            std::uint64_t{childPhysical}+CommandOffsetWords*4!=commandPhysical ||
            reinterpret_cast<std::uintptr_t>(bytes)>std::numeric_limits<std::uintptr_t>::max()-Bytes ||
            !command.matches(bytes+CommandOffsetWords*4,NativeScalerCommand::Bytes)) return std::nullopt;
        const auto word=[&](unsigned i) {
            const auto* p=bytes+i*4;
            return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
        };
        const auto overlaps=[](std::uint32_t a,std::uint32_t n,std::uint32_t b,std::uint32_t m) {
            return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
        };
        // Scratch base is derived from the first wait and cross-checked against
        // all five subsequent waits. A live consumer must independently match
        // retained SCRATCH_ADDR/UMSK and pin the full allocation before writing.
        const auto firstWait=word(6),callback=word(13),context=word(14);
        if ((firstWait&3)!=2 || (firstWait&~3u)<4 || !callback || (callback&3) ||
            callback==0x0BADF00D) return std::nullopt;
        const auto scratch=(firstWait&~3u)-4;
        if (std::uint64_t{scratch}+32>0x20000000 || overlaps(scratch,32,childPhysical,Bytes)) return std::nullopt;
        NativeScalerWrapper result(command,scratch,callback,context);
        unsigned operations=0,cursor=0;
        const auto add=[&](Kind kind,unsigned at,unsigned target,unsigned value) {
            result.operations_[operations++]={kind,at,target,value};
        };
        const auto pair=[&](Kind kind,unsigned header,unsigned value,unsigned target) {
            if (word(cursor)!=header || word(cursor+1)!=value) return false;
            add(kind,cursor,target,value);cursor+=2;return true;
        };
        const auto wait=[&](unsigned slot,unsigned value) {
            if (word(cursor)!=0xC0043C00 || word(cursor+1)!=0x13 ||
                word(cursor+2)!=(scratch+slot*4+2) || word(cursor+3)!=value ||
                word(cursor+4)!=0xFFFFFFFF || word(cursor+5)!=0x100) return false;
            add(Kind::WaitScratch,cursor,slot,value);cursor+=6;return true;
        };
        if (!pair(Kind::BinMaskLow,0xC0006000,0xFFFFFFFF,0) ||
            !pair(Kind::ScratchWrite,0x579,1,1) || !wait(1,1) ||
            !pair(Kind::WaitIdle,0x5C8,0x20000,0x5C8) || word(cursor)!=0x1057C) return std::nullopt;
        add(Kind::ScratchWrite,cursor,4,callback);add(Kind::ScratchWrite,cursor,5,context);cursor+=3;
        if (!pair(Kind::ScratchWrite,0x578,4,0) || !wait(0,4) || !wait(5,context) || !wait(4,callback) ||
            !pair(Kind::Interrupt,0xC0005400,4,4) ||
            !pair(Kind::CallbackAcknowledge,0x45E,4,0x45E) || !wait(0,0) ||
            !pair(Kind::ScratchWrite,0x57C,0x0BADF00D,4) || !wait(1,0) ||
            !pair(Kind::ScalerUpdateLock,0x1973,0x10000,0x1973) ||
            !pair(Kind::ScalerEnable,0x1964,1,0x1964) ||
            !pair(Kind::ScalerUpdateLock,0x1973,0,0x1973)) return std::nullopt;
        if (word(cursor)!=0xC0043C00 || word(cursor+1)!=3 || word(cursor+2)!=0x1973 ||
            word(cursor+3)!=0 || word(cursor+4)!=1 || word(cursor+5)!=0x100) return std::nullopt;
        add(Kind::WaitScalerUpdate,cursor,0x1973,0);cursor+=6;
        if (!pair(Kind::FlipControl,0x1852,0,0x1852) ||
            !pair(Kind::MasterUpdateLock,0x1838,1,0x1838) || cursor!=CommandOffsetWords) return std::nullopt;
        add(Kind::IdentityScaler,cursor,commandPhysical,NativeScalerCommand::Words);cursor+=NativeScalerCommand::Words;
        if (!pair(Kind::InvalidateState,0xC0003B00,0x7FFF,0) ||
            !pair(Kind::DiagnosticNop,0xC0001000,0x77777777,0)) return std::nullopt;
        std::uint32_t previousEvent=0;
        for (unsigned i=0;i<2;++i) {
            const auto raw=word(cursor+2),address=raw&~3u;
            if (word(cursor)!=0xC0025800 || word(cursor+1)!=3 || (raw&3)!=2 ||
                std::uint64_t{address}+4>0x20000000 || overlaps(address,4,childPhysical,Bytes) ||
                overlaps(address,4,scratch,32) || (i && address==previousEvent)) return std::nullopt;
            add(Kind::LiteralEvent,cursor,address,word(cursor+3));cursor+=4;previousEvent=address;
        }
        if (cursor!=Words || operations!=result.operations_.size()) return std::nullopt;
        return result;
    }
    const std::array<Operation,26>& operations() const noexcept { return operations_; }
    const NativeScalerCommand& command() const noexcept { return command_; }
    std::uint32_t scratchPhysical() const noexcept { return scratch_; }
    std::uint32_t callbackAddress() const noexcept { return callback_; }
    std::uint32_t callbackContext() const noexcept { return context_; }
    bool sameRequest(const NativeScalerWrapper& other) const noexcept {
        if (command_.bytes()!=other.command_.bytes() || scratch_!=other.scratch_ || callback_!=other.callback_ || context_!=other.context_)
            return false;
        for (unsigned i=0;i<operations_.size();++i) {
            const auto& a=operations_[i];const auto& b=other.operations_[i];
            if (a.kind!=b.kind || a.wordOffset!=b.wordOffset || a.target!=b.target || a.value!=b.value) return false;
        }
        return true;
    }
    // Other enabled slots are allowed and must be preserved; this wrapper
    // writes only slots0,1,4,5. No guess that the whole retained mask is20033.
    bool scratchStateMatches(std::uint32_t base,std::uint32_t mask) const noexcept {
        return base==scratch_ && scratchWritebackBigEndianProfile(mask) && (mask&RequiredScratchSlots)==RequiredScratchSlots;
    }
    static constexpr std::uint32_t InterruptCpuMask=4,InterruptCpuIndex=2;
    static constexpr bool RequiresGuestInterrupt=true,RequiresScalerUpdate=true;
private:
    NativeScalerWrapper(const NativeScalerCommand& command,unsigned scratch,unsigned callback,unsigned context) noexcept
        :command_(command),scratch_(scratch),callback_(callback),context_(context) {}
    NativeScalerCommand command_;
    std::uint32_t scratch_,callback_,context_;
    std::array<Operation,26> operations_{};
};
}
