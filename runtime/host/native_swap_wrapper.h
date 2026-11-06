#pragma once
#include "native_swap_command.h"

namespace recomp::gpu {
// Ordered, non-executable request plan for the observed native swap wrapper.
// Parsing does not authorize memory writes, satisfy counters/cache operations,
// invoke a frame consumer, acknowledge a GPU identifier or advance RPTR.
// A live transaction still needs allocation leases, an explicit counter source,
// native prefix/tail work and audits, and post-presentation fail-stop handling.
class NativeSwapWrapper final {
public:
    static constexpr std::uint32_t Words=90,Bytes=Words*4,CommandOffsetWords=10;
    enum class Source { Counter,Literal };
    enum class Phase { BeforeSwap,AfterSwap };
    struct Event {
        std::uint32_t wordOffset,physicalAddress,endian;
        Source source;
        Phase phase;
        std::optional<std::uint32_t> literal;
    };
    static std::optional<NativeSwapWrapper> parse(const NativeSwapCommand& command,
            std::uint32_t physical,const std::uint8_t* bytes,std::size_t count) noexcept {
        if(!bytes || count!=Bytes || (physical&3) || std::uint64_t{physical}+Bytes>0x20000000 ||
            reinterpret_cast<std::uintptr_t>(bytes)>std::numeric_limits<std::uintptr_t>::max()-Bytes ||
            std::uint64_t{physical}+CommandOffsetWords*4!=command.identity().commandPhysical ||
            !command.matches(command.identity().commandPhysical,bytes+CommandOffsetWords*4,NativeSwapCommand::Bytes))return std::nullopt;
        const auto overlaps=[](std::uint32_t a,std::uint32_t n,std::uint32_t b,std::uint32_t m){
            return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
        };
        if(overlaps(physical,Bytes,command.identity().imagePhysical,NativeSwapImagePlan::Capacity))return std::nullopt;
        const auto word=[&](unsigned i){const auto* p=bytes+i*4;
            return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];};
        constexpr std::array<std::uint32_t,8> prefix{0x5C8,0x20000,0xE00,1,0xE40,1,0xC0025800,0x80000003};
        for(unsigned i=0;i<prefix.size();++i)if(word(i)!=prefix[i])return std::nullopt;
        if(word(9)!=0xDEADBEEF || word(74)!=0xC0025800 || word(75)!=0x80000003 || word(77)!=0xDEADBEEF ||
            word(78)!=0xC0003B00 || word(79)!=0x7FFF || word(80)!=0xC0001000 || word(81)!=0x77777777 ||
            word(82)!=0xC0025800 || word(83)!=3 || word(86)!=0xC0025800 || word(87)!=3)return std::nullopt;
        NativeSwapWrapper result;
        for(unsigned i=0;i<4;++i){const auto offset=std::array<unsigned,4>{6,74,82,86}[i];
            const auto raw=word(offset+2),address=raw&~3u,endian=raw&3;
            if(std::uint64_t{address}+4>0x20000000 || endian!=(i<2?0u:2u) ||
                overlaps(address,4,physical,Bytes) ||
                overlaps(address,4,command.identity().imagePhysical,NativeSwapImagePlan::Capacity))return std::nullopt;
            for(unsigned j=0;j<i;++j)if(result.events_[j].physicalAddress==address)return std::nullopt;
            result.events_[i]={offset,address,endian,i<2?Source::Counter:Source::Literal,
                i==0?Phase::BeforeSwap:Phase::AfterSwap,i<2?std::nullopt:std::optional<std::uint32_t>{word(offset+3)}};
        }
        if(std::uint64_t{result.events_[0].physicalAddress}+4!=result.events_[1].physicalAddress)return std::nullopt;
        return result;
    }
    const std::array<Event,4>& events() const noexcept {return events_;}
    // These are obligations, not completed effects or verified retail semantics.
    static constexpr bool RequiresIdleCachePair=true;
    static constexpr bool RequiresPostSwapInvalidation=true;
    static constexpr bool RequiresCounterSource=true;
private:
    NativeSwapWrapper()=default;
    std::array<Event,4> events_{};
};
}
