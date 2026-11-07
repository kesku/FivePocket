// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include "apple/audio_output.h"
#include "apple_stereo_ring.h"
#include "apple/audio_signal_evidence.h"
#include "apple/audio_capture_session.h"
#include "apple/audio_route_state.h"
#include "apple/audio_session_lifecycle.h"
#import <Foundation/Foundation.h>
#include <AudioUnit/AudioUnit.h>
#include <TargetConditionals.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <ranges>
#include <vector>
#if TARGET_OS_IOS
#import <AVFoundation/AVFoundation.h>
#include <dispatch/dispatch.h>
#else
#include <CoreAudio/CoreAudio.h>
#endif

namespace recomp::audio {
#if TARGET_OS_IOS
namespace {
using AudioRouteState=apple::AudioRouteState<AudioUnit>;

apple::AudioRouteSnapshot audioRouteSnapshot(AudioUnit unit,AVAudioSession* session,
                                             bool expectedRunning) noexcept {
    AudioStreamBasicDescription format{};
    UInt32 size=sizeof(format),maximum=0,running=0;
    const bool formatOK=AudioUnitGetProperty(unit,kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input,0,&format,&size)==noErr && size==sizeof(format);
    size=sizeof(maximum);
    const bool maximumOK=AudioUnitGetProperty(unit,kAudioUnitProperty_MaximumFramesPerSlice,
        kAudioUnitScope_Global,0,&maximum,&size)==noErr && size==sizeof(maximum);
    size=sizeof(running);
    const bool runningOK=AudioUnitGetProperty(unit,kAudioOutputUnitProperty_IsRunning,
        kAudioUnitScope_Global,0,&running,&size)==noErr && size==sizeof(running);
    apple::AudioRouteSnapshot snapshot{};
    snapshot.querySucceeded=formatOK && maximumOK && runningOK;
    snapshot.hasOutput=session.currentRoute.outputs.count>0 && session.outputNumberOfChannels>0;
    snapshot.hardwareSampleRate=session.sampleRate;
    snapshot.ioBufferDuration=session.IOBufferDuration;
    snapshot.clientSampleRate=format.mSampleRate;
    snapshot.clientFloatPacked=format.mFormatID==kAudioFormatLinearPCM &&
        format.mFormatFlags==kAudioFormatFlagsNativeFloatPacked;
    snapshot.clientChannels=format.mChannelsPerFrame;
    snapshot.clientBytesPerFrame=format.mBytesPerFrame;
    snapshot.clientFramesPerPacket=format.mFramesPerPacket;
    snapshot.clientBytesPerPacket=format.mBytesPerPacket;
    snapshot.clientBitsPerChannel=format.mBitsPerChannel;
    snapshot.maximumFramesPerSlice=maximum;
    snapshot.expectedRunning=expectedRunning;
    snapshot.observedRunning=running!=0;
    return snapshot;
}

void logAudioRoute(const apple::AudioRouteObservation& observation,
                   NSInteger notificationReason,std::uint32_t expectedBurst) noexcept {
    const auto& snapshot=observation.snapshot;
    std::fprintf(stderr,"APPLE_AUDIO_ROUTE_VALIDATION reason=%ld compatible=%d "
                "hardware_rate=%.3f period=%.9f client_rate=%.3f channels=%u "
                "expected_burst=%u observed_period_frames=%.3f maximum_frames=%u running=%u invalidated=%d rejection=%s\n",
                static_cast<long>(notificationReason),int(!observation.rejection),snapshot.hardwareSampleRate,
                snapshot.ioBufferDuration,snapshot.clientSampleRate,snapshot.clientChannels,
                expectedBurst,snapshot.ioBufferDuration*48000.0,snapshot.maximumFramesPerSlice,unsigned(snapshot.observedRunning),
                int(observation.invalidated),observation.rejection?observation.rejection:"none");
}

dispatch_queue_t audioControlQueue() noexcept {
    static dispatch_queue_t queue=dispatch_queue_create("recomp.apple.audio.control",DISPATCH_QUEUE_SERIAL);
    return queue;
}

bool activateAudioSession(AVAudioSession* session) noexcept;
const char* releaseIdleAudioSession(AVAudioSession* session) noexcept;
std::atomic<unsigned> sessionRefusals{0}; // (session-refused) refused setActive:YES calls in this process
constexpr std::int64_t SessionRetryNs=2'000'000'000; // (session-refused) the retry spacing
bool logRefusal(unsigned n) noexcept { return n<=5 || n%30==0; } // first five, then about once a minute

void reconcileAudioRoute(const std::shared_ptr<AudioRouteState>& state,
                         AVAudioSession* session,std::uint32_t expectedBurst) noexcept {
    @autoreleasepool {
        const auto transition=state->reconcile([&] {
            return activateAudioSession(session);
        },[&](AudioUnit unit,bool expectedRunning) {
            return audioRouteSnapshot(unit,session,expectedRunning);
        },[](AudioUnit unit) { return AudioOutputUnitStart(unit)==noErr; },
          [](AudioUnit unit) { return AudioOutputUnitStop(unit)==noErr; });
        if(!transition.attached)return; // close won; no disposed unit was used
        const auto sessionRejection=releaseIdleAudioSession(session);
        if(transition.route)logAudioRoute(*transition.route,-1,expectedBurst);
        const auto status=state->lifecycleStatus();
        if(!status.sessionRefused || logRefusal(sessionRefusals.load(std::memory_order_relaxed)))
        std::fprintf(stderr,"APPLE_AUDIO_LIFECYCLE active=%d interrupted=%d resume_permitted=%d "
            "terminal=%d running=%d stop_pending=%d started=%d stopped=%d invalidated=%d session_refused=%d rejection=%s\n",
            int(status.policy.applicationActive),int(status.policy.interrupted),int(status.policy.resumePermitted),
            int(status.policy.terminal),int(status.running),int(status.stopPending),int(transition.started),int(transition.stopped),
            int(status.invalidated),int(status.sessionRefused),transition.rejection?transition.rejection:
                (sessionRejection?sessionRejection:"none"));
        // (session-refused) a refused session is asked again 2 s later, one retry in flight; the chain ends when a retry
        // is granted, the app leaves the foreground (its return reconciles again) or the stream closes.
        if(apple::sessionRetryDue(status) && state->claimSessionRetry()) {
            const auto owned=state; // an OWNED copy: blocks keep C++ reference captures as references
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,SessionRetryNs),audioControlQueue(),^{
                owned->releaseSessionRetry();
                reconcileAudioRoute(owned,AVAudioSession.sharedInstance,expectedBurst);
            });
        }
    }
}

