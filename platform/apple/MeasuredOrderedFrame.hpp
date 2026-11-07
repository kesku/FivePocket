// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "OrderedFrameWait.hpp"
#include <cstdint>
#include <optional>

namespace recomp::apple {

// Diagnostics must never decide whether the game's borrowed frame is accepted.
// A deferred attempt can straddle lifecycle/attachment transitions: abandon that
// diagnostic lease and close its window instead of exporting it as clean timing.
// The ordinary ordered retry and its exact return value remain unchanged.
template<class Measurement, class Wait, class Attempt>
int waitForMeasuredOrderedFrame(Measurement* measurement, std::uint32_t sequence,
                               Wait&& wait, Attempt&& attempt) {
    std::optional<typename Measurement::Frame> frame;
    if (measurement && !measurement->status().closing)
        frame.emplace(measurement->enter(sequence));
    auto mainEntered = [&] {if(frame)frame->mainEntered();};
    auto presenterReturned = [&] {if(frame)frame->presenterReturned();};
    const int delivered = waitForOrderedFrame(wait, [&] {
        const auto result = attempt(mainEntered, presenterReturned);
        if (result == OrderedFrameAttempt::Deferred && frame) {
            // Reset before close so the drain includes the abandoned lease.
            // No synthetic sample, renumbering, game rejection or early return.
            frame.reset();
            measurement->close();
        }
        return result;
    });
    if(frame)frame->returned(delivered != 0);
    return delivered;
}

} // namespace recomp::apple
