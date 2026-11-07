// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <utility>

namespace recomp::apple {

enum class OrderedFrameAttempt { Deferred, Rejected, Accepted };

// The original callback owns its pixels until this function returns. A scene
// transition between the off-main wait and main-queue dispatch is a deferral,
// not rejection, acceptance, a copied frame, or a sequence-number advance.
// Neither callable may retain the borrowed frame after this function returns.
template<class Wait, class Attempt>
int waitForOrderedFrame(Wait&& wait, Attempt&& attempt) {
    while (wait()) {
        switch (attempt()) {
        case OrderedFrameAttempt::Deferred: break;
        case OrderedFrameAttempt::Rejected: return 0;
        case OrderedFrameAttempt::Accepted: return 1;
        }
    }
    return 0;
}

} // namespace recomp::apple
