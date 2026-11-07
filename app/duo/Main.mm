// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <cstdio>
#include "ios_gpu_frame_bridge.h"
// Development shell for the decoder-enabled port. Start is explicit UI or a
// one-shot recorded launch argument, gated on full-catalog + fresh-request
// authentication. No bot, default autostart/retry or save manipulation.
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
// ios-mtl-audit (Claude 2026-10-17, MEASUREMENT ONLY: RECOMP_IOS_MTL_AUDIT=1): the Metal texture behind a Vulkan image, as Metal
// decides lossless compression from it (private storage, GPU-optimized contents, no pixel-format-view / shader-write usage).
// One APPLE_IOS_MTL line per distinct (size, format, usage, storage, compression) combination; generic_renderer.cpp calls it.
extern "C" void vkGetMTLTextureMVK(void* image, id<MTLTexture>* texture);
// ios-tile-bench (Claude 2026-10-17, MEASUREMENT ONLY: RECOMP_IOS_TILE_BENCH=1, runs once at process start, before the game).
// Question (Fable blocker packet, Option B): what does a resolve cost when the target stays in tile memory (a tile shader writes
// the colour attachment to the destination texture inside the pass) versus today's pattern (end the pass, store, blit-copy,
// re-begin with load)? 1280x768 RGBA8 + D32S8 like the game's full-size targets; K resolves per frame, each after one
// full-screen draw; F frames; GPU time from MTLCommandBuffer GPUStart/EndTime. Lines: APPLE_IOS_TILE_BENCH.
namespace {
static NSString* const kTileBenchSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; };
vertex VOut tbVs(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2); VOut o; o.pos = float4(p * 2.0 - 1.0, 0.5, 1.0); return o; }
fragment half4 tbFs(VOut in [[stage_in]], constant float4& c [[buffer(0)]]) { return half4(c) * half(0.5 + 0.5 * fract(in.pos.x * 0.01)); }
struct Px { half4 color [[color(0)]]; };
struct FbIn { half4 c0 [[color(0)]]; };
struct FbOut { half4 c0 [[color(0)]]; half4 c1 [[color(1)]]; };
fragment FbOut tbFetchCopy(VOut in [[stage_in]], FbIn f) { FbOut o; o.c0 = f.c0; o.c1 = f.c0; return o; }
kernel void tbTileResolve(imageblock<Px, imageblock_layout_implicit> blk, texture2d<half, access::write> dst [[texture(0)]],
                          ushort2 lid [[thread_position_in_threadgroup]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x < dst.get_width() && gid.y < dst.get_height()) dst.write(blk.read(lid).color, gid);
}
)MSL";
static double tbMedian(std::vector<double> v) { if (v.empty()) return -1; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
static void tileBench() {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); if (!dev) { std::fprintf(stderr, "APPLE_IOS_TILE_BENCH no-device\n"); return; }
    NSError* err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:kTileBenchSource options:nil error:&err];
    if (!lib) { std::fprintf(stderr, "APPLE_IOS_TILE_BENCH library-failed %s\n", err.localizedDescription.UTF8String); return; }
    const NSUInteger W = 1280, H = 768; const int K = 24, F = 24;
    MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; cd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> color = [dev newTextureWithDescriptor:cd];
    MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:W height:H mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [dev newTextureWithDescriptor:dd];
    NSMutableArray<id<MTLTexture>>* dstA = [NSMutableArray array]; NSMutableArray<id<MTLTexture>>* dstB = [NSMutableArray array];
    for (int i = 0; i < K; ++i) {
        MTLTextureDescriptor* t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
        t.storageMode = MTLStorageModePrivate; t.usage = MTLTextureUsageShaderRead; [dstA addObject:[dev newTextureWithDescriptor:t]];
        t.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; [dstB addObject:[dev newTextureWithDescriptor:t]];
    }
    MTLRenderPipelineDescriptor* rp = [MTLRenderPipelineDescriptor new];
    rp.vertexFunction = [lib newFunctionWithName:@"tbVs"]; rp.fragmentFunction = [lib newFunctionWithName:@"tbFs"];
    rp.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; rp.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    rp.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rp error:&err];
    MTLTileRenderPipelineDescriptor* tp = [MTLTileRenderPipelineDescriptor new];
    tp.tileFunction = [lib newFunctionWithName:@"tbTileResolve"]; tp.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    tp.threadgroupSizeMatchesTileSize = YES;
    id<MTLRenderPipelineState> tile = tp.tileFunction ? [dev newRenderPipelineStateWithTileDescriptor:tp options:0 reflection:nil error:&err] : nil;
    if (!pso || !tile) { std::fprintf(stderr, "APPLE_IOS_TILE_BENCH pipeline-failed %s\n", err ? err.localizedDescription.UTF8String : "?"); return; }
    // variant C: the resolve as a framebuffer-fetch draw into a second colour attachment (the destination), stored at pass end
    MTLRenderPipelineDescriptor* rp2 = [MTLRenderPipelineDescriptor new];
    rp2.vertexFunction = rp.vertexFunction; rp2.fragmentFunction = rp.fragmentFunction;
    rp2.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; rp2.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA8Unorm;
    rp2.colorAttachments[1].writeMask = MTLColorWriteMaskNone;
    rp2.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8; rp2.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLRenderPipelineState> pso2 = [dev newRenderPipelineStateWithDescriptor:rp2 error:&err];
    MTLRenderPipelineDescriptor* rp3 = [rp2 copy]; rp3.fragmentFunction = [lib newFunctionWithName:@"tbFetchCopy"];
    rp3.colorAttachments[1].writeMask = MTLColorWriteMaskAll;
    id<MTLRenderPipelineState> fetch = [dev newRenderPipelineStateWithDescriptor:rp3 error:&err];
    NSMutableArray<id<MTLTexture>>* dstC = [NSMutableArray array];
    for (int i = 0; i < K; ++i) {
        MTLTextureDescriptor* t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
        t.storageMode = MTLStorageModePrivate; t.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; [dstC addObject:[dev newTextureWithDescriptor:t]];
    }
    if (!pso2 || !fetch) { std::fprintf(stderr, "APPLE_IOS_TILE_BENCH fetch-pipeline-failed %s\n", err ? err.localizedDescription.UTF8String : "?"); return; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    auto passDesc = [&](MTLLoadAction load) {
        MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = color; d.colorAttachments[0].loadAction = load; d.colorAttachments[0].storeAction = MTLStoreActionStore;
        d.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        d.depthAttachment.texture = depth; d.depthAttachment.loadAction = load; d.depthAttachment.storeAction = MTLStoreActionStore;
        d.stencilAttachment.texture = depth; d.stencilAttachment.loadAction = load; d.stencilAttachment.storeAction = MTLStoreActionStore;
        return d;
    };
    auto draw = [&](id<MTLRenderCommandEncoder> e, int i) {
        [e setRenderPipelineState:pso]; float c[4] = {float(i % 7) / 7.f, float(i % 5) / 5.f, float(i % 3) / 3.f, 1.f};
        [e setFragmentBytes:c length:sizeof c atIndex:0]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    };
    auto timed = [&](void (^body)(id<MTLCommandBuffer>)) {
        id<MTLCommandBuffer> cb = [q commandBuffer]; body(cb); [cb commit]; [cb waitUntilCompleted];
        return (cb.GPUEndTime - cb.GPUStartTime) * 1e3;
    };
    std::vector<double> a, a2, b, base, cTimes;
    for (int f = 0; f < F + 4; ++f) {
        // A: today's pattern - K x (pass with load, one draw, store; blit copy of the colour = the resolve)
        const double ta = timed(^(id<MTLCommandBuffer> cb) {
            for (int i = 0; i < K; ++i) {
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:passDesc(i ? MTLLoadActionLoad : MTLLoadActionClear)];
                draw(e, i); [e endEncoding];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl copyFromTexture:color toTexture:dstA[i]]; [bl endEncoding];
            }
        });
        // A2: the pass splits alone (store/load), no copy
        const double ta2 = timed(^(id<MTLCommandBuffer> cb) {
            for (int i = 0; i < K; ++i) {
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:passDesc(i ? MTLLoadActionLoad : MTLLoadActionClear)];
                draw(e, i); [e endEncoding];
            }
        });
        // B: one pass; each resolve is a tile dispatch writing the colour from tile memory to its texture
        const double tb = timed(^(id<MTLCommandBuffer> cb) {
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:passDesc(MTLLoadActionClear)];
            for (int i = 0; i < K; ++i) {
                draw(e, i);
                [e setRenderPipelineState:tile]; [e setTileTexture:dstB[i] atIndex:0];
                [e dispatchThreadsPerTile:MTLSizeMake(e.tileWidth, e.tileHeight, 1)];
            }
            [e endEncoding];
        });
        // C1: K passes, each = draw + fetch-copy into a second attachment (its destination), stored at pass end (no blit)
        const double tc = timed(^(id<MTLCommandBuffer> cb) {
            for (int i = 0; i < K; ++i) {
                MTLRenderPassDescriptor* d = passDesc(i ? MTLLoadActionLoad : MTLLoadActionClear);
                d.colorAttachments[1].texture = dstC[i]; d.colorAttachments[1].loadAction = MTLLoadActionDontCare;
                d.colorAttachments[1].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
                [e setRenderPipelineState:pso2]; float c[4] = {float(i % 7) / 7.f, 0.3f, 0.6f, 1.f};
                [e setFragmentBytes:c length:sizeof c atIndex:0]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e setRenderPipelineState:fetch]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e endEncoding];
            }
        });
        cTimes.push_back(tc);
        // base: one pass, K draws, no resolves (the draw work alone)
        const double t0 = timed(^(id<MTLCommandBuffer> cb) {
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:passDesc(MTLLoadActionClear)];
            for (int i = 0; i < K; ++i) draw(e, i);
            [e endEncoding];
        });
        if (f >= 4) { a.push_back(ta); a2.push_back(ta2); b.push_back(tb); base.push_back(t0); }
    }
    if (cTimes.size() > 4) cTimes.erase(cTimes.begin(), cTimes.begin() + 4);
    const double ma = tbMedian(a), ma2 = tbMedian(a2), mb = tbMedian(b), m0 = tbMedian(base), mc = tbMedian(cTimes);
    std::fprintf(stderr, "APPLE_IOS_TILE_BENCH variant=C fetch_copy_store gpu_ms=%.3f per_resolve_us=%.1f saving_vs_today_us=%.1f\n",
        mc, (mc - m0) * 1e3 / K, (ma - mc) * 1e3 / K);
    std::fprintf(stderr, "APPLE_IOS_TILE_BENCH W=%lu H=%lu K=%d F=%d gpu_ms today=%.3f splits_only=%.3f tile=%.3f draws_only=%.3f "
        "per_resolve_us today=%.1f tile=%.1f saving=%.1f\n", (unsigned long)W, (unsigned long)H, K, F, ma, ma2, mb, m0,
        (ma - m0) * 1e3 / K, (mb - m0) * 1e3 / K, (ma - mb) * 1e3 / K);
}
struct TileBenchAtStart { TileBenchAtStart() { if (std::getenv("RECOMP_IOS_TILE_BENCH")) @autoreleasepool { tileBench(); } } };
static TileBenchAtStart tileBenchAtStart;
}
extern "C" void recomp_ios_mtl_audit(void* vkImage, const char* tag, unsigned vkFormat, unsigned vkUsage) {
    if (!vkImage) return;
    id<MTLTexture> t = nil; vkGetMTLTextureMVK(vkImage, &t);
    if (!t) { std::fprintf(stderr, "APPLE_IOS_MTL tag=%s no-texture\n", tag); return; }
    static std::mutex m; static std::map<std::string, unsigned> seen;
    char key[256];
    unsigned comp = 0;
    if (@available(iOS 15.0, *)) comp = unsigned(t.compressionType);
    std::snprintf(key, sizeof key, "%s %lux%lu vkf=%u vku=%x mtlf=%lu usage=%lx storage=%lu gpuopt=%u comp=%u heap=%u fbonly=%u view=%u",
        tag, (unsigned long)t.width, (unsigned long)t.height, vkFormat, vkUsage, (unsigned long)t.pixelFormat, (unsigned long)t.usage,
        (unsigned long)t.storageMode, unsigned(t.allowGPUOptimizedContents), comp, unsigned(t.heap != nil), unsigned(t.framebufferOnly),
        unsigned(t.parentTexture != nil));
    std::lock_guard<std::mutex> g(m);
    if (++seen[key] == 1) std::fprintf(stderr, "APPLE_IOS_MTL %s\n", key);
}
#include <atomic>
extern "C" std::atomic<std::uint64_t> recomp_ios_main_tid; // (Claude 2026-10-17; defined in trace_runner.cpp)
extern "C" std::atomic<unsigned> recomp_ios_main_port; // (the main guest thread's host mach port; trace_runner.cpp)
extern "C" std::atomic<int> recomp_ios_drawable_half;
extern "C" std::atomic<int> recomp_ios_settings_upscaler; // (generic_renderer.cpp; set by direct_present.h)
extern "C" std::atomic<int> recomp_ios_render_sixteenths; // (generic_renderer.cpp: the applied render scale, 16 = 100 %)
extern "C" std::atomic<int> recomp_ios_screen_fill; // (Claude 2026-10-20; ios-screen-fit A/B: generic_renderer.cpp; read by ios_layer_geometry.h)
extern "C" std::uint32_t recomp_apple_launcher_display_options() noexcept; // (the launcher's Screen row: Fit, bit 21 of the saved word)
// ios-overlay (Claude 2026-10-21): views drawn over the game layer cost GPU time every refresh (c379/c380, in-run A/B both orders:
// the renderer's GPU time per frame -1.1 / -1.6 ms with the touch controls and the status bar hidden; real frames not resolvably
// faster, -0.29 +- 0.25 ms). So in gameplay the status bar is hidden (iosStatusHidden, set at the first check after the game
// starts), and the touch controls hide while a physical game controller is connected (not Apple's virtual one) or while
// files/private-trace/ios-overlay-hide (or a RECOMP_IOS_SWITCHES_* list naming it) is present (iosOverlayHidden: the touch layer).
static std::atomic<int> iosOverlayHidden{0},iosStatusHidden{0};
namespace recomp::bootstrap { std::uint32_t currentGuestThreadToken() noexcept; } // (guest_cooperative_executor.cpp)
#include "AuthenticatedPort.hpp"
#include "PreparationTask.hpp"
#include "VerificationTask.hpp"
#include "ClockReadiness.hpp"
#include "ExplicitStart.hpp"
#include "VirtualControllerStart.hpp"
#include "RecordedVirtualInput.hpp"
#include "BoundedMetalCapture.hpp"
#include "OrderedFrameWait.hpp"
#include "PortLifecycle.hpp"
#include "apple/data_root.h"
#include "apple/gamepad_input.h"
#include "apple/gamepad_callbacks.h"
#include "apple/ios_frame_presenter.h"
#include "apple/storage_space.h"
#include "LauncherCoordinator.h"
#include "FullTouchControls.hpp"
#include "FullTouchInput.hpp"
#include "ios_qol_touch_view.h" // ios-qol (iPad 231): long presses on the game picture (android-qol v700 rules); ios-touchpad (235): the pad
extern "C" int recomp_touch_context_v1(void); // ios-touchpad (iPad 235): trace_runner.cpp, touch_context.h
#include "TouchFrontendRoute.hpp"
#include "AgentEnvironment.hpp"
#include "AgentGuestEnvironment.hpp"
#include "AgentMenuDelivery.hpp"
#include "AgentMenuHandoff.hpp"
#include "AgentObserverRegistration.hpp"
#include "AgentCapture.hpp"
#include "AgentRequest.hpp"
#include "AgentRunAuthorization.hpp"
#include "AgentDiscardStream.hpp"
#include "LauncherInputBridge.h"
#include "LauncherConfiguration.hpp"
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include "ios_sampler.h" // (Claude) measurement-only CPU sampler, RECOMP_IOS_SAMPLER
#include <pthread/qos.h>
#include "ios_switches.h" // (Claude) launch-environment switch overrides (RECOMP_IOS_SWITCHES_*); also used by ios-qos below
#include "PerformanceMeasurement.hpp"
#include "MeasuredOrderedFrame.hpp"
#include "native_xma_frame_decoder.h"
#include "guest_timebase.h"
#include "private_trace_api.h"
#include <cstdio>
#include <chrono>
#include <memory>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>
#include "ios_bot_commit.hpp"
#include "ios_failure_capture.hpp"
#include "ios_measurement_deadline.hpp"
#include "ios_mvk_policy.hpp"
#include "ios_renderer_qos.hpp"
#include "ios_async_worker_qos.hpp"
#include "save_sandbox.h"
#if defined(RECOMP_DUO_EDITION)
#include "duo_app.h" // ios-duo: the lower screen, touch, the toggle
#endif

