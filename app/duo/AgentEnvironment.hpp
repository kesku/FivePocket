// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AgentInputPolicy.hpp"
#include <mutex>

namespace recomp::apple::agentenvironment {
namespace ai=agentinput;
struct Limits {
    std::uint64_t stages=UINT64_MAX,progress=UINT64_MAX,observations=UINT64_MAX;
};

// Coherent, bounded provider for guest workers. This is not a transport, image
// verifier, authorization or clock implementation. The integrating Main must
// supply ONE trusted monotonic clock, its actual lifecycle, real UI/copy events
// and independently validated image metadata. Never call event methods merely
// because a timer or an ACK exists. Clock runs under the lock and must be
// bounded/non-reentrant: no UI/I/O/allocation or other lock acquisition.
class Record final {
public:
    using Clock=std::uint64_t(*)(void*) noexcept;
    Record(Clock clock,void* context,Limits limits={}) noexcept:clock_(clock),context_(context),limits_(limits){}
    Record(const Record&)=delete;
    Record& operator=(const Record&)=delete;
    bool begin(ai::Stage stage,bool active) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || failed_ || !ai::validStage(stage) || generation_>=limits_.stages)return false;
        const auto now=timeLocked();if(!now)return false;
        ++generation_;e_={};e_.stage=stage;e_.stageGeneration=generation_;e_.active=active;
        return true;
    }
    bool active(bool value) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || failed_ || !generation_)return false;
        if(!timeLocked())return false;e_.active=value;return true;
    }
    bool menuSample() noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || !usableLocked(ai::Stage::Menu))return false;
        const auto now=timeLocked();if(!now)return false;e_.sourceSampleMs=now;return true;
    }
    bool menuConsumed() noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || !usableLocked(ai::Stage::Menu) || !e_.sourceSampleMs)return false;
        if(!timeLocked() || !advanceProgressLocked())return false;return true;
    }
    // ONLY the callback after a successful final BE16 guest copy may call this.
    // Counts all normalized user0 copies, including manual/no-lease copies.
    // It is an input-consumption ordinal, NOT a frame or loading-progress count.
    bool guestCopied(std::uint32_t user,std::uint32_t output,const std::uint8_t* bytes,std::uint32_t count) noexcept {
        if(user || !output || (output&3) || !bytes || count!=16)return false;
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || !usableLocked(ai::Stage::Guest))return false;
        const auto now=timeLocked();if(!now || !advanceProgressLocked())return false;
        e_.sourceSampleMs=now;return true;
    }
    // Caller must first validate the actual image identity/stage/capture ID.
    // No timer, ordinary snapshot or input ACK can produce a validated image.
    bool image(ai::Stage stage,std::uint64_t stageGeneration,std::uint64_t captureId,
        std::uint64_t progress,std::uint64_t capturedAtMs) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || failed_ || !generation_ || !e_.active || stage!=e_.stage ||
            stageGeneration!=generation_ || !captureId || captureId<=captureId_ ||
            !progress || progress>e_.progressRevision || !capturedAtMs || capturedAtMs<e_.observedAtMs ||
            progress<e_.observedProgress || observed_>=limits_.observations)return false;
        const auto now=timeLocked();
        const auto age=capturedAtMs<=now?now-capturedAtMs:0; // No unsigned underflow; future is rejected separately.
        if(!now || capturedAtMs>now || age>ai::ObservationFreshMs)return false;
        captureId_=captureId;++observed_;e_.observedGeneration=observed_;
        e_.observedProgress=progress;e_.observedAtMs=capturedAtMs;return true;
    }
    bool read(ai::Environment& out) noexcept {
        out={};std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || failed_ || !generation_)return false;
        const auto now=timeLocked();if(!now)return false;
        out=e_;out.nowMs=now;
        // Real FullTouchInput overrides owner generation/manual neutrality.
        // Uninitialized sample/progress/observation stays zero and cannot lease.
        return true;
    }
    // MAIN shutdown only, after joining workers. Not a worker callback.
    void close() noexcept {
        std::lock_guard lock(mutex_);failed_=true;e_.active=false;
    }
private:
    bool usableLocked(ai::Stage stage) const noexcept {return !failed_ && generation_ && e_.active && e_.stage==stage;}
    std::uint64_t timeLocked() noexcept {
        const auto now=clock_?clock_(context_):0;
        if(!now || now<lastNow_){failed_=true;e_.active=false;return 0;}
        lastNow_=now;return now;
    }
    bool advanceProgressLocked() noexcept {
        if(e_.progressRevision>=limits_.progress){failed_=true;e_.active=false;return false;}
        ++e_.progressRevision;return true;
    }
    const Clock clock_;
    void* const context_;
    const Limits limits_;
    std::mutex mutex_;
    ai::Environment e_{};
    std::uint64_t generation_=0,observed_=0,captureId_=0,lastNow_=0;
    bool failed_=false;
};
}
