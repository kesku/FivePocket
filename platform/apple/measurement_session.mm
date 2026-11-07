// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/measurement_session.h"
#include "apple/counter_recording.h"
#include "apple/display_recording_export.h"
#include <dispatch/dispatch.h>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>

namespace recomp::apple::performance {
namespace {
struct CloseFile {void operator()(FILE* file) const noexcept {if(file)std::fclose(file);}};
using File=std::unique_ptr<FILE,CloseFile>;
bool sceneValid(const char* scene) noexcept {
    return scene && (!std::strcmp(scene,"house") || !std::strcmp(scene,"outdoor") ||
        !std::strcmp(scene,"driving") || !std::strcmp(scene,"prologue") || !std::strcmp(scene,"other"));
}
}
struct MeasurementSession::Impl:std::enable_shared_from_this<Impl> {
    File frames,counters,display;
    std::shared_ptr<DisplayRecording> displayRecording;
    std::shared_ptr<display_followup::Observer> displayFollowup;
    std::string runId,scene;
    unsigned fps;
    bool fsr;
    Completion completed;
    void* completionContext;
    MeasurementClock clock;
    CallbackMeasurementWindow window;
    Sampler sampler;
    CounterRecording counter;
    dispatch_queue_t queue;
    dispatch_source_t timer=nullptr;
    // Queue-confined flags; atomics below are the public cross-thread snapshot.
    bool framesDrained=false,countersDrained=false;
    std::atomic<bool> closing{false},finished{false},outputSucceeded{false},frameRecordsClean{false};
    std::atomic<std::uint64_t> sampleCount{0};
    Impl(File frameFile,File counterFile,File displayFile,std::shared_ptr<DisplayRecording> recording,
         const char* id,const char* sceneName,unsigned target,bool enabled,Completion done,void* context):
        frames(std::move(frameFile)),counters(std::move(counterFile)),display(std::move(displayFile)),
        displayRecording(std::move(recording)),runId(id),scene(sceneName),fps(target),fsr(enabled),completed(done),completionContext(context),
        window(&Impl::onFramesDrained,this),counter(counters.get(),id),
        queue(dispatch_queue_create("dev.recomp.measurement",dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL,QOS_CLASS_UTILITY,0))) {}
    ~Impl() {if(timer)dispatch_source_cancel(timer);}
    static void onFramesDrained(void* context) noexcept {
        auto self=static_cast<Impl*>(context)->shared_from_this();
        dispatch_async(self->queue, ^{self->framesDrained=true;self->finishIfDrained();});
    }
    void begin() {
        auto self=shared_from_this();
        dispatch_async(queue, ^{
            if(self->closing.load()) {self->countersDrained=true;self->finishIfDrained();return;}
            self->timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,self->queue);
            if(!self->timer) {self->countersDrained=true;self->requestClose();return;}
            std::weak_ptr<Impl> weak=self;
            dispatch_source_set_event_handler(self->timer, ^{
                if(auto owned=weak.lock();owned && !owned->closing.load()) {
                    const bool stored=owned->counter.append(owned->sampler.sample());
                    owned->sampleCount=owned->counter.status().stored;
                    if(!stored || owned->sampleCount==CounterRecording::MaxSamples)owned->requestClose();
                }
            });
            dispatch_source_set_cancel_handler(self->timer, ^{
                // Keep the owner alive until cancellation, then break the
                // source/block/owner cycle before releasing the closed files.
                self->timer=nullptr;self->countersDrained=true;self->finishIfDrained();
            });
            dispatch_source_set_timer(self->timer,dispatch_time(DISPATCH_TIME_NOW,0),NSEC_PER_SEC,10*NSEC_PER_MSEC);
            dispatch_resume(self->timer);
        });
    }
    void requestClose() noexcept {
        if(closing.exchange(true))return;
        if(displayRecording)displayRecording->close();
        window.close();
        auto self=shared_from_this();
        dispatch_async(queue, ^{
            if(self->timer)dispatch_source_cancel(self->timer);
            else {self->countersDrained=true;self->finishIfDrained();}
        });
    }
    void finishIfDrained() noexcept {
#if defined(APPLE_MEASUREMENT_SESSION_MUTANT_EARLY_FINISH)
        if(!countersDrained || finished.load())return;
#else
        if(!framesDrained || !countersDrained || finished.load())return;
#endif
        const bool a=window.exportDrained(frames.get(),runId.c_str(),scene.c_str(),fps,fsr);
        const bool b=counter.finish();
        const auto frameStatus=window.recordingStatus();
        const bool closedFrames=std::fclose(frames.release())==0;
        const bool closedCounters=std::fclose(counters.release())==0;
        // Advisory only: physical callbacks may still be in flight. Never
        // wait for UIKit or infer whole-game physical/frame-clock coverage.
        DisplaySnapshot originalDisplay;
        bool originalDisplayCaptured=false;
        bool displayWritten=!display;
        try {if(display) {originalDisplay=displayRecording->snapshot();originalDisplayCaptured=true;
            displayWritten=exportDisplaySnapshot(display.get(),runId.c_str(),originalDisplay);}}
        catch(...) {displayWritten=false;} // preserve failed output, still close every stream
        const bool displayClosed=!display || std::fclose(display.release())==0;
        frameRecordsClean=a && !frameStatus.loss && !frameStatus.invalid;
        outputSucceeded=a && b && closedFrames && closedCounters && displayWritten && displayClosed;
        finished.store(true,std::memory_order_release);
        if(completed)completed(completionContext,{true,true,outputSucceeded.load(),frameRecordsClean.load(),sampleCount.load()});
        // Independent follow-up: original export and native END are already complete.
        if(displayFollowup && originalDisplayCaptured)displayFollowup->begin(originalDisplay);
    }
};
MeasurementSession::MeasurementSession(FILE* frames,FILE* counters,const char* runId,const char* scene,unsigned fps,bool fsr,Completion done,void* context):
    MeasurementSession(frames,counters,nullptr,{},runId,scene,fps,fsr,done,context) {}
