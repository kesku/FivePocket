// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "apple/performance_sampler.h"

namespace recomp::apple::performance {
struct ClockStamp {
    std::uint64_t ns=0,kernelId=0;
    Invalid timeInvalid=Invalid::Value;
    std::int32_t identityResult=-1;
};
// Construct before the guest starts. All readers use the same boot-wide Mach
// continuous clock as Sampler; no wall/calendar time, epoch subtraction or
// timestamp-to-CPU conversion. Identity is the full pthread kernel thread ID.
class MeasurementClock {
public:
    MeasurementClock() noexcept;
    ClockStamp read(bool includeIdentity=true) const noexcept;
private:
    std::uint32_t numer_=0,denom_=0;
    std::int32_t timebaseResult_=-1;
};
}
