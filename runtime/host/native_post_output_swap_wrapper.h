#pragma once
#include "native_swap_wrapper.h"
#include "xenos_scratch_writeback.h"

namespace recomp::gpu {
// Exact129-word surface-swap request, NOT an executable90-word swap. The
// additional interrupt must run real guest code and observe its acknowledgement.
// The context packs a nonzero surface address and pacing flags. The reviewed
// guest callback may queue it or write MMIO; parsing satisfies neither path.
// Distinct types keep the completed second swap and deferred third request
// from being interchanged by consumers even when their syntax is identical.
template<std::uint64_t Ticket>
class NativeSurfaceSwapWrapper final {
    static_assert(Ticket==2 || Ticket==3);
public:
    static constexpr std::uint32_t Words=129,Bytes=Words*4,CommandOffsetWords=10;
    static constexpr std::uint32_t RequiredScratchSlots=0x31;
    enum class Kind {BinMaskLow,WaitIdle,ScratchWrite,WaitScratch,Interrupt,CallbackAcknowledge};
    struct Operation {Kind kind;std::uint32_t wordOffset,target,value;};
    static std::optional<NativeSurfaceSwapWrapper> parse(const NativeSwapCommand& command,
            std::uint32_t physical,const std::uint8_t* bytes,std::size_t count) noexcept {
        if(!bytes || count!=Bytes || (physical&3) || std::uint64_t{physical}+Bytes>0x20000000 ||
            reinterpret_cast<std::uintptr_t>(bytes)>std::numeric_limits<std::uintptr_t>::max()-Bytes ||
            command.identity().ticket!=Ticket || std::uint64_t{physical}+40!=command.identity().commandPhysical)return std::nullopt;
        const auto overlaps=[](std::uint32_t a,std::uint32_t n,std::uint32_t b,std::uint32_t m){
            return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;};
        const auto word=[&](unsigned i){const auto* p=bytes+i*4;
            return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];};
        // Reuse ONLY the immutable counter/ticket/tail syntax checks. This
        // temporary envelope is never exposed as a runnable native plan.
        std::array<std::uint8_t,NativeSwapWrapper::Bytes> envelope{};
        std::memcpy(envelope.data(),bytes,78*4);
        std::memcpy(envelope.data()+78*4,bytes+117*4,12*4);
        const auto outer=NativeSwapWrapper::parse(command,physical,envelope.data(),envelope.size());
        const auto callback=word(83),context=word(84),rawScratch=word(89),scratch=rawScratch&~3u;
        if(!outer || !callback || (callback&3) || callback==0x0BADF00D ||
            context!=(command.identity().imagePhysical|0x208) || (rawScratch&3)!=2 ||
            std::uint64_t{scratch}+32>0x20000000 ||
            overlaps(physical,Bytes,command.identity().imagePhysical,NativeSwapImagePlan::Capacity) ||
            overlaps(scratch,32,physical,Bytes) || overlaps(scratch,32,command.identity().imagePhysical,NativeSwapImagePlan::Capacity))return std::nullopt;
        NativeSurfaceSwapWrapper result(command,scratch,callback,context);
        result.events_=outer->events();
        for(auto& event:result.events_){
            if(event.wordOffset>=78)event.wordOffset+=39;
            if(overlaps(event.physicalAddress,4,physical,Bytes) || overlaps(event.physicalAddress,4,scratch,32))return std::nullopt;
        }
        unsigned cursor=78,operations=0;
        const auto add=[&](Kind kind,unsigned at,unsigned target,unsigned value){result.operations_[operations++]={kind,at,target,value};};
        const auto pair=[&](Kind kind,unsigned header,unsigned value,unsigned target){
            if(word(cursor)!=header || word(cursor+1)!=value)return false;
            add(kind,cursor,target,value);cursor+=2;return true;};
        const auto wait=[&](unsigned slot,unsigned value){
            if(word(cursor)!=0xC0043C00 || word(cursor+1)!=0x13 || word(cursor+2)!=scratch+slot*4+2 ||
                word(cursor+3)!=value || word(cursor+4)!=0xFFFFFFFF || word(cursor+5)!=0x100)return false;
            add(Kind::WaitScratch,cursor,slot,value);cursor+=6;return true;};
        if(!pair(Kind::BinMaskLow,0xC0006000,0xFFFFFFFF,0) || !pair(Kind::WaitIdle,0x5C8,0x20000,0x5C8) ||
            word(cursor)!=0x1057C)return std::nullopt;
        add(Kind::ScratchWrite,cursor,4,callback);add(Kind::ScratchWrite,cursor,5,context);cursor+=3;
        if(!pair(Kind::ScratchWrite,0x578,4,0) || !wait(0,4) || !wait(5,context) || !wait(4,callback) ||
            !pair(Kind::Interrupt,0xC0005400,4,4) || !pair(Kind::CallbackAcknowledge,0x45E,4,0x45E) ||
            !wait(0,0) || !pair(Kind::ScratchWrite,0x57C,0x0BADF00D,4) || cursor!=117 || operations!=12)return std::nullopt;
        return result;
    }
    const std::array<Operation,12>& operations() const noexcept {return operations_;}
    const std::array<NativeSwapWrapper::Event,4>& events() const noexcept {return events_;}
    std::uint32_t scratchPhysical() const noexcept {return scratch_;}
    std::uint32_t callbackAddress() const noexcept {return callback_;}
    std::uint32_t callbackContext() const noexcept {return context_;}
    const NativeSwapCommand& command() const noexcept {return command_;}
    // All variable request fields are retained. Fixed syntax is enforced by
    // parse; no borrowed pointer or collapsed90word execution plan escapes.
    bool sameRequest(const NativeSurfaceSwapWrapper& other) const noexcept {
        if(command_.bytes()!=other.command_.bytes() || scratch_!=other.scratch_ ||
            callback_!=other.callback_ || context_!=other.context_)return false;
        for(unsigned i=0;i<events_.size();++i){const auto& a=events_[i];const auto& b=other.events_[i];
            if(a.wordOffset!=b.wordOffset || a.physicalAddress!=b.physicalAddress || a.endian!=b.endian ||
                a.source!=b.source || a.phase!=b.phase || a.literal!=b.literal)return false;}
        return true;
    }
    bool scratchStateMatches(std::uint32_t base,std::uint32_t mask) const noexcept {
        return base==scratch_ && scratchWritebackBigEndianProfile(mask) && (mask&RequiredScratchSlots)==RequiredScratchSlots;
    }
    static constexpr bool RequiresFreshGuestInterrupt=true,RequiresSurfaceAddressHandling=true;
private:
    NativeSurfaceSwapWrapper(const NativeSwapCommand& command,unsigned scratch,unsigned callback,unsigned context) noexcept
        :command_(command),scratch_(scratch),callback_(callback),context_(context){}
    NativeSwapCommand command_;
    std::uint32_t scratch_,callback_,context_;
    std::array<Operation,12> operations_{};
    std::array<NativeSwapWrapper::Event,4> events_{};
};
using NativePostOutputSwapWrapper=NativeSurfaceSwapWrapper<2>;
using NativeDeferredSwapWrapper=NativeSurfaceSwapWrapper<3>;
}
