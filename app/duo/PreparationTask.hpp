// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "apple/ios_stage_preparation.h"
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <utility>

namespace recomp::apple {
// Main-thread-confined ownership for the future decoder-enabled host. Both
// read-only verification and run-file preparation must use this ONE instance.
// A failed/abandoned preparation is terminal: retain partial files, no retry.
class IosPreparationDiscipline final {
public:
    enum class State { Idle, Verifying, Preparing, Running, Terminal };
    enum class Admission { Started, Busy, DecoderUnavailable, SceneUnavailable, MissingInputs };
    enum class Completion { Started, Stale, Failed, SceneUnavailable };
    State state() const noexcept { return state_; }
    bool beginVerification() noexcept {
        if (state_ != State::Idle) return false;
        state_ = State::Verifying;
        return true;
    }
    void finishVerification() noexcept {
        if (state_ == State::Verifying) state_ = State::Idle;
    }
    Admission beginPreparation(bool decoder, bool active, bool signedInputs) noexcept {
#if !defined(APPLE_PREP_MUTANT_BUSY)
        if (state_ != State::Idle) return Admission::Busy;
#endif
        if (!decoder) return Admission::DecoderUnavailable;
        if (!active) return Admission::SceneUnavailable;
        if (!signedInputs) return Admission::MissingInputs;
        state_ = State::Preparing;
        return Admission::Started;
    }
    template<class Start>
    Completion finishPreparation(const IosStagePreparation& result, bool unexpected,
                                 bool active, Start&& start) {
        if (state_ != State::Preparing) return Completion::Stale;
        state_ = State::Terminal;
#if !defined(APPLE_PREP_MUTANT_RESULT)
        if (unexpected || !result.prepared() || !result.files.prepared() ||
            !result.assets.allMatched() || result.files.plan.arguments.empty())
            return Completion::Failed;
#else
        (void)result; (void)unexpected;
#endif
#if !defined(APPLE_PREP_MUTANT_SCENE)
        if (!active) return Completion::SceneUnavailable;
#else
        (void)active;
#endif
        // Mark ownership before calling out; a reentrant completion cannot run
        // a second guest. Start must move/copy the prepared arguments to Session.
        state_ = State::Running;
        try { std::forward<Start>(start)(); }
        catch (...) { state_ = State::Terminal; throw; }
        return Completion::Started;
    }
    void invalidate() noexcept { state_ = State::Terminal; }
private:
    State state_{State::Idle};
};

// Own pins, template bytes and root in Work, never borrowed spans into a UI
// object. This only dispatches preparation; completion must pass the gate above.
template<class Work, class Completion>
bool dispatchIosPreparation(Work work, Completion completion) {
    if (!pthread_main_np()) return false;
    auto background = ^{
        IosStagePreparation result;
        bool unexpected = false;
        try { result = work(); } catch (...) { unexpected = true; }
        dispatch_async(dispatch_get_main_queue(), ^{ completion(result, unexpected); });
    };
#if defined(APPLE_PREP_MUTANT_MAIN_THREAD)
    background();
#else
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), background);
#endif
    return true;
}
}
