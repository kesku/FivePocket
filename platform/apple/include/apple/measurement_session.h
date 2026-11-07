// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "apple/callback_measurement.h"
#include "apple/display_followup.h"
#include "apple/display_observation.h"
#include <memory>

namespace recomp::apple::performance {
// An exploratory recorder only. It makes no claim about physical display,
// CPU cores, runnable wait, exact jetsam limits or instrumentation overhead.
class MeasurementSession {
    struct Impl;
public:
    struct Status {
        bool closing=false,finished=false,outputSucceeded=false,frameRecordsClean=false;
        std::uint64_t samples=0;
    };
    using Completion = void (*)(void*,Status) noexcept; // called once on utility queue
    class Frame {
    public:
        Frame(const Frame&)=delete;
        Frame& operator=(const Frame&)=delete;
        Frame(Frame&&)=default;
        Frame& operator=(Frame&&)=delete;
        explicit operator bool() const noexcept {return bool(frame_);}
        void mainEntered() noexcept;
        void presenterReturned() noexcept;
        void returned(bool accepted) noexcept;
    private:
        friend class MeasurementSession;
        Frame(std::shared_ptr<Impl>,std::uint32_t) noexcept;
        // Destruction order keeps Impl alive until the last lease releases.
        std::shared_ptr<Impl> owner_;
        CallbackMeasurementWindow::Frame frame_;
    };
    // Takes ownership of two distinct streams on construction (also on a
    // thrown setup error). No file creation/overwrite or app/device action.
    // File buffers and all recording storage are allocated before guest start.
    MeasurementSession(FILE* frames,FILE* counters,const char* runId,const char* scene,
                       unsigned targetFps,bool fsr,Completion completed=nullptr,void* context=nullptr);
    // Optional third distinct owned stream + recorder. Existing constructor
    // and Status layout remain unchanged. Export is explicitly advisory.
    MeasurementSession(FILE* frames,FILE* counters,FILE* display,std::shared_ptr<DisplayRecording>,
                       const char* runId,const char* scene,unsigned targetFps,bool fsr,
                       Completion completed=nullptr,void* context=nullptr);
    ~MeasurementSession();
    MeasurementSession(const MeasurementSession&)=delete;
    MeasurementSession& operator=(const MeasurementSession&)=delete;
    Frame enter(std::uint32_t sequence) noexcept;
    // Nonblocking. New recordings stop; the game must still execute its normal
    // frame callback. In-flight frames and the serial sample handler drain first.
    void close() noexcept;
    void setDisplayFollowup(FILE*,std::shared_ptr<DisplayFollowupControl>);
    void cancelDisplayFollowup() noexcept;
    Status status() const noexcept;
private:
    std::shared_ptr<Impl> impl_;
};
}