void enqueueAudioReconciliation(std::shared_ptr<AudioRouteState> state,
                                 std::uint32_t expectedBurst) noexcept {
    // Capture an OWNED shared_ptr, not a reference to the caller's stack-local
    // handle. Objective-C blocks preserve C++ reference captures as references.
    // The owner detaches the shared state while disposing the unit, so a queued
    // callback retains the handle but never keeps a raw Impl alive.
    dispatch_async(audioControlQueue(), ^{
        reconcileAudioRoute(state,AVAudioSession.sharedInstance,expectedBurst);
    });
}

void enqueueIdleAudioSessionRelease() noexcept {
    // The session outlives individual routes; release must also run when the
    // registry contains no live stream. Capture no owner or native unit.
    dispatch_async(audioControlQueue(), ^{
        @autoreleasepool {
            if(const char* rejection=releaseIdleAudioSession(AVAudioSession.sharedInstance))
                std::fprintf(stderr,"APPLE_AUDIO_SESSION_RELEASE_FAILED rejection=%s\n",rejection);
        }
    });
}

struct AudioLifecycleRegistry {
    std::mutex mutex;
    std::vector<std::pair<std::weak_ptr<AudioRouteState>,std::uint32_t>> streams;
    bool applicationActive=true;
    bool terminal=false;
    apple::AudioSessionLifecycle session;
};
AudioLifecycleRegistry& audioLifecycleRegistry() {
    static AudioLifecycleRegistry registry;
    return registry;
}
bool activateAudioSession(AVAudioSession* session) noexcept {
    auto& registry=audioLifecycleRegistry();
    std::lock_guard lock(registry.mutex);
    if(registry.session.invalidated())return false;
    NSError* error=nil;
    if(![session setActive:YES error:&error]) {
        const auto n=sessionRefusals.fetch_add(1,std::memory_order_relaxed)+1;
        if(logRefusal(n))std::fprintf(stderr,"APPLE_AUDIO_SESSION_REFUSED n=%u code=%ld\n",n,long(error?error.code:0));
        return false;
    }
    if(sessionRefusals.load(std::memory_order_relaxed))
        std::fprintf(stderr,"APPLE_AUDIO_SESSION_GRANTED after_refusals=%u\n",sessionRefusals.load(std::memory_order_relaxed));
    return registry.session.markActive();
}
const char* releaseIdleAudioSession(AVAudioSession* session) noexcept {
    auto& registry=audioLifecycleRegistry();
    std::lock_guard lock(registry.mutex);
    // The registry stays locked while the policy visits these snapshots. Avoid
    // allocating a temporary status vector on this noexcept teardown path.
    const auto statuses=registry.streams | std::views::transform([](const auto& entry) {
        if(auto state=entry.first.lock())return state->lifecycleStatus();
        return apple::AudioRouteLifecycleStatus{};
    });
    const bool wasActive=registry.session.active();
    const auto rejection=registry.session.releaseIfIdle(registry.applicationActive,registry.terminal,statuses,[&] {
        NSError* error=nil;
        return bool([session setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&error]);
    });
    if(wasActive && !registry.session.active())
        std::fputs("APPLE_AUDIO_SESSION_DEACTIVATED all_units_stopped=1\n",stderr);
    return rejection;
}
void registerAudioLifecycle(const std::shared_ptr<AudioRouteState>& state,std::uint32_t burst) {
    auto& registry=audioLifecycleRegistry();
    std::lock_guard lock(registry.mutex);
    state->setApplicationActive(registry.applicationActive);
    if(registry.terminal)state->terminate();
    registry.streams.emplace_back(state,burst);
}
}
#endif

