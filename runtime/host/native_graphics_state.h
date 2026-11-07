#pragma once
#include <cstdint>

namespace recomp::gpu {
// Native bookkeeping, never hardware register readback. These counters alone
// cannot attest GPU work, allocation ownership, publication or guest progress.
struct NativeGraphicsState {
    std::uint64_t binMask=0,binSelect=0,invalidationEpoch=0;
    std::uint64_t experimentalIdleCachePairEpoch=0;
    std::uint32_t knownBinWords=0,batches=0,experimentalColorBatches=0;
};
}
