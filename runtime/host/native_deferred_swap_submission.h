#pragma once
#include "native_post_output_swap_wrapper.h"
#include "xenos_command_stream.h"
#include <utility>

namespace recomp::gpu {
// Owns a framed prefix and a separately typed third-swap request. This is NOT
// a rendering plan, GPU receipt, image lease, or permission to publish events
// or retire the combined IB. In particular, absent color-final events must
// never be synthesized to fit an older completed-batch profile.
class NativeDeferredSwapSubmission final {
public:
    static constexpr std::uint32_t MaximumWords=4096;
    static std::optional<NativeDeferredSwapSubmission> parse(const NativeSwapCommand& command,
            std::uint32_t physical,const std::uint8_t* bytes,std::size_t count) {
        using Wrapper=NativeDeferredSwapWrapper;
        if(!bytes || (physical|count)&3 || count<=Wrapper::Bytes || count>MaximumWords*4 ||
            std::uint64_t{physical}+count>0x20000000 ||
            reinterpret_cast<std::uintptr_t>(bytes)>std::numeric_limits<std::uintptr_t>::max()-count)return std::nullopt;
        const auto prefixBytes=static_cast<std::uint32_t>(count-Wrapper::Bytes);
        const auto overlaps=[&](std::uint32_t address,std::uint32_t size){
            return address<std::uint64_t{physical}+count && physical<std::uint64_t{address}+size;};
        if(overlaps(command.identity().imagePhysical,NativeSwapImagePlan::Capacity))return std::nullopt;
        const auto wrapper=Wrapper::parse(command,physical+prefixBytes,bytes+prefixBytes,Wrapper::Bytes);
        if(!wrapper || overlaps(wrapper->scratchPhysical(),32))return std::nullopt;
        for(const auto& event:wrapper->events())if(overlaps(event.physicalAddress,4))return std::nullopt;
        Pm4Snapshot prefix;Pm4Error error;
        if(!decodePm4(bytes,prefixBytes,{MaximumWords,512,false},prefix,error))return std::nullopt;
        return NativeDeferredSwapSubmission(physical,std::move(prefix),*wrapper);
    }
    const Pm4Snapshot& prefix() const noexcept {return prefix_;}
    const NativeDeferredSwapWrapper& swap() const noexcept {return swap_;}
    std::uint32_t physicalAddress() const noexcept {return physical_;}
    std::uint32_t swapOffsetWords() const noexcept {return static_cast<std::uint32_t>(prefix_.words().size());}
    bool sameRequest(const NativeDeferredSwapSubmission& other) const noexcept {
        return physical_==other.physical_ && prefix_.words()==other.prefix_.words() && swap_.sameRequest(other.swap_);
    }
private:
    NativeDeferredSwapSubmission(std::uint32_t physical,Pm4Snapshot prefix,NativeDeferredSwapWrapper swap):
        physical_(physical),prefix_(std::move(prefix)),swap_(std::move(swap)){}
    std::uint32_t physical_;
    Pm4Snapshot prefix_;
    NativeDeferredSwapWrapper swap_;
};
}
