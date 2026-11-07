// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace recomp::apple {

constexpr bool audioSessionQuiescent(bool running,bool stopPending,bool sessionActive) noexcept {
    return !running && !stopPending && !sessionActive;
}

// Protected by the audio registry mutex, never used on the render callback.
// AVAudioSession is process-wide: stopping ONE unit is insufficient permission
// to deactivate it. Keep this decision connected to every live unit's status.
class AudioSessionLifecycle {
public:
    bool active() const noexcept { return active_; }
    bool invalidated() const noexcept { return invalidated_; }
    bool markActive() noexcept {
        if(invalidated_)return false;
        active_=true;
        return true;
    }
    template<class StatusRange,class Deactivate>
    const char* releaseIfIdle(bool applicationActive,bool terminal,
                             const StatusRange& statuses,Deactivate deactivate) noexcept {
        if(invalidated_)return "session_deactivation_failed";
        if(!active_ || (applicationActive && !terminal))return nullptr;
        for(const auto& status:statuses) {
            if(status.attached && (status.running || status.stopPending || status.policy.pausePending))
                return nullptr;
        }
        bool released=false;
        try { released=deactivate(); } catch(...) {}
        if(!released) {
            invalidated_=true;
            return "session_deactivation_failed";
        }
        active_=false;
        return nullptr;
    }
private:
    bool active_=false,invalidated_=false;
};

} // namespace recomp::apple
