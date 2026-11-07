// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "apple/audio_route_policy.h"
#include "apple/audio_lifecycle_policy.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace recomp::apple {

// (Claude, 2026-10-20) rejections that describe a route in transition rather than a broken one (see validateLocked)
inline bool isTransientRouteRejection(const char* rejection) noexcept {
    if(!rejection)return false;
    const char* t="missing_output";
    while(*rejection && *rejection==*t){++rejection;++t;}
    return *rejection==*t;
}

struct AudioRouteObservation {
    AudioRouteSnapshot snapshot;
    const char* rejection{nullptr};
    bool invalidated{false};
};

struct AudioRouteLifecycleStatus {
    AudioLifecyclePolicySnapshot policy;
    bool attached{false};
    bool startRequested{false};
    bool running{false};
    bool stopPending{false};
    bool invalidated{false};
    bool sessionRefused{false}; // (session-refused) the last activation was refused; retried, never fatal
};

// (Claude 2026-11-05, session-refused) iOS may refuse setActive:YES (a phone call, Siri, another app's non-mixable audio):
// an expected answer, not a broken unit. Until a retry is granted no unit runs and the title's audio clock is kept in
// real time by the stall drain (silent play), as during an interruption (c408). Before this, a refusal at open failed
// trace setup (the game closed at launch) and a refusal on resume invalidated the stream for the rest of the process.
// Whether the title's audio clock should advance: a healthy running unit, or a pause the app cannot end itself
// (interruption or refused session) while it stays in the foreground.
inline bool audioClockShouldRun(const AudioRouteLifecycleStatus& s) noexcept {
    if(s.invalidated)return false;
    if(s.running && !s.stopPending && s.policy.mayRun())return true;
    return s.policy.applicationActive && !s.policy.terminal && (s.policy.interrupted || s.sessionRefused);
}
// Whether a refused session is due another activation attempt (the owner schedules one at a time, 2 s apart).
inline bool sessionRetryDue(const AudioRouteLifecycleStatus& s) noexcept {
    return s.sessionRefused && s.attached && !s.invalidated && s.policy.mayRun();
}

struct AudioRouteTransition {
    bool attached{false};
    bool started{false};
    bool stopped{false};
    const char* rejection{nullptr};
    std::optional<AudioRouteObservation> route;
};

// Shared by the output-unit owner and queued control-thread validations. This
// object does not own Unit. The owner supplies its disposal operation to
// detach(), so an in-flight query finishes before disposal and later queries
// cannot obtain the stale handle. The route stays attached until disposal
// completes, preventing session release while the unit is still running.
// Never call from the realtime render callback.
template<class Unit>
class AudioRouteState {
public:
    AudioRouteState(Unit unit, std::uint32_t burst, std::uint32_t capacity,
                    std::shared_ptr<std::atomic<bool>> invalidated)
        : unit_(unit), burst_(burst), capacity_(capacity),
          invalidated_(invalidated ? std::move(invalidated)
                                   : std::make_shared<std::atomic<bool>>(true)) {}
    AudioRouteState(const AudioRouteState&) = delete;
    AudioRouteState& operator=(const AudioRouteState&) = delete;

    template<class Query>
    std::optional<AudioRouteObservation> validate(Query query) noexcept {
        std::lock_guard lock(mutex_);
        if (unit_ == Unit{}) return std::nullopt;
        // A notification during a temporary pause is not a stable route
        // observation. Resume always performs its own stopped-unit validation.
        if (!lifecycle_.snapshot().mayRun()) return std::nullopt;
        return validateLocked(query);
    }

