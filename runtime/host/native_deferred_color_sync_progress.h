#pragma once
#include <cstdint>

namespace recomp::gpu {
// Prefix-only bookkeeping for the explicit fresh-owned-resources policy, NOT
// an independent fence certificate or publication receipt. Leading completion
// is recorded only after a new queue fence; stage completion only after the
// actual fence AND source/readback audits. No absent post-copy waits, third
// invalidation or literal color events may be borrowed from the old batch.
class NativeDeferredColorSyncProgress final {
public:
    bool finishAuditedLeadingFence() noexcept {
        if(leading_ || inFlight_ || completed_)return false;
        leading_=true;return true;
    }
    bool readyForStages() const noexcept{return leading_ && !inFlight_ && !completed_;}
    bool begin(unsigned stage) noexcept {
        if(!leading_ || inFlight_ || stage!=completed_ || stage>=4)return false;
        inFlight_=true;
        if(!stage){busy_=true;invalidations_=1;}
        return true;
    }
    bool finishAuditedStage(unsigned stage) noexcept {
        if(!leading_ || !inFlight_ || stage!=completed_ || stage>=4)return false;
        inFlight_=false;++completed_;
        if(!stage){busy_=false;depthWaits_=1;}
        if(stage==2){copyIdleCleans_=1;invalidations_=2;}
        if(stage==3)flushes_=1;
        return true;
    }
    bool depthWaitSatisfied() const noexcept{return completed_>=1 && !busy_;}
    bool complete() const noexcept{return leading_ && completed_==4 && !inFlight_;}
    unsigned leadingFences() const noexcept{return leading_?1:0;}
    unsigned completedStages() const noexcept{return completed_;}
    unsigned invalidations() const noexcept{return invalidations_;}
    unsigned copyIdleCleans() const noexcept{return copyIdleCleans_;}
    unsigned depthWaits() const noexcept{return depthWaits_;}
    unsigned cacheFlushes() const noexcept{return flushes_;}
    unsigned literalEvents() const noexcept{return 0;}
    bool busyBit(std::uint32_t& output) const noexcept {
        if(!completed_ && !inFlight_)return false;
        output=busy_?0x80000000:0;return true;
    }
private:
    unsigned completed_=0,invalidations_=0,copyIdleCleans_=0,depthWaits_=0,flushes_=0;
    bool leading_=false,inFlight_=false,busy_=false;
};
}
