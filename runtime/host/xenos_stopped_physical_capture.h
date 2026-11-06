#pragma once
#include "xenos_indirect_inspection.h"
#include <array>
#include <optional>
namespace recomp::gpu {
using StoppedPhysicalRanges = std::vector<std::array<std::uint32_t,2>>;
constexpr std::uint32_t DefaultStoppedPhysicalByteBudget=4096;
constexpr std::uint32_t MaxStoppedPhysicalByteBudget=2*1024*1024;
// Pure diagnostic bounds, NOT an allocation lease or command-execution plan.
// Caller supplies a validated stopped request. Payloads are restricted to its
// command owner up to the request's end; image scope permits metadata ONLY.
class StoppedSwapRequestScope final {
public:
    static constexpr std::uint32_t ImageBytes=1280*736*4;
    static std::optional<StoppedSwapRequestScope> make(std::uint32_t commandVirtual,
            std::uint32_t commandPhysical,std::uint32_t allocationVirtual,std::uint32_t allocationBytes,
            std::optional<std::uint32_t> imagePhysical={}) noexcept {
        if((commandVirtual|commandPhysical|allocationVirtual|allocationBytes)&3 || allocationBytes<256 ||
            allocationVirtual>commandVirtual || std::uint64_t{commandVirtual}+256>std::uint64_t{allocationVirtual}+allocationBytes ||
            std::uint64_t{allocationVirtual}+allocationBytes>0x100000000ull || commandVirtual<commandPhysical ||
            commandVirtual-allocationVirtual>commandPhysical)return std::nullopt;
        const auto physical=commandPhysical-(commandVirtual-allocationVirtual);
        if(std::uint64_t{physical}+allocationBytes>0x20000000)return std::nullopt;
        if(imagePhysical && ((*imagePhysical&4095) || std::uint64_t{*imagePhysical}+ImageBytes>0x20000000 ||
            (*imagePhysical<std::uint64_t{physical}+allocationBytes && physical<std::uint64_t{*imagePhysical}+ImageBytes)))return std::nullopt;
        return StoppedSwapRequestScope(physical,allocationBytes,commandPhysical+256,imagePhysical);
    }
    bool permitsCommandPayload(std::uint32_t address,std::uint32_t bytes) const noexcept {
        return bytes && !((address|bytes)&3) && address>=base_ && std::uint64_t{address}+bytes<=end_;
    }
    bool permitsOwnershipQuery(std::uint32_t address,std::uint32_t bytes) const noexcept {
        if(!bytes || ((address|bytes)&3))return false;
        return (address>=base_ && std::uint64_t{address}+bytes<=std::uint64_t{base_}+size_) ||
            (image_ && address>=*image_ && std::uint64_t{address}+bytes<=std::uint64_t{*image_}+ImageBytes);
    }
private:
    StoppedSwapRequestScope(std::uint32_t base,std::uint32_t size,std::uint32_t end,
        std::optional<std::uint32_t> image):base_(base),size_(size),end_(end),image_(image){}
    std::uint32_t base_,size_,end_;
    std::optional<std::uint32_t> image_;
};
// Metadata-only ranges never reach a payload reader or allocate range-sized data.
inline bool validStoppedOwnershipRanges(const StoppedPhysicalRanges& ranges) {
    if(ranges.empty() || ranges.size()>8)return false;
    for(std::size_t i=0;i<ranges.size();++i){
        const auto a=ranges[i][0],n=ranges[i][1];
        if(!n || n>0x1000000 || ((a|n)&3) || std::uint64_t{a}+n>0x20000000)return false;
        for(std::size_t j=0;j<i;++j)if(a<std::uint64_t{ranges[j][0]}+ranges[j][1] &&
            ranges[j][0]<std::uint64_t{a}+n)return false;
    }
    return true;
}
inline bool validStoppedPhysicalRanges(const StoppedPhysicalRanges& ranges,
        std::uint32_t byteBudget=DefaultStoppedPhysicalByteBudget) {
    if (ranges.empty() || ranges.size() > 8 || !byteBudget || (byteBudget&3) ||
        byteBudget>MaxStoppedPhysicalByteBudget) return false;
    std::uint32_t total = 0;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const auto a = ranges[i][0], n = ranges[i][1];
        if (!n || ((a | n) & 3) || n > byteBudget - total || std::uint64_t{a} + n > 0x20000000)
            return false;
        total += n;
        for (std::size_t j = 0; j < i; ++j)
            if (a < std::uint64_t{ranges[j][0]} + ranges[j][1] && ranges[j][0] < std::uint64_t{a} + n)
                return false;
    }
    return true;
}
// Explicit diagnostic ranges, not inferred resource sizes or execution-time
// contents. All guest writers must be joined and reader must enforce complete
// allocation ownership. No GPU work, read pointer or guest memory is changed.
inline bool captureStoppedPhysicalRanges(const StoppedPhysicalRanges& ranges,
        const Pm4PhysicalReader& reader, std::vector<std::vector<std::uint8_t>>& output,
        std::uint32_t byteBudget=DefaultStoppedPhysicalByteBudget) {
    if (!reader || !validStoppedPhysicalRanges(ranges,byteBudget)) return false;
    try {
        std::vector<std::vector<std::uint8_t>> candidate;
        candidate.reserve(ranges.size());
        for (const auto& r : ranges) {
            auto bytes = reader(r[0], r[1]);
            if (!bytes || bytes->size() != r[1]) return false;
            candidate.push_back(std::move(*bytes));
        }
        output = std::move(candidate); return true;
    } catch (...) { return false; }
}
}