namespace {
using namespace recomp::apple;
struct Session {
    IosPreparationDiscipline discipline;
    ExplicitStart explicitStart;
    VirtualControllerStart virtualStart;
    GCVirtualController* virtualController=nil;
    bool recordedInputRequested=false;
    bool launcherDiagnostic=false;
    bool recordedAimRequested=false;
    bool metalCaptureRequested=false;
    bool resumableLifecycleRequested=false;
    std::unique_ptr<RecordedVirtualInput> recordedInput;
    std::unique_ptr<BoundedMetalCapture> metalCapture;
    NSTimer* inputTimer=nil;
    NSMutableArray* audioDiagnostics=nil;
    std::mutex inputLogMutex;
    int lastInputStatus=-2;
    std::uint32_t lastInputPacket=0;
    std::unique_ptr<IosFramePresenter> presenter;
    std::shared_ptr<performance::MeasurementSession> measurement;
    const dispatch_time_t measurementDeadline=dispatch_time(DISPATCH_TIME_NOW,120*NSEC_PER_SEC);
    std::shared_ptr<performance::DisplayRecording> displayRecording;
    const performance::ClockStamp followupSessionStart=performance::MeasurementClock{}.read(false);
    std::shared_ptr<performance::DisplayFollowupControl> displayFollowupControl;
    std::unique_ptr<PortLifecycle> lifecycle;
    std::unique_ptr<AppleGamepadInput> input;
    std::unique_ptr<fulltouch::Gamepad> fullTouchInput;
    PortFullTouchControls* fullTouchControls=nil;
    PortQolTouchView* qolTouch=nil; // ios-qol (iPad 231): below fullTouchControls
    std::string root;
    std::string botSession;
    bool botStartAttempted=false;
    std::unique_ptr<iosbot::CommitPublisher> botPublisher;
    NSTimer* botTimer=nil;
    NSTimer* launcherCommandTimer=nil; // (Claude) launcher-hold-<session>: the host's launcher commands, one at a time
    unsigned long launcherCommandSeq=0,launcherCommandPolls=0,launcherCommandReads=0;
    std::string launcherCommandText;
    bool rendererQosRequested=false;
    bool asyncWorkerQosRequested=false;
    NSTimer* rendererQosTimer=nil;
    std::vector<std::string> arguments;
    std::vector<char*> pointers;
    RecompPrivateTraceCallbacksV5 callbacks{};
    RecompIosGpuCallbacksV1 gpuCallbacks{};
    bool sharedDevicePresent=false;
    // ios-direct-present (Claude, 2026-10-16): the library's own presenter draws to the game view's CAMetalLayer (V6), as
    // on Android: no readback, no second upload, no per-frame CPU comparison or queue idle on the main thread.
    bool directPresent=false;
    RecompPrivateTraceCallbacksV6 directCallbacks{};
    std::atomic<void*> directLayer{nullptr}; // retained CAMetalLayer, published on main once laid out and on screen
    NSTimer* directLayerTimer=nil;
    std::thread worker;
    bool active=false,terminal=false;
    unsigned displayed=0;

    bool agentRequested=false,agentPlay=false;
    bool agentAcknowledgementsFailed=false;
    static std::uint64_t agentClock(void*) noexcept {
        const auto value=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return value>0?std::uint64_t(value):0;
    }
    agentenvironment::Record agentEnvironment{agentClock,nullptr};
    std::unique_ptr<agentmenu::Adapter> agentMenu;
    std::unique_ptr<agenthandoff::Gate> agentHandoff;
    std::unique_ptr<agentenvironment::Guest> agentGuest;
    agentobserver::Registration agentRegistration;
    std::unique_ptr<IosStagePreparation> agentPrepared;
    NSTimer* agentTimer=nil;

    using CaptureIngress=agentingress::Dispatcher<fulltouch::Gamepad>;
    using AgentCapture=agentcapture::Controller<CaptureIngress>;
    std::unique_ptr<agentobservation::Ledger> agentImages;
    std::unique_ptr<CaptureIngress> agentIngress;
    std::unique_ptr<AgentCapture> agentCapture;

    agentrun::Authorization agentAuthorization;

