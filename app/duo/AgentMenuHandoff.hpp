// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "FullTouchInput.hpp"

namespace recomp::apple::agenthandoff {
namespace ai=agentinput;
namespace ft=fulltouch;
enum class Phase {Idle,WaitingRelease,Released,Consumed,Cancelled};
inline bool matchingRelease(const ai::Publication& event,const ai::Publication& p,
    const ai::Completion& c,std::uint64_t armedProgress,std::uint64_t currentProgress) noexcept {
    return p.available && p.token.phase==ai::PublicationPhase::Released && p.state.neutral() &&
        p.token.leaseGeneration==event.token.leaseGeneration && p.token.stage==ai::Stage::Menu &&
        p.token.stageGeneration==event.token.stageGeneration && p.token.observedGeneration==event.token.observedGeneration &&
        p.token.sequence==event.token.sequence && c.sequence==event.token.sequence &&
        c.heldGeneration==event.token.publicationGeneration && c.releaseGeneration==p.token.publicationGeneration &&
        c.releaseGeneration>c.heldGeneration && c.heldProgress>armedProgress && c.releaseProgress>c.heldProgress &&
        currentProgress>=c.releaseProgress;
}

// MAIN actor, one accepted menu launch intent. This owns no asset/preparation
// result and never starts a worker. Caller must pass the publication from the
// actual currently executing UI event, not a historical snapshot or timer.
// Completion comes from the real source's held/later-neutral consumer ACKs.
class Gate final {
public:
    explicit Gate(ft::Gamepad& source) noexcept:source_(source){}
    Gate(const Gate&)=delete;
    Gate& operator=(const Gate&)=delete;
    bool arm(const ai::Publication& event,const ai::Environment& e) noexcept {
        const auto source=source_.agentStatus();
        if(phase_!=Phase::Idle)return false;
        if(!event.available || event.token.stage!=ai::Stage::Menu || event.token.phase!=ai::PublicationPhase::Held ||
            !event.token.sequence || !event.token.publicationGeneration || !event.token.leaseGeneration ||
            !event.token.stageGeneration || !event.token.observedGeneration || event.state.neutral() ||
            source.phase!=ai::Phase::Holding || source.publication!=event || !source.active || !source.manualNeutral ||
            !e.active || e.stage!=ai::Stage::Menu || e.stageGeneration!=event.token.stageGeneration ||
            e.observedGeneration!=event.token.observedGeneration)return false;
        event_=event;owner_=source.ownerGeneration;progress_=e.progressRevision;lastNow_=e.nowMs;
        phase_=Phase::WaitingRelease;return true;
    }
    // Observe immediately after actual menu confirmation, before completion can
    // be overwritten by another command. Longer preparation does not fabricate
    // source freshness/ACKs: a captured RELEASE is an already consumed action.
    bool observe(const ai::Environment& e) noexcept {
        const auto source=source_.agentStatus();
        if(phase_!=Phase::WaitingRelease && phase_!=Phase::Released)return false;
        if(!source.active || !source.manualNeutral || source.ownerGeneration!=owner_ ||
            source.phase==ai::Phase::Closed || !e.active || e.stage!=ai::Stage::Menu ||
            e.stageGeneration!=event_.token.stageGeneration || e.nowMs<lastNow_) {
            cancel();return false;
        }
        lastNow_=e.nowMs;
        if(phase_==Phase::Released) {
            if(source.phase==ai::Phase::Holding || source.phase==ai::Phase::ReleasePending ||
                (source.publication.available && (!source.publication.state.neutral() ||
                    source.publication.token.leaseGeneration!=event_.token.leaseGeneration))) {
                cancel();return false;
            }
            return true;
        }
        if(source.phase==ai::Phase::Revoked || source.phase==ai::Phase::Unavailable) {cancel();return false;}
        if(source.phase!=ai::Phase::Ready)return false;
        const auto& p=source.publication;const auto& c=source.completion;
        if(!matchingRelease(event_,p,c,progress_,e.progressRevision)) {cancel();return false;}
        completion_=c;phase_=Phase::Released;return true;
    }
    bool take(bool preparationReady,const ai::Environment& e) noexcept {
        if(!observe(e) || !preparationReady)return false;
        phase_=Phase::Consumed;return true;
    }
    void cancel() noexcept {if(phase_!=Phase::Consumed)phase_=Phase::Cancelled;}
    Phase phase() const noexcept {return phase_;}
    ai::Completion completion() const noexcept {return completion_;}
private:
    ft::Gamepad& source_; // Borrowed, must outlive this one-shot pending intent.
    Phase phase_=Phase::Idle;
    ai::Publication event_{};
    ai::Completion completion_{};
    std::uint64_t owner_=0,progress_=0,lastNow_=0;
};
}
