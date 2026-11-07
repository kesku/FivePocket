// SPDX-License-Identifier: GPL-3.0-or-later
#include "ios_renderer_failure_capture.hpp"
#include "apple/ios_frame_presenter.h"
#include "shared_gpu_objects.hpp"
#include "shared_gpu_contract.hpp"
#include "apple/display_observation.h"
#include "apple/metal_surface.h"
#include "rgba_transfer.h"
#include "apple/exact_bgra_compare.h"
#include "apple/ios_layer_geometry.h"
#include "apple/ios_gpu_lifecycle.h"
#include "device.h"
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include "apple/measurement_clock.h"
#include "presenter_phase.hpp"
#include "ios_failure_capture.hpp"
#include <cstdlib>
#include <cmath>
#include <atomic>
extern "C" std::atomic<int> recomp_ios_drawable_half; // (generic_renderer.cpp)
#if defined(RECOMP_DUO_EDITION)
#include "duo_state.h" // ios-duo (the stage's copy, overlaid)
#endif
extern "C" std::atomic<unsigned> recomp_ios_layout_gen; // (generic_renderer.cpp; build 192)
// (build 194) the drawable size layoutSubviews set last. The frame contract's "exact drawable" checks (frame admission, the
// CPU present, resume) required 1280x720; with drawable-half the layer is 640x360, so resume() failed "resume-drawable" and
// the lifecycle ended in presenter_resume_invalid after any app switch (c460). They now accept 1280x720 or this size.
static std::atomic<int> gLaidOutDrawableW{1280},gLaidOutDrawableH{720};
static bool drawableMatchesLayout(CAMetalLayer* layer) noexcept {
    const int w=int(layer.drawableSize.width),h=int(layer.drawableSize.height);
    return (w==1280 && h==720) || (w==gLaidOutDrawableW.load(std::memory_order_relaxed) && h==gLaidOutDrawableH.load(std::memory_order_relaxed));
}
extern "C" std::atomic<int> recomp_ios_render_sixteenths; // (generic_renderer.cpp)
#include <algorithm>
#include <vector>
#include <stdexcept>
extern "C" { std::atomic<unsigned> recomp_ios_app_inactive{0}; } // (2026-10-21) stall watcher: 1 between resign-active and become-active

// A container, not a full-screen singleton: the shell owns window/scene layout.
@interface RecompIosMetalView : UIView
@end
@implementation RecompIosMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

@interface RecompIosObservedMetalView : RecompIosMetalView
@end
@implementation RecompIosObservedMetalView
+ (Class)layerClass { return (__bridge Class)recomp::apple::performance::observedMetalLayerClass(); }
@end

static Class displayMetalViewClass(bool requested) noexcept {
    return requested ? RecompIosObservedMetalView.class : RecompIosMetalView.class;
}