    std::unique_ptr<agentdiscardstream::Server> agentStream;
    // Opt-in trusts an explicit host assertion about these exact pixels. The
    // one-run capability binds its connection; it cannot prove the host looked.
    bool agentTrustHostReview=false;
    using StreamReview=bool(*)(void*,const agentobservation::Reference&,std::string_view) noexcept;
    StreamReview agentStreamReview=nullptr;void* agentStreamReviewContext=nullptr;
    bool agentStreamReviewConfigured=false,agentStreamPortReported=false;
    bool agentStreamHasImage=false,agentStreamResultPending=false;
    bool agentStreamDiscardPending=false;
    std::uint64_t agentStreamDiscardId=0;
    agentobservation::Reference agentStreamDiscardReference{};
    std::uint64_t agentStreamCapture=0,agentStreamCaptureStarted=0,agentStreamResultId=0;
    std::uint32_t agentStreamResultStatus=0;
    agentrequest::Result agentStreamResult{};
    agentcapture::Publication agentStreamPublication{};
    bool configureAgentStreamReview(StreamReview review,void* context) noexcept {
        if(!pthread_main_np() || !review || agentStreamReviewConfigured)return false;
        agentStreamReview=review;agentStreamReviewContext=context;agentStreamReviewConfigured=true;return true;
    }
    static bool verifyAgentStreamReview(void* context,const agentobservation::Reference& reference,
                                        std::string_view wire) noexcept {
        if(!pthread_main_np())return false;
        auto& self=*static_cast<Session*>(context);
        return self.agentTrustHostReview && self.agentStream && self.agentStreamHasImage &&
            reference==self.agentStreamPublication.observation.metadata.reference &&
            self.agentStream->acceptReview(reference,wire);
    }
    bool startAgentStream() noexcept {
        if(!pthread_main_np())return false;
        if(!agentTrustHostReview)return true; // No listener and no trusted reviewer by default.
        if(agentStream || !agentCapture || !agentIngress)return false;
        agentinput::Nonce capability{};
        if(!makeAgentNonce(capability) || capability==agentAuthorization.run())return false;
        try {
            agentStream=std::make_unique<agentdiscardstream::Server>(agentAuthorization.run(),capability,agentClock,nullptr);
            if(agentStream->failure()!=agentdiscardstream::Failure::None ||
               !configureAgentStreamReview(verifyAgentStreamReview,this))return false;
            // Explicit owner-authorized launch control output, never HELLO or
            // an observation. Keep it out of ordinary capture/request logs.
            const auto run=agentAuthorization.run();
            std::fprintf(stderr,"APPLE_AGENT_REVIEW_CAPABILITY version=3 run=%.*s capability=%.*s\n",
                int(run.size()),run.data(),int(capability.size()),capability.data());
            return true;
        } catch(...) {return false;}
    }
    void closeAgentStream() noexcept {
        if(!pthread_main_np())return;
        if(agentStream)agentStream->close();
        agentStreamReview=nullptr;agentStreamReviewContext=nullptr;
        agentStreamCapture=0;agentStreamResultPending=false;agentStreamHasImage=false;
        agentStreamDiscardPending=false;agentStreamDiscardId=0;agentStreamDiscardReference={};
        agentStreamPublication={};
    }
    void queueAgentStreamResult(std::uint64_t id,std::uint32_t status,agentrequest::Result result={}) noexcept {
        agentStreamResultId=id;agentStreamResultStatus=status;agentStreamResult=result;
        agentStreamResultPending=true;
    }
    void closeAgentAuthorization() noexcept {
        if(!pthread_main_np())return;
        agentAuthorization.close();
        closeAgentStream();
        cancelAgentCapture();
        if(fullTouchInput)fullTouchInput->revokeAgent();
        // A transport adapter must disable its epoch/connection here after the
        // grant closes; it must never synchronously join its worker on main.
    }
    static bool authorizationGrant(void* context,agentingress::Grant& out) noexcept {
        auto& self=*static_cast<Session*>(context);out={};
        if(!pthread_main_np() || !self.fullTouchInput)return false;
        if(!self.active || self.terminal || self.agentAcknowledgementsFailed || self.measurement || self.displayRecording) {
            self.closeAgentAuthorization();return false;
        }
        const bool result=self.agentAuthorization.grant(self.agentEnvironment,*self.fullTouchInput,out);
        if(self.agentAuthorization.phase()==agentrun::Phase::Closed)self.closeAgentAuthorization();
        return result;
    }
    static bool makeAgentNonce(agentinput::Nonce& out) noexcept {
        @try {
            NSString* value=[[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString];
            const char* text=value.UTF8String;
            return text && value.length==32 && agentinput::parseNonce(std::string_view(text,32),out);
        } @catch(NSException*) {return false;}
    }
    void prepareAgentAuthorization() noexcept {
        if(!pthread_main_np() || agentAuthorization.phase()!=agentrun::Phase::PendingMenu)return;
        agentrun::Identities ids{};
        if(!fullTouchInput || !makeAgentNonce(ids.run) || !makeAgentNonce(ids.menu) || !makeAgentNonce(ids.guest) ||
           !agentAuthorization.prepare(ids,*fullTouchInput))closeAgentAuthorization();
    }
    bool enterAuthorizedAgentMenu() noexcept {
        if(!pthread_main_np())return false;
        if(agentAuthorization.phase()==agentrun::Phase::Disabled)return true;
        if(agentAuthorization.phase()==agentrun::Phase::Menu)return bool(agentCapture);
        if(!fullTouchInput || !agentAuthorization.enterMenu(agentEnvironment,*fullTouchInput) ||
           !configureAgentCapture(agentAuthorization.run(),authorizationGrant,this)) {
            closeAgentAuthorization();return false;
        }
        if(!startAgentStream()){closeAgentAuthorization();return false;}
        return true;
    }
    void advanceAuthorizedAgentGuest() noexcept {
        if(agentAuthorization.phase()==agentrun::Phase::Disabled)return;
        const bool consumed=agentPlay && agentHandoff && agentHandoff->phase()==agenthandoff::Phase::Consumed;
        if(!fullTouchInput || !agentAuthorization.advanceGuest(agentEnvironment,*fullTouchInput,consumed))
            closeAgentAuthorization();
    }
    static bool captureEnvironment(void* context,agentinput::Environment& e) noexcept {
        auto& self=*static_cast<Session*>(context);
        if(!self.agentRequested || !self.active || self.terminal || self.measurement || self.displayRecording ||
           self.agentAcknowledgementsFailed)return false;
        return self.agentEnvironment.read(e);
    }
    // Called only by a trusted app-owned control path, never inferred from a
    // transport request or --agent-input-v1. No caller is installed here.
    bool configureAgentCapture(agentobservation::RunId run,CaptureIngress::GrantProvider provider,void* context) noexcept {
        if(!pthread_main_np() || agentCapture || !fullTouchInput || !agentinput::validNonce(run) || !provider)return false;
        agentinput::Environment e{};agentingress::Grant grant{};
        if(!captureEnvironment(this,e) || !provider(context,grant) || grant.run!=run ||
           grant.stage!=e.stage || grant.stageGeneration!=e.stageGeneration ||
           !agentinput::validNonce(grant.acquisition.nonce) || !grant.acquisition.authorizationRevision)return false;
        try {
            constexpr auto capacity=agentobservation::Ledger::MaxBytes;
            auto images=std::make_unique<agentobservation::Ledger>(run,capacity,agentClock,nullptr,agentcapture::nativeSha256,nullptr);
            auto ingress=std::make_unique<CaptureIngress>(*images,agentEnvironment,*fullTouchInput,
                captureEnvironment,this,provider,context);
            auto capture=std::make_unique<AgentCapture>(*images,*ingress,capacity,agentClock,nullptr);
            agentImages=std::move(images);agentIngress=std::move(ingress);agentCapture=std::move(capture);return true;
        } catch(...) {return false;}
    }
    bool requestAgentGuestCapture() noexcept {
        if(!pthread_main_np() || !agentCapture || !presenter || !presenter->activeAndAttached())return false;
        agentinput::Environment e{};
        return captureEnvironment(this,e) && e.stage==agentinput::Stage::Guest && bool(agentCapture->armGuest());
    }
    // Synchronous borrowed envelope: its storage remains alive and immutable
    // through dispatch. The result reports admission/refusal, never delivery ACK.
    agentrequest::Result submitAgentRequest(std::string_view text) noexcept {
        if(!pthread_main_np())return {agentrequest::Reason::Ingress,{agentingress::Reason::Thread}};
        if(!agentIngress)return {agentrequest::Reason::Ingress,{agentingress::Reason::Closed}};
        return agentrequest::submit(*agentIngress,text);
    }
    void cancelAgentCapture() noexcept {if(agentCapture)(void)agentCapture->cancel();}
    static bool agentProvide(void* context,agentinput::Environment& e) noexcept {
        return static_cast<Session*>(context)->agentEnvironment.read(e);
    }
    static void agentEvent(void* context,agentmenu::EnvironmentEvent event) noexcept {
        auto& record=static_cast<Session*>(context)->agentEnvironment;
        if(event==agentmenu::EnvironmentEvent::SourceSample)(void)record.menuSample();
        else(void)record.menuConsumed();
    }
    static void agentBefore(void* context,bool connected,const recomp::input::GamepadState& state,agentmenu::Receipt& receipt) noexcept {
        auto& self=*static_cast<Session*>(context);const auto hooks=self.agentMenu->hooks();
        hooks.before(hooks.owner,connected,state,receipt);
    }
    static void agentAfter(void* context,agentmenu::Receipt& receipt,const recomp::input::GamepadState& state,bool admitted) noexcept {
        auto& self=*static_cast<Session*>(context);const auto hooks=self.agentMenu->hooks();
        hooks.after(hooks.owner,receipt,state,admitted); // actual UI ACK before capture
        agentinput::Environment e{};
        if(self.agentHandoff && self.agentEnvironment.read(e))(void)self.agentHandoff->observe(e);
    }
    agentmenu::Hooks agentHooks() noexcept {
        return {fullTouchInput.get(),this,agentBefore,agentAfter};
    }
    bool initAgentMenu() {
        if(!agentRequested)return true;
        if(!fullTouchInput || !active || terminal)return false;
        if(agentMenu)return true;
        if(!agentEnvironment.begin(agentinput::Stage::Menu,active))return false;
        agentMenu=std::make_unique<agentmenu::Adapter>(*fullTouchInput,agentProvide,this,agentEvent);
        agentHandoff=std::make_unique<agenthandoff::Gate>(*fullTouchInput);
        agentGuest=std::make_unique<agentenvironment::Guest>(*fullTouchInput,agentEnvironment);
        return true;
    }
    void agentActive(bool value) noexcept {
        if(!agentRequested)return;
        value=value && !agentAcknowledgementsFailed;
        (void)agentEnvironment.active(value);
        if(!value)closeAgentAuthorization();
        if(!value && agentHandoff)agentHandoff->cancel();
    }
    void drainAgentAcknowledgements() noexcept {
        if(!agentRequested || !agentGuest)return;
        agentdelivery::Result result{};
        for(std::size_t i=0;i<agentenvironment::Acknowledgements::Capacity && agentGuest->takeAcknowledgement(result);++i) {
            const auto& t=result.token;
            std::fprintf(stderr,"APPLE_IOS_AGENT_GUEST_ACK version=1 stage=%u stage_generation=%llu lease_generation=%llu publication_generation=%llu observed_generation=%llu sequence=%u phase=%u output=%u progress=%llu acknowledged_at_ms=%llu\n",
                unsigned(t.stage),(unsigned long long)t.stageGeneration,(unsigned long long)t.leaseGeneration,
                (unsigned long long)t.publicationGeneration,(unsigned long long)t.observedGeneration,
                t.sequence,unsigned(t.phase),result.output,(unsigned long long)result.progress,(unsigned long long)result.acknowledgedAtMs);
        }
        if(agentGuest->acknowledgementsFailed() && !agentAcknowledgementsFailed) {
            agentAcknowledgementsFailed=true;agentActive(false);
            std::fprintf(stderr,"APPLE_IOS_AGENT_GUEST_ACK_FAILED version=1 reason=journal_loss episode_closed=1\n");
        }
    }
    void startAgentGuest() {
        if(!agentRequested)return;
        cancelAgentCapture();
        if(agentAcknowledgementsFailed || !agentGuest || !agentEnvironment.begin(agentinput::Stage::Guest,active))
            throw std::runtime_error("agent guest environment refused");
        if(agentRegistration.resolve(reinterpret_cast<const void*>(&recomp_run_private_trace_v5))!=agentobserver::Result::Ok ||
            agentRegistration.install(agentenvironment::Guest::copied,agentGuest.get())!=agentobserver::Result::Ok)
            throw std::runtime_error("actual engine observer registration refused");
        advanceAuthorizedAgentGuest();
    }
    void joinedAgentGuest() noexcept {
        if(!agentRequested)return;
        [agentTimer invalidate];agentTimer=nil;agentActive(false);
        drainAgentAcknowledgements();
        const auto result=agentRegistration.clear();
        if(result==agentobserver::Result::Ok)agentEnvironment.close();
        // Failed clear preserves all borrowed owners in the immortal Session;
        // no automatic retry/unload/reset after exceptional engine completion.
        std::fprintf(stderr,"APPLE_IOS_AGENT_JOIN observer_clear=%u owners_retained=1\n",unsigned(result));
    }

    int snapshot(std::uint32_t user,recomp::input::GamepadState* state) noexcept {
        const auto aimToken=recordedInput?recordedInput->guestReadToken():0;
        const int result=agentRequested && agentGuest?agentGuest->read(user,state):fullTouchInput?fullTouchInput->snapshot(user,state):input->snapshot(user,state);
#if defined(RECOMP_DUO_EDITION)
        if(!user && state && result>0)recomp::duo::app::mergePad(*state); // ios-duo: the lower / top screen gestures (WheelTouch.merge)
#endif
        if(!user && state) {
            std::lock_guard lock(inputLogMutex);
            if(result!=lastInputStatus || state->packet!=lastInputPacket) {
                lastInputStatus=result;lastInputPacket=state->packet;
                std::fprintf(stderr,"APPLE_IOS_GUEST_INPUT connected=%d packet=%u buttons=%04x sticks=%d,%d,%d,%d triggers=%u,%u source=%s physical_input_unproven=1\n",
                    result,state->packet,state->buttons,state->leftX,state->leftY,state->rightX,state->rightY,state->leftTrigger,state->rightTrigger,fullTouchInput?"AppOwnedMultiplexer":"GameController");
            }
        }
        if(!user && state && recordedInput)recordedInput->observeGuest(aimToken,result,*state);
        return result;
    }
    int capabilities(std::uint32_t user,recomp::input::GamepadState* state) noexcept {return fullTouchInput?fullTouchInput->capabilities(user,state):input->capabilities(user,state);}
    void closeFullTouch() noexcept { iosasyncworkerqos::close(); if(displayFollowupControl)displayFollowupControl->cancel(); [rendererQosTimer invalidate];rendererQosTimer=nil;if(rendererQosRequested)iosrendererqos::experiment().close(); [botTimer invalidate];botTimer=nil;if(botPublisher)botPublisher->stop(); [agentTimer invalidate];agentTimer=nil;agentActive(false);[qolTouch close];qolTouch=nil;[fullTouchControls close];if(fullTouchInput)fullTouchInput->close(); }
};
// Runtime-owned threads can outlive V5 return; never free their callback owner.
Session& session() {static auto* value=new Session;return *value;}
int frame(void* context,std::uint32_t sequence,const std::uint8_t* pixels,
          std::uint32_t bytes,std::uint32_t width,std::uint32_t height) noexcept {
    auto* owner=static_cast<Session*>(context);
    if(!owner || pthread_main_np())return 0;
    return waitForMeasuredOrderedFrame(owner->measurement.get(),sequence,[&] {
        return !owner->resumableLifecycleRequested || iosGpuLifecycle().waitUntilRunning();
    },[&](auto measuredMainEntered,auto measuredPresenterReturned) {
      __block auto result=OrderedFrameAttempt::Rejected;
      dispatch_sync(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            measuredMainEntered();
            if(!owner->terminal && owner->lifecycle && !owner->presenter->activeAndAttached()) {
                const bool foreground=UIApplication.sharedApplication.applicationState==UIApplicationStateActive;
                owner->active=foreground;
                owner->lifecycle->setActive(false);
                // A temporarily detached/not-yet-active scene may have no new
                // app activation event. Retain foreground intent; the resume
                // timer still requires actual scene/view readiness.
                if(foreground)owner->lifecycle->setActive(true);
                result=OrderedFrameAttempt::Deferred;return;
            }
            if(!owner->terminal && owner->lifecycle && !owner->lifecycle->running()) {
                result=OrderedFrameAttempt::Deferred;return;
            }
            if(owner->active && !owner->terminal && owner->presenter) {
                const int accepted=IosFramePresenter::callback(owner->presenter.get(),sequence,pixels,bytes,width,height);
                measuredPresenterReturned();
                if(accepted && owner->agentCapture)(void)owner->agentCapture->acceptedGuest(sequence,pixels,bytes,width,height);
                result=accepted?OrderedFrameAttempt::Accepted:OrderedFrameAttempt::Rejected;
                if(accepted && (++owner->displayed==1 || owner->displayed%60==0))
                    std::fprintf(stderr,"APPLE_IOS_PORT_DISPLAY accepted=%u sequence=%u gameplay_unproven=1\n",owner->displayed,sequence);
                if(owner->metalCapture)owner->metalCapture->tick(owner->displayed,owner->active,owner->terminal,
                    owner->recordedInput && owner->recordedInput->neutral());
            }
        }
      });
      return result;
    });
}
int gpuFrame(void* context,const RecompIosGpuFrameV1* input) noexcept {
  try {
    if(!input || input->structureBytes!=sizeof(*input) || input->version!=1 || input->reserved0 || input->reserved1) return RECOMP_IOS_GPU_TERMINAL;
    const auto sequence=input->sequence;const auto* pixels=input->mappedRgba;
    const auto bytes=input->mappedBytes;const auto width=input->width,height=input->height;
    if(bytes!=1280ull*720*4 || width!=1280 || height!=720 || !pixels) return RECOMP_IOS_GPU_TERMINAL;
    auto* owner=static_cast<Session*>(context);
    if(!owner || !owner->presenter || pthread_main_np())return RECOMP_IOS_GPU_TERMINAL;
    const int completed=waitForMeasuredOrderedFrame(owner->measurement.get(),sequence,[&] {
        return !owner->resumableLifecycleRequested || iosGpuLifecycle().waitUntilRunning();
    },[&](auto measuredMainEntered,auto measuredPresenterReturned) {
      __block auto result=OrderedFrameAttempt::Rejected;
      dispatch_sync(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            measuredMainEntered();
            if(!owner->terminal && owner->lifecycle && !owner->presenter->activeAndAttached()) {
                const bool foreground=UIApplication.sharedApplication.applicationState==UIApplicationStateActive;
                owner->active=foreground;
                owner->lifecycle->setActive(false);
                // A temporarily detached/not-yet-active scene may have no new
                // app activation event. Retain foreground intent; the resume
                // timer still requires actual scene/view readiness.
                if(foreground)owner->lifecycle->setActive(true);
                result=OrderedFrameAttempt::Deferred;return;
            }
            if(!owner->terminal && owner->lifecycle && !owner->lifecycle->running()) {
                result=OrderedFrameAttempt::Deferred;return;
            }
            if(owner->active && !owner->terminal && owner->presenter) {
                const int resultCode=owner->presenter->presentGpu(*input);
                const bool accepted=resultCode==RECOMP_IOS_GPU_ACCEPTED;
                measuredPresenterReturned();
                if(accepted && owner->agentCapture)(void)owner->agentCapture->acceptedGuest(sequence,pixels,bytes,width,height);
                result=accepted?OrderedFrameAttempt::Accepted:OrderedFrameAttempt::Rejected;
                if(accepted && (++owner->displayed==1 || owner->displayed%60==0))
                    std::fprintf(stderr,"APPLE_IOS_PORT_DISPLAY accepted=%u sequence=%u gameplay_unproven=1\n",owner->displayed,sequence);
                if(owner->metalCapture)owner->metalCapture->tick(owner->displayed,owner->active,owner->terminal,
                    owner->recordedInput && owner->recordedInput->neutral());
            }
        }
      });
      return result;
    });
    return completed==1?RECOMP_IOS_GPU_ACCEPTED:RECOMP_IOS_GPU_TERMINAL;
  } catch(...) { std::fputs("APPLE_IOS_SHARED_GPU_FAILURE exception=1\n",stderr);return RECOMP_IOS_GPU_TERMINAL; }
}
void closeGpuFrame(void* context,std::uint32_t terminal) noexcept {
    auto* owner=static_cast<Session*>(context);
    if(!owner || !owner->presenter)return;
    @try {
      try {
        auto close=^{ owner->presenter->closeGpu(terminal!=0); };
        if(pthread_main_np())close();else dispatch_sync(dispatch_get_main_queue(),close);
      } catch(...) { std::fputs("APPLE_IOS_SHARED_GPU_FAILURE close_exception=1\n",stderr);std::_Exit(4); }
    } @catch(NSException* exception) { (void)exception;std::fputs("APPLE_IOS_SHARED_GPU_FAILURE close_objc_exception=1\n",stderr);std::_Exit(4); }
}
extern "C" int recomp_run_private_trace_gpu_v1(int,char**,const RecompPrivateTraceCallbacksV5*,const RecompIosGpuCallbacksV1*) noexcept;
// ios-direct-present: RECOMP_IOS_DIRECT_PRESENT=1 selects V6 (the library presents); 0 or absent keeps V5 (this shell presents).
bool directPresentRequested() {
    const char* text=std::getenv("RECOMP_IOS_DIRECT_PRESENT");
    if(!text || !std::strcmp(text,"0"))return false;
    if(!std::strcmp(text,"1"))return true;
    throw std::runtime_error("RECOMP_IOS_DIRECT_PRESENT must be 0 or 1");
}
// V6 window callbacks (any thread): the layer once published, never before (the library then waits and asks again).
ANativeWindow* directAcquireWindow(void* user,std::uint64_t* generation) noexcept {
    auto* owner=static_cast<Session*>(user);
    if(generation)*generation=1;
    return owner?static_cast<ANativeWindow*>(owner->directLayer.load(std::memory_order_acquire)):nullptr;
}
void directReleaseWindow(void*,ANativeWindow*) noexcept {} // (the session keeps its one retained reference for the run)
std::uint64_t directWindowGeneration(void*) noexcept {return 1;} // (one view and one layer for the process)
// Main thread: publish the game view's CAMetalLayer once the view is active, attached and laid out with a size.
void publishDirectLayer(Session& value) {
    if(value.directLayer.load(std::memory_order_acquire) || !value.presenter)return;
    if(!value.presenter->activeAndAttached())return;
    UIView* display=(__bridge UIView*)value.presenter->nativeView();
    UIView* metal=display.subviews.firstObject;
    if(!metal || ![metal.layer isKindOfClass:CAMetalLayer.class])return;
    CAMetalLayer* layer=(CAMetalLayer*)metal.layer;
    if(layer.bounds.size.width<=0 || layer.bounds.size.height<=0 || layer.drawableSize.width<=0)return;
    value.directLayer.store(const_cast<void*>(CFBridgingRetain(layer)),std::memory_order_release);
    std::fprintf(stderr,"APPLE_IOS_DIRECT_PRESENT layer=1 bounds=%.0fx%.0f scale=%.2f drawable=%.0fx%.0f\n",layer.bounds.size.width,
        layer.bounds.size.height,layer.contentsScale,layer.drawableSize.width,layer.drawableSize.height);
}
bool sharedDevicePresentRequested() {
    const char* text=std::getenv("RECOMP_IOS_SHARED_DEVICE_PRESENT");
    if(!text || !std::strcmp(text,"0"))return false;
    if(!std::strcmp(text,"1"))return true;
    throw std::runtime_error("RECOMP_IOS_SHARED_DEVICE_PRESENT must be 0 or 1");
}
void startWorker(const IosStagePreparation& preparation) {
    auto& value=session();
    if(!value.botSession.empty()) {
        const auto expected=value.root+"/saves-sandbox/ios-bot-"+value.botSession;
        if(!iosbot::sandboxSwitchMatches(value.root+"/private-trace",value.botSession))
            throw std::runtime_error("exact bot save-sandbox switch required");
        const auto actual=recomp::bootstrap::save_sandbox::filesRootFor(value.root.c_str());
        if(actual!=expected)throw std::runtime_error("verified isolated bot save/profile root required");
        if(!value.fullTouchInput || !value.fullTouchInput->agentStatus().manualNeutral)
            throw std::runtime_error("owner input is active; bot start refused");
        const auto owner=value.fullTouchInput->agentStatus().ownerGeneration;
        if(!iosfailure::configure((value.root+"/private-trace").c_str(),value.botSession.c_str()))
            throw std::runtime_error("failure evidence directory unavailable");
        value.botPublisher=std::make_unique<iosbot::CommitPublisher>(value.root+"/private-trace",value.botSession,true,owner);
        value.botTimer=[NSTimer timerWithTimeInterval:0.016 repeats:YES block:^(NSTimer*) {
            auto& current=session();
            if(current.asyncWorkerQosRequested && (iosbot::revoked.load(std::memory_order_acquire) || !current.active || current.terminal))iosasyncworkerqos::close();
            if(!current.botPublisher || !current.fullTouchInput)return;
            try {
                current.botPublisher->poll(Session::agentClock(nullptr),current.fullTouchInput->agentStatus().ownerGeneration,
                    current.active && !current.terminal);
            } catch(...) {
                current.botPublisher->stop();
                std::fputs("APPLE_IOS_BOT_TRANSPORT_FAILED no_retry=1\n",stderr);
            }
        }];
        [NSRunLoop.mainRunLoop addTimer:value.botTimer forMode:NSRunLoopCommonModes];
    }
    if(!iosmvkpolicy::verify("before-guest"))throw std::runtime_error("MoltenVK policy mismatch");
    if(value.displayFollowupControl)value.displayFollowupControl->cancel();
    value.displayFollowupControl=value.displayRecording?
        std::make_shared<performance::DisplayFollowupControl>(value.followupSessionStart):nullptr;
    value.measurement=preparePerformanceMeasurement(value.displayRecording,value.displayFollowupControl);
    iosmeasurement::arm(value,value.measurementDeadline);
    if(value.asyncWorkerQosRequested)dispatch_after(value.measurementDeadline,dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{iosasyncworkerqos::close();});
    if(iosmvkpolicy::requested()>=0)dispatch_after(value.measurementDeadline,
        dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{(void)iosmvkpolicy::verify("deadline");});
    if(value.rendererQosRequested) {
        iosrendererqos::experiment().configure();
        value.rendererQosTimer=[NSTimer timerWithTimeInterval:0.25 repeats:YES block:^(NSTimer*) {
            if(iosbot::revoked.load(std::memory_order_acquire) || !session().active || session().terminal) {
                iosrendererqos::experiment().close();return;
            }
            const auto path=session().root+"/private-trace/diag-ios-render-qos";
            iosrendererqos::experiment().poll(::access(path.c_str(),F_OK)==0);
        }];
        [NSRunLoop.mainRunLoop addTimer:value.rendererQosTimer forMode:NSRunLoopCommonModes];
        dispatch_after(value.measurementDeadline,dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
            iosrendererqos::experiment().close();
            dispatch_async(dispatch_get_main_queue(), ^{
                [session().rendererQosTimer invalidate];session().rendererQosTimer=nil;
            });
        });
    }
    value.arguments=preparation.files.plan.arguments;
    value.pointers.clear();
    for(auto& argument:value.arguments)value.pointers.push_back(argument.data());
    value.pointers.push_back(nullptr);
    value.callbacks={sizeof(RecompPrivateTraceCallbacksV5),5,0,0,
        {sizeof(RecompPrivateTraceCallbacksV4),4,0,0,&value,frame},nullptr,nullptr,nullptr};
    bindGamepadCallbacks(value.callbacks,value);
    { // ios-qos (Claude, 2026-10-16): the guest thread that polls the pad (the game's main loop) asks for user-interactive QoS while
      // files/private-trace/ios-qos-main is present (or RECOMP_IOS_SWITCHES_ON/AB name it), else its starting QoS (default).
        static decltype(value.callbacks.gamepadState) original=nullptr;original=value.callbacks.gamepadState;
        value.callbacks.gamepadState=[](void* user,std::uint32_t index,recomp::input::GamepadState* state) noexcept -> int {
            static thread_local int applied=-1;static thread_local unsigned n=0;
            {std::uint64_t self=0;pthread_threadid_np(nullptr,&self);recomp_ios_main_tid.store(self,std::memory_order_relaxed);
             // (c151: this hook runs on several host threads - the guest threads that poll the pad; the port is kept for the main
             // guest thread only, token 63001000, and each polling guest thread is named once)
             const std::uint32_t token=recomp::bootstrap::currentGuestThreadToken();
             if(token==0x63001000u)recomp_ios_main_port.store(unsigned(pthread_mach_thread_np(pthread_self())),std::memory_order_relaxed);
             static thread_local std::uint32_t named=~0u;
             if(named!=token){named=token;std::fprintf(stderr,"APPLE_IOS_PADPOLL token=%08x mach=%u\n",token,unsigned(pthread_mach_thread_np(pthread_self())));}}
            if(!(n++%30)) {
                // ios-qos-main: user-interactive (1); otherwise utility (2) by default (Claude 2026-10-17, ADOPTED: c129 sit GPU -1.7 /
                // frame -1.6 ms, c130 drive frame -7 ms (confounded), c107-c109 the same effect by accident: CPU and GPU share one power
                // budget and the GPU limits); ios-qos-main-keep: the starting QoS (0).
                static const std::string home=std::string(std::getenv("HOME")?std::getenv("HOME"):"")+"/Library/r/private-trace/";
                static const std::string path=home+"ios-qos-main",keep=home+"ios-qos-main-keep";
                // (c152: five guest threads poll the pad - main 63001000, render 6408e000, audio 64141000, 64272000, 640ea000 - and all
                // five were demoted. ios-qos-render-keep: the render thread keeps its starting QoS; ios-qos-padpoll-main-only: only main.)
                static const std::string renderKeep=home+"ios-qos-render-keep",mainOnly=home+"ios-qos-padpoll-main-only";
                const std::uint32_t pollToken=recomp::bootstrap::currentGuestThreadToken();
                const bool exempt=(pollToken==0x6408e000u && recomp_ios_switches_access(renderKeep.c_str(),F_OK)==0) ||
                                  (pollToken!=0x63001000u && recomp_ios_switches_access(mainOnly.c_str(),F_OK)==0);
                // (Claude, 2026-10-18) ios-qos-main-low: utility even when ios-qos-main-keep is on (the reversed phase order of a keep A/B)
                static const std::string low=home+"ios-qos-main-low";
                const int want=recomp_ios_switches_access(path.c_str(),F_OK)==0?1:recomp_ios_switches_access(low.c_str(),F_OK)==0?2:
                    recomp_ios_switches_access(keep.c_str(),F_OK)==0 || exempt?0:2;
                if(want!=applied) {
                    const int r=pthread_set_qos_class_self_np(want==1?QOS_CLASS_USER_INTERACTIVE:want==2?QOS_CLASS_UTILITY:QOS_CLASS_DEFAULT,0);
                    std::fprintf(stderr,"APPLE_IOS_QOS thread=pad-poller token=%08x mach=%u want=%d result=%d\n",pollToken,unsigned(pthread_mach_thread_np(pthread_self())),want,r);
                    applied=want;
                }
            }
            return original(user,index,state);
        };
    }
    value.sharedDevicePresent=sharedDevicePresentRequested();
    if(value.sharedDevicePresent && !value.measurement)throw std::runtime_error("shared GPU presentation requires measured run");
    value.directPresent=directPresentRequested();
    if(value.directPresent && value.sharedDevicePresent)throw std::runtime_error("direct and shared presentation are exclusive");
    if(value.directPresent) {
        // (device.cpp reads RECOMP_IOS_DIRECT_PRESENT itself when the renderer's device is made: surface extensions on.)
        value.directCallbacks={sizeof(RecompPrivateTraceCallbacksV6),6,0,0,value.callbacks,&value,
            directAcquireWindow,directReleaseWindow,directWindowGeneration};
        publishDirectLayer(value);
        if(!value.directLayer.load(std::memory_order_acquire)) {
            value.directLayerTimer=[NSTimer timerWithTimeInterval:0.05 repeats:YES block:^(NSTimer* timer) {
                auto& current=session();publishDirectLayer(current);
                if(current.directLayer.load(std::memory_order_acquire) || current.terminal){[timer invalidate];current.directLayerTimer=nil;}
            }];
            [NSRunLoop.mainRunLoop addTimer:value.directLayerTimer forMode:NSRunLoopCommonModes];
        }
        std::fputs("APPLE_IOS_DIRECT_PRESENT requested=1 abi=6\n",stderr);
    }
    { // ios-screen-fit (Claude 2026-10-20; A/B aid): files/private-trace/ios-screen-fit, or RECOMP_IOS_SWITCHES_* naming it, shows the
      // letterbox instead of the fill while present (RECOMP_IOS_SCREEN_FIT=fit: always). Checked every 0.5 s on the main thread; a
      // change lays the presenter view out again (ios_layer_geometry.h) and logs APPLE_IOS_SCREEN_FIT.
        static const bool envFit=[]{const char* e=std::getenv("RECOMP_IOS_SCREEN_FIT");return e && !std::strcmp(e,"fit");}();
        NSTimer* fitTimer=[NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer* timer) {
            auto& current=session();
            if(current.terminal || !current.presenter){[timer invalidate];return;}
            const auto path=current.root+"/private-trace/ios-screen-fit",over=current.root+"/private-trace/ios-screen-overhang",
                cap2=current.root+"/private-trace/ios-screen-cap2"; // (3: fill by crop capped at 2.0x, A/B aid)
            static const int start=[]{const char* e=std::getenv("RECOMP_IOS_SCREEN_FIT");return e && !std::strcmp(e,"overhang")?2:1;}();
            // (fill by crop = 1, the default; ios-screen-overhang = 2: the c1-fill-137 layout, for A/Bs)
            const std::uint32_t word=recomp_apple_launcher_display_options(); // (0 until the saved choices are published at start)
            const bool rowFit=recomp::apple::launcher::screenFit(word); // launcher Screen: Fit
            const int fill=(envFit || rowFit || ::access(path.c_str(),F_OK)==0)?0 : (::access(over.c_str(),F_OK)==0?2 : ::access(cap2.c_str(),F_OK)==0?3:start);
            { // ios-overlay
                const auto hidePath=current.root+"/private-trace/ios-overlay-hide";
                bool physicalPad=false;
                for(GCController* c in GCController.controllers) {
                    if(current.virtualController && c==current.virtualController.controller)continue;
                    if(c.extendedGamepad && !c.snapshot){physicalPad=true;break;}
                }
                const int hide=(physicalPad || ::access(hidePath.c_str(),F_OK)==0)?1:0;
                const bool statusChanged=iosStatusHidden.exchange(1,std::memory_order_relaxed)!=1;
                const bool touchChanged=iosOverlayHidden.exchange(hide,std::memory_order_relaxed)!=hide;
                if(touchChanged && current.fullTouchControls)current.fullTouchControls.hidden=hide?YES:NO;
                if(current.qolTouch)[current.qolTouch controllerPresent:physicalPad?YES:NO]; // (ios-touchpad: auto-hide on a detected pad)
                if(statusChanged || touchChanged) {
                    UIView* display=(__bridge UIView*)current.presenter->nativeView();
                    if(UIViewController* root=display.window.rootViewController) {
                        [root setNeedsStatusBarAppearanceUpdate];[root setNeedsUpdateOfHomeIndicatorAutoHidden];
                    }
                    std::fprintf(stderr,"APPLE_IOS_OVERLAY hidden=%d status_hidden=1 physical_pad=%d touch=%d\n",hide,int(physicalPad),current.qolTouch?1:0);
                }
            }
            { // ios-test-audio-interrupt (Claude 2026-10-21; TEST ONLY, off unless the switch is present, e.g. RECOMP_IOS_SWITCHES_LATE):
              // posts a synthetic AVAudioSessionInterruptionNotification "began" to this process's own observers, then "ended" (should
              // resume) 6 s later, once per run: the c408 path (an interruption while the app stays active) without another app.
                static int stage=0;static CFAbsoluteTime beganAt=0;
                const auto testPath=current.root+"/private-trace/ios-test-audio-interrupt";
                if(stage==0 && ::access(testPath.c_str(),F_OK)==0) {
                    stage=1;beganAt=CFAbsoluteTimeGetCurrent();std::fputs("APPLE_IOS_TEST_AUDIO_INTERRUPT phase=began\n",stderr);
                    [NSNotificationCenter.defaultCenter postNotificationName:AVAudioSessionInterruptionNotification object:AVAudioSession.sharedInstance
                        userInfo:@{AVAudioSessionInterruptionTypeKey:@(AVAudioSessionInterruptionTypeBegan)}];
                } else if(stage==1 && CFAbsoluteTimeGetCurrent()-beganAt>=6.0) {
                    stage=2;std::fputs("APPLE_IOS_TEST_AUDIO_INTERRUPT phase=ended\n",stderr);
                    [NSNotificationCenter.defaultCenter postNotificationName:AVAudioSessionInterruptionNotification object:AVAudioSession.sharedInstance
                        userInfo:@{AVAudioSessionInterruptionTypeKey:@(AVAudioSessionInterruptionTypeEnded),
                                   AVAudioSessionInterruptionOptionKey:@(AVAudioSessionInterruptionOptionShouldResume)}];
                }
            }
            { // ios-drawable-half (see ios_frame_presenter.mm): files/private-trace/ios-drawable-half (or a RECOMP_IOS_SWITCHES_* name)
              // DEFAULT ON since 2026-10-21 (build 190; c444/c446: presenter 636 -> 142 us per present, c446 frame -2.7 ms on foot):
              // the drawable is the render-scaled frame's own size and the system compositor does the single upscale. It stays
              // off while the player's scaler.txt picks an upscaler (cubic, fsr1, gsr1: the presenter must scale then) and when
              // files/private-trace/ios-drawable-full exists (the original full-size drawable; also the A/B switch now).
              // At render scale 100 the two layouts are the same.
                const auto halfPath=current.root+"/private-trace/ios-drawable-half",fullPath=current.root+"/private-trace/ios-drawable-full";
                const int half=(::access(halfPath.c_str(),F_OK)==0 || (::access(fullPath.c_str(),F_OK)!=0 &&
                                recomp_ios_settings_upscaler.load(std::memory_order_relaxed)==0))?1:0;
                // (build 191) the layout reads the APPLIED render scale, which the renderer sets only once the game draws (16 = 100 %
                // before that): c447's start laid out 1280x720 with half on and kept it until a later toggle. Lay out again whenever
                // the applied scale changes while half is on.
                static int laidOutSixteenths=-1;
                const int sixteenths=recomp_ios_render_sixteenths.load(std::memory_order_relaxed);
                const bool halfChanged=recomp_ios_drawable_half.exchange(half,std::memory_order_relaxed)!=half;
                if(halfChanged || (half && sixteenths!=laidOutSixteenths)) {
                    if(UIView* display=(__bridge UIView*)current.presenter->nativeView())[display setNeedsLayout];
                    std::fprintf(stderr,"APPLE_IOS_DRAWABLE_HALF on=%d sixteenths=%d\n",half,sixteenths);
                    laidOutSixteenths=sixteenths;
                }
            }
            if(recomp_ios_screen_fill.exchange(fill,std::memory_order_relaxed)!=fill) {
                if(UIView* display=(__bridge UIView*)current.presenter->nativeView())[display setNeedsLayout];
                std::fprintf(stderr,"APPLE_IOS_SCREEN_FIT fill=%d\n",fill);
            }
        }];
        [NSRunLoop.mainRunLoop addTimer:fitTimer forMode:NSRunLoopCommonModes];
    }
    value.gpuCallbacks={sizeof(RecompIosGpuCallbacksV1),1,0,0,&value,gpuFrame,closeGpuFrame};
    value.startAgentGuest();
    if(value.resumableLifecycleRequested) {
        value.lifecycle=std::make_unique<PortLifecycle>(*value.presenter,[owner=&value] {
            // Pending automation must not continue after an owner interruption.
            // Keep the virtual controller attached for ordinary manual play.
            if(owner->recordedInput)owner->recordedInput->close();
            [owner->inputTimer invalidate];

            if(owner->measurement) {owner->presenter->stopDisplayObservation();owner->measurement->cancelDisplayFollowup();owner->measurement->close();}
            if(owner->metalCapture)owner->metalCapture->close("scene_pause",owner->displayed);
        },[owner=&value](const char*) {
            owner->terminal=true;owner->discipline.invalidate();owner->presenter->close();owner->closeFullTouch();
            if(owner->measurement) {owner->presenter->stopDisplayObservation();owner->measurement->cancelDisplayFollowup();owner->measurement->close();}
            if(owner->recordedInput)owner->recordedInput->close();
            [owner->inputTimer invalidate];
        },[] {
        });
        value.lifecycle->start();
    }