void setAppleAudioApplicationActive(bool active) noexcept {
#if TARGET_OS_IOS
    auto& registry=audioLifecycleRegistry();
    std::lock_guard lock(registry.mutex);
    registry.applicationActive=active;
    auto& streams=registry.streams;
    streams.erase(std::remove_if(streams.begin(),streams.end(),[&](auto& entry) {
        auto state=entry.first.lock();
        if(!state)return true;
        state->setApplicationActive(active);
        enqueueAudioReconciliation(state,entry.second);
        return false;
    }),streams.end());
    if(!active)enqueueIdleAudioSessionRelease();
#else
    (void)active;
#endif
}
void terminateAppleAudioLifecycle() noexcept {
#if TARGET_OS_IOS
    auto& registry=audioLifecycleRegistry();
    std::lock_guard lock(registry.mutex);
    registry.terminal=true;
    for(auto& entry:registry.streams)if(auto state=entry.first.lock()) {
        state->terminate();
        enqueueAudioReconciliation(state,entry.second);
    }
    enqueueIdleAudioSessionRelease();
#endif
}
AppleAudioLifecycleStatus appleAudioLifecycleStatus() noexcept {
    AppleAudioLifecycleStatus result;
#if TARGET_OS_IOS
    auto& registry=audioLifecycleRegistry();
    std::lock_guard lock(registry.mutex);
    result.applicationActive=registry.applicationActive;
    result.terminal=registry.terminal;
    result.sessionActive=registry.session.active();
    result.invalidated=registry.session.invalidated();
    for(auto& entry:registry.streams)if(auto state=entry.first.lock()) {
        const auto status=state->lifecycleStatus();
        if(!status.attached)continue;
        result.available=true;
        result.running|=status.running;
        result.stopPending|=status.stopPending || status.policy.pausePending;
        result.interrupted|=status.policy.interrupted;
        result.resumePermitted&=status.policy.resumePermitted;
        result.pauseRequested|=status.policy.pausePending;
        result.terminal|=status.policy.terminal;
        result.invalidated|=status.invalidated;
    }
    result.pauseRequested=result.pauseRequested || !result.applicationActive || result.interrupted ||
                          !result.resumePermitted || result.terminal;
#endif
    return result;
}