    template<class Start>
    bool start(Start operation) noexcept {
        std::lock_guard lock(mutex_);
        if (unit_ == Unit{} || startRequested_.load(std::memory_order_acquire) ||
            lifecycle_.snapshot().terminal ||
            invalidated_->load(std::memory_order_acquire)) return false;
        startRequested_.store(true,std::memory_order_release);
        // A start request made during a deliberate pause is retained, not a
        // backend failure. Reconciliation starts the same unit when permitted.
        const auto beforeStart=lifecycle_.snapshot();
        if (!beforeStart.mayRun()) return true;
        // (session-refused) retained like a pause: a granted retry's reconciliation starts the unit.
        if (sessionRefused_.load(std::memory_order_acquire)) return true;
        stopPending_.store(true,std::memory_order_release);
        bool started = false;
        try { started = operation(unit_); } catch (...) { started = false; }
        if (!started) {
            // The failed call can be a consequence of a new interruption, not
            // a permanent backend failure. Its side effects are still unknown:
            // preserve a required Stop before a later authorized restart.
            if(beforeStart.generation!=lifecycle_.snapshot().generation &&
               !invalidated_->load(std::memory_order_acquire))return true;
            invalidated_->store(true, std::memory_order_release);
            return false;
        }
        // Publish the unit's actual state while still excluding validation.
        // A concurrent external interruption remains sticky; never report a
        // healthy start merely because the underlying operation succeeded.
        expectedRunning_ = true;
        running_.store(true,std::memory_order_release);
        stopPending_.store(false,std::memory_order_release);
        return !invalidated_->load(std::memory_order_acquire);
    }

    void setApplicationActive(bool active) noexcept { lifecycle_.setApplicationActive(active); }
    void interruptionBegan() noexcept { lifecycle_.interruptionBegan(); }
    void interruptionEnded(bool shouldResume) noexcept { lifecycle_.interruptionEnded(shouldResume); }
    void terminate() noexcept { lifecycle_.terminate(); }
    // (session-refused) the owner's activation at open was refused (no unit started yet).
    void noteSessionRefused() noexcept { sessionRefused_.store(true,std::memory_order_release); }
    // (session-refused) one scheduled retry at a time: claim before scheduling, release when it has run.
    bool claimSessionRetry() noexcept { return !retryScheduled_.exchange(true,std::memory_order_acq_rel); }
    void releaseSessionRetry() noexcept { retryScheduled_.store(false,std::memory_order_release); }

    AudioRouteLifecycleStatus lifecycleStatus() const noexcept {
        return {lifecycle_.snapshot(),attached_.load(std::memory_order_acquire),
                startRequested_.load(std::memory_order_acquire),running_.load(std::memory_order_acquire),
                stopPending_.load(std::memory_order_acquire),
                invalidated_->load(std::memory_order_acquire),
                sessionRefused_.load(std::memory_order_acquire)};
    }