@interface RecompIosFrameView : UIView {
@public RecompIosMetalView* metalView;
@public recomp::apple::detail::IosFrameContract* contract;
@public BOOL wasAttached;
@public BOOL managedLifecycle;
#if defined(RECOMP_DUO_EDITION)
@public RecompIosMetalView* duoLowerView; // ios-duo: the lower screen (the bottom half in the portrait split), its own CAMetalLayer
#endif
}
- (instancetype)initWithFrame:(CGRect)frame displayRecording:(BOOL)requested;
@end
@implementation RecompIosFrameView
- (instancetype)initWithFrame:(CGRect)frame {
    return [self initWithFrame:frame displayRecording:NO];
}
- (instancetype)initWithFrame:(CGRect)frame displayRecording:(BOOL)requested {
    self=[super initWithFrame:frame];
    if (self) {
        self.backgroundColor=UIColor.blackColor;
        metalView=[[displayMetalViewClass(requested) alloc] initWithFrame:CGRectZero];
        metalView.userInteractionEnabled=NO;
        [self addSubview:metalView]; // (2026-11-05: 26s put it in a CAReplicatorLayer for Fit + Blur and the Duo showed black: back to 25s's tree)
#if defined(RECOMP_DUO_EDITION)
        duoLowerView=[[RecompIosMetalView alloc] initWithFrame:CGRectZero];
        duoLowerView.userInteractionEnabled=NO;duoLowerView.hidden=YES; // (touches: duo_app.h's touch view above it)
        CAMetalLayer* lower=(CAMetalLayer*)duoLowerView.layer;
        lower.device=MTLCreateSystemDefaultDevice();lower.pixelFormat=MTLPixelFormatBGRA8Unorm;lower.opaque=YES;
        [self addSubview:duoLowerView];
        // (2026-11-03, the real Duo) the hinge: status changes re-lay the split (duo_state.h atFold) and are logged for the device checks
#if defined(__IPHONE_27_1) // (2026-11-05: a build on an older SDK - the mini's Xcode 26.5, relabelled 27.1 - has no UIHingeInteraction; the split ignores the hinge anyway)
        if(@available(iOS 27.1,*)) {
            __weak UIView* weakSelf=self;
            UIHingeInteraction* hinge=[[UIHingeInteraction alloc] initWithUpdateHandler:^(UIHingeInteraction*,UIHingeInteractionUpdate* update){
                namespace ds=recomp::duo::state;
                UIHinge* h=update.hinge;const int st=h?int(h.status):-1;const float angle=h?float(h.angle):0.0f;
                ds::hingeAngle.store(angle,std::memory_order_relaxed);
                const int old=ds::hingeStatus.exchange(st,std::memory_order_relaxed);
                if(old!=st) {
                    std::fprintf(stderr,"APPLE_IOS_DUO_HINGE status=%d angle=%.3f (was %d)\n",st,double(angle),old);
                    UIView* v=weakSelf;[v setNeedsLayout];for(UIView* s in v.superview.subviews)[s setNeedsLayout];
                }
            }];
            [self addInteraction:hinge];
        } else std::fprintf(stderr,"APPLE_IOS_DUO_HINGE unavailable (iOS < 27.1)\n");
#else
        std::fprintf(stderr,"APPLE_IOS_DUO_HINGE not in this build (SDK < 27.1)\n");
#endif
#endif
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    // Source resolution remains exact; only UIKit layer geometry letterboxes.
    const double width=self.bounds.size.width, height=self.bounds.size.height;
#if defined(RECOMP_DUO_EDITION)
    if([self duoLayout:width height:height])return;
#endif
    const auto geometry=recomp::apple::detail::iosLayerGeometry(width,height);
    if (!geometry.valid) {
        metalView.bounds=CGRectZero;
        if (contract && wasAttached && !managedLifecycle) contract->close();
        return;
    }
    { // ios-screen-native (Claude 2026-10-21; MEASUREMENT ONLY, RECOMP_IOS_SCREEN_FIT=native): the layer at the view's full size in
      // native pixels, no transform, no crop, so the swapchain (bounds*contentsScale in MoltenVK) is the panel's size and
      // direct_present.h letterboxes the 1280x720 frame into it (fit). Question (c424/c425): the system composites our scaled
      // layer on the GPU every refresh (Instruments Display: direct=No; backboardd ~2-2.6 ms GPU per real frame); can an
      // unscaled full-screen layer go direct to display?
        // native1: the same layer at contentsScale 1 (a 1133x744 drawable on the iPad mini): does the display pipe scale it up and
        // still take it direct (no GPU composition, a cheap presenter pass)?
        static const int native=[]{const char* e=std::getenv("RECOMP_IOS_SCREEN_FIT");return !e?0:!std::strcmp(e,"native")?1:!std::strcmp(e,"native1")?2:0;}();
        if(native) {
            const CGFloat s=native==2?1.0:(self.window?self.window.screen.nativeScale:UIScreen.mainScreen.nativeScale);
            metalView.transform=CGAffineTransformIdentity;metalView.frame=self.bounds;
            CAMetalLayer* layer=(CAMetalLayer*)metalView.layer;
            layer.contentsRect=CGRectMake(0,0,1,1);layer.contentsScale=s;layer.opaque=YES;
            layer.drawableSize=CGSizeMake(std::round(width*s),std::round(height*s));
            recomp_ios_visible_x.store(1.0f,std::memory_order_relaxed);recomp_ios_visible_y.store(1.0f,std::memory_order_relaxed);
            static bool logged=false;if(!logged){logged=true;std::fprintf(stderr,"APPLE_IOS_SCREEN_NATIVE bounds=%.0fx%.0f scale=%.2f drawable=%.0fx%.0f\n",
                width,height,double(s),std::round(width*s),std::round(height*s));}
            return;
        }
    }
    // MoltenVK1.4.2 reports bounds*contentsScale as currentExtent, not
    // drawableSize. Preserve1280x720 bounds and scale only the composed view.
    // ios-drawable-half (Claude 2026-10-21): a 640x360 layer (half the 1280x720 bounds, twice the scale), so the swapchain is the
    // render-scale-50 frame's own size: the presenter copies it 1:1 (no 640x360 -> 1280x720 up-blit, a quarter of the copy) and
    // the system compositor, off the frame's critical path (c429/c430), does the one upscale. c437: the presenter's GPU time is
    // on the critical path. Set by Main.mm's 0.5 s timer from files/private-trace/ios-drawable-half (A/B-able).
    // (generalised: the drawable is the renderer's applied frame size, 1280x720 x sixteenths/16 - never smaller than the frame)
    const int rsk=recomp_ios_drawable_half.load(std::memory_order_relaxed)?std::clamp(recomp_ios_render_sixteenths.load(std::memory_order_relaxed),1,16):16;
    const double half=16.0/double(rsk);
    metalView.bounds=CGRectMake(0,0,std::round(geometry.boundsWidth/half),std::round(geometry.boundsHeight/half));
    metalView.center=CGPointMake(CGRectGetMidX(self.bounds),CGRectGetMidY(self.bounds));
    // (Claude 2026-10-20, fill by crop: per-axis scale and a cropping contentsRect; fit: uniform scale, whole picture)
    metalView.transform=CGAffineTransformMakeScale(geometry.scaleX*half,geometry.scaleY*half);
    CAMetalLayer* layer=(CAMetalLayer*)metalView.layer;
    layer.contentsRect=CGRectMake(geometry.cropX,geometry.cropY,geometry.cropW,geometry.cropH);
    layer.contentsScale=1.0;
    const CGSize drawable=CGSizeMake(std::round(1280/half),std::round(720/half));
    // (build 192) a new size: MoltenVK's next drawables follow the layer at once while the Vulkan swapchain keeps its old extent
    // until it is made again (c447: 84 suboptimal presents; c449: the first GPU audit failed on such a frame and the presenter
    // fell back to its CPU path for the rest of the run). The presenter re-checks the extent on this counter's change.
    const bool sizeChanged=!CGSizeEqualToSize(layer.drawableSize,drawable);
    gLaidOutDrawableW.store(int(drawable.width),std::memory_order_relaxed);gLaidOutDrawableH.store(int(drawable.height),std::memory_order_relaxed);
    layer.drawableSize=drawable;
    if(sizeChanged)recomp_ios_layout_gen.fetch_add(1,std::memory_order_release);
}
#if defined(RECOMP_DUO_EDITION)
// ios-duo: the top screen (2026-11-05: every orientation, by the top mode - duo_state.h topPicture: Stretch (the default) / Fit /
// Zoom) and, in the portrait split, the lower screen below the middle at the panel's native pixels. The game's 1280x720 layer keeps the
// regular build's drawable rule (the render-scaled size while drawable-half is on); its transform is per axis and its contentsRect
// crops in Zoom. Every show / hide / resize of the lower layer publishes it with a new generation (duo_state.h). The layout line is
// printed again whenever a value in it changes (a rotation, a size change, the mode, the lower screen).
- (BOOL)duoLayout:(double)width height:(double)height {
    namespace ds=recomp::duo::state;
    if(!(width>0 && height>0) || !std::isfinite(width) || !std::isfinite(height))return NO;
    const bool split=ds::splitLayout(width,height);
    CAMetalLayer* lower=(CAMetalLayer*)duoLowerView.layer;
    bool lowerChanged=false;
    if(!split) {
        duoLowerView.hidden=YES;
        if(ds::lowerLayer.exchange(nullptr,std::memory_order_acq_rel)){ds::lowerGeneration.fetch_add(1,std::memory_order_acq_rel);lowerChanged=true;}
    }
    const int mode=ds::clampMode(ds::topMode.load(std::memory_order_relaxed));
    const ds::Rect area=ds::topArea(width,height);
    const bool narrow=ds::frame4x3.load(std::memory_order_relaxed)!=0;
    const auto t=ds::topPicture(area,mode,narrow);
    if(!t.valid)return NO;
    recomp_ios_visible_x.store(float(t.cropW),std::memory_order_relaxed);recomp_ios_visible_y.store(float(t.cropH),std::memory_order_relaxed);
    const int rsk=recomp_ios_drawable_half.load(std::memory_order_relaxed)?std::clamp(recomp_ios_render_sixteenths.load(std::memory_order_relaxed),1,16):16;
    const double half=16.0/double(rsk);
    metalView.bounds=CGRectMake(0,0,std::round(1280/half),std::round(720/half));
    metalView.center=CGPointMake(t.cx,t.cy);
    metalView.transform=CGAffineTransformMakeScale(t.scaleX*half,t.scaleY*half);
    CAMetalLayer* layer=(CAMetalLayer*)metalView.layer;
    layer.contentsRect=CGRectMake(t.cropX,t.cropY,t.cropW,t.cropH);layer.contentsScale=1.0;
    const CGSize drawable=CGSizeMake(std::round(1280/half),std::round(720/half));
    const bool sizeChanged=!CGSizeEqualToSize(layer.drawableSize,drawable);
    gLaidOutDrawableW.store(int(drawable.width),std::memory_order_relaxed);gLaidOutDrawableH.store(int(drawable.height),std::memory_order_relaxed);
    layer.drawableSize=drawable;
    if(sizeChanged)recomp_ios_layout_gen.fetch_add(1,std::memory_order_release);
    ds::gameX.store(float(t.picture.x));ds::gameY.store(float(t.picture.y));ds::gameW.store(float(t.picture.w));ds::gameH.store(float(t.picture.h));
    CGRect frame=CGRectZero;CGSize pixels=CGSizeZero;
    if(split) { // the lower screen: below the top area, the rest of the height
        frame=CGRectMake(0,area.h,width,height-area.h);
        const CGFloat ns=self.window?self.window.screen.nativeScale:UIScreen.mainScreen.nativeScale;
        pixels=CGSizeMake(std::round(frame.size.width*ns),std::round(frame.size.height*ns));
        lowerChanged=!CGRectEqualToRect(duoLowerView.frame,frame) || lower.contentsScale!=ns || !CGSizeEqualToSize(lower.drawableSize,pixels) || duoLowerView.hidden;
        duoLowerView.transform=CGAffineTransformIdentity;duoLowerView.frame=frame;duoLowerView.hidden=NO;
        lower.contentsScale=ns;lower.drawableSize=pixels;
        ds::lowerW.store(float(frame.size.width));ds::lowerH.store(float(frame.size.height));ds::lowerScale.store(float(ns));
        static void* retained=nullptr;
        if(!retained)retained=const_cast<void*>(CFBridgingRetain(lower)); // (the view lives for the process; one reference kept for the library)
        if(lowerChanged || !ds::lowerLayer.load(std::memory_order_acquire)) {
            ds::lowerLayer.store(retained,std::memory_order_release);ds::lowerGeneration.fetch_add(1,std::memory_order_acq_rel);lowerChanged=true;
        }
    }
    static ds::LayoutKey logged{};
    const auto key=ds::layoutKey(split,mode,width,height,t.picture,frame.size.width,frame.size.height,narrow);
    if(lowerChanged || !(key==logged)) {
        logged=key;
        std::fprintf(stderr,"APPLE_IOS_DUO_LAYOUT split=%d mode=%s frame=%s bounds=%.0fx%.0f game=%.0f,%.0f %.0fx%.0f visible=%.3f,%.3f lower=%.0fx%.0f pixels=%.0fx%.0f generation=%llu\n",
            split?1:0,ds::TopModeNames[mode],narrow?"4:3":"16:9",width,height,t.picture.x,t.picture.y,t.picture.w,t.picture.h,t.cropW,t.cropH,
            frame.size.width,frame.size.height,pixels.width,pixels.height,(unsigned long long)ds::lowerGeneration.load(std::memory_order_relaxed));
    }
    return YES;
}
#endif
- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (self.window) wasAttached=YES;
    else if (contract && wasAttached && !managedLifecycle) contract->close();
}
@end