struct AppleAudioOutputStream::Impl {
    static constexpr std::uint32_t Capacity=16384;
    AudioUnit unit=nullptr;
    apple::StereoRing<Capacity> ring;
    apple::AudioSignalEvidence signalEvidence;
    bool signalDiagnostic=false; // configured before the AudioUnit starts
    std::atomic<std::uint32_t> lastSignalReport{0};
    std::atomic<bool> running{false}, usable{true};
    std::unique_ptr<apple::AudioCaptureSession> capture;
    std::string captureId;
    std::atomic<bool> captureReported{false};
    std::shared_ptr<std::atomic<bool>> invalidated=std::make_shared<std::atomic<bool>>(false);
    std::uint32_t burst=0;
    // stall drain (Claude, 2026-10-20; stereo_ring.h): the title's polling thread only. RECOMP_IOS_AUDIO_STALL_DRAIN=0 disables.
    apple::StallDrain stall;
    const bool drainEnabled=[] {const char* e=std::getenv("RECOMP_IOS_AUDIO_STALL_DRAIN");return !(e && e[0]=='0' && !e[1]);}();
    void drainStall() noexcept;
    // audio target (Claude, 2026-10-20): the frames the title keeps queued, reported as bufferCapacityFrames (the title's
    // renderer fills to it). The ring's 16384 frames held ~333 ms of sound behind the picture in every run (RENDER_DIAG_AU_AHEAD
    // inflight_ms). RECOMP_IOS_AUDIO_TARGET_FRAMES=N (2048..16384) reports N instead; default the ring size. Smaller targets cut
    // the delay (4096: ~80 ms, c293-c296) but the title's audio callback barely outpaces real time, so a dip is refilled
    // with back-to-back callbacks that collide with the game frame: pacer-late real frames on foot 1.33 % at 4096 (c296),
    // 0.73 % at 8192 (c298), 0.17 % at 16384 (c297). Framerate first: the default stays 16384.
    const std::uint32_t target=[] {
        const char* e=std::getenv("RECOMP_IOS_AUDIO_TARGET_FRAMES");const long v=e?std::strtol(e,nullptr,10):0;
        return v>=2048 && v<=long(Capacity)?std::uint32_t(v):Capacity;
    }();
#if TARGET_OS_IOS
    std::shared_ptr<AudioRouteState> routeState;
    id interruptionObserver=nil;
    id routeObserver=nil;
#endif
    static OSStatus render(void* context,AudioUnitRenderActionFlags*,const AudioTimeStamp*,
                            UInt32,UInt32 frames,AudioBufferList* data) noexcept {
        auto& self=*static_cast<Impl*>(context);
        if (frames>Capacity || !data || data->mNumberBuffers!=1 || !data->mBuffers[0].mData ||
            data->mBuffers[0].mNumberChannels!=2 ||
            std::uint64_t(data->mBuffers[0].mDataByteSize)<std::uint64_t(frames)*8) {
            self.usable.store(false,std::memory_order_release);
#if TARGET_OS_IOS
            self.invalidated->store(true,std::memory_order_release);
#endif
            return kAudioUnitErr_FormatNotSupported;
        }
        apple::renderAudioObserved(self.ring,static_cast<float*>(data->mBuffers[0].mData),frames,
                                  self.signalDiagnostic?&self.signalEvidence:nullptr);
        // Exact final output, including underrun silence. Bounded memcpy only;
        // the separate worker owns every filesystem operation.
        if(self.capture)self.capture->push(static_cast<const float*>(data->mBuffers[0].mData),frames);
        return noErr;
    }
    void reportCapture() noexcept {
        if(!capture || captureReported.load(std::memory_order_relaxed))return;
        apple::AudioCaptureWriter::Result result;
        if(!capture->result(result) || captureReported.exchange(true))return;
        std::fprintf(stderr,"APPLE_AUDIO_CAPTURE_END session=%s captured_frames=%llu written_frames=%llu "
            "written_bytes=%llu error=%d overflow=%d complete=%d physical_audibility_unproven=1\n",
            captureId.c_str(),(unsigned long long)result.capturedFrames,(unsigned long long)result.writtenFrames,
            (unsigned long long)result.writtenBytes,result.error,int(result.overflow),int(result.complete));
    }
    void configureCapture() noexcept {
        const char* option=std::getenv("RECOMP_APPLE_AUDIO_CAPTURE_SECONDS");
        if(!option)return; // No allocation or file creation when disabled.
        const unsigned seconds=apple::audioCaptureSeconds(option);
        if(!seconds){std::fputs("APPLE_AUDIO_CAPTURE_REFUSED reason=invalid_duration\n",stderr);return;}
        @autoreleasepool {
            try {
#if TARGET_OS_IOS
                NSString* parent=[NSHomeDirectory() stringByAppendingPathComponent:@"Library"];
#else
                NSString* parent=NSTemporaryDirectory();
#endif
                NSString* root=[parent stringByAppendingPathComponent:@"PortAudio"];
#if defined(APPLE_AUDIO_CAPTURE_HOST_TEST) && !TARGET_OS_IOS
                const char* testRoot=std::getenv("RECOMP_APPLE_AUDIO_CAPTURE_TEST_ROOT");
                if(testRoot)root=[NSString stringWithUTF8String:testRoot];
#endif
                if(mkdir(root.fileSystemRepresentation,0700)!=0 && errno!=EEXIST)
                    throw std::runtime_error("capture root creation failed");
                struct stat st{};
                if(lstat(root.fileSystemRepresentation,&st)!=0 || !S_ISDIR(st.st_mode) ||
                   st.st_uid!=getuid() || (st.st_mode&0077))
                    throw std::runtime_error("capture root must be private owned directory");
                NSURL* url=[NSURL fileURLWithPath:root isDirectory:YES];
                NSError* error=nil;
                if(![url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&error])
                    throw std::runtime_error("capture backup exclusion failed");
                captureId=[[[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString] UTF8String];
                capture=std::make_unique<apple::AudioCaptureSession>(root.fileSystemRepresentation,captureId,seconds);
                std::fprintf(stderr,"APPLE_AUDIO_CAPTURE_READY session=%s seconds=%u rate=48000 channels=2 "
                    "format=f32le relative=Library/PortAudio/%s/output.f32le epoch=%.6f device=%s\n",
                    captureId.c_str(),seconds,captureId.c_str(),NSDate.date.timeIntervalSince1970,
#if TARGET_OS_IOS
                    "ios"
#else
                    "mac-host-test-not-device-evidence"
#endif
                );
            }catch(const std::exception& e){std::fprintf(stderr,"APPLE_AUDIO_CAPTURE_REFUSED reason=%s\n",e.what());}
        }
    }
    ~Impl() {
        const auto disposeUnit=[](AudioUnit output) noexcept {
            AudioOutputUnitStop(output);
            AudioUnitUninitialize(output);
            AudioComponentInstanceDispose(output);
        };
#if TARGET_OS_IOS
        if(interruptionObserver)[NSNotificationCenter.defaultCenter removeObserver:interruptionObserver];
        if(routeObserver)[NSNotificationCenter.defaultCenter removeObserver:routeObserver];
        if(routeState) {
            routeState->detach(disposeUnit);
            unit=nullptr;
            // A release attempted during disposal sees this route as pending.
            // Detached reconciliations return early, so retry after teardown
            // without retaining the owner or the disposed unit.
            enqueueIdleAudioSessionRelease();
        }
#endif
        if(unit)disposeUnit(unit);
        if(capture){capture->finishAfterProducerStopped();reportCapture();}
    }
};
AppleAudioOutputStream::AppleAudioOutputStream()=default;
AppleAudioOutputStream::~AppleAudioOutputStream(){close();}
bool AppleAudioOutputStream::open() {
    if(impl_)return false;
#if TARGET_OS_IOS
    // Output only: no microphone permission or audio input. A suspended stream
    // resumes only through the foreground/interruption/route policy below.
    NSError* error=nil;
    auto session=AVAudioSession.sharedInstance;
    if(![session setCategory:AVAudioSessionCategoryPlayback error:&error] ||
       ![session setPreferredSampleRate:48000 error:&error])return false;
    // (session-refused) a refused activation no longer fails the open (it closed the game at launch): the unit is made
    // as usual, nothing starts, and the route state retries the session every 2 s while the title plays silently.
    const bool sessionGranted=activateAudioSession(session);
#endif
    auto candidate=std::make_unique<Impl>();
    const char* signalOption=std::getenv("RECOMP_APPLE_AUDIO_SIGNAL_DIAGNOSTIC");
    candidate->signalDiagnostic=signalOption && std::strcmp(signalOption,"1")==0;
    AudioComponentDescription desc{};
    desc.componentType=kAudioUnitType_Output;
    desc.componentManufacturer=kAudioUnitManufacturer_Apple;
#if TARGET_OS_IOS
    desc.componentSubType=kAudioUnitSubType_RemoteIO;
#else
    desc.componentSubType=kAudioUnitSubType_DefaultOutput;
#endif
    auto component=AudioComponentFindNext(nullptr,&desc);
    if(!component || AudioComponentInstanceNew(component,&candidate->unit)!=noErr)return false;
    AudioStreamBasicDescription format{};
    format.mSampleRate=48000;format.mFormatID=kAudioFormatLinearPCM;
    format.mFormatFlags=kAudioFormatFlagsNativeFloatPacked;
    format.mBytesPerPacket=8;format.mFramesPerPacket=1;format.mBytesPerFrame=8;
    format.mChannelsPerFrame=2;format.mBitsPerChannel=32;
    if(AudioUnitSetProperty(candidate->unit,kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input,0,&format,sizeof(format))!=noErr)return false;
    AURenderCallbackStruct callback{Impl::render,candidate.get()};
    if(AudioUnitSetProperty(candidate->unit,kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input,0,&callback,sizeof(callback))!=noErr)return false;
    UInt32 maximum=0,size=sizeof(maximum);
    if(AudioUnitGetProperty(candidate->unit,kAudioUnitProperty_MaximumFramesPerSlice,
            kAudioUnitScope_Global,0,&maximum,&size)!=noErr || !maximum || maximum>Impl::Capacity)return false;
    // Snapshot the startup output period in client frames, NOT the allocation
    // bound MaximumFramesPerSlice. Keep this published scheduling hint stable:
    // NativeAudioRenderClient caches it, while RemoteIO may later vary its
    // actual callback size. The callback/ring account actual consumed frames.
    double period=0;
#if TARGET_OS_IOS
    period=session.IOBufferDuration;
#else
    AudioDeviceID device=0;UInt32 deviceSize=sizeof(device);
    if(AudioUnitGetProperty(candidate->unit,kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global,0,&device,&deviceSize)!=noErr)return false;
    UInt32 frames=0;size=sizeof(frames);
    AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeOutput,kAudioObjectPropertyElementMain};
    if(AudioObjectGetPropertyData(device,&address,0,nullptr,&size,&frames)!=noErr)return false;
    Float64 rate=0;size=sizeof(rate);address.mSelector=kAudioDevicePropertyNominalSampleRate;
    if(AudioObjectGetPropertyData(device,&address,0,nullptr,&size,&rate)!=noErr || !(rate>0))return false;
    period=double(frames)/rate;
#endif
    if(!std::isfinite(period) || !(period>0) || period*48000>Impl::Capacity)return false;
    candidate->burst=static_cast<std::uint32_t>(std::ceil(period*48000));
    if(AudioUnitInitialize(candidate->unit)!=noErr)return false;
#if TARGET_OS_IOS
    // Shared flag outlives an in-flight notification during close; never
    // capture a raw Impl pointer that could race removal of the observer.
    auto invalidated=candidate->invalidated;
    auto routeState=std::make_shared<AudioRouteState>(candidate->unit,candidate->burst,
                                                    Impl::Capacity,invalidated);
    candidate->routeState=routeState;
    const auto expectedBurst=candidate->burst;
    registerAudioLifecycle(routeState,expectedBurst);
    if(!sessionGranted) { // (session-refused) the first reconciliation schedules the 2 s retries
        routeState->noteSessionRefused();
        std::fputs("APPLE_AUDIO_SESSION_REFUSED_AT_OPEN silent_until_granted=1\n",stderr);
        enqueueAudioReconciliation(routeState,expectedBurst);
    }
    candidate->interruptionObserver=[NSNotificationCenter.defaultCenter
        addObserverForName:AVAudioSessionInterruptionNotification object:session queue:nil
        usingBlock:^(NSNotification* notification) {
            const auto type=[notification.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
            const auto options=[notification.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
            const auto reason=[notification.userInfo[AVAudioSessionInterruptionReasonKey] unsignedIntegerValue];
            // The historical wasSuspended key/reason is no longer supported
            // by current iOS. Log the actual supplied reason, not an invented
            // suspension classification or a forged interruption-ended event.
            const bool hasReason=notification.userInfo[AVAudioSessionInterruptionReasonKey]!=nil;
            std::fprintf(stderr,"APPLE_AUDIO_INTERRUPTION type=%lu options=%lu reason=%lu reason_present=%d\n",
                (unsigned long)type,(unsigned long)options,(unsigned long)reason,int(hasReason));
            if(type==AVAudioSessionInterruptionTypeBegan)routeState->interruptionBegan();
            else if(type==AVAudioSessionInterruptionTypeEnded) {
                routeState->interruptionEnded((options&AVAudioSessionInterruptionOptionShouldResume)!=0);
            } else return;
            enqueueAudioReconciliation(routeState,expectedBurst);
        }];
    candidate->routeObserver=[NSNotificationCenter.defaultCenter
        addObserverForName:AVAudioSessionRouteChangeNotification object:session queue:nil
        usingBlock:^(NSNotification* notification) {
            const auto reason=[notification.userInfo[AVAudioSessionRouteChangeReasonKey] integerValue];
            // Always asynchronous: AudioOutputUnitStart may itself trigger a
            // notification while holding the route-state lock.
            dispatch_async(audioControlQueue(), ^{
                @autoreleasepool {
                    const auto observation=routeState->validate([&](AudioUnit unit,bool expectedRunning) {
                        return audioRouteSnapshot(unit,session,expectedRunning);
                    });
                    if(observation)logAudioRoute(*observation,reason,expectedBurst);
                    // Inactive/interrupted observations are deferred until the
                    // mandatory resume query. A genuine active-route failure
                    // remains sticky and stops the unit on this same queue.
                    reconcileAudioRoute(routeState,session,expectedBurst);
                }
            });
        }];
    // ios-fake-audio-stall (Claude 2026-10-20, TEST ONLY: RECOMP_IOS_FAKE_AUDIO_STALL_AT=seconds): the output unit stops pulling for
    // 3 s (AudioOutputUnitStop, then Start), as during a Bluetooth route switch (c285: 4.3 s frozen picture), without a route
    // notification: shows whether the title freezes with the device and whether the stall drain keeps it running.
    if(const char* at=std::getenv("RECOMP_IOS_FAKE_AUDIO_STALL_AT")) {
        const double seconds=std::atof(at);const AudioUnit unit=candidate->unit;
        if(seconds>0 && seconds<3600) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(seconds*NSEC_PER_SEC)),audioControlQueue(),^{
                std::fprintf(stderr,"APPLE_IOS_FAKE_AUDIO_STALL phase=stopped result=%d\n",int(AudioOutputUnitStop(unit)));});
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)((seconds+3)*NSEC_PER_SEC)),audioControlQueue(),^{
                std::fprintf(stderr,"APPLE_IOS_FAKE_AUDIO_STALL phase=started result=%d\n",int(AudioOutputUnitStart(unit)));});
        }
    }
    // ios-fake-route-loss (Claude 2026-10-20, TEST ONLY: RECOMP_IOS_FAKE_ROUTE_LOSS_AT=seconds after the stream opens): the
    // route-change path runs once with an observation whose output is missing (what iOS reports while it switches devices,
    // c280), then 1 s later once with the real route, so a device test shows whether the stream survives a route switch.
    if(const char* at=std::getenv("RECOMP_IOS_FAKE_ROUTE_LOSS_AT")) {
        const double seconds=std::atof(at);
        if(seconds>0 && seconds<3600) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(seconds*NSEC_PER_SEC)),audioControlQueue(),^{
                @autoreleasepool {
                    const auto o=routeState->validate([&](AudioUnit unit,bool expectedRunning) {
                        auto snap=audioRouteSnapshot(unit,session,expectedRunning);snap.hasOutput=false;return snap;});
                    std::fprintf(stderr,"APPLE_IOS_FAKE_ROUTE_LOSS phase=lost\n");
                    if(o)logAudioRoute(*o,1,expectedBurst);
                    reconcileAudioRoute(routeState,session,expectedBurst);
                }
            });
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)((seconds+1)*NSEC_PER_SEC)),audioControlQueue(),^{
                @autoreleasepool {
                    const auto o=routeState->validate([&](AudioUnit unit,bool expectedRunning) {
                        return audioRouteSnapshot(unit,session,expectedRunning);});
                    std::fprintf(stderr,"APPLE_IOS_FAKE_ROUTE_LOSS phase=restored\n");
                    if(o)logAudioRoute(*o,1,expectedBurst);
                    reconcileAudioRoute(routeState,session,expectedBurst);
                }
            });
        }
    }
