// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace recomp::apple::performance {
// Apple's drawable host-time clock, NOT Mach continuous time, guest time,
// callback duration, GPU execution time, or input-to-photon latency.
struct DisplaySample {
    std::uint64_t ordinal=0,drawableId=0;
    double presentedHostSeconds=0;
};
struct DisplaySnapshot {
    bool closed=false,overflow=false,duplicateCallback=false,invalidTime=false,invalidIdentity=false;
    std::uint64_t acquired=0,completed=0,rejectedRegistrations=0,layerEpoch=0;
    std::vector<DisplaySample> samples;
    // Advisory snapshot of registered protocol records only. A later duplicate
    // callback can invalidate a later snapshot. Not whole-game/display proof.
    bool deliveryComplete() const noexcept {
        return closed && acquired && completed==acquired && samples.size()==acquired &&
            !rejectedRegistrations && !overflow && !duplicateCallback && !invalidTime && !invalidIdentity;
    }
};

// Allocate before guest start. Admission/close share one atomic state so closing
// cannot falsely certify completion while a new drawable is being admitted.
// Callbacks only fill preallocated slots; snapshot allocates outside callbacks.
class DisplayRecording {
public:
    explicit DisplayRecording(std::uint32_t capacity);
    ~DisplayRecording();
    DisplayRecording(const DisplayRecording&)=delete;
    DisplayRecording& operator=(const DisplayRecording&)=delete;
    bool acquire(std::uint64_t drawableId,std::uint32_t& slot) noexcept;
    void presented(std::uint32_t slot,std::uint64_t drawableId,double hostSeconds) noexcept;
    void registrationRejected() noexcept;
    bool bindLayerEpoch(std::uint64_t epoch) noexcept;
    void close() noexcept;
    DisplaySnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Opaque Objective-C boundary. A null recording registers NO handler. The
// handler owns the recording, not the drawable/layer/raw owner. It may safely
// finish after the caller releases its owner and never waits/logs/writes files.
bool observeDisplayDrawable(void* drawable,const std::shared_ptr<DisplayRecording>&) noexcept;

// A future, explicit presenter overlay can select this CAMetalLayer subclass.
// No method swizzling, proxy drawable or changes to present/scheduling/pixels.
// This module alone does not change any existing presenter or build.
void* observedMetalLayerClass() noexcept;
bool configureDisplayLayer(void* layer,std::shared_ptr<DisplayRecording>) noexcept;
}
