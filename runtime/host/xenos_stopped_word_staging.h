#pragma once
#include "native_gpu_word_updates.h"
#include "xenos_stopped_memory.h"

namespace recomp::gpu {
struct StoppedWordDestination {
    std::uint32_t physicalAddress = 0, stagingByteOffset = 0;
};
struct StoppedWordStaging {
    NativeWordUpdatePlan updates;
    std::array<StoppedWordDestination, NativeWordUpdatePlan::Limit> destinations{};
    std::size_t destinationCount = 0;
};
// Read-only bridge from inspected requests to a compact native staging layout.
// This is a MEMORY SUBPLAN, not permission to submit this GPU batch or commit
// guest words. Graphics/hardware semantics, live ownership/aliases/lifetime and
// actual native completion still need validation by the eventual executor.
// Only before-all / after-all graphics phases are supported. Mid-draw events,
// unknown scratch byte order and duplicate same-phase words remain unsupported.
inline bool planStoppedWordStaging(const StoppedMemoryEffects& effects, StoppedWordStaging& output) noexcept {
    if (effects.writes.empty() || effects.writes.size() > NativeWordUpdatePlan::Limit ||
        effects.unmodeledScratchMaskBits || !effects.drawRequests || effects.drawRequests > 64) return false;
    StoppedWordStaging candidate;
    std::array<NativeWordRequest, NativeWordUpdatePlan::Limit> requests{};
    for (std::size_t i = 0; i < effects.writes.size(); ++i) {
        const auto& w = effects.writes[i];
        if (w.kind > 1 || !w.bigEndianProfile || (w.physicalAddress & 3) ||
            std::uint64_t{w.physicalAddress} + 4 > 0x20000000 ||
            (w.priorDrawRequests && w.priorDrawRequests != effects.drawRequests)) return false;
        std::size_t slot = 0;
        while (slot < candidate.destinationCount &&
            candidate.destinations[slot].physicalAddress != w.physicalAddress) ++slot;
        if (slot == candidate.destinationCount) {
            candidate.destinations[slot] = {w.physicalAddress, static_cast<std::uint32_t>(slot * 4)};
            ++candidate.destinationCount;
        }
        requests[i] = {static_cast<std::uint32_t>(slot * 4), w.value,
            w.priorDrawRequests ? NativeWordPhase::AfterGraphics : NativeWordPhase::BeforeGraphics};
    }
    if (!planNativeWordUpdates(requests.data(),effects.writes.size(),
            static_cast<std::uint32_t>(candidate.destinationCount * 4),candidate.updates)) return false;
    output = candidate; return true;
}
}
