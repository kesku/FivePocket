#pragma once
#include <cstdint>

namespace recomp::gpu {
// Bookkeeping for ConservativeOwnedResourcesV1, NOT an independent fence or
// Xbox-hardware certificate. The executor must use fresh, command-selected
// immutable derived bindings (no cross-batch resource cache), keep the complete
// written EDRAM view, and issue the real native barriers, fences and audits.
// Zero COHER_SIZE never removes work. Only the busy BIT is modeled; other
// COHER_STATUS bits and WAIT_UNTIL readback remain unknown, not guessed.
// Methods are intentionally testable without GPU work. Calling them alone
// cannot authorize publication; the production executor calls finish only
// after its actual queue wait AND source/output audits succeed.
class NativeColorSyncProgress {
public:
    bool begin(unsigned stage) noexcept {
        if(inFlight_ || stage!=completed_ || stage>=4)return false;
        inFlight_=true;
        if(!stage){busy_=true;invalidations_=1;}
        return true;
    }
    bool finishAuditedStage(unsigned stage) noexcept {
        if(!inFlight_ || stage!=completed_ || stage>=4)return false;
        inFlight_=false;++completed_;
        if(!stage){busy_=false;depthWaits_=1;}
        if(stage==2){idleCleans_=1;invalidations_=2;}
        if(stage==3){idleCleans_=3;invalidations_=3;flushes_=1;events_=2;}
        return true;
    }
    bool depthWaitSatisfied() const noexcept{return completed_>=1 && !busy_;}
    bool complete() const noexcept{return completed_==4 && !inFlight_;}
    unsigned completedStages() const noexcept{return completed_;}
    unsigned invalidations() const noexcept{return invalidations_;}
    unsigned idleCleans() const noexcept{return idleCleans_;}
    unsigned depthWaits() const noexcept{return depthWaits_;}
    unsigned cacheFlushes() const noexcept{return flushes_;}
    unsigned literalEvents() const noexcept{return events_;}
    bool busyBit(std::uint32_t& output) const noexcept {
        if(!completed_ && !inFlight_)return false;
        output=busy_?0x80000000:0;return true;
    }
private:
    unsigned completed_=0,invalidations_=0,idleCleans_=0,depthWaits_=0,flushes_=0,events_=0;
    bool inFlight_=false,busy_=false;
};
}
