// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace recomp::apple {
// The original startup consumer waits for a nonzero low32 timebase. Require
// measured progress before starting it, without inferring the Mac's counter
// defect also exists on iPad or selecting an unmeasured fallback there.
inline bool startupClockReady(std::uint64_t first,std::uint64_t second) noexcept {
#if defined(APPLE_PORT_MUTANT_CLOCK_GATE)
    (void)first;(void)second;return true;
#else
    return second>first && static_cast<std::uint32_t>(second)!=0;
#endif
}
}
