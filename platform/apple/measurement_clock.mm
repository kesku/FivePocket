// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/measurement_clock.h"
#include <mach/mach_time.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

namespace recomp::apple::performance {
MeasurementClock::MeasurementClock() noexcept {
    mach_timebase_info_data_t info{};
    timebaseResult_=mach_timebase_info(&info);numer_=info.numer;denom_=info.denom;
}
ClockStamp MeasurementClock::read(bool includeIdentity) const noexcept {
    ClockStamp out;
    if(!timebaseResult_) {
#if defined(APPLE_MEASUREMENT_MUTANT_CALENDAR_CLOCK)
        timespec time{};clock_gettime(CLOCK_REALTIME,&time);
        out.ns=std::uint64_t(time.tv_sec)*1'000'000'000+time.tv_nsec;
        out.timeInvalid=Invalid::None;
#else
        out.timeInvalid=checkedNanos(mach_continuous_time(),numer_,denom_,out.ns);
#endif
    } else out.timeInvalid=Invalid::ApiError;
    if(includeIdentity) {
#if defined(APPLE_MEASUREMENT_MUTANT_PID_AS_THREAD)
        out.kernelId=getpid();out.identityResult=0;
#else
        out.identityResult=pthread_threadid_np(nullptr,&out.kernelId);
#endif
        if(out.identityResult)out.kernelId=0;
    }
    return out;
}
}