#endif
    candidate->configureCapture();
    impl_=std::move(candidate);return true;
}
void AppleAudioOutputStream::close() noexcept {impl_.reset();}
bool AppleAudioOutputStream::requestStart() noexcept {
    if(!impl_)return false;
#if TARGET_OS_IOS
    if(!configuration())return false;
    return impl_->routeState->start([](AudioUnit unit) { return AudioOutputUnitStart(unit)==noErr; });
#else
    const auto start=[&](AudioUnit unit) {
        if(!configuration() || impl_->running.load(std::memory_order_acquire))return false;
        if(AudioOutputUnitStart(unit)!=noErr){impl_->usable.store(false);return false;}
        impl_->running.store(true,std::memory_order_release);
        return true;
    };
    return start(impl_->unit);
#endif
}
bool AppleAudioOutputStream::started() const noexcept {
#if TARGET_OS_IOS
    return configuration().has_value() && impl_->routeState->lifecycleStatus().running;
#else
    return configuration().has_value() && impl_->running.load(std::memory_order_acquire);
#endif
}
std::int32_t AppleAudioOutputStream::writeStereo(const float* data,std::uint32_t frames) noexcept {
    if(!data || !frames || frames>AudioFramesPerPacket)return -1;
    if(!configuration())return -1;
    for(std::size_t i=0;i<std::size_t(frames)*2;++i)
        if(!std::isfinite(data[i]) || data[i]<-1 || data[i]>1)return -1;
    return static_cast<std::int32_t>(impl_->ring.write(data,frames));
}
std::optional<std::uint64_t> AppleAudioOutputStream::framesWritten() const noexcept {
    if(!configuration())return std::nullopt;
    return impl_->ring.written();
}
std::optional<std::uint64_t> AppleAudioOutputStream::framesRead() const noexcept {
    if(!configuration())return std::nullopt;
    impl_->reportCapture(); // guest polling thread, never the audio callback
    // On the guest-side polling thread, never the realtime render callback.
    // At most one record per consumed second after the first nonempty sample.
    if(impl_->signalDiagnostic) {
        const auto signal=impl_->signalEvidence.snapshot();
        auto previous=impl_->lastSignalReport.load(std::memory_order_relaxed);
        if(apple::shouldReportAudioSignal(signal.frames,previous) &&
           impl_->lastSignalReport.compare_exchange_strong(previous,signal.frames,std::memory_order_relaxed)) {
            std::fprintf(stderr,"APPLE_AUDIO_OUTPUT_SIGNAL frames=%u nonzero_frames=%u invalid_samples=%d "
                         "counter_saturated=%d physical_audibility_unproven=1\n",signal.frames,signal.nonzeroFrames,
                         int(signal.invalidSamples),int(signal.frames==UINT32_MAX));
        }
    }
    {static std::atomic<bool> logged{false};if(!logged.exchange(true,std::memory_order_relaxed))
        std::fprintf(stderr,"APPLE_AUDIO_TARGET frames=%u ring=%u burst=%u\n",impl_->target,Impl::Capacity,impl_->burst);}
    impl_->drainStall(); // (stall drain: the device stopped pulling; keep the title's audio clock in real time)
    return impl_->ring.read();
}
void AppleAudioOutputStream::Impl::drainStall() noexcept {
#if TARGET_OS_IOS
    if(!drainEnabled || !routeState)return;
    const auto status=routeState->lifecycleStatus();
    // (Claude 2026-10-21, c408: a 6.3 s frozen picture) an audio-session interruption while the app stays active (another app or
    // the system took the audio; no resign-active) stops the unit, and the title waits for its audio clock: every thread idled
    // until the interruption ended. Keep the title's audio clock in real time then too: the game runs on, silently.
    const bool interruptedActive=status.policy.interrupted && status.policy.applicationActive && !status.policy.terminal &&
                                 !status.invalidated;
    // (session-refused) the same for a refused session (apple::audioClockShouldRun: running, interrupted or refused).
    const bool run=apple::audioClockShouldRun(status);
    {static bool wasInterrupted=false;if(interruptedActive!=wasInterrupted){wasInterrupted=interruptedActive;
        std::fprintf(stderr,"APPLE_AUDIO_STALL_DRAIN interrupted_active=%d\n",int(interruptedActive));}}
    {static bool wasRefused=false;if(status.sessionRefused!=wasRefused){wasRefused=status.sessionRefused;
        std::fprintf(stderr,"APPLE_AUDIO_STALL_DRAIN session_refused=%d\n",int(wasRefused));}}
    const auto r=ring.read();
    const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto prevDropped=stall.dropped;const auto prevStalled=stall.stalledNs;
    const auto n=apple::stallDrainDue(stall,r,now,run,100'000'000,48000); // (100 ms: 5-10 callback periods; c287 at 200 ms left one 132 ms frame)
    if(prevDropped && !stall.dropped)
        std::fprintf(stderr,"APPLE_AUDIO_STALL_DRAIN phase=ended stalled_ms=%.0f dropped_frames=%llu\n",
                     double(prevStalled)/1e6,(unsigned long long)prevDropped);
    if(n) {
        const auto d=ring.discard(n);
        apple::stallDrainNote(stall,r,d);
        if(!prevDropped && d)std::fprintf(stderr,"APPLE_AUDIO_STALL_DRAIN phase=began stalled_ms=%.0f\n",double(stall.stalledNs)/1e6);
    }
#endif
}
std::optional<bootstrap::NativeAudioConfiguration> AppleAudioOutputStream::configuration() const noexcept {
    if(!impl_ || !impl_->usable.load(std::memory_order_acquire) ||
       impl_->invalidated->load(std::memory_order_acquire))return std::nullopt;
    return bootstrap::NativeAudioConfiguration{48000,2,std::max(impl_->target,impl_->burst),impl_->burst};
}
}
