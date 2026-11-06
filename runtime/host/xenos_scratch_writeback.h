#pragma once
#include <cstdint>

namespace recomp::gpu {
// Restricted source-corroborated Xenos profile, not all swap-mode semantics.
// LibXenon xe.c (a3272457ffecfe5cf449c6198c434a9a4e2db67c): Xe_Init
// programs SCRATCH_UMSK=0x20033; Xe_pDebugSync writes a scratch register then
// compares its cache-invalidated writeback as a native big-endian CPU u32.
// Pinned Xenia independently uses eight low enable bits and BE word stores.
// Other upper-bit combinations remain unknown. This function writes nothing.
constexpr bool scratchWritebackBigEndianProfile(std::uint32_t mask) noexcept {
    return (mask & ~0xFFu) == 0x20000;
}
}
