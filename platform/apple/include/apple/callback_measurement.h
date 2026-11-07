// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "apple/frame_recording.h"
#include "apple/measurement_clock.h"
#include "apple/measurement_lifetime.h"

namespace recomp::apple::performance {
// No hooks installed here. The app may opt in before launch; the original
// callback still owns its return value. Diagnostic failure changes no delivery.
// Owner lifetime must cover the queued drain and every guest callback attempt.
class CallbackMeasurementWindow {
public:
    class Frame {
    public:
        Frame(const Frame&)=delete;
        Frame& operator=(const Frame&)=delete;
        Frame(Frame&&)=default;
        Frame& operator=(Frame&&)=delete;
        explicit operator bool() const noexcept {return bool(lease_);}
        void mainEntered(const ClockStamp& stamp) noexcept;
        void presenterReturned(const ClockStamp& stamp) noexcept;
        bool returned(const ClockStamp& stamp,bool accepted) noexcept;
    private:
        friend class CallbackMeasurementWindow;
        Frame(CallbackMeasurementWindow*,MeasurementLifetime::Lease&&,std::uint32_t,const ClockStamp&) noexcept;
        CallbackMeasurementWindow* owner_;
        MeasurementLifetime::Lease lease_;
        CallbackMeasurement value_{};
        unsigned phase_=0;
    };
    CallbackMeasurementWindow(MeasurementLifetime::Notify notify,void* context,
                              std::size_t capacity=FrameRecording::MaxCapacity):
        recording_(capacity),lifetime_(notify,context) {}
    Frame enter(std::uint32_t sequence,const ClockStamp& stamp) noexcept {
        return Frame(this,lifetime_.tryAcquire(),sequence,stamp);
    }
    bool close() noexcept {return lifetime_.close();}
    MeasurementLifetime::Status lifetimeStatus() const noexcept {return lifetime_.status();}
    RecordingStatus recordingStatus() const noexcept {return recording_.status();}
    // Serialized consumer, only after the queued drain notification. A lost
    // lease must produce an explicitly incomplete footer, never clean success.
    bool exportDrained(FILE*,const char* runId,const char* scene,unsigned fps,bool fsr) noexcept;
private:
    FrameRecording recording_;
    MeasurementLifetime lifetime_;
};
}
