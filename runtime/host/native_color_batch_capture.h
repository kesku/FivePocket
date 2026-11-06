#pragma once
#include "native_color_chain_capture.h"
#include "native_color_chain_sync.h"

namespace recomp::gpu {
// One owned frontend snapshot, NOT a live execution/retirement certificate.
// Binds the command-order plan, raw source copies, known geometry/state requests,
// copy/events, pending sync requests and memory exclusions to the same input.
// All payloads stay in owned storage; const views are valid only while the owner
// remains alive and unmoved. The caller must separately acquire/retain lifecycle
// ownership, park every guest writer and discharge backend semantic obligations.
class NativeColorBatchCapture {
public:
    NativeColorBatchCapture(const NativeColorBatchCapture&)=delete;
    NativeColorBatchCapture& operator=(const NativeColorBatchCapture&)=delete;
    NativeColorBatchCapture(NativeColorBatchCapture&&) noexcept=default;
    NativeColorBatchCapture& operator=(NativeColorBatchCapture&&) noexcept=default;
    static std::optional<NativeColorBatchCapture> create(const Pm4Snapshot& child,const DrawBinState& initial,
            const NativeColorChainProgram& program,const NativeColorMemoryControls& controls,const Pm4PhysicalReader& reader,
            NativeColorBatchPhase phase=NativeColorBatchPhase::Startup) noexcept {
        // Reject malformed roots/control aliases before any external source read.
        if(!reader || (phase!=NativeColorBatchPhase::Startup && phase!=NativeColorBatchPhase::PostOutput) ||
            program.coordinates()!=NativeColorCoordinateProfile::Padded1280W1 ||
            !validNativeColorMemoryControls(controls,child.words().size()))return std::nullopt;
        try {
            NativeColorBatchCapture candidate;candidate.controls_=controls;candidate.initialBins_=initial;DrawCommandError error;
            if(!planDrawCommandCandidates(child,initial,reader,candidate.plan_,error) || candidate.plan_.draws.size()!=4 ||
                !planNativeColorChainFinalEvents(candidate.plan_,candidate.events_) ||
                !planNativeColorChainSync(candidate.plan_,candidate.sync_,phase) ||
                !planNativeColorCopyRequest(candidate.plan_,3,reader,candidate.copy_))return std::nullopt;
            candidate.color_=NativeColorChainCapture::create(candidate.plan_,{0,1,2},program,reader);
            if(!candidate.color_ || !planNativeColorMemory(candidate.color_->sourceRanges(),child.words().size(),
                candidate.copy_,candidate.events_,controls,candidate.memory_))return std::nullopt;
            return candidate;
        } catch (...) {return std::nullopt;}
    }
    const DrawCommandPlan& commands() const noexcept{return plan_;}
    const NativeColorChainCapture& color() const noexcept{return *color_;}
    const NativeColorCopyRequest& copy() const noexcept{return copy_;}
    const NativeColorChainEventRequests& events() const noexcept{return events_;}
    const NativeColorChainSyncRequests& synchronization() const noexcept{return sync_;}
    const NativeColorMemoryRequest& memory() const noexcept{return memory_;}
    const NativeColorMemoryControls& controls() const noexcept{return controls_;}
    const DrawBinState& initialBins() const noexcept{return initialBins_;}
private:
    NativeColorBatchCapture()=default;
    DrawCommandPlan plan_;
    std::optional<NativeColorChainCapture> color_;
    NativeColorCopyRequest copy_;
    NativeColorChainEventRequests events_;
    NativeColorChainSyncRequests sync_;
    NativeColorMemoryRequest memory_;
    NativeColorMemoryControls controls_;
    DrawBinState initialBins_;
};
}