#if defined(RECOMP_DUO_EDITION)
    if(value.directPresent)recomp::duo::app::registerLowerScreen(); // ios-duo: before the V6 entry (recomp_set_hud_window_v1's contract)
#endif
    value.worker=std::thread([owner=&value] {
        @autoreleasepool {
            const int code=owner->directPresent
                ? recomp_run_private_trace_v6(int(owner->arguments.size()),owner->pointers.data(),&owner->directCallbacks)
                : owner->sharedDevicePresent
                ? recomp_run_private_trace_gpu_v1(int(owner->arguments.size()),owner->pointers.data(),&owner->callbacks,&owner->gpuCallbacks)
                : recomp_run_private_trace_v5(int(owner->arguments.size()),owner->pointers.data(),&owner->callbacks);
            // Publish thread ownership before joining; never block the UI while
            // a borrowed frame is synchronously dispatched to it.
            dispatch_async(dispatch_get_main_queue(), ^{
                dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
                    owner->worker.join();
                    dispatch_async(dispatch_get_main_queue(), ^{
                        owner->joinedAgentGuest();
                        if(owner->lifecycle)owner->lifecycle->terminate();
                        owner->terminal=true;owner->discipline.invalidate();owner->presenter->close();
            if(owner->measurement) {owner->presenter->stopDisplayObservation();owner->measurement->cancelDisplayFollowup();owner->measurement->close();}
                        owner->closeFullTouch();
                        if(owner->recordedInput)owner->recordedInput->close();
                        if(owner->metalCapture)owner->metalCapture->close("guest_return",owner->displayed);
                        [owner->inputTimer invalidate];
                        UIApplication.sharedApplication.idleTimerDisabled=NO;
                        std::fprintf(stderr,"APPLE_IOS_PORT_RETURN code=%d displayed=%u restart_disabled=1 gameplay_unproven=1\n",code,owner->displayed);
                    });
                });
            });
        }
    });
    std::fputs("APPLE_IOS_PORT_GUEST_START abi=5 authenticated_request=1 native_xma=1\n",stderr);
}
}

