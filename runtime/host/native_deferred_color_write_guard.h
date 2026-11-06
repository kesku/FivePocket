#pragma once
#include "native_deferred_color_memory.h"
#include "native_tiled_copy_publication.h"

namespace recomp::gpu {
// Before-images and failure-atomic COPY-only publication for a future live
// prefix subtransaction. NOT a lifecycle lock, parked-writer gate, GPU receipt,
// tail completion or RPTR retirement. The caller must hold real allocator
// ownership and exclude all writers continuously from create through publish.
// All mappings must be OS-readable; arithmetic metadata cannot establish that.
// Stopped diagnostic output must never be promoted into a live GPU receipt.
class NativeDeferredColorWriteGuard final {
public:
    NativeDeferredColorWriteGuard(const NativeDeferredColorWriteGuard&)=delete;
    NativeDeferredColorWriteGuard& operator=(const NativeDeferredColorWriteGuard&)=delete;
    NativeDeferredColorWriteGuard(NativeDeferredColorWriteGuard&&) noexcept=default;
    NativeDeferredColorWriteGuard& operator=(NativeDeferredColorWriteGuard&&) noexcept=default;
    static std::optional<NativeDeferredColorWriteGuard> create(const NativeDeferredColorCapture& capture,
            const NativeDeferredColorMemoryPlan& plan,const NativeDeferredColorResolvedMemory& memory) noexcept {
        if(!matchesNativeDeferredColorMemory(capture,plan,memory))return std::nullopt;
        try{
            NativeDeferredColorWriteGuard guard(capture.submission(),plan,memory);
            std::size_t total=0;
            const auto save=[&](const auto& m,auto& bytes){
                if(m.range.bytes>MaximumSnapshotBytes-total)return false;
                total+=m.range.bytes;bytes.assign(m.bytes(),m.bytes()+m.range.bytes);return true;
            };
            for(unsigned i=0;i<plan.inputCount();++i)if(!save(memory.inputs[i],guard.inputs_[i]))return std::nullopt;
            for(unsigned i=0;i<5;++i)if(!save(memory.protectedRanges[i],guard.protected_[i]))return std::nullopt;
            for(unsigned i=0;i<7;++i)if(!save(memory.writes[i],guard.writes_[i]))return std::nullopt;
            guard.snapshotBytes_=total;return guard;
        }catch(...){return std::nullopt;}
    }
    bool matches(const NativeDeferredColorCapture& capture,const NativeDeferredColorResolvedMemory& memory) const noexcept {
        if(published_ || !snapshotBytes_ || !frame_.sameRequest(capture.submission()) ||
            !matchesNativeDeferredColorMemory(capture,plan_,memory))return false;
        const auto same=[](const auto& current,const auto& original,const auto& before){
            return color_memory_detail::same(current.range,original.range) &&
                color_memory_detail::same(current.allocation,original.allocation) &&
                current.allocationBytes==original.allocationBytes && current.writable==original.writable &&
                before.size()==current.range.bytes && !std::memcmp(before.data(),current.bytes(),before.size());
        };
        for(unsigned i=0;i<plan_.inputCount();++i)if(!same(memory.inputs[i],memory_.inputs[i],inputs_[i]))return false;
        for(unsigned i=0;i<5;++i)if(!same(memory.protectedRanges[i],memory_.protectedRanges[i],protected_[i]))return false;
        for(unsigned i=0;i<7;++i)if(!same(memory.writes[i],memory_.writes[i],writes_[i]))return false;
        return true;
    }
    bool publishCopy(const NativeDeferredColorCapture& capture,const NativeDeferredColorResolvedMemory& memory,
            const NativeTiledCopyPublication& copy) noexcept {
        if(!copy.matchesPlan(capture.copy().copy) || !matches(capture,memory))return false;
        const auto target=memory.writes[0];
        // Under the caller's retained real ownership, all fallible checks are
        // before the first store. The copy helper preserves arbitrary padding;
        // only the one-shot latch changes afterwards. Events/IRQ/RPTR untouched.
        if(!copy.publishToOwnedMemory(target.range.address,target.bytes(),target.range.bytes))return false;
        published_=true;return true;
    }
    bool published() const noexcept{return published_;}
    std::size_t snapshotBytes() const noexcept{return snapshotBytes_;}
private:
    static constexpr std::size_t MaximumSnapshotBytes=32*1024*1024;
    NativeDeferredColorWriteGuard(const NativeDeferredSwapSubmission& frame,const NativeDeferredColorMemoryPlan& plan,
            const NativeDeferredColorResolvedMemory& memory):frame_(frame),plan_(plan),memory_(memory){}
    NativeDeferredSwapSubmission frame_;
    NativeDeferredColorMemoryPlan plan_;
    NativeDeferredColorResolvedMemory memory_;
    std::array<std::vector<std::uint8_t>,48> inputs_;
    std::array<std::vector<std::uint8_t>,5> protected_;
    std::array<std::vector<std::uint8_t>,7> writes_;
    std::size_t snapshotBytes_=0;
    bool published_=false;
};
}
