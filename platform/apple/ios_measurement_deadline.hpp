// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <dispatch/dispatch.h>
#include <cstdio>

namespace iosmeasurement {
// Only explicitly requested measurements have an owner. Keep that session alive
// until close, even if its UI owner changes; never wait for a blocked main queue.
template<class Owner> void arm(Owner& owner,dispatch_time_t deadline) {
    auto measurement=owner.measurement;
    if(!measurement)return;
    auto* identity=&owner;
    dispatch_after(deadline,
        dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
            measurement->close();
            std::fputs("APPLE_MEASUREMENT_DEADLINE seconds=120 physical_unproven=1\n",stderr);
            dispatch_async(dispatch_get_main_queue(), ^{
                if(identity->measurement.get()==measurement.get() && identity->presenter)
                    identity->presenter->stopDisplayObservation();
            });
        });
}
}
