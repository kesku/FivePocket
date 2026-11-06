#pragma once
#include "native_gpu_word_readback.h"
#include "native_tiled_copy_publication.h"
#include <limits>

namespace recomp::gpu {
// These are already-resolved, writable spans, NOT ownership certificates. The
// registry caller must retain its lifecycle lock AND park every guest writer
// across resolution, native execution/fence, auditing and publication. Never
// resolve these from an old stopped capture or use them after releasing a lease.
struct NativeOutputSpan {
    std::uint32_t physicalAddress = 0;
    std::uint8_t* bytes = nullptr;
    std::size_t size = 0;
};
struct NativeOutputWord {
    std::uint32_t stagingByteOffset = 0;
    NativeOutputSpan target;
};
namespace output_publication_detail {
inline bool valid(const NativeOutputSpan& span) noexcept {
    return span.bytes && span.size && !(span.physicalAddress&3) && !(span.size&3) &&
        span.size<=0x20000000 && std::uint64_t{span.physicalAddress}+span.size<=0x20000000 &&
        reinterpret_cast<std::uintptr_t>(span.bytes)<=std::numeric_limits<std::uintptr_t>::max()-span.size;
}
inline bool hostOverlap(const std::uint8_t* a,std::size_t n,const std::uint8_t* b,std::size_t m) noexcept {
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
    // Callers validate non-overflowing spans before reaching this function.
    return x<y+m && y<x+n;
}
inline bool overlap(const NativeOutputSpan& a,const NativeOutputSpan& b) noexcept {
    return (a.physicalAddress<std::uint64_t{b.physicalAddress}+b.size &&
        b.physicalAddress<std::uint64_t{a.physicalAddress}+a.size) || hostOverlap(a.bytes,a.size,b.bytes,b.size);
}
inline std::array<std::uint8_t,4> bigEndian(std::uint32_t value) noexcept {
    return {std::uint8_t(value>>24),std::uint8_t(value>>16),std::uint8_t(value>>8),std::uint8_t(value)};
}
}
// Failure-atomic publication, not a concurrent/hardware atomic transaction.
// Caller MUST have completed the actual GPU fence, host visibility and every
// command's semantic obligation before calling. No bool here stands in for a
// fence, coherency WAIT, allocation lease, or permission to retire a draw.
// Inputs/outputs must also exclude the command ring, children, source resources
// and identifier; those ownership/hazard checks belong to the registry caller.
// Re-audit all words; require a bijective staging-to-output map and reject both
// physical and host aliases. Prepare all event bytes before the first store.
// Optional copy publishes first, then events, then the big-endian RPTR LAST.
// No allocation, callback or fallible operation follows the first copy store.
inline bool publishNativeGpuOutputs(const NativeWordUpdatePlan& plan,const std::uint8_t* readback,
        std::size_t readbackBytes,const NativeOutputWord* outputs,std::size_t outputCount,
        const NativeOutputSpan& readPointer,std::uint32_t previousWord,std::uint32_t nextWord,
        const NativeTiledCopyPublication* copy=nullptr,const NativeOutputSpan& copyTarget={}) noexcept {
    using namespace output_publication_detail;
    if (outputCount>NativeWordUpdatePlan::Limit || (outputCount && !outputs) ||
        !valid(readPointer) || readPointer.size!=4 || nextWord<=previousWord || !readback ||
        readbackBytes>4*1024*1024 ||
        reinterpret_cast<std::uintptr_t>(readback)>std::numeric_limits<std::uintptr_t>::max()-readbackBytes ||
        hostOverlap(readPointer.bytes,4,readback,readbackBytes)) return false;
    const auto previous=bigEndian(previousWord),next=bigEndian(nextWord);
    if (std::memcmp(readPointer.bytes,previous.data(),4)) return false;
    AuditedNativeWords audited;NativeWordReadbackFailure error;
    if (!auditNativeWordReadback(plan,readback,readbackBytes,audited,error) || audited.count!=outputCount) return false;
    if (copy) {
        if (!valid(copyTarget) || overlap(copyTarget,readPointer) ||
            hostOverlap(copyTarget.bytes,copyTarget.size,readback,readbackBytes) ||
            !copy->canPublishToOwnedMemory(copyTarget.physicalAddress,copyTarget.bytes,copyTarget.size)) return false;
    } else if (copyTarget.bytes || copyTarget.size || copyTarget.physicalAddress) return false;
    std::array<std::array<std::uint8_t,4>,NativeWordUpdatePlan::Limit> values{};
    std::array<std::uint8_t*,NativeWordUpdatePlan::Limit> destinations{};
    for (std::size_t i=0;i<outputCount;++i) {
        const auto& output=outputs[i];const auto& target=output.target;
        if (!valid(target) || target.size!=4 || overlap(target,readPointer) ||
            (copy && overlap(target,copyTarget)) || hostOverlap(target.bytes,4,readback,readbackBytes)) return false;
        for (std::size_t j=0;j<i;++j)
            if (outputs[j].stagingByteOffset==output.stagingByteOffset || overlap(outputs[j].target,target)) return false;
        std::size_t slot=0;
        while (slot<audited.count && audited.words[slot].byteOffset!=output.stagingByteOffset) ++slot;
        if (slot==audited.count) return false;
        destinations[i]=target.bytes;values[i]=audited.words[slot].bigEndianBytes;
    }
    // Save pointer metadata too: nothing used after the copy may depend on
    // caller-owned descriptors that could themselves occupy an output span.
    auto* const cursor=readPointer.bytes;
    if (copy && !copy->publishToOwnedMemory(copyTarget.physicalAddress,copyTarget.bytes,copyTarget.size)) return false;
    for (std::size_t i=0;i<outputCount;++i) std::memcpy(destinations[i],values[i].data(),4);
    std::memcpy(cursor,next.data(),4);
    return true;
}
}
