// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#import <UIKit/UIKit.h>
#include "apple/audio_output.h"
#include "apple/audio_session_lifecycle.h"
#include "apple/ios_frame_presenter.h"
#include "apple/ios_gpu_lifecycle.h"
#include <cstdio>
#include <functional>

namespace recomp::apple {

// A retained, single-scene owner. All methods except the explicitly dispatched
// GPU drain run on main. Destruction is not part of a suspend/resume transition.
// This coordinates GPU admission and the audio unit; it is NOT a whole guest
// CPU/timer pause and does not make the app immune to iOS memory termination.
class PortLifecycle final {
public:
    using ReleaseInputs=std::function<void()>;
    using Failure=std::function<void(const char*)>;
    PortLifecycle(IosFramePresenter& presenter,ReleaseInputs release,Failure failed,ReleaseInputs resumed={})
        : presenter_(presenter),release_(std::move(release)),failed_(std::move(failed)),resumed_(std::move(resumed)) {}

    void start() {
        requireMain();started_=true;
        timer_=[NSTimer timerWithTimeInterval:0.05 repeats:YES block:^(NSTimer*) { retryResume(); }];
        [NSRunLoop.mainRunLoop addTimer:timer_ forMode:NSRunLoopCommonModes];
    }
    void setActive(bool active) {
        requireMain();
        if(terminal_ || !started_)return;
        active_=active;
        recomp::audio::setAppleAudioApplicationActive(active);
        if(active) {retryResume();return;}
        release_();
        if(!presenter_.suspend()) {fail("presenter_suspend");return;}
        const bool begin=iosGpuLifecycle().requestPause();
        std::fputs("APPLE_IOS_LIFECYCLE_PAUSE_REQUESTED gpu_drained=0 guest_cpu_paused=0\n",stderr);
        if(!begin)return; // One drain owns this closed epoch.
        drainPending_=true;
        const auto backgroundGeneration=++backgroundGeneration_;
        backgroundTask_=[UIApplication.sharedApplication beginBackgroundTaskWithName:@"Finish admitted GPU work" expirationHandler:^{
            // This is an actual OS deadline, not permission to resume in the
            // background. Preserve the closed gate and record a terminal stop.
            if(backgroundGeneration_==backgroundGeneration && backgroundTask_!=UIBackgroundTaskInvalid)
                fail("background_drain_expired");
        }];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
            const bool drained=iosGpuLifecycle().drain();
            dispatch_async(dispatch_get_main_queue(), ^{
                drainPending_=false;
                if(terminal_)return;
                if(!drained) {fail("gpu_drain");return;}
                const auto gpu=iosGpuLifecycle().snapshot();
                const auto audio=recomp::audio::appleAudioLifecycleStatus();
                std::fprintf(stderr,"APPLE_IOS_LIFECYCLE_GPU_DRAINED queues=%zu admitted=%zu audio_stopped=%u guest_cpu_paused=0\n",
                    gpu.registeredQueues,gpu.admittedOperations,unsigned(!audio.running && !audio.stopPending));
                retryResume();
            });
        });
    }
    void terminate() {
        requireMain();
        if(terminal_)return;
        terminal_=true;
        endBackgroundTask();
        [timer_ invalidate];timer_=nil;
        iosGpuLifecycle().stop();
        recomp::audio::terminateAppleAudioLifecycle();
    }
    bool running() const {
        requireMain();
        return started_ && active_ && !terminal_ &&
            iosGpuLifecycle().snapshot().phase==IosGpuLifecycle::Phase::Running;
    }
private:
    void endBackgroundTask() {
        if(backgroundTask_==UIBackgroundTaskInvalid)return;
        const auto task=backgroundTask_;backgroundTask_=UIBackgroundTaskInvalid;++backgroundGeneration_;
        [UIApplication.sharedApplication endBackgroundTask:task];
    }
    static void requireMain() {
        if(!pthread_main_np()) {std::fputs("APPLE_IOS_LIFECYCLE_FATAL non_main=1\n",stderr);std::_Exit(4);}
    }
    void fail(const char* reason) {
        terminate();
        std::fprintf(stderr,"APPLE_IOS_LIFECYCLE_FAILED reason=%s\n",reason);
        failed_(reason);
    }
    void retryResume() {
        requireMain();
        if(!started_ || terminal_ || drainPending_)return;
        const auto gpu=iosGpuLifecycle().snapshot();
        if(gpu.phase!=IosGpuLifecycle::Phase::Drained)return;
        const auto audio=recomp::audio::appleAudioLifecycleStatus();
        if(audio.invalidated || audio.terminal) {fail("audio_invalidated");return;}
        // A GPU drain can beat the serialized audio stop. Keep the finite OS
        // background allowance until both the unit and its nonmixable session
        // are quiescent; otherwise suspension can deliver a delayed Began on
        // return, with no corresponding Ended. Foreground needs no allowance.
        if(backgroundTask_!=UIBackgroundTaskInvalid &&
           (active_ || audioSessionQuiescent(audio.running,audio.stopPending,audio.sessionActive))) {
            if(!active_)std::fputs("APPLE_IOS_LIFECYCLE_QUIESCENT gpu_drained=1 audio_stopped=1 audio_session_inactive=1\n",stderr);
            endBackgroundTask();
        }
        if(!active_)return;
        if(!presenter_.healthy()) {fail("presenter_invalidated");return;}
        // Audio activation is serialized by its control queue. Never clear its
        // invalidation or override an interruption's missing resume permission.
        if(audio.available && (audio.stopPending || audio.pauseRequested || audio.interrupted || !audio.resumePermitted))return;
        if(!presenter_.resume()) {
            // Only temporary scene/view unavailability may wait. A permanent
            // contract failure must release the parked producer via stop().
            if(!presenter_.healthy())fail("presenter_resume_invalid");
            return;
        }
        if(!iosGpuLifecycle().resume()) {fail("gpu_resume");return;}
        if(resumed_)resumed_();
        std::fputs("APPLE_IOS_LIFECYCLE_RESUMED ordered_frame_preserved=1 guest_timer_pause_unproven=1\n",stderr);
    }
    IosFramePresenter& presenter_;
    ReleaseInputs release_;
    Failure failed_;
    ReleaseInputs resumed_;
    NSTimer* timer_=nil;
    UIBackgroundTaskIdentifier backgroundTask_=UIBackgroundTaskInvalid;
    std::uint64_t backgroundGeneration_=0;
    bool started_=false,active_=true,terminal_=false,drainPending_=false;
};

} // namespace recomp::apple
