// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "apple/asset_verifier.h"
#include <dispatch/dispatch.h>
#include <pthread.h>

namespace recomp::apple {
// Main-thread confined UI admission; result freshness never authorizes a game
// start. It only prevents simultaneous full-data hashing operations.
class IosVerificationDiscipline final {
public:
    enum class Admission { Started, MissingPins, MissingRoot, Busy };
    Admission begin(bool pins,bool root) noexcept {
        if (!pins) return Admission::MissingPins;
        if (!root) return Admission::MissingRoot;
#if !defined(APPLE_HOST_MUTANT_VERIFY_BUSY)
        if (busy_) return Admission::Busy;
#endif
        busy_=true;return Admission::Started;
    }
    void finish() noexcept {busy_=false;}
    bool busy() const noexcept {return busy_;}
private:
    bool busy_=false;
};

// Used unchanged by UIKit and a real-verifier Mac component test. Work must own
// its pins/root by value. Blocks copy those owners until main completion; no
// borrowed UIView/controller lifetime is needed. No UI-thread file hashing.
template<class Work,class Completion>
bool dispatchHostVerification(Work work,Completion completion) {
    if (!pthread_main_np()) return false;
    auto background=^{
        AssetVerification result;
        bool unexpectedFailure=false;
        try {result=work();} catch (...) {unexpectedFailure=true;}
        dispatch_async(dispatch_get_main_queue(), ^{completion(result,unexpectedFailure);});
    };
#if defined(APPLE_HOST_MUTANT_HASH_ON_MAIN)
    background();
#else
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),background);
#endif
    return true;
}
}