@interface PortController : UIViewController<PortLauncherHost>
@property(nonatomic,strong) UILabel* status;
@property(nonatomic,strong) UIButton* verify;
@property(nonatomic,strong) UIButton* start;
@property(nonatomic,strong) PortLauncherCoordinator* launcher;
@property(nonatomic,strong) PortLauncherInput* menuInput;
- (void)setupLauncher;
- (void)showFullTouchForGuest;
- (void)agentTick;
- (BOOL)captureAgentMenu;
- (void)serviceAgentStream;
- (void)tryAgentPrepared;
- (void)prepared:(const IosStagePreparation&)result unexpected:(BOOL)unexpected;
- (void)finishPrepared:(const IosStagePreparation&)result unexpected:(BOOL)unexpected;
- (void)attachMenuInput;
- (void)prepareAndStart;
@end
#if defined(RECOMP_DUO_EDITION)
static DuoOverlay* duoOverlay=nil; // ios-duo (main thread)
extern "C" int recomp_duo_split() noexcept {return recomp::duo::state::split.load();} // (the launcher's Screens row)
extern "C" void recomp_duo_request_split(int on) noexcept {[duoOverlay setSplit:on!=0 source:"launcher"];} // (main thread: the launcher's Save)
#endif
// (2026-11-05) LauncherCoordinator.mm (unit 18, the compiled payload pin): the builder's parts from the .app into Library/r
extern "C" bool port_seed_bundle_payload(const char* root,char* error,size_t size);
@implementation PortController
#if defined(RECOMP_DUO_EDITION)
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {return recomp::duo::app::orientations();} // ios-duo: split = portrait
- (void)duoTwoFingerDoubleTap:(UITapGestureRecognizer*)r {
    if(r.state==UIGestureRecognizerStateRecognized && !recomp::duo::state::split.load())[duoOverlay setSplit:true source:"two-finger double tap (touch input)"]; // (fullscreen -> split)
}
#endif
- (BOOL)prefersStatusBarHidden {return YES;} // (ios-duo 2026-11-05, main: hidden everywhere, launcher included - a full-screen game; the clock/battery overlapped the launcher header in portrait and landscape. Was: hidden in gameplay only, ios-overlay)
- (BOOL)prefersHomeIndicatorAutoHidden {return iosStatusHidden.load(std::memory_order_relaxed)!=0;}
- (void)viewDidLoad {
    [super viewDidLoad];self.view.backgroundColor=UIColor.blackColor;
    auto& value=session();
    value.presenter=value.displayRecording?
        std::make_unique<IosFramePresenter>(value.resumableLifecycleRequested,value.displayRecording):
        std::make_unique<IosFramePresenter>(value.resumableLifecycleRequested);
    value.input=std::make_unique<AppleGamepadInput>();
    if(!value.launcherDiagnostic) {
        value.fullTouchInput=std::make_unique<fulltouch::Gamepad>(*value.input);
        value.fullTouchInput->setTouchAvailable(false); // No invisible pre-game pad.
        value.prepareAgentAuthorization();
    }
    value.audioDiagnostics=[NSMutableArray new];
    auto audioSession=AVAudioSession.sharedInstance;
    for(NSString* name in @[AVAudioSessionRouteChangeNotification,AVAudioSessionInterruptionNotification]) {
        id token=[NSNotificationCenter.defaultCenter addObserverForName:name object:audioSession queue:nil usingBlock:^(NSNotification* note) {
            auto audio=AVAudioSession.sharedInstance;
            std::fprintf(stderr,"APPLE_IOS_AUDIO_EVENT event=%s route_reason=%ld interruption=%ld sample_rate=%.1f period=%.6f output=%s\n",
                note.name.UTF8String,long([note.userInfo[AVAudioSessionRouteChangeReasonKey] integerValue]),
                long([note.userInfo[AVAudioSessionInterruptionTypeKey] integerValue]),audio.sampleRate,audio.IOBufferDuration,
                audio.currentRoute.outputs.firstObject.portType.UTF8String?:"none");
        }];
        [value.audioDiagnostics addObject:token];
    }
    UIView* display=(__bridge UIView*)value.presenter->nativeView();display.frame=self.view.bounds;
    display.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;[self.view addSubview:display];
#if defined(RECOMP_DUO_EDITION)
    duoOverlay=[[DuoOverlay alloc] initWithOwner:self];duoOverlay.frame=self.view.bounds;
    duoOverlay.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;[self.view addSubview:duoOverlay];
    { UITapGestureRecognizer* back=[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(duoTwoFingerDoubleTap:)];
      back.numberOfTouchesRequired=2;back.numberOfTapsRequired=2;back.cancelsTouchesInView=NO;back.delaysTouchesEnded=NO;
      [self.view addGestureRecognizer:back]; }
#endif
    self.status=[UILabel new];self.status.numberOfLines=0;self.status.textColor=UIColor.whiteColor;
    self.status.textAlignment=NSTextAlignmentCenter;
#if defined(RECOMP_DUO_EDITION)
    self.status.text=@"FivePocket Duo — development test\nVerify data before starting. A failed attempt is retained; there is no automatic retry.";
#else
    self.status.text=@"FivePocket iOS — development test\nVerify data before starting. A failed attempt is retained; there is no automatic retry.";
#endif
    self.verify=[UIButton buttonWithType:UIButtonTypeSystem];[self.verify setTitle:@"Verify game data" forState:UIControlStateNormal];
    [self.verify addTarget:self action:@selector(verifyData) forControlEvents:UIControlEventTouchUpInside];
    self.start=[UIButton buttonWithType:UIButtonTypeSystem];[self.start setTitle:@"Start recorded test" forState:UIControlStateNormal];
    [self.start addTarget:self action:@selector(prepareAndStart) forControlEvents:UIControlEventTouchUpInside];
    UIStackView* controls=[[UIStackView alloc] initWithArrangedSubviews:@[self.status,self.verify,self.start]];
    controls.axis=UILayoutConstraintAxisVertical;controls.spacing=20;controls.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:controls];
    [NSLayoutConstraint activateConstraints:@[[controls.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [controls.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [controls.widthAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.widthAnchor multiplier:0.9]]];
}


// ios-sidemenu (SPEC §12; owner 2026-10-25 ~00:40): the in-game side menu's rows and effects. The Settings page is the launcher's own ten
// settings, saved through the launcher's own store (the same file its Save writes); the live ones also go to the game at once through the
// live word (launcher_display_options.cpp; every restart field kept at its start value). Touch Controls writes settings/touch-controls.txt
// (the other words kept) and gives the pad its words again. The trainer and script mods are looked up by name (dlsym): absent in this
// library, their rows say so.
+ (void)attachSideMenu:(PortQolTouchView*)view root:(const std::string&)root {
    namespace sm=recomp::apple::sidemenu;namespace la=recomp::apple::launcher;
    auto rows=std::make_shared<sm::IosRows>();
    const int rootFd=::open(root.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    const auto captured=rootFd>=0?la::captureLauncherConfiguration(rootFd):la::OriginalConfiguration{};
    const int dir=rootFd>=0 && captured.status==la::ConfigurationStatus::Captured?la::openLauncherSettingsDirectory(rootFd):-1;
    if(rootFd>=0)::close(rootFd);
    auto store=std::make_shared<la::LauncherSettingsStore>(dir);
    if(dir>=0)::close(dir); // (the store keeps its own descriptor)
    rows->allowed=captured.policy.allowed;rows->saved=captured.policy.original;
    const auto loaded=store->ready()?store->load():la::LoadedSettings{la::LoadStatus::Unavailable,std::nullopt,0};
    if(loaded.status==la::LoadStatus::Loaded && loaded.value)rows->saved=loaded.value->choices;
    rows->settingsAvailable=captured.status==la::ConfigurationStatus::Captured && store->ready() &&
        (loaded.status==la::LoadStatus::Loaded || loaded.status==la::LoadStatus::Missing);
    const auto startWord=la::decode(recomp_apple_launcher_display_options()); // (frozen at the start; 0 = the original's legacy route)
    rows->start=startWord?*startWord:captured.policy.original;
    const std::string touchPath=root+"/settings/touch-controls.txt",modsPath=root+"/settings/mods.txt",trainerPath=root+"/settings/trainer.txt";
    const auto readText=[](const std::string& path){NSString* t=[NSString stringWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()] encoding:NSASCIIStringEncoding error:nil];
        return std::string(t.UTF8String?:"");};
    const auto writeText=[](const std::string& path,const std::string& text){
        return [[NSString stringWithUTF8String:text.c_str()] writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES encoding:NSASCIIStringEncoding error:nil]==YES;};
    rows->touch=sm::parseTouch(readText(touchPath));
#if defined(RECOMP_DUO_EDITION)
    // ios-duo (the Thor mirror): the Dual Screens tab's state from the Thor's files and the Duo's split
    namespace dst=recomp::duo::state;
    const std::string secondPath=root+"/settings/second-screen.txt",minimapPath=root+"/settings/minimap.txt",povPath=root+"/private-trace/diag-phone-pov";
    {const auto t=readText(secondPath);rows->dual.weaponMain=dst::hasToken(t,"wheel=main");rows->dual.radioMain=dst::hasToken(t,"radio=main");
     rows->dual.characterMain=dst::hasToken(t,"character=main");rows->dual.phoneMain=dst::hasToken(t,"phone=main");}
    rows->dual.minimap=dst::minimapOf(readText(minimapPath));rows->dual.phoneBg=dst::phoneBgOf(readText(povPath));
    rows->dual.lowerShown=dst::split.load()!=0;
    rows->dual.topMode=dst::clampMode(dst::topMode.load()); // (read at launch: duo_app.h loadMode)
    rows->dual.frame4x3=dst::frame4x3.load()!=0;
#endif
    if(auto& s=session();s.fullTouchInput)rows->touchShown=!s.fullTouchInput->pad().userHidden();
    using TrainerFn=int(*)(int,int,int,int);using StateFn=unsigned(*)();using ListFn=int(*)(int,int,char*,int);
    const auto act=reinterpret_cast<TrainerFn>(::dlsym(RTLD_DEFAULT,"recomp_trainer_v1"));
    const auto state=reinterpret_cast<StateFn>(::dlsym(RTLD_DEFAULT,"recomp_trainer_state_v1"));
    const auto list=reinterpret_cast<ListFn>(::dlsym(RTLD_DEFAULT,"recomp_trainer_list_v1"));
    if(act && state && list) {
        rows->trainer.present=[]{return true;};
        rows->trainer.state=[state]{return state();};
        rows->trainer.count=[list](int which){return list(which,-1,nullptr,0);};
        rows->trainer.label=[list](int which,int i){char b[96]{};list(which,i,b,int(sizeof b));return std::string(b);};
    }
    using TextFn=int(*)(char*,int); // (recomp_trainer_status_v1 / recomp_mods_status_v1: a line into the buffer)
    if(const auto status=reinterpret_cast<TextFn>(::dlsym(RTLD_DEFAULT,"recomp_trainer_status_v1")))
        rows->trainer.status=[status]{char b[256]{};status(b,int(sizeof b));return std::string(b);};
    if(const auto mods=reinterpret_cast<TextFn>(::dlsym(RTLD_DEFAULT,"recomp_mods_status_v1")))
        rows->trainer.mods=[mods]{char b[1024]{};mods(b,int(sizeof b));return std::string(b);};
    rows->scriptModsPresent=::dlsym(RTLD_DEFAULT,"recomp_mods_marker")!=nullptr;
    {const auto m=readText(modsPath);rows->scriptModsOn=m.find("scripts=on")!=std::string::npos || m.find("scripts=1")!=std::string::npos ||
        m.find("scripts=true")!=std::string::npos || m.find("scripts=yes")!=std::string::npos;}
    sm::Effects fx;
    fx.saveDisplay=[store,rows](const la::DisplayChoices& choices){
        const auto current=store->load();
        if(current.status!=la::LoadStatus::Loaded && current.status!=la::LoadStatus::Missing){
            std::fprintf(stderr,"APPLE_IOS_SIDE_MENU save=refused load_status=%d\n",int(current.status));return false;}
        const auto saved=store->save(current.value,choices);
        if(saved.status!=la::SaveStatus::Saved || !saved.value){std::fprintf(stderr,"APPLE_IOS_SIDE_MENU save=failed status=%d errno=%d\n",int(saved.status),saved.error);return false;}
        const auto live=sm::liveChoices(choices,rows->start);
        const bool published=la::publishLiveLauncherDisplayOptions(live);
        std::fprintf(stderr,"APPLE_IOS_SIDE_MENU saved generation=%llu live_word=0x%08x published=%d\n",(unsigned long long)saved.value->generation,
            la::encode(live).value_or(0u),int(published));
        return true;
    };
    __weak PortQolTouchView* weak=view;
    fx.saveTouch=[touchPath,readText,writeText,weak](const sm::TouchSettings& t){
        const auto text=sm::touchText(readText(touchPath),t);
        if(!writeText(touchPath,text)){std::fputs("APPLE_IOS_SIDE_MENU touch_settings=failed\n",stderr);return false;}
        if(auto& s=session();s.fullTouchInput)s.fullTouchInput->pad().settings(text.c_str());
        [weak padSettingsChanged];
        std::fprintf(stderr,"APPLE_IOS_SIDE_MENU touch_settings=\"%.*s\"\n",int(text.size()?text.size()-1:0),text.c_str());
        return true;
    };
    fx.touchShown=[weak](bool shown){
        if(auto& s=session();s.fullTouchInput)s.fullTouchInput->pad().showControls(shown,fulltouch::Input::qolNow());
        [weak padSettingsChanged];
    };
    fx.scriptMods=[modsPath,writeText](bool on){return writeText(modsPath,on?"scripts=on\n":"scripts=off\n");};
    fx.trainerEnabled=[trainerPath,writeText,act](bool on){
        const bool ok=writeText(trainerPath,on?"trainer=on\n":"trainer=off\n");
        const int sync=act?act(0,0,0,0):-1;
        std::fprintf(stderr,"APPLE_IOS_SIDE_MENU trainer=%s written=%d sync=%d\n",on?"on":"off",int(ok),sync);
    };
    fx.trainer=[act](int a,int a0,int a1){const int r=act?act(a,a0,a1,0):-1;std::fprintf(stderr,"APPLE_IOS_SIDE_MENU trainer_request action=%d a0=%d a1=%d result=%d\n",a,a0,a1,r);return r;};
#if defined(RECOMP_DUO_EDITION)
    // ios-duo: second-screen.txt (its other words kept), minimap.txt, the phone's background tunables (written, never removed), and the
    // Lower Screen row = the Duo's split (live, as the toggle).
    const std::string topPath=root+"/settings/duo-top-screen.txt";
    fx.saveDual=[secondPath,minimapPath,povPath,topPath,readText,writeText,root,weak](const sm::DualScreens& d){
        namespace dst=recomp::duo::state;
        ::mkdir((root+"/settings").c_str(),0700);
        const bool a=writeText(secondPath,dst::secondScreenText(readText(secondPath),d.weaponMain,d.radioMain,d.characterMain,d.phoneMain));
        const bool b=writeText(minimapPath,d.minimap==0?"both\n":"bottom\n");
        const bool c=writeText(povPath,dst::phonePovText(d.phoneBg));
        const bool e=writeText(topPath,dst::topModeText(d.topMode));
        if(dst::topMode.exchange(dst::clampMode(d.topMode),std::memory_order_relaxed)!=dst::clampMode(d.topMode)) {
            // (live: the presenter's layout picks the new mode at once and prints the layout line; the pad maps touches by it)
            if(auto& s=session();s.presenter)if(UIView* display=(__bridge UIView*)s.presenter->nativeView()){[display setNeedsLayout];[display layoutIfNeeded];}
            [weak setNeedsLayout];[weak setNeedsDisplay];[duoOverlay setNeedsLayout];
            std::fprintf(stderr,"APPLE_IOS_DUO_TOP_SCREEN mode=%s written=%d frame=%s restart_pending=%d source=side menu\n",dst::TopModeNames[dst::clampMode(d.topMode)],int(e),
                dst::frame4x3.load()?"4:3":"16:9",int(dst::restartPending(d.topMode,dst::frame4x3.load()!=0)));
        }
        if(d.lowerShown!=(dst::split.load()!=0))[duoOverlay setSplit:d.lowerShown source:"side menu (touch input)"];
        std::fprintf(stderr,"APPLE_IOS_SIDE_MENU dual_screens top=%d minimap=%d lower=%d weapon=%d radio=%d character=%d phone=%d phone_bg=%d written=%d%d%d%d\n",
            d.topMode,d.minimap,int(d.lowerShown),int(d.weaponMain),int(d.radioMain),int(d.characterMain),int(d.phoneMain),d.phoneBg,int(a),int(b),int(c),int(e));
        return a && b && c && e;
    };
#endif
    [view attachMenu:rows effects:std::move(fx)
                open:[](bool open){auto& s=session();if(s.fullTouchInput)s.fullTouchInput->setMenuOpen(open);
#if defined(RECOMP_DUO_EDITION)
                    [duoOverlay setNeedsLayout];[duoOverlay layoutIfNeeded]; // ios-duo: the menu over both halves while open (DuoOverlay layoutSubviews)
#endif
                    std::fprintf(stderr,"APPLE_IOS_SIDE_MENU %s game_input=%s\n",open?"open":"closed",open?"neutral":"restored");}
             buttons:[]() -> unsigned {auto& s=session();return s.fullTouchInput?s.fullTouchInput->controllerButtons():0u;}];
}

- (void)showFullTouchForGuest {
    if(!pthread_main_np() || session().launcherDiagnostic || session().terminal ||
       !session().fullTouchInput || session().qolTouch)return;
    auto& value=session();value.fullTouchInput->setTouchAvailable(true);
    // ios-touch (build 234): Apple's GCVirtualController exists only with the launch argument --virtual-controller (VirtualControllerStart);
    // it draws its own controls in a system window above the app. Once the game's touch controls are up it is not needed: disconnect it
    // (hidden recorded-input runs keep it: they drive it programmatically). files/private-trace/ios-keep-virtual-controller keeps it.
    if(value.virtualController && !value.recordedInputRequested &&
       ::access((value.root+"/private-trace/ios-keep-virtual-controller").c_str(),F_OK)!=0) {
        [value.virtualController disconnect];
        std::fputs("APPLE_IOS_VIRTUAL_DISCONNECTED reason=full-touch-ready\n",stderr);
    }
    // ios-touchpad (iPad 235): the universal touch controls (ios_touch_pad.h, SPEC v2) replace the FullTouch* controls (owner 2026-10-24:
    // "ipads touch controller doesnt even work. Make your own"; "These are the universal touch controls"). One full-screen view routes
    // every finger to the pad (controls, floating sticks, HIDE) or the long presses (radar, weapon icon; all four while hidden / off),
    // draws the pad and polls the control context (recomp_touch_context_v1). Library/r/settings/touch-controls.txt: mode=context|pad|off
    // hide=0|1 size=85..115 opacity=60..100.
    {
        const std::string pt=value.root+"/private-trace/";
        value.qolTouch=[[PortQolTouchView alloc] initWithGesture:[]() -> recomp::apple::qol::Gesture* {
                auto& s=session();return s.fullTouchInput?&s.fullTouchInput->qol():nullptr;}
            pad:[]() -> recomp::apple::touchpad::Pad* {auto& s=session();return s.fullTouchInput?&s.fullTouchInput->pad():nullptr;}
            context:[](){return recomp_touch_context_v1();}
            changed:[](){auto& s=session();if(s.fullTouchInput)s.fullTouchInput->qolChanged();}
            now:[](){return fulltouch::Input::qolNow();}
#if defined(RECOMP_DUO_EDITION)
            // ios-duo (2026-11-05): only Zoom crops (split or full screen, either orientation); Stretch has its own frame
            // (ios_qol_touch_view.h stretchFrame), Fit shows the whole picture
            fill:[](){return recomp::duo::state::topMode.load(std::memory_order_relaxed)==1;} // (Zoom)
#else
            fill:[](){const int now=recomp_ios_screen_fill.load(std::memory_order_relaxed);
                if(now>=0)return now!=0;
                const char* e=std::getenv("RECOMP_IOS_SCREEN_FIT");return !e || std::strcmp(e,"fit")!=0;}
#endif
            offPath:[NSString stringWithUTF8String:(pt+"ios-qol-off").c_str()]
            hudFitOffPath:[NSString stringWithUTF8String:(pt+"ios-hud-fit-off").c_str()]
            settingsPath:[NSString stringWithUTF8String:(value.root+"/settings/touch-controls.txt").c_str()]];
        value.qolTouch.frame=self.view.bounds;
        value.qolTouch.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        [self.view addSubview:value.qolTouch];
#if defined(RECOMP_DUO_EDITION)
        duoOverlay.touchControls=value.qolTouch;[duoOverlay setNeedsLayout]; // ios-duo: the universal pad over the top half (the game) while split
#endif
        [PortController attachSideMenu:value.qolTouch root:value.root];
        // (ios-touch probe, iPad 235k: where every touch lands, on the window: APPLE_IOS_TOUCH_PROBE lines)
        {UIView* root=self.view.window?:self.view;[root addGestureRecognizer:[PortTouchProbe new]];
         std::fprintf(stderr,"APPLE_IOS_TOUCH_PROBE installed on=%s qol_ui=%d window=%d\n",NSStringFromClass(root.class).UTF8String,
             int(value.qolTouch.userInteractionEnabled),self.view.window?1:0);}
    }
    std::fputs("APPLE_IOS_FULL_TOUCH_READY app_owned=1 universal_pad=1 physical_input_unproven=1\n",stderr);
}

- (void)setupLauncher {
    if(!pthread_main_np() || self.launcher || session().root.empty())return;
    { // (2026-11-05) the builder's parts from the .app into Library/r first: the configuration gate below reads its switches
        char error[256]{};
        if(!port_seed_bundle_payload(session().root.c_str(),error,sizeof error)) {
            self.status.text=@"The game's files in this app could not be prepared. Reinstall the app; nothing was started.";return;}
    }
    const int root=::open(session().root.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(root<0){self.status.text=@"Launcher root unavailable. No normal start.";return;}
    const auto captured=recomp::apple::launcher::captureLauncherConfiguration(root);
    const int settings=captured.status==recomp::apple::launcher::ConfigurationStatus::Captured?
        recomp::apple::launcher::openLauncherSettingsDirectory(root):-1;
    ::close(root);
    if(settings<0){self.status.text=@"Original configuration unavailable or unsupported. No normal start; recorded diagnostics retain legacy route.";return;}
    @try {
        self.launcher=[[PortLauncherCoordinator alloc] initWithDirectory:settings policy:captured.policy about:@[
#if defined(RECOMP_DUO_EDITION)
            @"FivePocket Duo 1.1.0-alpha.1 (the Thor Edition's lower screen). Play retains full asset/request/decoder/scene authentication; no automatic retry.",
#else
            @"FivePocket iOS 1.1.0-alpha.1. Play retains full asset/request/decoder/scene authentication; no automatic retry.",
#endif
            @"App-owned fifteen-control touch route. Native/recorded diagnostics remain separate; physical input latency unqualified.",
            @"Scaler/sharpness unavailable. Internal720p scaling; standing60 limiter; graphics and performance qualification pending.",
            [NSString stringWithFormat:@"Build %s · SDK-built Apple shell · measured game results are separate evidence.\n\nThe menus' face: Nunito Sans, "
                "SIL Open Font License 1.1 (the licence text: NunitoSans-OFL.txt in the app).",portbuild::Identity]]];
    } @catch(NSException* error) {
        ::close(settings);self.status.text=error.reason;return;
    }
    ::close(settings);self.launcher.host=self;
    auto* child=self.launcher.controller;[self addChildViewController:child];
    child.view.frame=self.view.bounds;child.view.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:child.view];[child didMoveToParentViewController:self];
    self.status.hidden=YES;self.verify.hidden=YES;self.start.hidden=YES;
    [self attachMenuInput];
    std::fprintf(stderr,"APPLE_IOS_LAUNCHER_READY original_files_captured=1 saved_effect_unproven=1 ownership=%u\n",unsigned(self.launcher.ownership));
}
- (void)attachMenuInput {
    if(!pthread_main_np())return;
    [self.menuInput close];self.menuInput=nil;
    if(session().launcherDiagnostic || session().terminal || !session().active ||
       !session().input || !self.launcher || self.launcher.controller.inputClosed)return;
    self.launcher.controller.manualInputHandler=^{
        if(session().fullTouchInput)session().fullTouchInput->ownerActivity();
    };
    if(!session().initAgentMenu()){self.status.text=@"Agent menu environment refused. No game started.";return;}
    const auto hooks=session().agentRequested?session().agentHooks():agentmenu::Hooks{};
    self.menuInput=session().fullTouchInput?
        [[PortLauncherInput alloc] initWithAppSource:session().fullTouchInput.get() controller:self.launcher.controller agentHooks:session().agentRequested?&hooks:nullptr]:
        [[PortLauncherInput alloc] initWithSource:session().input.get() controller:self.launcher.controller];
    if(self.menuInput.active)(void)session().enterAuthorizedAgentMenu();
    if(session().agentRequested && !session().agentTimer && self.menuInput.active) {
        __weak PortController* weak=self;
        session().agentTimer=[NSTimer timerWithTimeInterval:1.0/60.0 repeats:YES block:^(NSTimer*){[weak agentTick];}];
        [NSRunLoop.mainRunLoop addTimer:session().agentTimer forMode:NSRunLoopCommonModes];
    }
    self.launcher.controller.controllerText=self.menuInput.active?
        @"Pre-game event feed ready. A selects, B backs out, shoulders change pages, D-pad/left stick navigate, Start saves. Hardware latency and guest-transfer behavior still need device qualification.":
        @"Menu event observer unavailable; touch controls remain. No automatic observer retry.";
}
- (BOOL)launcherRequestsPlay:(PortLauncherCoordinator*)owner {
    if(!pthread_main_np() || owner!=self.launcher || session().discipline.state()!=IosPreparationDiscipline::State::Idle)return NO;
    if(session().agentRequested) {
        const auto event=[self.menuInput executingAgentPublication];
        session().agentPlay=event.available;
        agentinput::Environment e{};
        if(session().agentPlay && (!session().agentHandoff || !session().agentEnvironment.read(e) || !session().agentHandoff->arm(event,e)))return NO;
    }
    [self prepareAndStart];
    if(session().discipline.state()==IosPreparationDiscipline::State::Terminal) {
        [owner invalidateWithStatus:self.status.text];return YES; // admitted, then terminal; never successful game proof
    }
    return session().discipline.state()==IosPreparationDiscipline::State::Preparing;
}
- (BOOL)launcherRequestsVerification:(PortLauncherCoordinator*)owner {
    if(!pthread_main_np() || owner!=self.launcher || session().discipline.state()!=IosPreparationDiscipline::State::Idle)return NO;
    [self verifyData];return session().discipline.state()==IosPreparationDiscipline::State::Verifying;
}

- (void)verifyData {
    if(!pthread_main_np() || session().root.empty() || !session().discipline.beginVerification())return;
    self.verify.enabled=NO;self.start.enabled=NO;self.status.text=@"Verifying all game data…";
    const auto pins=std::vector<ExpectedAsset>(portbuild::ExpectedAssets.begin(),portbuild::ExpectedAssets.end());
    const auto root=session().root;__weak PortController* weak=self;
    const bool queued=dispatchHostVerification([root,pins]{return verifyAuthenticatedAssets(root,pins);},
        ^(const AssetVerification& result,bool unexpected) {
            session().discipline.finishVerification();PortController* view=weak;
            [weak.launcher verificationFinished];
            if(view) {
                const bool idle=session().discipline.state()==IosPreparationDiscipline::State::Idle;
                view.verify.enabled=idle;view.start.enabled=idle;
                view.status.text=(!unexpected && result.allMatched())?@"Game data verified. Start will authenticate it again and prepare a fresh request.":@"Game data verification failed. No game was started.";
                view.launcher.controller.statusText=view.status.text;
            }
            std::fprintf(stderr,"APPLE_IOS_PORT_VERIFY matched=%u verified=%zu bytes=%llu failure=%s errno=%d guest_started=0\n",
                unsigned(!unexpected && result.allMatched()),result.verifiedCount,(unsigned long long)result.verifiedBytes,assetFailureName(result.failure),result.systemError);
        });
    if(!queued){session().discipline.finishVerification();[self.launcher verificationFinished];self.verify.enabled=YES;self.start.enabled=YES;}
}


- (BOOL)captureAgentMenu {
    auto& value=session();
    if(!NSThread.isMainThread || !value.agentCapture || !value.active || value.terminal ||
       value.measurement || value.displayRecording || !self.launcher ||
       self.launcher.ownership!=launcher::SettingsOwnership::Menu)return NO;
    @try {
        agentcapture::UIKitMenu menu(self.menuInput,self.launcher.controller,self.view.window);
        return bool(value.agentCapture->captureMenu(menu));
    } @catch(NSException*) {value.cancelAgentCapture();return NO;}
}


- (void)serviceAgentStream {
    if(!NSThread.isMainThread)return;
    auto& value=session();if(!value.agentStream)return;
    (void)value.agentStream->consumeFailure([](void* owner) noexcept {
        static_cast<Session*>(owner)->closeAgentAuthorization();
    },&value);
    if(value.agentStream->failure()!=agentdiscardstream::Failure::None)return;
    if(!value.agentStreamPortReported && value.agentStream->port()) {
        const auto run=value.agentAuthorization.run();
        std::fprintf(stderr,"APPLE_AGENT_STREAM_LISTEN version=3 run=%.*s address=127.0.0.1 port=%u ack_supported=0\n",
            int(run.size()),run.data(),unsigned(value.agentStream->port()));
        value.agentStreamPortReported=true;
    }
    if(value.agentStreamDiscardPending) {
        if(value.agentStream->discarded(value.agentStreamDiscardId,value.agentStreamDiscardReference,value.agentClock(nullptr))) {
            value.agentStreamDiscardPending=false;value.agentStreamDiscardId=0;value.agentStreamDiscardReference={};
        }
        return; // Do not repeat cancellation while the response waits for the worker.
    }
    if(value.agentStreamResultPending) {
        if(value.agentStream->result(value.agentStreamResultId,value.agentStreamResultStatus,
                                     value.agentStreamResult,value.agentClock(nullptr)))value.agentStreamResultPending=false;
        return; // Sending backpressure must never dispatch the command twice.
    }
    if(value.agentStreamCapture) {
        const auto now=value.agentClock(nullptr);
        if(now<value.agentStreamCaptureStarted || now-value.agentStreamCaptureStarted>agentinput::ObservationFreshMs) {
            const auto id=value.agentStreamCapture;value.agentStreamCapture=0;
            value.cancelAgentCapture();value.queueAgentStreamResult(id,2);return;
        }
        auto pixels=value.agentStream->imageBuffer(value.agentStreamCapture);
        if(pixels.empty())return;
        agentcapture::Publication publication{};
        const auto copied=value.agentCapture->copyPublication(pixels.data(),pixels.size(),publication);
        if(!copied)return; // Waiting for the actual accepted callback, never invent a frame.
        agentingress::Grant grant{};
        if(!Session::authorizationGrant(&value,grant)) {value.closeAgentAuthorization();return;}
        if(value.agentStream->observation(value.agentStreamCapture,publication,grant,value.agentClock(nullptr))) {
            value.agentStreamPublication=publication;value.agentStreamHasImage=true;value.agentStreamCapture=0;
        }
        return;
    }
    agentdiscardstream::Request request{};
    if(!value.agentStream->take(request))return;
    if(request.header.type==agentdiscardstream::Type::Capture) {
        agentinput::Environment e{};
        const bool captured=value.agentIngress->read(e) &&
            (e.stage==agentinput::Stage::Menu?[self captureAgentMenu]:value.requestAgentGuestCapture());
        if(!captured){value.cancelAgentCapture();value.queueAgentStreamResult(request.header.correlation,2);return;}
        value.agentStreamCapture=request.header.correlation;value.agentStreamCaptureStarted=value.agentClock(nullptr);return;
    }
    if(request.header.type==agentdiscardstream::Type::Discard) {
        if(!value.agentStreamHasImage || request.reference!=value.agentStreamPublication.observation.metadata.reference ||
           !agentdiscardstream::cancelDiscard(value.agentCapture.get())) {
            value.closeAgentAuthorization();return;
        }
        value.agentStreamHasImage=false;value.agentStreamPublication={};
        value.agentStreamDiscardReference=request.reference;
        value.agentStreamDiscardId=request.header.correlation;value.agentStreamDiscardPending=true;
        return;
    }
    // The worker validates the complete immutable frame and embedded reference.
    // This additional binding selects the actual publication exported this turn.
    if(!value.agentStreamHasImage || request.reference!=value.agentStreamPublication.observation.metadata.reference) {
        value.closeAgentAuthorization();return;
    }
    const bool reviewed=value.agentStreamReview &&
        value.agentStreamReview(value.agentStreamReviewContext,request.reference,request.wire());
    value.agentStreamHasImage=false;
    agentrequest::Result result{};std::uint32_t status=3;
    if(reviewed) {
        agentinput::Environment current{};
        if(!value.agentIngress->read(current))result={agentrequest::Reason::Ingress,{agentingress::Reason::Provider}};
        else {
            const auto reviewed=value.agentImages->attest(request.reference,current);
            if(!reviewed)result={agentrequest::Reason::Ingress,{agentingress::Reason::Image,reviewed.reason}};
            else result=value.submitAgentRequest(request.wire());
        }
        status=result?0:1;
    }
    value.cancelAgentCapture();value.agentStreamPublication={};
    value.queueAgentStreamResult(request.header.correlation,status,result);
}

- (void)agentTick {
    auto& value=session();if(!value.agentRequested || !value.fullTouchInput)return;
    value.drainAgentAcknowledgements();
    if(!value.agentAuthorization.checkOwner(*value.fullTouchInput))value.closeAgentAuthorization();
    agentinput::Environment e{};
    if(!value.agentEnvironment.read(e)){if(value.agentHandoff)value.agentHandoff->cancel();}
    else { (void)value.fullTouchInput->updateAgent(e);
        if(value.agentHandoff)(void)value.agentHandoff->observe(e); }
    [self serviceAgentStream];
    [self tryAgentPrepared];
}
- (void)tryAgentPrepared {
    auto& value=session();if(!value.agentRequested || !value.agentPlay || !value.agentPrepared)return;
    agentinput::Environment e{};
    if(!value.agentEnvironment.read(e))value.agentHandoff->cancel();
    if(value.agentHandoff->phase()==agenthandoff::Phase::Cancelled) {
        value.terminal=true;value.discipline.invalidate();value.closeFullTouch();
        [self.menuInput close];[self.launcher invalidateWithStatus:@"Agent launch interrupted; no game started. Restart disabled."];
        return; // retain prepared evidence, never retry
    }
    if(!value.agentHandoff->take(true,e))return;
    const auto result=std::move(value.agentPrepared);
    [self finishPrepared:*result unexpected:NO];
}
- (void)prepared:(const IosStagePreparation&)result unexpected:(BOOL)unexpected {
    auto& value=session();
    // A spent/invalidated attempt cannot acquire another pending result. In
    // particular, a duplicate after startup must not reach the pending-duplicate
    // shutdown branch on a subsequent callback. Preserve the default route.
    if(value.agentRequested && value.agentPlay && value.discipline.state()!=IosPreparationDiscipline::State::Preparing) {
        std::fprintf(stderr,"APPLE_IOS_AGENT_PREPARATION_STALE state=%u\n",unsigned(value.discipline.state()));
        return;
    }
    // Failed preparation always reaches the ORIGINAL authenticated failure
    // gate immediately. Only a complete successful result waits for release.
    if(value.agentRequested && value.agentPlay && !unexpected && result.prepared() &&
       result.files.prepared() && result.assets.allMatched() && !result.files.plan.arguments.empty()) {
        if(value.agentPrepared){value.terminal=true;value.discipline.invalidate();value.closeFullTouch();return;}
        value.agentPrepared=std::make_unique<IosStagePreparation>(result);
        [self tryAgentPrepared];return;
    }
    [self finishPrepared:result unexpected:unexpected];
}

- (void)finishPrepared:(const IosStagePreparation&)result unexpected:(BOOL)unexpected {
    PortController* view=self;
        try {
            const auto done=session().discipline.finishPreparation(result,unexpected,
                session().active && !session().terminal,[&]{
                    if(!view)throw std::runtime_error("launcher view unavailable at authenticated handoff");
                    if(session().launcherDiagnostic) {
                        if(!recomp::apple::launcher::freezeLegacyLauncherDisplayOptions())throw std::runtime_error("diagnostic options could not freeze0");
                        [view.launcher invalidateWithStatus:@"Recorded diagnostic owns the legacy path; launcher closed."];
                        [view.launcher.controller closeForGuest];
                    } else if(![view.launcher commitAuthenticatedGuest:NO])throw std::runtime_error("saved settings handoff refused");
                    [view.menuInput close];view.launcher.controller.view.hidden=YES;[view showFullTouchForGuest];startWorker(result);
                });
            if(view) {
                if(done==IosPreparationDiscipline::Completion::Started) {
                    view.status.hidden=YES;view.verify.hidden=YES;view.start.hidden=YES;
                } else {view.status.text=@"Preparation stopped. No game started. Evidence and partial files are retained; restart is disabled.";[view.launcher invalidateWithStatus:view.status.text];}
            }
            if(!result.assets.allMatched())std::fprintf(stderr,
                "APPLE_IOS_ASSET_REJECT failure=%s index=%zu path=%s errno=%d\n",
                assetFailureName(result.assets.failure),result.assets.failedIndex,
                result.assets.failedPath.c_str(),result.assets.systemError);
            std::fprintf(stderr,"APPLE_IOS_PORT_PREPARED completion=%u stage_failure=%u files_failure=%u verified=%zu unexpected=%u\n",
                unsigned(done),unsigned(result.failure),unsigned(result.files.failure),result.assets.verifiedCount,unsigned(unexpected));
        } catch(...) {
            try {throw;} catch(const std::exception& e) {std::fprintf(stderr,"APPLE_IOS_PORT_WORKER_START_EXCEPTION what=%s\n",e.what());} catch(...) {}
            session().terminal=true;session().discipline.invalidate();session().closeFullTouch();
            // Lifecycle startup precedes thread creation; retire it even when launch throws.
            if(session().lifecycle)session().lifecycle->terminate();
            else if(session().resumableLifecycleRequested) {
                iosGpuLifecycle().stop();
                recomp::audio::terminateAppleAudioLifecycle();
            }
            if(session().presenter)session().presenter->close();
            if(session().measurement) {session().presenter->stopDisplayObservation();session().measurement->cancelDisplayFollowup();session().measurement->close();}
            if(session().recordedInput)session().recordedInput->close();
            [session().inputTimer invalidate];[view.menuInput close];
            if(session().metalCapture)session().metalCapture->close("worker_start_failed",session().displayed);
            UIApplication.sharedApplication.idleTimerDisabled=NO;
            if(view){view.status.text=@"Could not start the game worker. Restart disabled.";[view.launcher invalidateWithStatus:view.status.text];}
            std::fputs("APPLE_IOS_PORT_WORKER_START_FAILED\n",stderr);
        }
}

- (void)prepareAndStart {
    if(!pthread_main_np())return;
    if(!session().launcherDiagnostic && (!self.launcher || self.launcher.ownership!=recomp::apple::launcher::SettingsOwnership::Preparing))return;
    auto& value=session();
    if(!value.virtualStart.ready()) {
        self.status.text=@"Waiting for the requested virtual controller. No game started.";
        std::fputs("APPLE_IOS_VIRTUAL_WAIT guest_started=0\n",stderr);return;
    }
    if(value.recordedInputRequested && (!value.recordedInput || !value.recordedInput->neutral())) {
        self.status.text=@"Recorded controller unavailable or not neutral. No game started.";
        std::fputs("APPLE_IOS_INPUT_START_REFUSED guest_started=0\n",stderr);return;
    }
    recomp::audio::NativeXmaFrameDecoder decoder;
    const bool actualDecoder=portbuild::NativeDecoderAvailable && decoder.available();
    const auto admission=value.discipline.beginPreparation(actualDecoder,value.active && !value.terminal,
        !value.root.empty() && !portbuild::ExpectedAssets.empty() && !portbuild::RequestTemplate.empty());
    if(admission!=IosPreparationDiscipline::Admission::Started) {
        std::fprintf(stderr,"APPLE_IOS_PORT_ADMISSION reason=%u guest_started=0\n",unsigned(admission));return;
    }
    self.verify.enabled=NO;self.start.enabled=NO;self.status.text=@"Authenticating game data and preparing a fresh test…";
    const auto root=value.root;
    const auto pins=std::vector<ExpectedAsset>(portbuild::ExpectedAssets.begin(),portbuild::ExpectedAssets.end());
    const auto bytes=std::vector<std::uint8_t>(portbuild::RequestTemplate.begin(),portbuild::RequestTemplate.end());
    const std::string requestId=[[[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString] UTF8String];
    __weak PortController* weak=self;
    const bool queued=dispatchIosPreparation([root,pins,bytes,requestId] {
        // Native measurement only, not a guest start. A failed clock leaves
        // every run/save untouched; never spin inside the original initializer.
        const auto before=recomp::bootstrap::readGuestTimebase();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto after=recomp::bootstrap::readGuestTimebase();
        const bool clockReady=startupClockReady(before,after);
        std::fprintf(stderr,"APPLE_IOS_PORT_CLOCK first=%llu second=%llu ready=%u guest_started=0\n",
            (unsigned long long)before,(unsigned long long)after,unsigned(clockReady));
        if(!clockReady)throw std::runtime_error("native startup clock did not advance");
        return prepareIosStage(root,pins,bytes,portbuild::TemplateDigest,portbuild::TemplateRoot,portbuild::TemplateId,requestId);
    }, ^(const IosStagePreparation& result,bool unexpected) {
        PortController* view=weak;
        if(session().agentRequested && view){[view prepared:result unexpected:unexpected];return;}
        try {
            const auto done=session().discipline.finishPreparation(result,unexpected,
                session().active && !session().terminal,[&]{
                    if(!view)throw std::runtime_error("launcher view unavailable at authenticated handoff");
                    if(session().launcherDiagnostic) {
                        if(!recomp::apple::launcher::freezeLegacyLauncherDisplayOptions())throw std::runtime_error("diagnostic options could not freeze0");
                        [view.launcher invalidateWithStatus:@"Recorded diagnostic owns the legacy path; launcher closed."];
                        [view.launcher.controller closeForGuest];
                    } else if(![view.launcher commitAuthenticatedGuest:NO])throw std::runtime_error("saved settings handoff refused");
                    [view.menuInput close];view.launcher.controller.view.hidden=YES;[view showFullTouchForGuest];startWorker(result);
                });
            if(view) {
                if(done==IosPreparationDiscipline::Completion::Started) {
                    view.status.hidden=YES;view.verify.hidden=YES;view.start.hidden=YES;
                } else {view.status.text=@"Preparation stopped. No game started. Evidence and partial files are retained; restart is disabled.";[view.launcher invalidateWithStatus:view.status.text];}
            }
            if(!result.assets.allMatched())std::fprintf(stderr,
                "APPLE_IOS_ASSET_REJECT failure=%s index=%zu path=%s errno=%d\n",
                assetFailureName(result.assets.failure),result.assets.failedIndex,
                result.assets.failedPath.c_str(),result.assets.systemError);
            std::fprintf(stderr,"APPLE_IOS_PORT_PREPARED completion=%u stage_failure=%u files_failure=%u verified=%zu unexpected=%u\n",
                unsigned(done),unsigned(result.failure),unsigned(result.files.failure),result.assets.verifiedCount,unsigned(unexpected));
        } catch(...) {
            try {throw;} catch(const std::exception& e) {std::fprintf(stderr,"APPLE_IOS_PORT_WORKER_START_EXCEPTION what=%s\n",e.what());} catch(...) {}
            session().terminal=true;session().discipline.invalidate();session().closeFullTouch();
            // Lifecycle startup precedes thread creation; retire it even when launch throws.
            if(session().lifecycle)session().lifecycle->terminate();
            else if(session().resumableLifecycleRequested) {
                iosGpuLifecycle().stop();
                recomp::audio::terminateAppleAudioLifecycle();
            }
            if(session().presenter)session().presenter->close();
            if(session().measurement) {session().presenter->stopDisplayObservation();session().measurement->cancelDisplayFollowup();session().measurement->close();}
            if(session().recordedInput)session().recordedInput->close();
            [session().inputTimer invalidate];[view.menuInput close];
            if(session().metalCapture)session().metalCapture->close("worker_start_failed",session().displayed);
            UIApplication.sharedApplication.idleTimerDisabled=NO;
            if(view){view.status.text=@"Could not start the game worker. Restart disabled.";[view.launcher invalidateWithStatus:view.status.text];}
            std::fputs("APPLE_IOS_PORT_WORKER_START_FAILED\n",stderr);
        }
    });
    if(!queued){value.terminal=true;value.discipline.invalidate();value.closeFullTouch();[self.menuInput close];self.status.text=@"Preparation dispatch rejected. Restart disabled.";}
}
@end

@interface PortScene : UIResponder<UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow* window;
- (void)startRequestedTestIfReady;
@end
@implementation PortScene
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)connection options:(UISceneConnectionOptions*)options {
    (void)connection;(void)options;
    if(![scene isKindOfClass:UIWindowScene.class] || session().presenter)return;
    self.window=[[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController=[PortController new];[self.window makeKeyAndVisible];
    std::string error;const bool prepared=prepareIosDataRoot(session().root,error);
#if defined(RECOMP_DUO_EDITION)
    if(prepared) {recomp::duo::app::loadMode(session().root);recomp::duo::app::requestOrientation(self.window.rootViewController);
        for(UIView* v in self.window.rootViewController.view.subviews)[v setNeedsLayout];}
#endif
    std::fprintf(stderr,"APPLE_IOS_PORT_READY build=%s root_prepared=%u guest_started=0 auto_start=0\n",portbuild::Identity,unsigned(prepared));
    if(prepared) {
        [(PortController*)self.window.rootViewController setupLauncher];
        const auto space=storageSpace(session().root.c_str());
        std::fprintf(stderr,"APPLE_IOS_PORT_ROOT path=%s bytes=%zu available_bytes=%llu storage_valid=%u\n",
            session().root.c_str(),session().root.size(),(unsigned long long)space.availableBytes,unsigned(space.valid));
    }
}
- (void)startRequestedTestIfReady {
    if(session().virtualStart.ready() && session().explicitStart.consume(session().active,session().terminal)) {
        std::fputs("APPLE_IOS_PORT_EXPLICIT_START source=launch_argument physical_input=0\n",stderr);
        [(PortController*)self.window.rootViewController prepareAndStart];
    }
}
- (void)sceneDidBecomeActive:(UIScene*)scene {
    (void)scene;session().active=true;session().agentActive(true);
    if(session().fullTouchInput)session().fullTouchInput->setApplicationActive(true);
    [session().fullTouchControls setApplicationActive:YES];
    [session().qolTouch setApplicationActive:YES]; // ios-qol (iPad 231)
    [(PortController*)self.window.rootViewController attachMenuInput];
    if(!session().botSession.empty() && !session().botStartAttempted) {
        session().botStartAttempted=true;
        auto* controller=(PortController*)self.window.rootViewController;
        // launcher-commands (Claude, 2026-10-18; synthetic test input): with private-trace/launcher-hold-<session> the bot's
        // session does not press Play itself. The host writes private-trace/launcher-command-<session>.txt = "SEQ NAME" (NAME:
        // up down left right confirm back prev next menu state); each new SEQ is ONE LauncherNavigation command through the
        // launcher's own handleCommand (the gamepad's path), then APPLE_IOS_LAUNCHER_CMD logs the view's state. The host checks
        // that line and a screenshot before it sends the next. Play is the launcher's Play (confirm on PLAY / Play).
        const auto hold=session().root+"/private-trace/launcher-hold-"+session().botSession;
        if(::access(hold.c_str(),F_OK)==0) {
            std::fprintf(stderr,"APPLE_IOS_LAUNCHER_HOLD session=%s synthetic=1 %s\n",session().botSession.c_str(),
                controller.launcher.controller.stateLine.UTF8String?:"");
            const auto path=session().root+"/private-trace/launcher-command-"+session().botSession+".txt";
            session().launcherCommandTimer=[NSTimer timerWithTimeInterval:0.25 repeats:YES block:^(NSTimer* timer) {
                auto& value=session();
                PortLauncherController* view=controller.launcher.controller;
                if(!view || view.inputClosed || value.terminal){[timer invalidate];value.launcherCommandTimer=nil;return;}
                char text[64]={0};const bool opened=[&]{FILE* f=std::fopen(path.c_str(),"rb");if(!f)return false;
                    std::fread(text,1,sizeof text-1,f);std::fclose(f);return true;}();
                // (every 16 polls, ~4 s, and whenever the file's text changes: what the poll saw, so a missing acknowledgement
                // tells a command that never arrived from one that arrived and was not taken)
                ++value.launcherCommandPolls;if(opened)++value.launcherCommandReads;
                std::string seen(text);for(auto& c:seen)if(c=='\n')c='|';
                if(seen!=value.launcherCommandText || !(value.launcherCommandPolls%16)) {
                    value.launcherCommandText=seen;
                    std::fprintf(stderr,"APPLE_IOS_LAUNCHER_POLL polls=%lu reads=%lu opened=%d last_seq=%lu text=\"%s\"\n",value.launcherCommandPolls,
                        value.launcherCommandReads,int(opened),value.launcherCommandSeq,seen.c_str());
                }
                if(!opened)return;
                unsigned long seq=0;char name[16]={0};
                if(std::sscanf(text,"%lu %15s",&seq,name)!=2 || seq<=value.launcherCommandSeq)return;
                value.launcherCommandSeq=seq;
                static const char* names[]={"none","up","down","left","right","confirm","back","prev","next","menu"};
                int command=-1;for(int i=1;i<10;++i)if(!std::strcmp(name,names[i]))command=i;
                if(command>0)[view handleCommand:command repeat:NO];
                else if(std::strcmp(name,"state"))std::fprintf(stderr,"APPLE_IOS_LAUNCHER_CMD_UNKNOWN seq=%lu name=%s\n",seq,name);
                std::fprintf(stderr,"APPLE_IOS_LAUNCHER_CMD seq=%lu cmd=%s %s\n",seq,name,view.stateLine.UTF8String?:"");
            }];
            [NSRunLoop.mainRunLoop addTimer:session().launcherCommandTimer forMode:NSRunLoopCommonModes];
        } else {
            std::fprintf(stderr,"APPLE_IOS_BOT_EXPLICIT_START session=%s synthetic=1\n",session().botSession.c_str());
            if(!session().fullTouchInput || !session().fullTouchInput->agentStatus().manualNeutral ||
               ![controller.launcher launcher:controller.launcher.controller action:PortLauncherPlay])
                std::fputs("APPLE_IOS_BOT_START_REFUSED no_retry=1\n",stderr);
        }
    }
    if(session().lifecycle && !session().terminal) {
        session().lifecycle->setActive(true);
    }
    // Only this foreground session; never change the device lock settings.
    // The optional lifecycle coordinator owns resume, never a new guest start.
    UIApplication.sharedApplication.idleTimerDisabled=!session().terminal;
    // Defer until activation completes; consume once and retain every normal
    // preparation gate. A late background event prevents the request starting.
    dispatch_async(dispatch_get_main_queue(), ^{
        auto& value=session();
        if(value.virtualStart.begin(value.active,value.terminal)) {
            GCVirtualControllerConfiguration* configuration=[GCVirtualControllerConfiguration new];
            configuration.elements=[NSSet setWithArray:@[GCInputLeftThumbstick,GCInputRightThumbstick,
                GCInputButtonA,GCInputButtonB,GCInputButtonX,GCInputButtonY,
                GCInputLeftShoulder,GCInputRightShoulder,GCInputLeftTrigger,GCInputRightTrigger]];
            // Apple's programmatic setValue/setPosition API requires hidden
            // mode. Normal touch tests retain the system on-screen overlay.
            configuration.hidden=value.recordedInputRequested;
            std::fprintf(stderr,"APPLE_IOS_VIRTUAL_MODE hidden=%u recorded_input=%u\n",
                unsigned(configuration.hidden),unsigned(value.recordedInputRequested));
            value.virtualController=[[GCVirtualController alloc] initWithConfiguration:configuration];
            [value.virtualController connectWithReplyHandler:^(NSError* error) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    auto& owner=session();
                    GCController* controller=owner.virtualController.controller;
                    const bool connected=!error && controller && !controller.snapshot && controller.extendedGamepad &&
                        [GCController.controllers containsObject:controller];
                    // Only before V5: reconnect notifications may have preceded
                    // the reply. Rebuild our own observer, never steal handlers.
                    if(connected && owner.active && !owner.terminal) {
                        [((PortController*)self.window.rootViewController).menuInput close];
                        owner.input.reset();owner.input=std::make_unique<AppleGamepadInput>();
                        [(PortController*)self.window.rootViewController attachMenuInput];
                    }
                    recomp::input::GamepadState state{};
                    const bool readable=connected && owner.input && owner.input->snapshot(0,&state)==1;
                    const bool ready=owner.virtualStart.finish(readable,owner.active,owner.terminal);
                    std::fprintf(stderr,"APPLE_IOS_VIRTUAL_CONNECTED connected=%u adapter_readable=%u ready=%u error=%ld physical_input=0 buttons=%04x\n",
                        unsigned(connected),unsigned(readable),unsigned(ready),long(error.code),unsigned(state.buttons));
                    if(!ready) {
                        [owner.virtualController disconnect];
                        ((PortController*)self.window.rootViewController).status.text=@"Virtual controller unavailable. No automatic retry.";
                    }
                    if(ready && owner.recordedInputRequested) {
                        owner.recordedInput=std::make_unique<RecordedVirtualInput>(owner.virtualController,owner.input.get(),owner.recordedAimRequested);
                        if(owner.metalCaptureRequested)owner.metalCapture=std::make_unique<BoundedMetalCapture>();
                        owner.inputTimer=[NSTimer timerWithTimeInterval:owner.recordedAimRequested?0.016:0.05 repeats:YES block:^(NSTimer*) {
                            auto& current=session();current.recordedInput->tick(current.displayed,current.active,current.terminal);
                            if(current.metalCapture)current.metalCapture->tick(current.displayed,current.active,current.terminal,
                                current.recordedInput->neutral());
                        }];
                        [NSRunLoop.mainRunLoop addTimer:owner.inputTimer forMode:NSRunLoopCommonModes];
                    }
                    [self startRequestedTestIfReady];
                });
            }];
        }
        [self startRequestedTestIfReady];
    });
}
- (void)sceneWillResignActive:(UIScene*)scene {
    iosasyncworkerqos::close();
#if defined(RECOMP_DUO_EDITION)
    recomp::duo::app::cancelAll(); // ios-duo: focus lost = every gesture lets go (WheelTouch.cancelAll)
#endif
    if(session().botPublisher)session().botPublisher->stop();
    session().agentActive(false);
    [((PortController*)self.window.rootViewController).menuInput close];
    if(session().fullTouchInput)session().fullTouchInput->setApplicationActive(false);
    [session().fullTouchControls setApplicationActive:NO];
    [session().qolTouch setApplicationActive:NO]; // ios-qol (iPad 231)
    if(!session().launcherDiagnostic && !session().measurement && !session().terminal &&
       session().discipline.state()==IosPreparationDiscipline::State::Idle &&
       ((PortController*)self.window.rootViewController).launcher.ownership==launcher::SettingsOwnership::Menu) {
        session().active=false;
        UIApplication.sharedApplication.idleTimerDisabled=NO;
        std::fputs("APPLE_IOS_LAUNCHER_SUSPENDED idle_normal=1 guest_started=0\n",stderr);
        return;
    }
    if(session().measurement) {session().presenter->stopDisplayObservation();session().measurement->cancelDisplayFollowup();session().measurement->close();}
    if(session().lifecycle && !session().terminal) {
        session().active=false;
        UIApplication.sharedApplication.idleTimerDisabled=NO;
        session().lifecycle->setActive(false);
        return;
    }
    (void)scene;auto& value=session();value.active=false;value.terminal=true;value.discipline.invalidate();
    [((PortController*)self.window.rootViewController).menuInput close];
    [((PortController*)self.window.rootViewController).launcher invalidateWithStatus:@"Scene inactive; restart disabled."];
    UIApplication.sharedApplication.idleTimerDisabled=NO;
    if(value.recordedInput)value.recordedInput->close();[value.inputTimer invalidate];
    if(value.metalCapture)value.metalCapture->close("scene_inactive",value.displayed);
    value.closeFullTouch();
    value.virtualStart.close();[value.virtualController disconnect];
    if(value.presenter)value.presenter->close();
    // No cooperative V5 cancellation API: preserve owners, never join here or
    // auto-resume a guest after its view was closed.
    std::fputs("APPLE_IOS_PORT_SCENE_INACTIVE restart_disabled=1\n",stderr);
}
- (void)sceneDidDisconnect:(UIScene*)scene {
    if(session().lifecycle)session().lifecycle->terminate();
    session().terminal=true;
    [self sceneWillResignActive:scene];
}
@end
@interface PortApp : UIResponder<UIApplicationDelegate>
@end
@implementation PortApp
- (UISceneConfiguration*)application:(UIApplication*)application configurationForConnectingSceneSession:(UISceneSession*)connection options:(UISceneConnectionOptions*)options {
    (void)application;(void)options;
    UISceneConfiguration* configuration=[[UISceneConfiguration alloc] initWithName:@"PortTest" sessionRole:connection.role];
    configuration.delegateClass=PortScene.class;return configuration;
}
@end
int main(int argc,char** argv){@autoreleasepool {
    // ios-defaults (Claude, 2026-10-16): the measured steady-30 configuration is what a normal launch gets. A value given in the
    // launch environment (tests) wins: setenv never overwrites here. MoltenVK reads its MVK_CONFIG_* at the first Vulkan call.
    for(const auto& [k,v]:std::initializer_list<std::pair<const char*,const char*>>{
            {"RECOMP_IOS_DIRECT_PRESENT","1"},{"RECOMP_IOS_RENDERER_ASYNC","1"},{"RECOMP_IOS_QUEUED_PRESENT","1"},
            {"RECOMP_TEST_QUEUED_PRESENT","1"},{"MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS","0"},{"RECOMP_TIMEBASE_STEADY_CLOCK","1"},
            {"MVK_CONFIG_RESUME_LOST_DEVICE","1"}}) // (a GPU watchdog abort loses that frame's work, not the device and the game)
    // (build 227: fast math stays ON; 226's global MVK_CONFIG_FAST_MATH_ENABLED=0 froze the picture in c570 and recompiled every
    // shader; the NaN-prone shaders get finite-RSQ overrides instead, generic_nan_overrides.inc)
        (void)::setenv(k,v,0);
    recomp::ios_sampler::startFromEnvironment(); // (Claude; off unless RECOMP_IOS_SAMPLER is set)
    recomp::ios_sampler::startStallFromEnvironment(); // (Claude 2026-10-21; off unless RECOMP_IOS_STALL_SAMPLER is set)
    std::vector<std::string_view> arguments;
    for(int i=1;i<argc;++i)arguments.emplace_back(argv[i]);
    const auto measurementRequest=measurementLaunch(arguments);
    if(iosmvkpolicy::requested()!=-1 && measurementRequest.request!=MeasurementRequest::Enabled)return 10;
    if(!iosmvkpolicy::verify("launch"))return 10;
    { // (Claude) the MoltenVK settings in effect for this run (read only)
        MVKConfiguration c{};size_t n=sizeof(c);
        if(vkGetMoltenVKConfigurationMVK(VK_NULL_HANDLE,&c,&n)==VK_SUCCESS || n)
            std::fprintf(stderr,"APPLE_IOS_MVK_CONFIG sync=%u prefill=%u fast_math=%d mtl_heap=%d arg_buffers=%u resume_lost=%u max_cbs=%u pooling=%u timeout_ns=%llu\n",
                unsigned(c.synchronousQueueSubmits),unsigned(c.prefillMetalCommandBuffers),int(c.fastMathEnabled),int(c.useMTLHeap),
                unsigned(c.useMetalArgumentBuffers),unsigned(c.resumeLostDevice),unsigned(c.maxActiveMetalCommandBuffersPerQueue),
                unsigned(c.useCommandPooling),(unsigned long long)c.metalCompileTimeout);
    }
    if(measurementRequest.request==MeasurementRequest::Invalid)return 5;
    if(measurementRequest.request==MeasurementRequest::Enabled)
        session().displayRecording=std::make_shared<performance::DisplayRecording>(65536);
    session().explicitStart=ExplicitStart(arguments);
    const auto touchRoute=fulltouch::route(arguments);
    if(touchRoute==fulltouch::Route::Invalid)return 6;
    session().launcherDiagnostic=touchRoute==fulltouch::Route::LegacyDiagnostic;
    unsigned agentFlags=0;for(auto arg:arguments)if(arg=="--agent-input-v1")++agentFlags;
    if(agentFlags>1 || (agentFlags && session().launcherDiagnostic))return 7;
    session().agentRequested=agentFlags==1; // explicit/default-off, not transport authorization
    const auto authorize=agentrun::launch(arguments,!session().launcherDiagnostic,measurementRequest.request!=MeasurementRequest::Disabled);
    if(authorize==agentrun::Launch::Invalid)return 8;
    if(authorize==agentrun::Launch::Enabled && !session().agentAuthorization.enable())return 8;
    const auto trustReview=agentdiscardstream::launch(arguments,!session().launcherDiagnostic,measurementRequest.request!=MeasurementRequest::Disabled);
    if(trustReview==agentdiscardstream::Launch::Invalid)return 8;
    session().agentTrustHostReview=trustReview==agentdiscardstream::Launch::Enabled;
    session().virtualStart=VirtualControllerStart(arguments);
    unsigned recordedInputFlags=0;for(auto arg:arguments)if(arg=="--recorded-input")++recordedInputFlags;
    session().recordedInputRequested=recordedInputFlags==1;
    unsigned aimFlags=0;for(auto arg:arguments)if(arg=="--recorded-aim-input")++aimFlags;
    session().recordedAimRequested=aimFlags==1 && session().recordedInputRequested;
    unsigned captureFlags=0;for(auto arg:arguments)if(arg=="--metal-capture")++captureFlags;
    session().metalCaptureRequested=captureFlags==1 && session().recordedInputRequested;
    unsigned lifecycleFlags=0;for(auto arg:arguments)if(arg=="--resumable-lifecycle")++lifecycleFlags;
    session().resumableLifecycleRequested=lifecycleFlags==1 || !session().launcherDiagnostic;
    if(session().resumableLifecycleRequested && !iosGpuLifecycle().activate())return 4;
    for(auto arg:arguments)if(arg.starts_with("--ios-bot-session=")) {
        if(!session().botSession.empty())return 9;
        session().botSession=std::string(arg.substr(std::string_view("--ios-bot-session=").size()));
        if(!iosbot::hex(session().botSession,32))return 9;
    }
    if(!session().botSession.empty() && (session().agentRequested || session().launcherDiagnostic || session().recordedInputRequested))return 9;
    if(const char* qos=std::getenv("RECOMP_IOS_RENDER_QOS_EXPERIMENT")) {
        if(std::strcmp(qos,"1") || session().botSession.empty() || measurementRequest.request!=MeasurementRequest::Enabled)return 11;
        session().rendererQosRequested=true;
    }
    session().asyncWorkerQosRequested=iosasyncworkerqos::parse(std::getenv("RECOMP_IOS_ASYNC_WORKER_QOS"));
    if(session().asyncWorkerQosRequested) {
        if(session().botSession.empty() || measurementRequest.request!=MeasurementRequest::Enabled ||
           !std::getenv("RECOMP_IOS_RENDERER_ASYNC") || std::strcmp(std::getenv("RECOMP_IOS_RENDERER_ASYNC"),"1") ||
           !std::getenv("RECOMP_IOS_QUEUED_PRESENT") || std::strcmp(std::getenv("RECOMP_IOS_QUEUED_PRESENT"),"0"))return 11;
    }
    std::fprintf(stderr,"APPLE_IOS_ASYNC_WORKER_QOS startup requested=%u low_power=%u thermal=%ld\n",unsigned(session().asyncWorkerQosRequested),unsigned(NSProcessInfo.processInfo.isLowPowerModeEnabled),long(NSProcessInfo.processInfo.thermalState));
    return UIApplicationMain(argc,argv,nil,NSStringFromClass(PortApp.class));
}}