MeasurementSession::MeasurementSession(FILE* frames,FILE* counters,FILE* display,std::shared_ptr<DisplayRecording> recording,
                                     const char* runId,const char* scene,unsigned fps,bool fsr,Completion done,void* context) {
    // Establish RAII before allocation/validation, including the duplicate
    // pointer failure case (one fclose, not two).
    File frameOwner(frames),counterOwner(counters==frames?nullptr:counters),
         displayOwner(display==frames || display==counters?nullptr:display);
    if(!frames || !counters || frames==counters || (display && (display==frames || display==counters)) ||
       bool(display)!=bool(recording) || !runId || !sceneValid(scene) || (fps!=30 && fps!=60))
        throw std::invalid_argument("distinct owned streams and explicit scene/FPS required");
    impl_=std::make_shared<Impl>(std::move(frameOwner),std::move(counterOwner),std::move(displayOwner),
        std::move(recording),runId,scene,fps,fsr,done,context);
    impl_->begin();
}
MeasurementSession::~MeasurementSession() {cancelDisplayFollowup();impl_->requestClose();}
MeasurementSession::Frame::Frame(std::shared_ptr<Impl> owner,std::uint32_t sequence) noexcept:
    owner_(std::move(owner)),frame_(owner_->window.enter(sequence,owner_->clock.read())) {}
void MeasurementSession::Frame::mainEntered() noexcept {if(frame_)frame_.mainEntered(owner_->clock.read());}
void MeasurementSession::Frame::presenterReturned() noexcept {if(frame_)frame_.presenterReturned(owner_->clock.read());}
void MeasurementSession::Frame::returned(bool accepted) noexcept {if(frame_)frame_.returned(owner_->clock.read(),accepted);}
MeasurementSession::Frame MeasurementSession::enter(std::uint32_t sequence) noexcept {return Frame(impl_,sequence);}
void MeasurementSession::close() noexcept {impl_->requestClose();}
MeasurementSession::Status MeasurementSession::status() const noexcept {
    return {impl_->closing.load(),impl_->finished.load(std::memory_order_acquire),impl_->outputSucceeded.load(),
        impl_->frameRecordsClean.load(),impl_->sampleCount.load()};
}
}

namespace recomp::apple::performance {
void MeasurementSession::setDisplayFollowup(FILE* stream,std::shared_ptr<DisplayFollowupControl> control) {
    display_followup::File owned(stream);
    if(!stream || !control || !impl_->displayRecording || impl_->closing.load() || impl_->displayFollowup)
        throw std::invalid_argument("followup setup requires one pre-start stream and recorder");
    impl_->displayFollowup=std::make_shared<display_followup::Observer>(std::move(owned),impl_->displayRecording,std::move(control),impl_->runId);
}
void MeasurementSession::cancelDisplayFollowup() noexcept {
    if(impl_->displayFollowup)impl_->displayFollowup->cancel();
}
}
