// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace recomp::apple {

struct AudioLifecyclePolicySnapshot {
    bool applicationActive{true};
    bool interrupted{false};
    bool resumePermitted{true};
    bool terminal{false};
    bool pausePending{false};
    std::uint64_t generation{0};
    bool mayRun() const noexcept {
        return applicationActive && !interrupted && resumePermitted && !terminal && !pausePending;
    }
};

// Notification callbacks change intent only. In particular, these methods do
// not take the AudioUnit lifetime mutex: AudioUnit start/stop can synchronously
// deliver a notification while that mutex is held by the control thread.
class AudioLifecyclePolicy final {
    static constexpr std::uint64_t Active=1, Interrupted=2, ResumePermitted=4, Terminal=8, NeedsStop=16, Flags=31;
    std::atomic<std::uint64_t> bits_{Active|ResumePermitted};
    static std::uint64_t next(std::uint64_t before,std::uint64_t flags) noexcept {
        return ((before&~Flags)+32)|(flags&Flags);
    }
public:
    void setApplicationActive(bool active) noexcept {
        auto before=bits_.load(std::memory_order_acquire);
        for(;;) {
            const auto flags=active?(before|Active):((before&~Active)|NeedsStop);
            if((flags&Flags)==(before&Flags))return;
            if(bits_.compare_exchange_weak(before,next(before,flags),std::memory_order_acq_rel))return;
        }
    }
    void interruptionBegan() noexcept {
        auto before=bits_.load(std::memory_order_acquire);
        while(!bits_.compare_exchange_weak(before,next(before,(before|Interrupted|NeedsStop)&~ResumePermitted),
                                           std::memory_order_acq_rel)) {}
    }
    void interruptionEnded(bool shouldResume) noexcept {
        auto before=bits_.load(std::memory_order_acquire);
        for(;;) {
            // A duplicate or unsolicited Ended cannot grant new permission.
            if(!(before&Interrupted))return;
            auto after=before&~Interrupted;
            if(shouldResume)after|=ResumePermitted;
            else after&=~ResumePermitted;
            if(bits_.compare_exchange_weak(before,next(before,after),std::memory_order_acq_rel))return;
        }
    }
    void terminate() noexcept {
        auto before=bits_.load(std::memory_order_acquire);
        while(!(before&Terminal) && !bits_.compare_exchange_weak(before,next(before,before|Terminal|NeedsStop),
                                                               std::memory_order_acq_rel)) {}
    }
    // A fast Began->Ended or inactive->active pair still requires one real
    // stop before resume. A notification that races the stop is not consumed
    // by an acknowledgement for an older generation.
    void acknowledgeStopped(std::uint64_t generation) noexcept {
        auto before=bits_.load(std::memory_order_acquire);
        while((before&NeedsStop) && (before>>5)==generation) {
            if(bits_.compare_exchange_weak(before,next(before,before&~NeedsStop),std::memory_order_acq_rel))return;
        }
    }
    AudioLifecyclePolicySnapshot snapshot() const noexcept {
        const auto bits=bits_.load(std::memory_order_acquire);
        return {bool(bits&Active),bool(bits&Interrupted),bool(bits&ResumePermitted),bool(bits&Terminal),
                bool(bits&NeedsStop),bits>>5};
    }
};

} // namespace recomp::apple