namespace recomp::apple {
namespace {
constexpr std::uint32_t Width=1280, Height=720, Bytes=Width*Height*4;
bool combinedSubmissionMode() {
    const char* value=std::getenv("RECOMP_IOS_COMBINED_PRESENT");
    if(!value || std::strcmp(value,"1")==0)return true;
    if(std::strcmp(value,"0")==0)return false;
    throw std::runtime_error("invalid combined presentation mode");
}
bool compareSubmissionMode() {
    const char* value=std::getenv("RECOMP_IOS_PRESENT_COMPARE");
    if(!value)return false;
    if(std::strcmp(value,"1") || std::getenv("RECOMP_IOS_COMBINED_PRESENT"))
        throw std::runtime_error("invalid presenter comparison mode");
    return true;
}
void requireMain() {
    if (![NSThread isMainThread]) {
        std::fputs("APPLE_IOS_PRESENTER_FATAL non_main_thread=1\n",stderr);
        std::_Exit(4);
    }
}
}

struct IosFramePresenter::Impl {
    using ObserverGate=detail::IosPresenterObserverGate<Impl>;
    // Declaration order keeps the device alive through all resources' cleanup.
    recomp::gpu::ProbeDevice device;
    RgbaTransfer upload;
    detail::IosFrameContract contract;
    RecompIosFrameView* view=nil;
    CAMetalLayer* layer=nil;
    NSMutableArray* observers=nil;
    std::shared_ptr<ObserverGate> observerGate=std::make_shared<ObserverGate>(this);
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkSemaphore acquired=VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered;
    VkFence fence=VK_NULL_HANDLE;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    void* mapped=nullptr;
    std::vector<VkImage> images;
    std::vector<bool> initialized;
    bool ready=false;
    sharedpresent::Contract sharedContract;
    std::unique_ptr<sharedpresent::Objects> sharedObjects;
    bool managedLifecycle=false, suspended=false;
    std::uint32_t uploads=0;
    const bool compareSubmission=compareSubmissionMode();
    bool combinedSubmission=compareSubmission?false:combinedSubmissionMode();
    iospresenterphase::Schedule phaseSchedule;
    std::unique_ptr<performance::MeasurementClock> phaseClock=compareSubmission?std::make_unique<performance::MeasurementClock>():nullptr;

