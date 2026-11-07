#pragma once
#include "xenos_color_copy.h"
#include <cstring>
#include <optional>
#include <vector>

namespace recomp::gpu {
// An owned, immutable readback snapshot, not a GPU completion certificate or
// allocation lease. Caller MUST first fence the GPU and establish host visibility.
// Publication requires a writable destination held under the registry lifecycle
// lock with every guest writer parked. This helper never resolves guest pointers.
class NativeTiledCopyPublication {
public:
    static std::optional<NativeTiledCopyPublication> capture(const ColorCopyPlan& plan,
            const std::uint8_t* guardedReadback,std::size_t bytes) {
        ColorCopyPlan checked;
        if (!guardedReadback || !planColorCopy(plan.source,0x100000,plan.destinationBase,
                plan.source.width|(plan.source.height<<16),0x300|(plan.swapRedBlue?0x1000000u:0),checked) ||
            plan.capacity!=checked.capacity || plan.touchedSpan!=checked.touchedSpan ||
            plan.pixelBytes!=checked.pixelBytes || checked.capacity>4*1024*1024 || bytes!=checked.capacity+32)
            return std::nullopt;
        for (unsigned i=0;i<16;++i)
            if (guardedReadback[i]!=0x5A || guardedReadback[bytes-16+i]!=0x5A) return std::nullopt;
        try {
            NativeTiledCopyPublication result;
            result.plan_=checked;
            result.base_=checked.destinationBase;result.pixelBytes_=checked.pixelBytes;
            result.active_.resize(checked.capacity/4);
            for (unsigned y=0;y<checked.source.height;++y) for (unsigned x=0;x<checked.source.width;++x) {
                std::uint32_t offset=0;
                if (!colorCopyOffset(checked.source.width,checked.source.height,x,y,offset) ||
                    offset%4 || std::uint64_t{offset}+4>checked.capacity || result.active_[offset/4]) return std::nullopt;
                result.active_[offset/4]=1;
            }
            for (std::size_t word=0;word<result.active_.size();++word) if (!result.active_[word])
                for (unsigned lane=0;lane<4;++lane)
                    if (guardedReadback[16+word*4+lane]!=0x5A) return std::nullopt;
            // No borrowed mapped GPU memory survives this call.
            result.bytes_.assign(guardedReadback+16,guardedReadback+16+checked.capacity);
            return result;
        } catch (...) { return std::nullopt; }
    }
    std::size_t capacity() const noexcept { return bytes_.size(); }
    std::uint32_t pixelBytes() const noexcept { return pixelBytes_; }
    // Same-capacity images are not interchangeable. Bind the complete source
    // view/layout/channel order and destination request before batch publication.
    // This establishes request identity, not GPU execution or pixel provenance.
    bool matchesPlan(const ColorCopyPlan& plan) const noexcept {
        return !bytes_.empty() && bytes_.size()==plan_.capacity && active_.size()==bytes_.size()/4 &&
            plan.source.surface==plan_.source.surface && plan.source.info==plan_.source.info &&
            plan.source.width==plan_.source.width && plan.source.height==plan_.source.height &&
            plan.source.depth==plan_.source.depth && plan.destinationBase==plan_.destinationBase &&
            plan.capacity==plan_.capacity && plan.touchedSpan==plan_.touchedSpan &&
            plan.pixelBytes==plan_.pixelBytes && plan.swapRedBlue==plan_.swapRedBlue;
    }
    bool canPublishToOwnedMemory(std::uint32_t physicalBase,const std::uint8_t* destination,
            std::size_t destinationBytes) const noexcept {
        if (!destination || physicalBase!=base_ || destinationBytes!=bytes_.size() || bytes_.empty() ||
            active_.size()!=bytes_.size()/4) return false;
        const auto begin=reinterpret_cast<std::uintptr_t>(destination);
        if (begin>std::numeric_limits<std::uintptr_t>::max()-destinationBytes) return false;
        const auto source=reinterpret_cast<std::uintptr_t>(bytes_.data());
        if (begin<source+bytes_.size() && source<begin+destinationBytes) return false;
        const auto mask=reinterpret_cast<std::uintptr_t>(active_.data());
        if (begin<mask+active_.size() && mask<begin+destinationBytes) return false;
        return true;
    }
    bool publishToOwnedMemory(std::uint32_t physicalBase,std::uint8_t* destination,
            std::size_t destinationBytes) const noexcept {
        if (!canPublishToOwnedMemory(physicalBase,destination,destinationBytes)) return false;
        // All failure-prone validation and allocation precede the FIRST store.
        // Copy only active texels. The destination's arbitrary old tile padding
        // is not replaced with the diagnostic source's5A padding/canaries.
        for (std::size_t word=0;word<active_.size();++word) if (active_[word])
            std::memcpy(destination+word*4,bytes_.data()+word*4,4);
        return true;
    }
    // Read-only continuity check for an already published copy. The caller
    // still needs the SAME current allocation identity and writer exclusion.
    // Ignore unowned tile padding, but compare ALL four lanes of active texels.
    bool matchesPublishedOwnedMemory(std::uint32_t physicalBase,const std::uint8_t* destination,
            std::size_t destinationBytes) const noexcept {
        if(!canPublishToOwnedMemory(physicalBase,destination,destinationBytes))return false;
        for(std::size_t word=0;word<active_.size();++word)if(active_[word] &&
            std::memcmp(destination+word*4,bytes_.data()+word*4,4))return false;
        return true;
    }
    // The same result as matchesPublishedOwnedMemory with one memcmp per run of active words (the tiled layout's padding
    // leaves long runs: 98% of a 1280x720 copy's words are active), the runs found 8 mask bytes at a time. A separate
    // member function: the class layout is unchanged, so objects compiled against the previous header stay compatible.
    bool matchesPublishedOwnedMemoryRuns(std::uint32_t physicalBase,const std::uint8_t* destination,
            std::size_t destinationBytes) const noexcept {
        if(!canPublishToOwnedMemory(physicalBase,destination,destinationBytes))return false;
        const std::size_t words=active_.size();const std::uint8_t* mask=active_.data();
        const auto differs=[&](std::size_t from,std::size_t to){
            return to>from && std::memcmp(destination+from*4,bytes_.data()+from*4,(to-from)*4)!=0;};
        constexpr std::uint64_t allActive=0x0101010101010101ull;
        std::size_t run=0,word=0; // [run, word): active words not compared yet
        for(;word+8<=words;word+=8) {
            std::uint64_t group;std::memcpy(&group,mask+word,8);
            if(group==allActive)continue;
            for(unsigned k=0;k<8;++k)if(!mask[word+k]){if(differs(run,word+k))return false;run=word+k+1;}
        }
        for(;word<words;++word)if(!mask[word]){if(differs(run,word))return false;run=word+1;}
        return !differs(run,words);
    }
private:
    NativeTiledCopyPublication()=default;
    ColorCopyPlan plan_{};
    std::uint32_t base_=0,pixelBytes_=0;
    std::vector<std::uint8_t> bytes_,active_;
};
}
