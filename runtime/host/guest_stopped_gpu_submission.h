#pragma once
#include "guest_import_registry.h"
#include "guest_scalar_memory.h"
#include "xenos_command_stream.h"
#include <algorithm>

namespace recomp::bootstrap {
enum class StoppedGpuSubmissionStatus : std::uint32_t {
    NotSubmission, Unconfigured, InvalidWritePointer, SnapshotUnavailable, DecodeRejected, Decoded
};
struct StoppedGpuSubmission {
    StoppedGpuSubmissionStatus status = StoppedGpuSubmissionStatus::NotSubmission;
    std::uint32_t readWord = 0, writeWord = 0;
    gpu::Pm4Snapshot snapshot;
    gpu::Pm4Error error;
};
// Post-stop diagnostics ONLY, after all guest writers join. The MMIO store
// remains rejected. This does not submit/consume commands, move a pointer or
// write any guest memory. Command payloads stay in this private owned snapshot.
inline StoppedGpuSubmission inspectStoppedGpuSubmission(const GuestScalarAccess& access,
                                                       const GuestImportRegistry& registry) {
    StoppedGpuSubmission result;
    if (access.region != GuestScalarRegion::GpuRegisters || !access.write ||
        access.address != 0x7FC80714 || access.width != 4) return result;
    const auto ring = registry.graphicsRingConfiguration();
    if (!ring) { result.status = StoppedGpuSubmissionStatus::Unconfigured; return result; }
    if (access.value >= ring->bytes / 4) {
        result.status = StoppedGpuSubmissionStatus::InvalidWritePointer; return result;
    }
    result.readWord = ring->readWord; result.writeWord = static_cast<std::uint32_t>(access.value);
    const auto bytes = registry.snapshotStoppedGraphicsRing();
    if (!bytes) { result.status = StoppedGpuSubmissionStatus::SnapshotUnavailable; return result; }
    gpu::Pm4Limits limits;
    limits.maxWords = ring->bytes / 4;
    limits.maxPackets = std::min(limits.maxWords, 65536U);
    result.status = gpu::decodePm4Ring(bytes->data(), bytes->size(), result.readWord, result.writeWord,
        limits, result.snapshot, result.error) ? StoppedGpuSubmissionStatus::Decoded : StoppedGpuSubmissionStatus::DecodeRejected;
    return result;
}
}