    explicit Impl(bool managed,std::shared_ptr<performance::DisplayRecording> recording={}) : managedLifecycle(managed) {
        view=[[RecompIosFrameView alloc] initWithFrame:CGRectZero displayRecording:bool(recording)];
        view->contract=&contract;
        view->managedLifecycle=managedLifecycle;
        layer=(CAMetalLayer*)view->metalView.layer;
        if(recording && !performance::configureDisplayLayer((__bridge void*)layer,recording)) {
            recording->registrationRejected();recording->close();
            throw std::runtime_error("display observation layer refused before surface creation");
        }
        std::fprintf(stderr,"APPLE_IOS_PRESENTER_SUBMISSION combined=%d exact_source=1 exact_destination=1\n",int(combinedSubmission));
        layer.device=MTLCreateSystemDefaultDevice();
        layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.framebufferOnly=NO;
        layer.drawableSize=CGSizeMake(Width,Height);
        // Closure is permanent. A future active notification never reopens V4.
        observers=[NSMutableArray new];
        // (Claude 2026-10-21, c485b) the app's active state for the stall watcher (ios_sampler.h): a lifecycle pause stops the
        // presents on purpose, and the watcher captured it as a hang. Published only; nothing here reads it.
        for (NSNotificationName name in @[UIApplicationWillResignActiveNotification,UIApplicationDidEnterBackgroundNotification,
                                          UIApplicationDidBecomeActiveNotification]) {
            const bool active=[name isEqualToString:UIApplicationDidBecomeActiveNotification];
            id token=[NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification*) {recomp_ios_app_inactive.store(active?0u:1u,std::memory_order_relaxed);}];
            [observers addObject:token];
        }
        const std::weak_ptr<ObserverGate> weak=observerGate;
        for (NSNotificationName name in @[UIApplicationWillResignActiveNotification,
                                          UIApplicationDidEnterBackgroundNotification,
                                          UISceneWillDeactivateNotification,
                                          UISceneDidEnterBackgroundNotification,
                                          UISceneDidDisconnectNotification]) {
            id token=[NSNotificationCenter.defaultCenter addObserverForName:name object:nil
                queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* notification) {
                    requireMain();
                    // No raw-this capture. A queued delivery after destruction
                    // may still execute, but cannot access the destroyed owner.
                    ObserverGate::deliver(weak,[&](Impl& owner) {
                        if ([notification.object isKindOfClass:UIScene.class] &&
                            notification.object!=owner.view.window.windowScene) return;
                        // Managed pause is owned by the scene coordinator.
                        // Independently latching an asynchronously delivered
                        // notification could suspend a newer resumed epoch.
                        if(!owner.managedLifecycle || [notification.name isEqualToString:UISceneDidDisconnectNotification])
                            owner.contract.close();
                    });
                }];
            [observers addObject:token];
        }
    }
    ~Impl() {
        requireMain();
        closeShared(false);
        observerGate->invalidate();
        for (id token in observers) [NSNotificationCenter.defaultCenter removeObserver:token];
        observerGate.reset();
        view->contract=nullptr;
        // Explicit destruction only. Normal suspend/resume retains the owner.
        // Exclude the lifecycle drainer before destroying any device resource.
        if(device.device && !iosGpuLifecycle().retireDevice(device.device)) {
            std::fputs("APPLE_IOS_LIFECYCLE_TEARDOWN_FAILED presenter_retire=1\n",stderr);std::_Exit(4);
        }
        // If the driver never becomes idle, the supervising process timeout is
        // the bound. Do not free memory still owned by an outstanding submission.
        if (device.device) {
            const auto idle=vkDeviceWaitIdle(device.device);
            if(managedLifecycle && idle!=VK_SUCCESS) {
                std::fputs("APPLE_IOS_LIFECYCLE_TEARDOWN_FAILED presenter_idle=1\n",stderr);std::_Exit(4);
            }
        }
        if (mapped) vkUnmapMemory(device.device,memory);
        if (buffer) vkDestroyBuffer(device.device,buffer,nullptr);
        if (memory) vkFreeMemory(device.device,memory,nullptr);
        if (fence) vkDestroyFence(device.device,fence,nullptr);
        if (acquired) vkDestroySemaphore(device.device,acquired,nullptr);
        for (auto semaphore:rendered) if (semaphore) vkDestroySemaphore(device.device,semaphore,nullptr);
        if (pool) vkDestroyCommandPool(device.device,pool,nullptr);
        if (swapchain) vkDestroySwapchainKHR(device.device,swapchain,nullptr);
        if (surface) vkDestroySurfaceKHR(device.instance,surface,nullptr);
        (void)performance::configureDisplayLayer((__bridge void*)layer,{});
        [view removeFromSuperview];
    }
    void closeShared(bool terminal) noexcept {
        sharedContract.close();
        if(sharedObjects)sharedObjects->close(terminal);
    }
    int sharedFail(const char* operation) noexcept {
        const int error=sharedObjects?int(sharedObjects->error()):int(VK_ERROR_UNKNOWN);
        iosfailure::capture(operation,error,contract.count());
        closeShared(true);contract.close();
        std::fprintf(stderr,"APPLE_IOS_SHARED_GPU_FATAL operation=%s vk=%d accepted=%u\n",operation,error,contract.count());
        return RECOMP_IOS_GPU_TERMINAL;
    }
    bool detachCpuSwapchain() {
        // No new surface is created until the prior one on this layer is gone.
        // Old device remains owned by ProbeDevice; shared device is never adopted.
        if(device.device) {
            auto operation=iosGpuLifecycle().acquire(device.queue);
            if(!operation || vkDeviceWaitIdle(device.device)!=VK_SUCCESS)return false;
            if(mapped){vkUnmapMemory(device.device,memory);mapped=nullptr;}
            if(buffer){vkDestroyBuffer(device.device,buffer,nullptr);buffer=VK_NULL_HANDLE;}
            if(memory){vkFreeMemory(device.device,memory,nullptr);memory=VK_NULL_HANDLE;}
            if(fence){vkDestroyFence(device.device,fence,nullptr);fence=VK_NULL_HANDLE;}
            if(acquired){vkDestroySemaphore(device.device,acquired,nullptr);acquired=VK_NULL_HANDLE;}
            for(auto sem:rendered)if(sem)vkDestroySemaphore(device.device,sem,nullptr);
            rendered.clear();images.clear();initialized.clear();
            if(pool){vkDestroyCommandPool(device.device,pool,nullptr);pool=VK_NULL_HANDLE;command=VK_NULL_HANDLE;}
            if(swapchain){vkDestroySwapchainKHR(device.device,swapchain,nullptr);swapchain=VK_NULL_HANDLE;}
            if(surface){vkDestroySurfaceKHR(device.instance,surface,nullptr);surface=VK_NULL_HANDLE;}
        }
        ready=false;return true;
    }
    int gpuFrame(const RecompIosGpuFrameV1& frame) {
        // The caller has already entered a one-shot ordered callback. No failure
        // below permits a CPU retry, including unsupported first-frame setup.
        if(!sharedContract.valid(frame)||suspended)return sharedFail("metadata-or-suspended");
        auto operation=iosGpuLifecycle().acquire(frame.queue);
        if(!operation)return sharedFail("lifecycle-admission");
        UIWindow* window=view.window;
        const bool active=UIApplication.sharedApplication.applicationState==UIApplicationStateActive &&
            window.windowScene.activationState==UISceneActivationStateForegroundActive;
        const bool attached=window && window.windowScene && !window.hidden && !view.hidden && !view->metalView.hidden &&
            view.bounds.size.width>0 && view.bounds.size.height>0 && view->metalView.bounds.size.width>0 && view->metalView.bounds.size.height>0;
        const bool exact=drawableMatchesLayout(layer) && layer.pixelFormat==MTLPixelFormatBGRA8Unorm && !layer.framebufferOnly; // (build 194)
        if(contract.begin(frame.sequence,frame.mappedRgba,Bytes,frame.width,frame.height,active,attached,exact)!=detail::IosFrameContract::Admission::Ready)return sharedFail("frame-contract");
        if(!sharedContract.committed()) {
            if(!detachCpuSwapchain())return sharedFail("old-swapchain-drain");
            sharedContract.adopt(frame); // Irreversible before shared surface creation.
            sharedObjects=std::make_unique<sharedpresent::Objects>();
            if(!sharedObjects->setup(frame.instance,frame.physicalDevice,frame.device,frame.queue,frame.queueFamily,(__bridge void*)layer))return sharedFail("adopt");
            std::fprintf(stderr,"APPLE_IOS_SHARED_GPU_ACTIVE epoch=%llu queue_family=%u source_readback=1 destination_readback=1 layout=%u\n",(unsigned long long)frame.deviceEpoch,frame.queueFamily,unsigned(frame.layout));
        }
        if(!sharedObjects->draw(frame.image,frame.view,frame.nativeLut,frame.mappedRgba)) {
            const bool accepted=sharedObjects->queueAccepted();
            contract.finish(accepted,accepted);
            return sharedFail(accepted?"failure-after-queue-acceptance":"draw-or-pixel-proof");
        }
        if(contract.finish(true,false)!=detail::IosFrameContract::Completion::Accepted)return sharedFail("acceptance");
        sharedContract.accepted(frame);
        std::printf("APPLE_IOS_FRAME_ACCEPTED sequence=%u pixels=%u exact_readback=1 physical_vblank_unmeasured=1 shared_gpu=1 generation=%llu source_frame=%llu\n",frame.sequence,Width*Height,(unsigned long long)frame.sourceGeneration,(unsigned long long)frame.sourceFrame);
        return RECOMP_IOS_GPU_ACCEPTED;
    }
    bool fail(const char* operation, VkResult result=VK_ERROR_UNKNOWN) {
        iosfailure::capture(operation,int(result),contract.count());
        contract.close();
        std::fprintf(stderr,"APPLE_IOS_PRESENTER_REJECT operation=%s vk=%d accepted=%u uploads=%u\n",
                     operation,int(result),contract.count(),uploads);
        return false;
    }
    bool good(VkResult result, const char* operation) {
        return result==VK_SUCCESS || fail(operation,result);
    }
    bool setup() {
        recomp::gpu::ShaderRequirements requirements;
        if (!device.inspect() || !device.create(requirements,true,true,true)) return fail("device");
        if (!upload.create(device.physical,device.device,device.queue,device.queueFamily,Width,Height)) return fail("upload-create");
        if (!good(createMetalSurface(device.instance,(__bridge void*)layer,&surface),"surface")) return false;
        VkBool32 supported=VK_FALSE;
        if (!good(vkGetPhysicalDeviceSurfaceSupportKHR(device.physical,device.queueFamily,surface,&supported),"surface-support") || !supported) return fail("queue-surface");
        VkSurfaceCapabilitiesKHR caps{};
        if (!good(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.physical,surface,&caps),"surface-caps")) return false;
        std::fprintf(stderr,"APPLE_IOS_SURFACE_EXTENT current=%ux%u min=%ux%u max=%ux%u bounds=%.3fx%.3f scale=%.3f drawable=%.3fx%.3f\n",
            caps.currentExtent.width,caps.currentExtent.height,caps.minImageExtent.width,caps.minImageExtent.height,
            caps.maxImageExtent.width,caps.maxImageExtent.height,layer.bounds.size.width,layer.bounds.size.height,
            layer.contentsScale,layer.drawableSize.width,layer.drawableSize.height);
        constexpr auto usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((caps.supportedUsageFlags&usage)!=usage || !(caps.supportedCompositeAlpha&VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) return fail("surface-usage");
        if ((caps.currentExtent.width!=UINT32_MAX && (caps.currentExtent.width!=Width || caps.currentExtent.height!=Height)) ||
            Width<caps.minImageExtent.width || Width>caps.maxImageExtent.width || Height<caps.minImageExtent.height || Height>caps.maxImageExtent.height) return fail("surface-extent");
        std::uint32_t count=0;
        if (!good(vkGetPhysicalDeviceSurfaceFormatsKHR(device.physical,surface,&count,nullptr),"surface-format-count")) return false;
        std::vector<VkSurfaceFormatKHR> formats(count);
        if (!good(vkGetPhysicalDeviceSurfaceFormatsKHR(device.physical,surface,&count,formats.data()),"surface-formats")) return false;
        bool found=false;
        for (const auto& f:formats) if (f.format==VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) found=true;
        if (!found) return fail("surface-format");
        VkFormatProperties src{},dst{};
        vkGetPhysicalDeviceFormatProperties(device.physical,VK_FORMAT_R8G8B8A8_UNORM,&src);
        vkGetPhysicalDeviceFormatProperties(device.physical,VK_FORMAT_B8G8R8A8_UNORM,&dst);
        if (!(src.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(dst.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT)) return fail("blit-format");
        VkSwapchainCreateInfoKHR swap{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swap.surface=surface;swap.minImageCount=caps.minImageCount+1;
        if (caps.maxImageCount && swap.minImageCount>caps.maxImageCount) swap.minImageCount=caps.maxImageCount;
        swap.imageFormat=VK_FORMAT_B8G8R8A8_UNORM;swap.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        swap.imageExtent={Width,Height};swap.imageArrayLayers=1;swap.imageUsage=usage;
        swap.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;swap.preTransform=caps.currentTransform;
        swap.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;swap.presentMode=VK_PRESENT_MODE_FIFO_KHR;swap.clipped=VK_TRUE;
        if (!good(vkCreateSwapchainKHR(device.device,&swap,nullptr,&swapchain),"swapchain")) return false;
        if (!good(vkGetSwapchainImagesKHR(device.device,swapchain,&count,nullptr),"image-count")) return false;
        images.resize(count);initialized.resize(count,false);rendered.resize(count,VK_NULL_HANDLE);
        if (!good(vkGetSwapchainImagesKHR(device.device,swapchain,&count,images.data()),"images")) return false;
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!good(vkCreateSemaphore(device.device,&sem,nullptr,&acquired),"acquire-semaphore")) return false;
        // Render-finished semaphores belong to images: reacquisition establishes
        // that the previous presentation wait on that image has completed.
        for (auto& semaphore:rendered) if (!good(vkCreateSemaphore(device.device,&sem,nullptr,&semaphore),"render-semaphore")) return false;
        VkBufferCreateInfo buf{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buf.size=Bytes;buf.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;buf.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if (!good(vkCreateBuffer(device.device,&buf,nullptr,&buffer),"readback-buffer")) return false;
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device.device,buffer,&req);
        VkPhysicalDeviceMemoryProperties types{};vkGetPhysicalDeviceMemoryProperties(device.physical,&types);
        std::uint32_t type=UINT32_MAX;
        constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (unsigned i=0;i<types.memoryTypeCount;++i) if ((req.memoryTypeBits&(1u<<i)) && (types.memoryTypes[i].propertyFlags&flags)==flags) {type=i;break;}
        if (type==UINT32_MAX) return fail("readback-memory-type");
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;alloc.memoryTypeIndex=type;
        if (!good(vkAllocateMemory(device.device,&alloc,nullptr,&memory),"readback-memory") ||
            !good(vkBindBufferMemory(device.device,buffer,memory,0),"readback-bind") ||
            !good(vkMapMemory(device.device,memory,0,VK_WHOLE_SIZE,0,&mapped),"readback-map")) return false;
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=device.queueFamily;pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (!good(vkCreateCommandPool(device.device,&pc,nullptr,&pool),"command-pool")) return false;
        VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cb.commandPool=pool;cb.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cb.commandBufferCount=1;
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!good(vkAllocateCommandBuffers(device.device,&cb,&command),"command-buffer") ||
            !good(vkCreateFence(device.device,&fc,nullptr,&fence),"fence")) return false;
        ready=true;
        return true;
    }
    const bool timingEnabled=presenter_timing::enabled();
    std::unique_ptr<performance::MeasurementClock> timingClock=timingEnabled?std::make_unique<performance::MeasurementClock>():nullptr;
    static bool readTiming(void*ctx,std::uint64_t&ns) noexcept {const auto s=static_cast<performance::MeasurementClock*>(ctx)->read(false);ns=s.ns;return s.timeInvalid==performance::Invalid::None;}
    bool frame(std::uint32_t sequence, const std::uint8_t* rgba) {
        presenter_timing::Snapshot timing(sequence,timingEnabled?&readTiming:nullptr,timingClock.get());
        presenter_timing::Partial partial{timing};
        // Main never waits at the lifecycle gate. Its owner defers this entire
        // callback off-main until the same ordered frame can be presented.
        auto operation=timing.measure(presenter_timing::Phase::Admission,[&]{return iosGpuLifecycle().acquire(device.queue);});
        if(!operation)return fail("lifecycle-admission");
        if(compareSubmission) {
            const auto stamp=phaseClock->read(false);
            if(stamp.timeInvalid!=performance::Invalid::None)return fail("comparison-clock");
            const auto phase=phaseSchedule.observe(stamp.ns);
            if(!phase.valid)return fail("comparison-clock-order");
            combinedSubmission=phase.mode;
            if(phase.changed)std::fprintf(stderr,"APPLE_IOS_PRESENTER_PHASE phase=%u combined=%d sequence=%u time_ns=%llu frozen=%d\n",
                phase.phase,int(phase.mode),sequence,static_cast<unsigned long long>(stamp.ns),int(phase.frozen));
        }
        ++uploads;
        if (!combinedSubmission && upload.uploadAndVerify(rgba,Bytes,timing.active()?&timing:nullptr)!=RgbaTransfer::Result::Verified) return fail("source-pixel-readback");
        std::uint32_t index=0;
        if (!good(timing.measure(presenter_timing::Phase::Acquire,[&]{return vkAcquireNextImageKHR(device.device,swapchain,2'000'000'000ULL,acquired,VK_NULL_HANDLE,&index);}),"acquire")) return false;
        if (index>=images.size()) return fail("acquire-index");
        if (!good(timing.measure(presenter_timing::Phase::Record,[&]{return vkResetCommandBuffer(command,0);}),"command-reset")) return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!good(timing.measure(presenter_timing::Phase::Record,[&]{return vkBeginCommandBuffer(command,&begin);}),"command-begin")) return false;
        if (combinedSubmission && timing.measure(presenter_timing::Phase::Record,[&]{return upload.recordUpload(command,rgba,Bytes);})!=RgbaTransfer::Result::Recorded) return fail("source-upload-record",upload.error());
        timing.measure(presenter_timing::Phase::Record,[&]{
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout=initialized[index]?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=images[index];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource=blit.srcSubresource;
        blit.srcOffsets[1]={int(Width),int(Height),1};blit.dstOffsets[1]=blit.srcOffsets[1];
        vkCmdBlitImage(command,upload.image(),upload.imageLayout(),images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={Width,Height,1};
        vkCmdCopyImageToBuffer(command,images[index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=0;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        });
        if (!good(timing.measure(presenter_timing::Phase::Record,[&]{return vkEndCommandBuffer(command);}),"command-end")) return false;
        VkPipelineStageFlags wait=VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&acquired;submit.pWaitDstStageMask=&wait;
        submit.commandBufferCount=1;submit.pCommandBuffers=&command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&rendered[index];
        if (combinedSubmission) {
            if (upload.submitAndVerify(submit,fence,timing.active()?&timing:nullptr)!=RgbaTransfer::Result::Verified) return fail("combined-source-pixel-readback",upload.error());
        } else {
            if (!good(timing.measure(presenter_timing::Phase::Submit,[&]{return vkResetFences(device.device,1,&fence);}),"fence-reset") ||
                !good(timing.measure(presenter_timing::Phase::Submit,[&]{return vkQueueSubmit(device.queue,1,&submit,fence);}),"queue-submit") ||
                !good(timing.measure(presenter_timing::Phase::Fence,[&]{return vkWaitForFences(device.device,1,&fence,VK_TRUE,5'000'000'000ULL);}),"render-fence")) return false;
        }
        auto* pixels=static_cast<std::uint8_t*>(mapped);
#if defined(APPLE_IOS_PRESENTER_MUTANT_DESTINATION)
        pixels[0]^=1;
#endif
        if (!timing.measure(presenter_timing::Phase::DestinationCompare,[&]{return exactBgraEqualsRgba(pixels,Bytes,rgba,Bytes);})) return fail("destination-pixel-readback");
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&rendered[index];
        present.swapchainCount=1;present.pSwapchains=&swapchain;present.pImageIndices=&index;
        const auto result=timing.measure(presenter_timing::Phase::Present,[&]{return vkQueuePresentKHR(device.queue,&present);});
        // SUBOPTIMAL already accepts the image. Never return a false callback
        // rejection for an accepted image, including any later wait failure.
        const bool acceptedByQueue=result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR;
        if (!acceptedByQueue) {contract.finish(false,false);return fail("queue-present",result);}
        const auto idle=timing.measure(presenter_timing::Phase::Idle,[&]{return vkQueueWaitIdle(device.queue);});
#if defined(APPLE_IOS_PRESENTER_MUTANT_LATE_FAILURE)
        const bool lateFailure=true;
#else
        const bool lateFailure=result==VK_SUBOPTIMAL_KHR || idle!=VK_SUCCESS;
#endif
        if (contract.finish(true,lateFailure)!=detail::IosFrameContract::Completion::Accepted) {
            timing.write(stderr,"APPLE_IOS_PRESENTER_TIMING_PARTIAL","accepted-queue-fatal");
            std::fprintf(stderr,"APPLE_IOS_PRESENTER_FATAL after_acceptance=1 vk=%d present=%d\n",int(idle),int(result));std::_Exit(4);
        }
        initialized[index]=true;
        if(timing.active()){timing.finish();char prefix[256];std::snprintf(prefix,sizeof(prefix),"APPLE_IOS_FRAME_ACCEPTED sequence=%u pixels=%u exact_readback=1 physical_vblank_unmeasured=1 combined=%u linear=%u layout=%u",sequence,Width*Height,unsigned(combinedSubmission),unsigned(upload.linearHostUpload()),unsigned(upload.imageLayout()));timing.write(stdout,prefix,"accepted");}
        else std::printf("APPLE_IOS_FRAME_ACCEPTED sequence=%u pixels=%u exact_readback=1 physical_vblank_unmeasured=1\n",sequence,Width*Height);
        return true;
    }
};

IosFramePresenter::IosFramePresenter(bool managedLifecycle) {requireMain();impl_=std::make_unique<Impl>(managedLifecycle);}
IosFramePresenter::IosFramePresenter(bool managedLifecycle,std::shared_ptr<performance::DisplayRecording> recording) {
    requireMain();impl_=std::make_unique<Impl>(managedLifecycle,std::move(recording));
}
IosFramePresenter::~IosFramePresenter() {requireMain();}
void* IosFramePresenter::nativeView() const noexcept {requireMain();return (__bridge void*)impl_->view;}
bool IosFramePresenter::present(std::uint32_t sequence, const std::uint8_t* rgba, std::uint32_t bytes, std::uint32_t width, std::uint32_t height) {
    requireMain();
    @autoreleasepool {
        auto& state=*impl_;
        if(state.sharedContract.committed()){state.sharedFail("cpu-after-shared-adoption");return false;}
        if(state.suspended)return false;
        UIWindow* window=state.view.window;
        const bool active=UIApplication.sharedApplication.applicationState==UIApplicationStateActive &&
            window.windowScene.activationState==UISceneActivationStateForegroundActive;
        const bool attached=window && window.windowScene && !window.hidden &&
            !state.view.hidden && !state.view->metalView.hidden &&
            state.view.bounds.size.width>0 && state.view.bounds.size.height>0 &&
            state.view->metalView.bounds.size.width>0 && state.view->metalView.bounds.size.height>0;
        const bool exact=drawableMatchesLayout(state.layer) && // (build 194)
            state.layer.pixelFormat==MTLPixelFormatBGRA8Unorm && !state.layer.framebufferOnly;
        const auto admission=state.contract.begin(sequence,rgba,bytes,width,height,active,attached,exact);
        if (admission!=detail::IosFrameContract::Admission::Ready) {
            if (admission==detail::IosFrameContract::Admission::Sequence) return state.fail("sequence");
            if (admission==detail::IosFrameContract::Admission::Unavailable) return state.fail("inactive-detached-or-drawable");
            return false;
        }
        if (!state.ready && !state.setup()) return false;
        // A UIKit resize retains the fixed source/drawable size. Refuse a driver
        // extent change before uploading instead of silently stretching a blit.
        VkSurfaceCapabilitiesKHR caps{};
        if (!state.good(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(state.device.physical,state.surface,&caps),"frame-surface-caps")) return false;
        if (caps.currentExtent.width!=UINT32_MAX &&
            (caps.currentExtent.width!=Width || caps.currentExtent.height!=Height)) return state.fail("frame-surface-extent");
        return state.frame(sequence,rgba);
    }
}
int IosFramePresenter::callback(void* user, std::uint32_t sequence, const std::uint8_t* rgba,
                                std::uint32_t bytes, std::uint32_t width, std::uint32_t height) noexcept {
    requireMain();
    if (!user) return 0;
    try {return static_cast<IosFramePresenter*>(user)->present(sequence,rgba,bytes,width,height)?1:0;}
    catch (...) {std::fputs("APPLE_IOS_PRESENTER_FATAL exception=1\n",stderr);std::_Exit(4);}
}
int IosFramePresenter::presentGpu(const RecompIosGpuFrameV1& frame) {
    requireMain();
    @autoreleasepool {
        try{return impl_->gpuFrame(frame);}
        catch(...){return impl_->sharedFail("exception");}
    }
}
void IosFramePresenter::closeGpu(bool terminal) {requireMain();impl_->closeShared(terminal);}
void IosFramePresenter::close() {requireMain();stopDisplayObservation();impl_->contract.close();}
void IosFramePresenter::stopDisplayObservation() {
    requireMain();(void)performance::configureDisplayLayer((__bridge void*)impl_->layer,{});
}
bool IosFramePresenter::suspend() {
    requireMain();
    if(!impl_->managedLifecycle || !impl_->contract.healthy())return false;
    impl_->suspended=true;
    return true;
}
bool IosFramePresenter::resume() {
    requireMain();
    auto& state=*impl_;
    if(!state.managedLifecycle || !state.contract.healthy())return false;
    [state.view layoutIfNeeded];
    UIWindow* window=state.view.window;
    const bool active=UIApplication.sharedApplication.applicationState==UIApplicationStateActive &&
        window.windowScene.activationState==UISceneActivationStateForegroundActive;
    const bool attached=window && window.windowScene && !window.hidden &&
        !state.view.hidden && !state.view->metalView.hidden &&
        state.view.bounds.size.width>0 && state.view.bounds.size.height>0 &&
        state.view->metalView.bounds.size.width>0 && state.view->metalView.bounds.size.height>0;
    const bool exact=drawableMatchesLayout(state.layer) && // (build 194)
        state.layer.pixelFormat==MTLPixelFormatBGRA8Unorm && !state.layer.framebufferOnly;
    if(!active || !attached)return false;
    // An unavailable scene may return, but an attached incompatible drawable
    // is not a lifecycle wait. Preserve the exact-format contract and fail it.
    if(!exact)return state.fail("resume-drawable");
    state.suspended=false;
    return true;
}
bool IosFramePresenter::activeAndAttached() const noexcept {
    requireMain();
    const auto& state=*impl_;
    UIWindow* window=state.view.window;
    return UIApplication.sharedApplication.applicationState==UIApplicationStateActive &&
        window && window.windowScene && !window.hidden &&
        window.windowScene.activationState==UISceneActivationStateForegroundActive &&
        !state.view.hidden && !state.view->metalView.hidden &&
        state.view.bounds.size.width>0 && state.view.bounds.size.height>0 &&
        state.view->metalView.bounds.size.width>0 && state.view->metalView.bounds.size.height>0;
}
bool IosFramePresenter::healthy() const noexcept {requireMain();return impl_->contract.healthy();}
std::uint32_t IosFramePresenter::count() const noexcept {requireMain();return impl_->contract.count();}
std::uint32_t IosFramePresenter::uploadCount() const noexcept {requireMain();return impl_->uploads;}
}

// Internal optional diagnostic bridge only. No presenter/UI lookup or dispatch:
// renderer caller may hold its own lock, but has left all queue wrapper RAII.
extern "C" void recomp_ios_capture_renderer_failure(const char* operation,int result,std::uint64_t frames) noexcept {
    iosfailure::capture(operation,result,frames,true);
}
namespace {
// Process-lifetime function only: no presenter instance, UI access or teardown race.
const bool rendererFailureCaptureInstalled=[]() noexcept {
    recomp::gpu::rendererfailure::capture.store(&recomp_ios_capture_renderer_failure,std::memory_order_release);
    return true;
}();
}