    // All supplied operations run on the serialized control path with a valid
    // unit. Notification callbacks may change atomic intent during an operation
    // but must never synchronously call reconcile/validate/detach themselves.
    template<class Activate, class Query, class Start, class Stop>
    AudioRouteTransition reconcile(Activate activate, Query query, Start start, Stop stop) noexcept {
        std::lock_guard lock(mutex_);
        AudioRouteTransition result;
        if (unit_ == Unit{}) return result;
        result.attached=true;
        const auto mustStop=[&] {
            return !lifecycle_.snapshot().mayRun() || invalidated_->load(std::memory_order_acquire) ||
                   stopPending_.load(std::memory_order_acquire);
        };
        const auto stopRunning=[&] {
            if(!expectedRunning_ && !stopPending_.load(std::memory_order_acquire))return;
            bool stopped=false;
            try { stopped=stop(unit_); } catch(...) {}
            if(!stopped) {
                invalidated_->store(true,std::memory_order_release);
                result.rejection="stop_failed";
                return;
            }
            expectedRunning_=false;
            running_.store(false,std::memory_order_release);
            stopPending_.store(false,std::memory_order_release);
            result.stopped=true;
        };
        if(mustStop()) {
            const auto pending=lifecycle_.snapshot();
            stopRunning();
            if(!expectedRunning_ && !invalidated_->load(std::memory_order_acquire))
                lifecycle_.acknowledgeStopped(pending.generation);
            if(mustStop())return result;
        }
        if(expectedRunning_ || !startRequested_.load(std::memory_order_acquire))return result;
        const auto beforeActivation=lifecycle_.snapshot();
        if(!beforeActivation.mayRun())return result;
        bool activated=false;
        try { activated=activate(); } catch(...) {}
        if(!activated) {
            if(beforeActivation.generation!=lifecycle_.snapshot().generation &&
               !invalidated_->load(std::memory_order_acquire))return result;
            // (session-refused) the OS's answer, not a broken unit: nothing ran, nothing is invalidated; the owner retries.
            sessionRefused_.store(true,std::memory_order_release);
            result.rejection="session_refused";
            return result;
        }
        sessionRefused_.store(false,std::memory_order_release);
        // Activation can deliver another interruption/background notification.
        if(mustStop())return result;
        result.route=validateLocked(query);
        if(!result.route)return result; // intent changed during the route query
        if(result.route->rejection) {
            result.rejection=result.route->rejection;
            return result;
        }
        if(mustStop())return result;
        const auto beforeStart=lifecycle_.snapshot();
        if(!beforeStart.mayRun())return result;
        stopPending_.store(true,std::memory_order_release);
        bool started=false;
        try { started=start(unit_); } catch(...) {}
        if(!started) {
            if(beforeStart.generation!=lifecycle_.snapshot().generation &&
               !invalidated_->load(std::memory_order_acquire))return result;
            invalidated_->store(true,std::memory_order_release);
            result.rejection="start_failed";
            return result;
        }
        expectedRunning_=true;
        running_.store(true,std::memory_order_release);
        stopPending_.store(false,std::memory_order_release);
        result.started=true;
        // A notification emitted by start must not leave a knowingly unwanted
        // unit running until the next queued control task.
        if(mustStop())stopRunning();
        return result;
    }

    template<class Dispose>
    void detach(Dispose dispose) noexcept {
        std::lock_guard lock(mutex_);
        if (unit_ == Unit{}) return;
        lifecycle_.terminate();
        // Keep the terminal pause pending and the route attached throughout
        // native teardown. Session-idle queries do not take this mutex and
        // must not treat a unit awaiting Stop/Dispose as already quiescent.
        dispose(unit_);
        unit_ = Unit{};
        attached_.store(false,std::memory_order_release);
        running_.store(false,std::memory_order_release);
        stopPending_.store(false,std::memory_order_release);
    }

    // For callers without native teardown. Owners of a running native unit
    // must use the disposal overload to preserve the session-idle contract.
    void detach() noexcept { detach([](Unit) noexcept {}); }

private:
    template<class Query>
    std::optional<AudioRouteObservation> validateLocked(Query& query) noexcept {
        const auto before=lifecycle_.snapshot();
        AudioRouteSnapshot snapshot;
        try { snapshot=query(unit_,expectedRunning_); }
        catch(...) { snapshot.querySucceeded=false; }
        const auto after=lifecycle_.snapshot();
        if(!after.mayRun() || before.generation!=after.generation)return std::nullopt;
        const char* rejection=audioRouteRejection(snapshot,burst_,capacity_);
        // (Claude, 2026-10-20) "missing_output" is the transient state iOS reports while it switches devices (headphones,
        // Bluetooth: c280 saw route_reason=1 then output=none, then Speaker again). It is not sticky: the unit is left as it is
        // and the next route notification validates the new route. Every other rejection still invalidates permanently.
        if(rejection && !isTransientRouteRejection(rejection))invalidated_->store(true,std::memory_order_release);
        return AudioRouteObservation{snapshot,rejection,invalidated_->load(std::memory_order_acquire)};
    }
    std::mutex mutex_;
    Unit unit_;
    const std::uint32_t burst_, capacity_;
    const std::shared_ptr<std::atomic<bool>> invalidated_;
    AudioLifecyclePolicy lifecycle_;
    std::atomic<bool> attached_{true},startRequested_{false},running_{false},stopPending_{false};
    std::atomic<bool> sessionRefused_{false},retryScheduled_{false}; // (session-refused)
    bool expectedRunning_{false};
};

} // namespace recomp::apple
