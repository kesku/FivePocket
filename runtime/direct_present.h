#pragma once
#define RECOMP_IOS_SWITCHES_NO_MACRO
#include "ios_switches.h" // (Claude) the scaler poll's switch files go through the launch-environment overrides (by name, no macro)
#undef RECOMP_IOS_SWITCHES_NO_MACRO
// Attempt v255 (goal step 5 prerequisite, "direct present"). Until v254 every displayed frame went GPU -> CPU -> GPU: the
// renderer read the 1280x720 front buffer back (slot readback, opaqueRgbaInto), the presenter thread mapped it through
// the scan-out (VulkanEngine::mapNativeScanout: identity scaler copy + the game's gamma ramp as three 256-entry tables),
// and the app packed it for its window on the CPU (packNativeRgbaSurface: letterbox, integer nearest neighbour, black
// borders, alpha 255, R/B exchanged for BGRA), uploaded it with its own VkDevice, read it back for an audit, presented
// and waited for its queue to go idle, per frame.
//
// With files/private-trace/diag-direct-present (read once; device.cpp enables the window-system extensions only then) and
// an app that passes its window (RecompPrivateTraceCallbacksV6), the library owns the window's swapchain for the run:
//  - GPU frames (Renderer::present, when the gamma tables are known): one fullscreen pass samples the renderer's present
//    image and writes each window pixel as reference() defines it, then vkQueuePresentKHR (FIFO, as the app).
//  - CPU frames (everything else the app would have been given: loading screens, synchronous frames): the already mapped
//    bytes are uploaded and drawn by the same pass with identity tables.
// reference() is packNativeRgbaSurface(gammaInPlace(source)) (tests/direct_present_test.cpp checks it against both);
// the first frames of each kind, and one frame whenever files/private-trace/diag-direct-audit exists, are copied back
// and compared byte for byte with reference() of the same source (RENDER_DIRECT_AUDIT). A GPU mismatch turns the GPU
// path off (frames go back through the CPU path); a CPU mismatch releases the window to the app's own renderer.
//
// v42x (agent presenter-gpu): two cheaper forms of the cubic (mode 2), each its own pipeline (direct_present.frag and its
// SPIR-V stay as v416's): files/private-trace/diag-v42x-scale-gather = mode 4 (12 gathers of the picture, no per-tap
// conversions), diag-v42x-scale-packed = modes 6/7 (4 gathers of the picture's R32_UINT view, the bytes unpacked in the
// shader; its own MUTABLE_FORMAT pictures); both with diag-v409-scale-cubic, packed wins when both exist, both override
// diag-v411-scale-compute. Their map pass is direct_present_map.frag (mode 0 at 1:1 without its divisions, alpha 0). The
// shaders take the reference's own fp32 scale (push constants, now 32 bytes). Audited like every filtered mode (keys 6 and 7;
// a failure turns that mode off for the run, back to v410's path); pass_us slots 5 (gather) and 6 (packed).
//
// v450 (agent fsr1): AMD FidelityFX Super Resolution 1 as a further output scaler, files/private-trace/diag-v450-scale-fsr1
// (polled with the other scaler switches; it wins over the cubic / linear / fast / compute modes, which take over again when it
// is removed or fails). Passes: the fast map pass (the tables at 1:1 into v410's picture) -> EASU (direct_present_fsr_easu.frag,
// AMD's ffx_fsr1.h, unmodified) into the swapchain image's own RGBA8 picture of the image rect -> RCAS
// (direct_present_fsr_rcas.frag) into the swapchain image (letterbox black, alpha 1). diag-v450-fsr1-no-rcas: EASU straight into
// the swapchain image (no picture). files/private-trace/diag-v450-fsr1-stops (text, 0..4; default fsr1::DefaultStops = 0.2):
// RCAS sharpness in stops. Constants: AMD's FsrEasuCon / FsrRcasCon on the CPU (fsr1_cpu.h), pushed (80 bytes, a layout of its
// own). Filtering happens on the display-encoded (gamma) picture, as every other mode. The first entry into each FSR mode (and
// a sharpness change) audits a GPU frame against fsr1_reference.h (two stages: EASU's picture, then the window from RCAS of
// the GPU's own picture; <= 1 per colour channel, alpha / border exact, EASU's luma-tie pixels within their 2x2 clamp bounds
// +/- 1); a failure turns FSR off for the run. Pass times: slots 7 (EASU + RCAS) and 8 (EASU only) in RENDER_DIRECT_FSR (map /
// EASU split), RENDER_DIRECT_PRESENT unchanged. Memory while on: one RGBA8 picture of the rect per swapchain image (5 x 8.3 MB
// at 1920x1080), created at the first FSR frame; FSR's pipelines are built at that frame too (a one-off hitch, logged).
// v460 (agent gsr1): Qualcomm Snapdragon Game Super Resolution 1 (SGSR 1, BSD-3-Clause; direct_present_gsr.glsl is derived from
// Qualcomm's shader) as a further output scaler, files/private-trace/diag-v460-scale-gsr1 (polled with the other scaler
// switches). Precedence: GSR 1 > FSR 1 (diag-v450-scale-fsr1) > cubic / linear (diag-v409-*) > nearest; when its switch is
// removed, or GSR fails, the next one in that order takes over again. Absent switch = v458: no GSR object is ever created
// (sampler, set pool, shader modules, pipelines and sets are all made at the first GSR frame) and every decision below
// reduces to v458's. Passes: the fast map pass (the tables at 1:1 into v410's picture) -> one GSR pass
// (direct_present_gsr.frag; with diag-v460-gsr1-edge-direction, direct_present_gsr_edge.frag) reading that picture through a
// LINEAR sampler straight into the swapchain image (letterbox black, alpha 1). No intermediate picture of its own.
// Tuning (text files, read with the switch; absent / unreadable / out of range = Qualcomm's default):
// diag-v460-gsr1-threshold (edge threshold in 1/255 units, 1..64, default 8) and diag-v460-gsr1-sharpness (1.0..2.0, default
// 2.0). Constants are pushed (48 bytes on FSR's 80-byte layout). The first entry into each GSR variant (and a tuning change)
// audits a GPU frame against gsr1_reference.h (ranges: see that file); a failure turns GSR off for the run unless
// diag-v460-gsr1-keep exists (then it is only logged). The tuning of the last GSR frame is kept while the switch is absent,
// so switching GSR on again with unchanged tuning files does not audit again. Pass time: slot 9 in RENDER_DIRECT_GSR.
#include "device.h"
#include "diag_timeline.h" // v318 (the latency probe's present events)
#include "lag_v337.h" // v337 (display lag histograms)
#include "pace_caps.h" // pace-caps (lane A): diag-pace-* (the slot of a paced frame; display times for the vsync estimate)
#include "fg_pair.h" // frame-gen (lane B): diag-fg-pair-* (the pair logic of the frame-generation option, stage 1a)
#include "fg_burst.h" // fg-burst (lane B, stage 1a): diag-fg-burst (the present-call and display times of the captured frames)
#include "fg_gen.h" // framegen-1b: diag-fg-between (the generator of the frame-generation option: its decisions and its slot keeper)
#include "latency_lat2.h" // latency-path (lat2): signed present margin, GPU-done watcher hand-off
#include "submit_early.h" // submit-early (package Q): the watcher's GPU -> steady clock offset for the chunk trace
#include "direct_present_reference.h"
#include "fsr1_reference.h" // v450 (agent fsr1): FSR 1's CPU constants (AMD's) and the audit's reference
#include "scaler_settings.h" // lane fsr-sharpness: files/settings/scaler.txt and fsr-sharpness.txt (the poll's decision)
#include "gsr1_reference.h" // v460 (agent gsr1): Snapdragon GSR 1's constants, precedence and the audit's reference
#include "present_source.h" // lane fsr-stage-b: the render-size source of GPU frames (render-resolution stage B)
#include "vulkan_queue_lock.h"
#ifdef RECOMP_THOR_EDITION
#include "hud_screen.h" // hud2 (v575) / hud3 (v585): the second screen's HUD
#endif /* RECOMP_THOR_EDITION */
#include "cpu_topology.h" // topology lane: the GPU-done watcher's little role
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <thread>
#include <cstring>
#include <cstdio> // v450 (diag-v450-fsr1-stops)
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#if defined(__ANDROID__)
#include <android/log.h>
#include <pthread.h> // latency-path (lat2): the watcher's name
#include <sched.h> // latency-path (lat2): the watcher's little-core mask
#include <unistd.h>
#include <vulkan/vulkan_android.h>
#include "direct_present_spirv.h"
#include "fg_gen_spirv.h" // framegen-1b: the generator's five passes
#elif defined(__APPLE__)
// ios-direct-present (Claude, 2026-10-16): the same presenter on iOS/iPadOS. The app's "window" is its CAMetalLayer, passed
// through V6's acquireWindow as an opaque pointer; MoltenVK makes the surface (VK_EXT_metal_surface, which device.cpp enables
// with diag-direct-present). Everything else (passes, scalers, pacing through VK_GOOGLE_display_timing) is the Android code.
#include <android/log.h> // (platform/apple/include/android/log.h: the port's log shim)
#include <pthread.h>
#include <unistd.h>
#include <vulkan/vulkan_metal.h>
#include "direct_present_spirv.h"
#include "fg_gen_spirv.h"
#endif
struct ANativeWindow;
#include "recomp_app_dir.h" // data-paths: the app's files directory.
namespace recomp::gpu {
bool probeSwapchainEnabled(VkDevice device); // device.cpp (v255)
bool displayTimingEnabled(); // device.cpp (v318): VK_GOOGLE_display_timing enabled on the swapchain device
bool directPresentWanted();                  // device.cpp (v255)
VkQueue probePresentQueue(VkDevice device);  // device.cpp (v259): a queue of the family only for vkQueuePresentKHR, or null
unsigned probeFamilyQueueCount();            // device.cpp (v259)
std::pair<VkQueue,uint32_t> probeSideQueue(VkDevice device); // device.cpp (ios-side-queue): another family's queue, or null
}
extern "C" std::atomic<int> recomp_ios_render_sixteenths; // (generic_renderer.cpp: the applied render scale, 16 = 100 %)
extern "C" std::atomic<int> recomp_ios_settings_upscaler; // (generic_renderer.cpp: the player's scaler.txt picks an upscaler)
extern "C" std::atomic<unsigned> recomp_ios_layout_gen; // (generic_renderer.cpp; build 192: bumped by a drawable-size change)
inline unsigned recompIosExtentLayoutGen=0,recompIosAuditLayoutGen=0; // (build 192: the layout generation the extent was checked at / the audit was recorded at)

namespace recomp::gpu::direct {
// v410: the presenter's filter mode (0 nearest, 1 linear, 2 cubic), for the renderer's RENDER_GENERIC_V210Y line (ab_windows.py).
inline std::atomic<unsigned> presentedFilter{0};
inline std::atomic<unsigned> presentedCompute{0}; // v411: 1 while filtered frames take the compute path
inline std::atomic<unsigned> presentedFast{0}; // v42x: 1 while cubic frames are wanted as mode 4 (gather), 2 as modes 6/7 (packed)
inline std::atomic<unsigned> presentedFsr{0}; // v450: 1 while frames are scaled by FSR 1 EASU + RCAS, 2 by EASU only (0 otherwise)
inline std::atomic<unsigned> presentedGsr{0}; // v460: 1 while frames are scaled by Snapdragon GSR 1, 2 with edge direction (0 otherwise)
// v412: the pass-time slot of a frame (nearest, linear, cubic, linear compute, cubic compute). v411 used filter + 1 + 3 for
// the compute path: cubic compute indexed slot 5 of 5 (wrote past passNs into passN[0]).
// v42x: + cubic gather (5) and cubic packed (6); fast = 0 (none), 1 (gather) or 2 (packed), only ever set with the cubic.
// v450: + FSR EASU + RCAS (7) and EASU only (8); fsr = 0 (none), 1 or 2 wins over everything else.
constexpr unsigned PassSlots=11; // (v460: + GSR 1 (9) = passSlot(.., fsr = gsr1::Mode); lane fsr-stage-b: + the 1:1 copy (rsb::OneSlot = 10))
static_assert(rsb::OneSlot==PassSlots-1,"the 1:1 copy's slot");
constexpr unsigned passSlot(unsigned filter,bool compute,unsigned fast=0,unsigned fsr=0) noexcept {
    return fsr?6u+fsr:fast?4u+fast:filter+(compute && filter?2u:0u);
}
static_assert(passSlot(0,false)==0 && passSlot(1,false)==1 && passSlot(2,false)==2 && passSlot(1,true)==3 && passSlot(2,true)==4 &&
              passSlot(0,true)==0,"pass slots");
static_assert(passSlot(2,false,1)==5 && passSlot(2,false,2)==6 && passSlot(2,true,2)==6,"v42x pass slots");
static_assert(passSlot(2,true,2,1)==7 && passSlot(0,false,0,2)==8 && passSlot(2,false,1,2)==8,"v450 pass slots");
static_assert(passSlot(2,true)<PassSlots && passSlot(2,false,2)<PassSlots && passSlot(0,false,0,2)<PassSlots,"every slot inside passNs");
static_assert(passSlot(0,false,0,gsr1::Mode)==9 && passSlot(2,true,2,gsr1::Mode)==9 && passSlot(0,false,0,gsr1::Mode)<PassSlots,"v460 pass slot");
// The app's window (RecompPrivateTraceCallbacksV6): acquire returns a new reference (or null) and the generation it
// belongs to; generation changes whenever the app's window is replaced or detached.
struct Window {
    void* user=nullptr;
    ANativeWindow* (*acquire)(void*,std::uint64_t*) noexcept=nullptr;
    void (*release)(void*,ANativeWindow*) noexcept=nullptr;
    std::uint64_t (*generation)(void*) noexcept=nullptr;
};

#if defined(__ANDROID__) || defined(__APPLE__)
#define RECOMP_DIRECT_LOG(...) __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",__VA_ARGS__)
// pace-caps (lane A): the panel's refresh length as VK_GOOGLE_display_timing reports it for this swapchain, handed to the pacer as
// the first guess of its vsync estimate (pace_caps.h setRefresh; 0 = not known, the pacer then assumes ~120 Hz as before). One
// read-only query per swapchain; out of line and noexcept, so the swapchain's creation keeps its control flow (PGO record).
[[gnu::noinline]] inline void paceRefresh(VkDevice device,VkSwapchainKHR swapchain) noexcept {
    VkRefreshCycleDurationGOOGLE r{};
    const auto get=displayTimingEnabled()?reinterpret_cast<PFN_vkGetRefreshCycleDurationGOOGLE>(vkGetDeviceProcAddr(device,"vkGetRefreshCycleDurationGOOGLE")):nullptr;
    recomp::pace::refreshIs(get && get(device,swapchain,&r)==VK_SUCCESS?r.refreshDuration:0);
}
class Presenter {
public:
    void setWindow(const Window& w){std::lock_guard l(m);window=w;}
    bool hasWindow(){std::lock_guard l(m);return window.acquire && window.release && window.generation;}
    void setDeviceSource(std::function<ProbeDevice*()> f){std::lock_guard l(m);deviceSource=std::move(f);}
    // The CPU bridge's first frame: true when the library presents for the rest of the run (the app never attaches).
    bool claim() {
        std::lock_guard l(m);
        if(claimed)return !released_;
        if(tried)return false;
        tried=true;
        const char* why=nullptr;
        ProbeDevice* d=deviceSource?deviceSource():nullptr;
        if(!directPresentWanted())why="switch-off";
        else if(!window.acquire || !window.release || !window.generation)why="no-window-callbacks";
        else if(!d || !d->device)why="no-device";
        else if(!probeSwapchainEnabled(d->device))why="no-swapchain-extension";
#if defined(__APPLE__)
        // ios-direct-present: the app gives its layer only once it is laid out and on screen (acquire returns null before).
        // That is not a failure: nothing is created, and the next frame asks again (claimPending(); the frame is dropped).
        if(!why) {
            std::uint64_t g=0;
            if(ANativeWindow* w=window.acquire(window.user,&g))window.release(window.user,w);
            else {
                tried=false;++claimRetries;
                if(claimRetries==1 || !(claimRetries%120))RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT claim=wait why=window-not-ready tries=%u",claimRetries);
                return false;
            }
        }
#endif
        if(!why){dev=d;if(!loadFunctions())why="functions";else if(!createStatic())why="static-objects";else if(!ensureSwapchain())why="swapchain";}
        if(why) {
            RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT claim=0 why=%s",why);
            destroyAll();return false;
        }
        claimed=true;
        // v259: vkQueuePresentKHR waits for the frame's GPU work on this driver (~4 ms per frame in v256, on the renderer's
        // worker and under the shared queue lock): it runs on its own thread and, when the family offers one, its own queue.
        presentQueue=probePresentQueue(dev->device);
        try {presentThread=std::thread([this]{presentLoop();});} catch(...) {presentQueue=VK_NULL_HANDLE;}
        // latency-path (lat2, measurement only): files/private-trace/diag-lat-gpu-done (read once) lets the armed timeline record
        // each tagged frame's presenter-pass GPU completion (PresenterGpuDone) from a watcher thread on the little cores.
        gpuDoneOn=access(RECOMP_APP_PATH("/private-trace/diag-lat-gpu-done"),F_OK)==0;
        motionOn=gpuDoneOn && access(RECOMP_APP_PATH("/private-trace/diag-lat-motion"),F_OK)==0;
        RECOMP_DIRECT_LOG("RENDER_LAT2 gpu_done=%u motion=%u auditable=%u",unsigned(gpuDoneOn),unsigned(motionOn),unsigned(auditable));
        RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT claim=1 extent=%ux%u format=%d images=%zu transform=%u auditable=%u family_queues=%u present_queue=%u "
            "present_thread=%u",extent.width,extent.height,int(format),images.size(),unsigned(currentTransform),unsigned(auditable),probeFamilyQueueCount(),
            unsigned(presentQueue!=VK_NULL_HANDLE),unsigned(presentThread.joinable()));
        return true;
    }
    bool released() {std::lock_guard l(m);return released_;}
    // ios-direct-present: the first claim found no laid-out window and will be tried again at the next frame.
    bool claimPending() {std::lock_guard l(m);return !tried && !claimed;}
    // Claimed and not failed: frames it could not show were dropped, not lost with the presenter.
    bool usable() {std::lock_guard l(m);return claimed && !released_ && !broken;}
    // After a failure: nothing of the presenter stays on the window (the app's renderer attaches next).
    void giveBack() {std::lock_guard l(m);if(dev)destroyAll();released_=true;gpuOn.store(false,std::memory_order_release);}
    // Whether the renderer's GPU frames are wanted (the tables are then worth computing).
    bool gpuWanted() {std::lock_guard l(m);return claimed && !released_ && !broken && !gpuAuditFailed;}
    // True once GPU frames may be presented directly (claimed, the tables known, no GPU audit mismatch).
    bool gpuReady() const noexcept {return gpuOn.load(std::memory_order_acquire);}
    // lane fsr-stage-b (review): the renderer is about to destroy `view` (it re-creates its present image: in a scaled run at
    // every change between a render-size frame and a read-back frame). If that view is the one setFor()'s cache names, the
    // presenter first waits for its own submitted passes (they may still sample the image) and then forgets the set, so
    // a new view with the same handle value gets a set written for it. Called only in a scaled run (Renderer::rsbForget).
    [[gnu::noinline]] void forgetSource(VkImageView view) noexcept {
        std::lock_guard l(m);
        if(view!=cachedView || !cachedSet)return;
        if(dev)waitAllMine();
        cachedView=VK_NULL_HANDLE;cachedSet=VK_NULL_HANDLE;++rsbForgets;
    }
    // frame-gen (lane B): the diag-fg-pair-* switch word (fg_pair.h Sw*), from the renderer's 60-present window; read at the next GPU frame.
    void fgSwitches(unsigned bits) noexcept {fg.setSwitches(bits);}
    void setLut(const Lut& lut) {
        std::lock_guard l(m);
        if(!claimed || released_ || !lutMapped)return;
        for(unsigned i=0;i<768;++i)lutMapped[i]=lut[i];
        gameLut=lut;
        gpuOn.store(!gpuAuditFailed,std::memory_order_release);
        unsigned changed=0;for(unsigned i=0;i<768;++i)changed+=lut[i]!=std::uint8_t(i%256);
        RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT lut=1 non_identity_entries=%u gpu=%u",changed,unsigned(!gpuAuditFailed));
    }
    // A mapped 1280x720 RGBA frame (the bytes the app would have been given).
    bool presentCpu(const std::uint8_t* rgba,std::size_t bytes,unsigned width,unsigned height) {
        std::lock_guard l(m);
        if(!claimed || released_ || broken || !rgba || bytes!=SourceBytes || width!=SourceWidth || height!=SourceHeight)return false;
        if(!ensureCpuImage())return false;
        const unsigned k=cpuNext;cpuNext^=1;
        if(stageSlot[k]>=0 && !waitAcquireSlot(unsigned(stageSlot[k])))return false;
        std::memcpy(stageMapped[k],rgba,SourceBytes);
        const auto t0=std::chrono::steady_clock::now();
        const bool ok=presentLocked(dev->queue,cpuImage.view,cpuImage.image,true,[&](VkCommandBuffer cb) {
            const bool first=!cpuImageGeneral;cpuImageGeneral=true; // (recorded only when a frame is recorded)
            VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=cpuImage.image;
            b.oldLayout=first?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
            b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            // (the previous pass's reads of the image, earlier in submission order, finish first)
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent={SourceWidth,SourceHeight,1};
            vkCmdCopyBufferToImage(cb,stage[k].buffer,cpuImage.image,VK_IMAGE_LAYOUT_GENERAL,1,&c);
            b.oldLayout=b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
        },&stageSlot[k]);
        cpuNs+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());
        ++(ok?cpuPresents:cpuDropped);
        return ok;
    }
    // The renderer's present image (1280x720 RGBA8 UNORM, GENERAL layout, sampled usage), written by commands submitted
    // to `queue` before this call and followed there by a full barrier (Renderer::submitCurrent). False: not shown.
    // v318: `tag` (the frame's present sequence, 0 = none) follows the image to presentNow for the latency probe.
    bool presentGpu(VkQueue queue,VkImage image,VkImageView view,std::uint32_t tag=0) {
        std::unique_lock l(m);
        if(!gpuOn.load(std::memory_order_relaxed) || !claimed || released_ || broken)return false;
        const auto t0=std::chrono::steady_clock::now();
#if defined(__APPLE__)
        iosLk=&l;struct ClearLk{std::unique_lock<std::mutex>*& p;~ClearLk(){p=nullptr;}} clearLk{iosLk};
        if(fgEver)iosAwaitPresented(); // ios-fg-one-acquire: the previous frame's pictures have had their present calls
        if(!gpuOn.load(std::memory_order_relaxed) || !claimed || released_ || broken)return false;
#endif
        currentTag=tag;
        fgBefore(queue,image,tag); // frame-gen (lane B): between = the made picture first (no diag-fg-pair-* file: a load and a return)
        const bool ok=presentLocked(queue,view,image,false,{},nullptr);
        fgAfter(queue,image,tag,ok); // frame-gen (lane B): ahead = the made picture second; the pair goes to the present thread in order
        currentTag=0;
        gpuNs+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());
        ++(ok?gpuPresents:gpuDropped);
        return ok;
    }
    void shutdown() {
#ifdef RECOMP_THOR_EDITION
        hud2::screen().shutdown(); // hud2 (v575): its present thread first (it may hold the present queue's lock)
#endif /* RECOMP_THOR_EDITION */
        {std::lock_guard l(m);stopPresenter=true;gpuOn.store(false,std::memory_order_release);}
        presentCv.notify_all();
        if(presentThread.joinable())presentThread.join(); // (outside m: the thread takes m)
        stopGpuDone(); // latency-path (lat2): before any fence is destroyed
        std::lock_guard l(m);
        if(dev)destroyAll();
        claimed=false;released_=true;
    }
private:
    struct Buffer {VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;void* mapped=nullptr;VkDeviceSize size=0;};
    struct Img {VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;};
    struct Acquire {VkSemaphore sem=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;bool submitted=false;};
    static constexpr const char* Switches=RECOMP_APP_PATH("/private-trace/");
    static constexpr std::uint64_t AcquireTimeout=100'000'000ull,FenceTimeout=2'000'000'000ull;

    std::mutex m;
    std::thread presentThread;std::condition_variable presentCv;std::deque<std::uint32_t> pendingPresents; // v259 (under m)
#if defined(__APPLE__)
    // ios-fg-one-acquire (Claude 2026-10-17; c177-c180 GPU watchdog crashes with frame generation): MoltenVK hands out a swapchain
    // image before its drawable is free and makes the GPU wait for it inside a command buffer; the 3-drawable iOS swapchain then
    // deadlocked when a pair's second image was acquired while the first picture's present call was still queued behind it.
    // With frame generation on, every picture's present CALL is made before the next image is acquired: at most one image is
    // acquired and not yet presented, and every present command precedes any command buffer that waits for a drawable.
    std::condition_variable presentedCv;std::unique_lock<std::mutex>* iosLk=nullptr; // (iosLk: presentGpu's lock while it runs)
    std::uint64_t iosWaits=0,iosWaitNs=0,iosWaitTimeouts=0;
#endif
    std::deque<std::uint32_t> pendingTags;std::uint32_t currentTag=0; // v318: the frame tag of each pending present (under m)
    PFN_vkGetPastPresentationTimingGOOGLE pastTiming=nullptr;std::uint64_t displayTimes=0; // v318 (VK_GOOGLE_display_timing)
    // v337: files/private-trace/diag-v337-lag (read once): every tagged frame asks for its display time (lag_v337.h); the
    // present-call time of each tag (presenter thread only)
    bool lagOn=false;std::array<std::uint32_t,recomp::v337::Ring> callTag{};std::array<std::uint64_t,recomp::v337::Ring> callAt{};
    bool stopPresenter=false;VkQueue presentQueue=VK_NULL_HANDLE;std::mutex presentQueueMutex;
    Window window{};
    std::function<ProbeDevice*()> deviceSource;
    ProbeDevice* dev=nullptr;
    bool tried=false,claimed=false,released_=false,broken=false,recreate=false,lost=false,auditable=false,bgra=false,fbOnly=false;
    static bool iosFbOnlyWanted() noexcept { // (ios-fbonly) checked at swapchain creation and every 120 presents
#if defined(__APPLE__)
        return recomp_ios_switches_access((std::string(Switches)+"ios-fbonly").c_str(),F_OK)==0;
#else
        return false;
#endif
    }
    std::atomic<bool> gpuOn{false};
    bool gpuAuditFailed=false,cpuImageGeneral=false;
    ANativeWindow* nw=nullptr;std::uint64_t gen=0;
    unsigned claimRetries=0; // ios-direct-present (claim())
    std::uint64_t tmCalls=0,tmTagZero=0,tmRequested=0,tmQueries=0,tmReturned=0,tmActual=0; // ios-direct-present: RENDER_IOS_TIMING
    // ios-direct-present: on-screen frame times from the driver's actual display times (MTLDrawable presentedTime on iOS):
    // RENDER_IOS_DISPLAY every 120 displayed frames (1 ms bins; band = within 10 % of 33.33 ms). Presenter thread only.
    std::uint64_t dispLast=0;std::uint32_t dispLastId=0;std::uint64_t presentCallLastNs=0,dispLongLogs=0;std::array<std::uint32_t,128> dispBins{};std::uint32_t dispN=0,dispOver50=0,dispBand=0,dispClockLogs=0,dispBand16=0,dispOver33=0;
    std::uint64_t dispMax=0,dispSum=0,dispTotal=0;
    VkSurfaceKHR surface=VK_NULL_HANDLE;VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    VkFormat format=VK_FORMAT_UNDEFINED,passFormat=VK_FORMAT_UNDEFINED;VkExtent2D extent{};VkSurfaceTransformFlagBitsKHR currentTransform{};
    std::vector<VkImage> images;std::vector<VkImageView> views;std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderDone;std::vector<VkCommandBuffer> commands;std::vector<int> imageSlot;
    std::vector<Acquire> acquires;unsigned nextAcquire=0;
    VkCommandPool pool=VK_NULL_HANDLE;VkRenderPass pass=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;VkDescriptorPool setPool=VK_NULL_HANDLE;
    VkSampler sampler=VK_NULL_HANDLE;VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    VkImageView cachedView=VK_NULL_HANDLE;VkDescriptorSet cachedSet=VK_NULL_HANDLE;unsigned setsUsed=0;
    static constexpr unsigned MaxSets=64;
    Buffer lutBuffer;std::uint32_t* lutMapped=nullptr;Lut gameLut=identityLut();
    Img cpuImage;std::array<Buffer,2> stage{};std::array<void*,2> stageMapped{};std::array<int,2> stageSlot{-1,-1};unsigned cpuNext=0;
    Buffer auditSource,auditTarget;unsigned cpuAuditsLeft=3,gpuAuditsLeft=3;std::uint64_t audits=0,auditFailures=0,auditChecks=0;
    // v409: the scale filter (diag-v409-scale-cubic / -linear, polled every 120 presents; an audit failure turns it off for the
    // run); the first entry into each filtered mode audits the next GPU frame. Pass timestamps (when the queue family has them)
    // give the presenter's GPU time per mode, reported per 256 presents.
    Filter filter=Filter::Nearest,auditFilter=Filter::Nearest;bool filterFailed=false,auditCompute=false;unsigned filterPolls=0,filterAudited=0;
    std::uint64_t filterChanges=0;
    static constexpr unsigned MaxTimed=32;
    VkQueryPool timePool=VK_NULL_HANDLE;double tsNs=0;std::uint64_t tsMask=0;std::array<std::uint8_t,MaxTimed> slotTimed{};
    std::array<std::uint64_t,PassSlots> passNs{},passN{}; // v411: nearest, linear, cubic, linear (compute), cubic (compute)
    // v42x (measurement only): the map pass's end per timed frame, in a pool of its own (the main pool's 2 per slot are
    // unchanged), so the log splits each slot's pass_us into the map pass (map_us) and the rest.
    VkQueryPool midPool=VK_NULL_HANDLE;std::array<std::uint8_t,MaxTimed> slotMid{};std::array<std::uint64_t,PassSlots> mapNs{},mapN{};
    // v410: a filtered frame maps the source through the tables once, at 1280x720, into the image's own intermediate (a pass
    // with the nearest pipeline: identical bytes to the v255 pass at 1:1), and the filter pass reads that with no table loads
    // (lutBase 1536). One intermediate per swapchain image: a frame re-records image i's commands only after the previous
    // frame on i completed (CPU and GPU frames may use different queues). Their sets come from a pool of their own.
    std::vector<Img> mapImages;std::vector<VkFramebuffer> mapFramebuffers;std::vector<VkDescriptorSet> mapSets;
    VkRenderPass mapPass=VK_NULL_HANDLE;VkDescriptorPool mapSetPool=VK_NULL_HANDLE;bool mapFailed=false;
    // v411: the filtered scale as a compute pass (files/private-trace/diag-v411-scale-compute; else v410's passes): each 16x16
    // group of output pixels loads its source footprint into shared memory once (tables applied as it loads) and writes the
    // filtered picture (image rect size) to the swapchain image's own storage image, which the swapchain pass copies 1:1 (mode
    // 3). Compute sets per (source view, image), from a pool of their own; the copy sets from mapSetPool.
    VkShaderModule scaleModule=VK_NULL_HANDLE;VkDescriptorSetLayout scaleSetLayout=VK_NULL_HANDLE;VkPipelineLayout scalePipelineLayout=VK_NULL_HANDLE;
    VkPipeline scalePipeline=VK_NULL_HANDLE;VkDescriptorPool scaleSetPool=VK_NULL_HANDLE;static constexpr unsigned MaxScaleSets=64;
    std::vector<Img> scaleImages;std::vector<VkDescriptorSet> scaleCopySets;std::uint32_t scaleW=0,scaleH=0;
    struct ScaleSet {VkImageView view;std::uint32_t image;VkDescriptorSet set;};std::vector<ScaleSet> scaleSets;
    bool scaleFailed=false,computeOn=false;unsigned filterKey=0;
    // v42x: the fast cubic modes. fastWant (from the switches, 0 / 1 gather / 2 packed), fastFailed (bit fast: off for the run
    // after an audit or resource failure), auditFast (the audited frame's mode). Pipelines: the map pass (mode 5) and the
    // swapchain passes (modes 4, 6/7), all on pipelineLayout. The packed mode's pictures: per swapchain image, the map pass's
    // target (the swapchain format, MUTABLE_FORMAT), its R32_UINT view, framebuffer and set (from mapSetPool).
    VkShaderModule mapFastModule=VK_NULL_HANDLE,gatherModule=VK_NULL_HANDLE,packedModule=VK_NULL_HANDLE;
    VkPipeline mapFastPipeline=VK_NULL_HANDLE,gatherPipeline=VK_NULL_HANDLE,packedPipeline=VK_NULL_HANDLE;
    unsigned fastWant=0,fastFailed=0,auditFast=0;
    std::vector<Img> packImages;std::vector<VkImageView> packViews;std::vector<VkFramebuffer> packFramebuffers;std::vector<VkDescriptorSet> packSets;
    // v450 (agent fsr1): FSR 1. fsrWant (from the switches: 0, 1 EASU + RCAS, 2 EASU only), fsrStops (RCAS sharpness), fsrFailed
    // (off for the run after an audit or resource failure), auditFsr / auditFsrCon (the audited frame's mode and constants).
    // Pipelines on fsrLayout (setLayout + an 80-byte push range): EASU into the picture (fsrPass, RGBA8), EASU into the swapchain
    // image and RCAS into the swapchain image (pass). Per swapchain image: the picture (the image rect's size, RGBA8; sampled, and
    // a transfer source for the audit), its framebuffer and set (fsrSetPool). fsrMidPool: EASU's end per timed frame.
    VkShaderModule easuModule=VK_NULL_HANDLE,rcasModule=VK_NULL_HANDLE;VkPipelineLayout fsrLayout=VK_NULL_HANDLE;
    VkRenderPass fsrPass=VK_NULL_HANDLE;VkPipeline easuPipeline=VK_NULL_HANDLE,easuDirectPipeline=VK_NULL_HANDLE,rcasPipeline=VK_NULL_HANDLE;
    VkDescriptorPool fsrSetPool=VK_NULL_HANDLE;static constexpr unsigned MaxFsrSets=16;
    std::vector<Img> fsrImages;std::vector<VkFramebuffer> fsrFramebuffers;std::vector<VkDescriptorSet> fsrSets;std::uint32_t fsrW=0,fsrH=0;
    unsigned fsrWant=0,auditFsr=0;bool fsrFailed=false,fsrBuilt=false;float fsrStops=fsr1::DefaultStops;fsr1::Constants fsrCon{},auditFsrCon{};
    Buffer auditMid;
    // lane fsr-sharpness: what the last poll found in files/settings (to log a change; Unset / false / 0 = v458's rules decided),
    // and the sharpness v458's rule last gave (diagnostic file or default): only a change of THAT value audits a frame again
    settings::Scaler settingsScaler=settings::Scaler::Unset;bool settingsStops=false;unsigned settingsIgnored=0;float ruleStops=fsr1::DefaultStops;
    VkQueryPool fsrMidPool=VK_NULL_HANDLE;std::array<std::uint8_t,MaxTimed> slotFsrMid{};std::array<std::uint64_t,PassSlots> easuNs{},easuN{};
    std::uint64_t fsrFrames=0;
    // v460 (agent gsr1): Snapdragon GSR 1. gsrWant / gsrEdge / gsrThreshold / gsrSharpness / gsrKeep (from the switch and tuning
    // files), gsrFailed (off for the run after an audit or resource failure), gsrCon / auditGsrCon (the pushed constants; the
    // audited frame's). Everything below is created at the first GSR frame: the linear sampler, the set pool, one module and
    // one pipeline per variant (0 mobile, 1 edge direction; fsrLayout, into `pass`), and per swapchain image a set reading
    // v410's picture through the linear sampler (reset with the pictures in destroyMaps).
    VkSampler gsrSampler=VK_NULL_HANDLE;VkDescriptorPool gsrSetPool=VK_NULL_HANDLE;static constexpr unsigned MaxGsrSets=16;
    std::array<VkShaderModule,2> gsrModule{};std::array<VkPipeline,2> gsrPipeline{};std::array<bool,2> gsrModuleTried{},gsrPipelineTried{};
    std::vector<VkDescriptorSet> gsrSets;
    bool gsrWant=false,gsrEdge=false,gsrFailed=false,gsrStaticTried=false,gsrKeep=false;
    float gsrThreshold=gsr1::DefaultThreshold255,gsrSharpness=gsr1::DefaultSharpness;gsr1::Constants gsrCon{},auditGsrCon{};
    std::uint64_t gsrFrames=0;
    // lane fsr-stage-b (render-resolution stage B; present_source.h): a GPU frame whose source is not 1280x720 (rsb::gpuSource,
    // set once by the renderer when files/settings/render-scale.txt applies a scale). Nothing below is created, and no
    // decision changes, while the source is 1280x720.
    //  - big*: per swapchain image a render-size map picture (the tables at 1:1 by the fast map pipeline), its framebuffer
    //    (mapPass), a set reading it through the NEAREST sampler (EASU's gathers, the 1:1 copy) and, for GSR frames, one
    //    through GSR's LINEAR sampler; sets from bigSetPool. Destroyed with the other per-image pictures (destroyMaps).
    //    bigFailed: they could not be made (or the fast map pipeline is missing): every later frame takes the down path.
    //  - down*: one 1280x720 RGBA8 image (GENERAL layout, as cpuImage) and its set: a frame that no render-size path takes
    //    is blitted into it (LINEAR: stage A's blit) and v463's passes run on it. Made before the frame's acquire; if it
    //    cannot be made the GPU path is turned off for the run (frames then come through the CPU path at 1280x720).
    //  - oneFailed: the 1:1 copy failed its audit (the frame's scaler or the down path takes such frames then);
    //    auditOne / auditW / auditH: the audited frame's path and the extent of the source picture read back for it;
    //    auditBlocked: no buffer for a render-size source readback (no audit of any kind then: recordAudit must not overflow).
    static constexpr unsigned MaxBigSets=32;
    std::vector<Img> bigImages;std::vector<VkFramebuffer> bigFramebuffers;std::vector<VkDescriptorSet> bigSets,bigGsrSets;
    VkDescriptorPool bigSetPool=VK_NULL_HANDLE;std::uint32_t bigW=0,bigH=0;bool bigFailed=false,bigPoolTried=false,oneFailed=false,auditOne=false;
    Img downImage;VkDescriptorPool downSetPool=VK_NULL_HANDLE;VkDescriptorSet downSet=VK_NULL_HANDLE;bool downTried=false,downGeneral=false;std::uint64_t rsbForgets=0;
    std::uint32_t auditW=SourceWidth,auditH=SourceHeight;bool auditBlocked=false;
    std::array<std::uint64_t,4> rsbFrames{};rsb::Size rsbLogged{};std::uint64_t rsbDownBlits=0;
    std::uint32_t fgW=SourceWidth,fgH=SourceHeight;bool fgSizeLogged=false; // frame generation's two pictures (the source's size when they were made)
    std::uint64_t cpuPresents=0,cpuDropped=0,gpuPresents=0,gpuDropped=0,cpuNs=0,gpuNs=0,acquireNs=0,presentNs=0,
        timeouts=0,outOfDate=0,suboptimal=0,surfaceLost=0,recreated=0,extentChecks=0,extentChanges=0,presentCalls=0;
    VkQueue lastQueue=VK_NULL_HANDLE; // v259: the queue of the latest pass (the present without a present queue)
    // latency-path (lat2): the GPU-done watcher (diag-lat-gpu-done; jobs only while the timeline is armed). A job carries copies
    // of every handle it uses; slotWatch keeps the presenter from resetting, resubmitting or destroying a watched fence.
    struct GpuDoneJob {VkDevice device;VkFence fence;VkQueryPool pool;double tsNs;std::uint64_t tsMask;unsigned slot;bool timed;std::uint32_t tag;
                       const std::uint8_t* band;std::uint32_t bandWidth;};
    bool gpuDoneOn=false;lat2::SlotWatch slotWatch;
    // latency-path motion probe (files/private-trace/diag-lat-motion, read at the claim; needs diag-lat-gpu-done and a
    // TRANSFER_SRC swapchain): armed tagged frames copy BandRows rows across the middle of the swapchain image after the pass
    // into slot k's host buffer; the watcher turns them into FrameShift events (latency_lat2.h estimateShift).
    static constexpr unsigned BandRows=8;bool motionOn=false;std::vector<Buffer> bands; // (bands: under m)
    std::vector<std::uint16_t> prevRow,curRow; // (watcher thread only)
    std::mutex gpuDoneMutex;std::condition_variable gpuDoneCv;std::deque<GpuDoneJob> gpuDoneJobs;std::thread gpuDoneThread;bool gpuDoneStop=false;
    std::uint64_t gpuDoneMarked=0,gpuDoneFailed=0,gpuDoneRefused=0; // (under gpuDoneMutex; logged at stop)
#if defined(__APPLE__)
    PFN_vkCreateMetalSurfaceEXT createSurface=nullptr;PFN_vkDestroySurfaceKHR destroySurface=nullptr; // ios-direct-present
#else
    PFN_vkCreateAndroidSurfaceKHR createSurface=nullptr;PFN_vkDestroySurfaceKHR destroySurface=nullptr;
#endif
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surfaceSupport=nullptr;PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surfaceCaps=nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR surfaceFormats=nullptr;PFN_vkCreateSwapchainKHR createSwapchain=nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain=nullptr;PFN_vkGetSwapchainImagesKHR swapchainImages=nullptr;
    PFN_vkAcquireNextImageKHR acquireImage=nullptr;PFN_vkQueuePresentKHR queuePresent=nullptr;

    bool loadFunctions() {
        const auto i=[&](const char* n){return vkGetInstanceProcAddr(dev->instance,n);};
        const auto d=[&](const char* n){return vkGetDeviceProcAddr(dev->device,n);};
#if defined(__APPLE__)
        createSurface=reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(i("vkCreateMetalSurfaceEXT")); // ios-direct-present
#else
        createSurface=reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(i("vkCreateAndroidSurfaceKHR"));
#endif
        destroySurface=reinterpret_cast<PFN_vkDestroySurfaceKHR>(i("vkDestroySurfaceKHR"));
        surfaceSupport=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(i("vkGetPhysicalDeviceSurfaceSupportKHR"));
        surfaceCaps=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(i("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        surfaceFormats=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(i("vkGetPhysicalDeviceSurfaceFormatsKHR"));
        createSwapchain=reinterpret_cast<PFN_vkCreateSwapchainKHR>(d("vkCreateSwapchainKHR"));
        destroySwapchain=reinterpret_cast<PFN_vkDestroySwapchainKHR>(d("vkDestroySwapchainKHR"));
        swapchainImages=reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(d("vkGetSwapchainImagesKHR"));
        acquireImage=reinterpret_cast<PFN_vkAcquireNextImageKHR>(d("vkAcquireNextImageKHR"));
        queuePresent=reinterpret_cast<PFN_vkQueuePresentKHR>(d("vkQueuePresentKHR"));
        // v318: only when device.cpp enabled VK_GOOGLE_display_timing (null otherwise: the probe then has no display times)
        pastTiming=displayTimingEnabled()?reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(d("vkGetPastPresentationTimingGOOGLE")):nullptr;
        lagOn=pastTiming && access(RECOMP_APP_PATH("/private-trace/diag-v337-no-lag"),F_OK)!=0; // v337 (v338: on by default)
        return createSurface && destroySurface && surfaceSupport && surfaceCaps && surfaceFormats && createSwapchain &&
            destroySwapchain && swapchainImages && acquireImage && queuePresent;
    }
    std::optional<std::uint32_t> memoryType(std::uint32_t bits,VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(dev->physical,&p);
        for(std::uint32_t i=0;i<p.memoryTypeCount;++i)if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&want)==want)return i;
        return std::nullopt;
    }
    bool makeBuffer(VkDeviceSize size,VkBufferUsageFlags usage,Buffer& out,bool cached=false) {
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=size;b.usage=usage;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(vkCreateBuffer(dev->device,&b,nullptr,&out.buffer)!=VK_SUCCESS){out.buffer=VK_NULL_HANDLE;return false;}
        VkMemoryRequirements r{};vkGetBufferMemoryRequirements(dev->device,out.buffer,&r);
        const VkMemoryPropertyFlags base=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto type=cached?memoryType(r.memoryTypeBits,base|VK_MEMORY_PROPERTY_HOST_CACHED_BIT):std::nullopt;
        if(!type)type=memoryType(r.memoryTypeBits,base);
        if(!type)return false;
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=*type;
        if(vkAllocateMemory(dev->device,&a,nullptr,&out.memory)!=VK_SUCCESS){out.memory=VK_NULL_HANDLE;return false;}
        if(vkBindBufferMemory(dev->device,out.buffer,out.memory,0)!=VK_SUCCESS)return false;
        if(vkMapMemory(dev->device,out.memory,0,VK_WHOLE_SIZE,0,&out.mapped)!=VK_SUCCESS){out.mapped=nullptr;return false;}
        out.size=size;return true;
    }
    void freeBuffer(Buffer& b) {
        if(b.mapped)vkUnmapMemory(dev->device,b.memory);
        if(b.buffer)vkDestroyBuffer(dev->device,b.buffer,nullptr);
        if(b.memory)vkFreeMemory(dev->device,b.memory,nullptr);
        b={};
    }
    // Everything that does not depend on the window: pool, sampler, shaders, set layout, LUT buffer.
    bool createStatic() {
        VkCommandPoolCreateInfo cp{};cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex=dev->queueFamily;
        if(vkCreateCommandPool(dev->device,&cp,nullptr,&pool)!=VK_SUCCESS){pool=VK_NULL_HANDLE;return false;}
        VkSamplerCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;s.magFilter=s.minFilter=VK_FILTER_NEAREST;
        s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;s.addressModeU=s.addressModeV=s.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.maxLod=0;
        if(vkCreateSampler(dev->device,&s,nullptr,&sampler)!=VK_SUCCESS){sampler=VK_NULL_HANDLE;return false;}
        VkShaderModuleCreateInfo sm{};sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize=sizeof VertexSpirv;sm.pCode=VertexSpirv;
        if(vkCreateShaderModule(dev->device,&sm,nullptr,&vertex)!=VK_SUCCESS){vertex=VK_NULL_HANDLE;return false;}
        sm.codeSize=sizeof FragmentSpirv;sm.pCode=FragmentSpirv;
        if(vkCreateShaderModule(dev->device,&sm,nullptr,&fragment)!=VK_SUCCESS){fragment=VK_NULL_HANDLE;return false;}
        { // v42x (a failure only leaves that mode unavailable)
            const std::array<std::pair<const std::uint32_t*,std::size_t>,3> code{{{MapFragmentSpirv,sizeof MapFragmentSpirv},
                {GatherFragmentSpirv,sizeof GatherFragmentSpirv},{PackedFragmentSpirv,sizeof PackedFragmentSpirv}}};
            const std::array<VkShaderModule*,3> out{&mapFastModule,&gatherModule,&packedModule};
            for(unsigned k=0;k<3;++k) {
                VkShaderModuleCreateInfo fm{};fm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;fm.codeSize=code[k].second;fm.pCode=code[k].first;
                if(vkCreateShaderModule(dev->device,&fm,nullptr,out[k])!=VK_SUCCESS)*out[k]=VK_NULL_HANDLE;
            }
        }
        std::array<VkDescriptorSetLayoutBinding,2> bindings{};
        bindings[0].binding=0;bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;bindings[0].descriptorCount=1;
        bindings[0].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding=1;bindings[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bindings[1].descriptorCount=1;
        bindings[1].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo sl{};sl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;sl.bindingCount=2;sl.pBindings=bindings.data();
        if(vkCreateDescriptorSetLayout(dev->device,&sl,nullptr,&setLayout)!=VK_SUCCESS){setLayout=VK_NULL_HANDLE;return false;}
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,32}; // (v409: + the filter mode; v42x: + the scale, 24 -> 32 bytes)
        VkPipelineLayoutCreateInfo pl{};pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pl.setLayoutCount=1;pl.pSetLayouts=&setLayout;
        pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        if(vkCreatePipelineLayout(dev->device,&pl,nullptr,&pipelineLayout)!=VK_SUCCESS){pipelineLayout=VK_NULL_HANDLE;return false;}
        { // v450: FSR's shaders, layout (the same set layout, push = rect + EASU's 4 constant words: 80 bytes) and set pool; a
          // failure only leaves FSR unavailable (its frames take the other modes' paths)
            const std::array<std::pair<const std::uint32_t*,std::size_t>,2> code{{{FsrEasuFragmentSpirv,sizeof FsrEasuFragmentSpirv},
                {FsrRcasFragmentSpirv,sizeof FsrRcasFragmentSpirv}}};
            const std::array<VkShaderModule*,2> out{&easuModule,&rcasModule};
            for(unsigned k=0;k<2;++k) {
                VkShaderModuleCreateInfo fm{};fm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;fm.codeSize=code[k].second;fm.pCode=code[k].first;
                if(vkCreateShaderModule(dev->device,&fm,nullptr,out[k])!=VK_SUCCESS)*out[k]=VK_NULL_HANDLE;
            }
            VkPushConstantRange fp{VK_SHADER_STAGE_FRAGMENT_BIT,0,80};
            VkPipelineLayoutCreateInfo fl=pl;fl.pPushConstantRanges=&fp;
            if(vkCreatePipelineLayout(dev->device,&fl,nullptr,&fsrLayout)!=VK_SUCCESS)fsrLayout=VK_NULL_HANDLE;
            std::array<VkDescriptorPoolSize,2> fs{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,MaxFsrSets},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,MaxFsrSets}}};
            VkDescriptorPoolCreateInfo fd{};fd.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;fd.maxSets=MaxFsrSets;fd.poolSizeCount=2;fd.pPoolSizes=fs.data();
            if(vkCreateDescriptorPool(dev->device,&fd,nullptr,&fsrSetPool)!=VK_SUCCESS)fsrSetPool=VK_NULL_HANDLE;
            RECOMP_DIRECT_LOG("RENDER_DIRECT_FSR static easu=%u rcas=%u layout=%u pool=%u",unsigned(easuModule!=VK_NULL_HANDLE),
                unsigned(rcasModule!=VK_NULL_HANDLE),unsigned(fsrLayout!=VK_NULL_HANDLE),unsigned(fsrSetPool!=VK_NULL_HANDLE));
        }
        std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,MaxSets},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,MaxSets}}};
        VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=MaxSets;dp.poolSizeCount=2;dp.pPoolSizes=sizes.data();
        if(vkCreateDescriptorPool(dev->device,&dp,nullptr,&setPool)!=VK_SUCCESS){setPool=VK_NULL_HANDLE;return false;}
        { // v410 (without it filtered frames use v409's single pass)
            std::array<VkDescriptorPoolSize,2> ms{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,48},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,48}}};
            VkDescriptorPoolCreateInfo mp{};mp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;mp.maxSets=48;mp.poolSizeCount=2;mp.pPoolSizes=ms.data(); // (v411: 16 -> 32, the copy sets; v42x: 48, the packed sets: 3 per swapchain image, <= 16 images)
            if(vkCreateDescriptorPool(dev->device,&mp,nullptr,&mapSetPool)!=VK_SUCCESS)mapSetPool=VK_NULL_HANDLE;
        }
        createScalePipeline(); // v411 (without it filtered frames use v410's passes)
        if(!makeBuffer(1536*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,lutBuffer))return false;
        lutMapped=static_cast<std::uint32_t*>(lutBuffer.mapped);
        for(unsigned i=0;i<1536;++i)lutMapped[i]=i%256; // [0,768): the game's tables (identity until setLut), [768,1536): identity
        { // v409: pass timestamps (measurement only: without them the pass is simply not timed)
            std::uint32_t nf=0;vkGetPhysicalDeviceQueueFamilyProperties(dev->physical,&nf,nullptr);
            std::vector<VkQueueFamilyProperties> fp(nf);vkGetPhysicalDeviceQueueFamilyProperties(dev->physical,&nf,fp.data());
            VkPhysicalDeviceProperties pp{};vkGetPhysicalDeviceProperties(dev->physical,&pp);
            const unsigned bits=dev->queueFamily<nf?fp[dev->queueFamily].timestampValidBits:0;
            if(bits && pp.limits.timestampPeriod>0) {
                VkQueryPoolCreateInfo qp{};qp.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;qp.queryType=VK_QUERY_TYPE_TIMESTAMP;qp.queryCount=2*MaxTimed;
                if(vkCreateQueryPool(dev->device,&qp,nullptr,&timePool)!=VK_SUCCESS)timePool=VK_NULL_HANDLE;
                tsNs=pp.limits.timestampPeriod;tsMask=bits>=64?~0ull:((1ull<<bits)-1);
                if(timePool){VkQueryPoolCreateInfo mq=qp;mq.queryCount=MaxTimed; // v42x
                    if(vkCreateQueryPool(dev->device,&mq,nullptr,&midPool)!=VK_SUCCESS)midPool=VK_NULL_HANDLE;
                    if(vkCreateQueryPool(dev->device,&mq,nullptr,&fsrMidPool)!=VK_SUCCESS)fsrMidPool=VK_NULL_HANDLE;} // v450
            }
        }
        return true;
    }
    bool ensureCpuImage() {
        if(cpuImage.view)return true;
        VkImageCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;i.imageType=VK_IMAGE_TYPE_2D;i.format=VK_FORMAT_R8G8B8A8_UNORM;
        i.extent={SourceWidth,SourceHeight,1};i.mipLevels=1;i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;
        i.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;i.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(dev->device,&i,nullptr,&cpuImage.image)!=VK_SUCCESS){cpuImage.image=VK_NULL_HANDLE;return false;}
        VkMemoryRequirements r{};vkGetImageMemoryRequirements(dev->device,cpuImage.image,&r);
        const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(!type)return false;
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=*type;
        if(vkAllocateMemory(dev->device,&a,nullptr,&cpuImage.memory)!=VK_SUCCESS){cpuImage.memory=VK_NULL_HANDLE;return false;}
        if(vkBindImageMemory(dev->device,cpuImage.image,cpuImage.memory,0)!=VK_SUCCESS)return false;
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=cpuImage.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
        v.format=VK_FORMAT_R8G8B8A8_UNORM;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(vkCreateImageView(dev->device,&v,nullptr,&cpuImage.view)!=VK_SUCCESS){cpuImage.view=VK_NULL_HANDLE;return false;}
        for(unsigned k=0;k<2;++k){if(!makeBuffer(SourceBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,stage[k]))return false;stageMapped[k]=stage[k].mapped;}
        return true;
    }
    bool waitAcquireSlot(unsigned k) {
        slotWatch.waitReleased(k); // latency-path (lat2): the watcher's vkWaitForFences on this slot has returned
        auto& a=acquires[k];
        if(!a.submitted)return true;
        if(vkWaitForFences(dev->device,1,&a.fence,VK_TRUE,FenceTimeout)!=VK_SUCCESS){broken=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT broken=fence-wait");return false;}
        a.submitted=false;
        if(k<MaxTimed && slotTimed[k]) { // v409
            std::uint64_t t[2]{};
            if(vkGetQueryPoolResults(dev->device,timePool,2*k,2,sizeof t,t,sizeof t[0],VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
                const auto d=((t[1]&tsMask)-(t[0]&tsMask))&tsMask;const unsigned m=slotTimed[k]-1u;
                if(m<PassSlots){passNs[m]+=std::uint64_t(double(d)*tsNs);++passN[m];} // (v412: bounded)
                std::uint64_t e=0; // v42x: the map pass's share (its end - the frame's start)
                if(slotMid[k] && m<PassSlots && vkGetQueryPoolResults(dev->device,midPool,k,1,sizeof e,&e,sizeof e,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS)
                    {mapNs[m]+=std::uint64_t(double(((e&tsMask)-(t[0]&tsMask))&tsMask)*tsNs);++mapN[m];}
                std::uint64_t g=0; // v450: EASU's end (- the frame's start)
                if(slotFsrMid[k] && m<PassSlots && fsrMidPool && vkGetQueryPoolResults(dev->device,fsrMidPool,k,1,sizeof g,&g,sizeof g,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS)
                    {easuNs[m]+=std::uint64_t(double(((g&tsMask)-(t[0]&tsMask))&tsMask)*tsNs);++easuN[m];}
            }
            slotTimed[k]=0;slotMid[k]=0;slotFsrMid[k]=0;
        }
        return true;
    }
    bool waitAllMine() {for(unsigned k=0;k<acquires.size();++k)if(!waitAcquireSlot(k))return false;return true;}
#if defined(__APPLE__)
#define IOS_PRESENTED_NOTIFY presentedCv.notify_all()
    // ios-fg-one-acquire: wait (m released) until every queued picture's present call was made and none is held.
    void iosAwaitPresented() noexcept {
        if(!iosLk || !presentThread.joinable() || stopPresenter)return;
        if(pendingPresents.empty() && !fgHolding)return;
        ++iosWaits;const auto t0=std::chrono::steady_clock::now();
        // ios-gpu-hang (Claude, 2026-10-20; c291: GPU timeouts, device lost, exit 4 after ~60 s of frame generation): going on
        // after a timed-out wait puts a second acquire outstanding, the deadlock of c177-c180. The wait never timed out in 1,037
        // logged windows, so it now keeps waiting up to 1 s in 100 ms slices; each timeout is logged at once.
        const auto done=[&]{return stopPresenter || (pendingPresents.empty() && !fgHolding);};
        unsigned slices=0;
        while(!presentedCv.wait_for(*iosLk,std::chrono::milliseconds(100),done)) {
            if(!slices++)++iosWaitTimeouts;
            RECOMP_DIRECT_LOG("RENDER_IOS_FG_AWAIT_TIMEOUT slice=%u pending=%zu holding=%d",slices,pendingPresents.size(),int(fgHolding));
            if(slices>=10)break;
        }
        iosWaitNs+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());
    }
#else
#define IOS_PRESENTED_NOTIFY ((void)0)
#endif
    // v259: the present thread (under m): presents each image the renderer's pass for it was submitted for, in order.
    void presentLoop() {
#if defined(__linux__)
        pthread_setname_np(pthread_self(),"recomp-dpresent");
#elif defined(__APPLE__)
        pthread_setname_np("recomp-dpresent"); // ios-direct-present: presents on time (it only waits and calls the driver)
        (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
#endif
        std::unique_lock l(m);
        for(;;) {
            presentCv.wait(l,[&]{return stopPresenter || !pendingPresents.empty();});
            if(stopPresenter)return;
            auto i=pendingPresents.front();pendingPresents.pop_front(); // (frame-gen: not const, see fgHold)
            const auto tag=popTag();
            fgHold(l,i,tag); // frame-gen (lane B): a held pair's second picture waits for its time (otherwise: a compare and a return)
            if(!swapchain || i>=renderDone.size()){IOS_PRESENTED_NOTIFY;continue;}
            presentNow(i,tag);
            IOS_PRESENTED_NOTIFY;
        }
    }
    std::uint32_t popTag() {if(pendingTags.empty())return 0;const auto t=pendingTags.front();pendingTags.pop_front();return t;}
    // latency-path (lat2): hands acquire slot k's fence (just submitted; the caller holds m) to the watcher; refused (left
    // unwatched) when the watcher cannot start or is 16 frames behind.
    void watchGpuDone(unsigned k,std::uint32_t tag,bool timed,const std::uint8_t* band) {
        std::lock_guard g(gpuDoneMutex);
        if(k>=lat2::SlotWatch::Slots || k>=acquires.size() || gpuDoneStop || gpuDoneJobs.size()>=16){++gpuDoneRefused;return;}
        if(!gpuDoneThread.joinable()) {
            try {gpuDoneThread=std::thread([this]{gpuDoneLoop();});} catch(...) {gpuDoneOn=false;++gpuDoneRefused;return;}
        }
        slotWatch.begin(k);
        gpuDoneJobs.push_back({dev->device,acquires[k].fence,timed?timePool:VK_NULL_HANDLE,tsNs,tsMask,k,timed && k<MaxTimed,tag,band,extent.width});
        gpuDoneCv.notify_one();
    }
    // motion probe: slot k's band buffer (host visible, cached when offered) for the current extent; false: no band this frame.
    bool ensureBand(unsigned k) {
        if(bands.size()<acquires.size())bands.resize(acquires.size());
        if(k>=bands.size())return false;
        const VkDeviceSize need=VkDeviceSize(extent.width)*BandRows*4;
        if(bands[k].size<need) { // (slot k was waited and released by the watcher: nothing uses the old buffer)
            if(bands[k].buffer || bands[k].memory)freeBuffer(bands[k]);
            if(!makeBuffer(need,VK_BUFFER_USAGE_TRANSFER_DST_BIT,bands[k],true)){freeBuffer(bands[k]);motionOn=false;
                RECOMP_DIRECT_LOG("RENDER_LAT2 motion=0 why=band-buffer");return false;}
        }
        return true;
    }
    // motion probe: the band copy after the pass (the audit's transitions: PRESENT_SRC -> TRANSFER_SRC -> PRESENT_SRC).
    void recordBand(VkCommandBuffer cb,VkImage target,unsigned k) {
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=target;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        c.imageOffset={0,std::int32_t(extent.height/2-BandRows/2),0};c.imageExtent={extent.width,BandRows,1};
        vkCmdCopyImageToBuffer(cb,target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,bands[k].buffer,1,&c);
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=0;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkMemoryBarrier h{};h.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;h.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;h.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&h,0,nullptr,0,nullptr);
    }
    void gpuDoneLoop() {
#if defined(__linux__)
        pthread_setname_np(pthread_self(),"recomp-lat-gpu");
        {cpu_set_t little;CPU_ZERO(&little);RECOMP_TOPOLOGY_ADD_CPUS(little,recomp_topology::roles().little);(void)sched_setaffinity(0,sizeof little,&little);} // topology lane: the little role (Thor: A510 0-2)
#endif
        namespace tl=recomp::diag::timeline;
        std::unique_lock l(gpuDoneMutex);
        for(;;) {
            gpuDoneCv.wait(l,[&]{return gpuDoneStop || !gpuDoneJobs.empty();});
            if(gpuDoneStop) { // (shutdown: every queued slot is released unwatched)
                for(const auto& j:gpuDoneJobs)slotWatch.end(j.slot);
                gpuDoneJobs.clear();return;
            }
            const GpuDoneJob job=gpuDoneJobs.front();gpuDoneJobs.pop_front();
            l.unlock();
            const VkResult r=vkWaitForFences(job.device,1,&job.fence,VK_TRUE,FenceTimeout);
            const auto at=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            std::uint16_t pass=0xFFFF;
            if(r==VK_SUCCESS && job.timed && job.pool) { // (the presenter reads the same two results later: host reads may overlap)
                std::uint64_t t[2]{};
                if(vkGetQueryPoolResults(job.device,job.pool,2*job.slot,2,sizeof t,t,sizeof t[0],VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
                    const auto d=((t[1]&job.tsMask)-(t[0]&job.tsMask))&job.tsMask;
                    pass=std::uint16_t(std::min(65534.0,double(d)*job.tsNs/10000.0));
                    se::gpuClock.sample(std::int64_t(at),std::int64_t(double(t[1]&job.tsMask)*job.tsNs)); // submit-early: seen at `at`, ended at t[1]
                }
            }
            if(r==VK_SUCCESS)tl::markAt(at,tl::PresenterGpuDone,pass,job.tag);
            if(r==VK_SUCCESS && job.band && job.bandWidth>=64) { // motion probe (the band is complete: the fence covers its copy)
                curRow.resize(job.bandWidth/2);
                lat2::bandToRow(job.band,job.bandWidth,BandRows,curRow.data());
                if(prevRow.size()==curRow.size()) {
                    const auto s=lat2::estimateShift(prevRow.data(),curRow.data(),unsigned(curRow.size()),64);
                    tl::mark(tl::FrameShift,lat2::shiftArg(s),job.tag);
                }
                prevRow.swap(curRow);
            } else prevRow.clear(); // (a frame without a band: the next one has nothing to compare with)
            slotWatch.end(job.slot); // (after the last use of the fence, the query results and the band)
            l.lock();
            ++(r==VK_SUCCESS?gpuDoneMarked:gpuDoneFailed);
        }
    }
    void stopGpuDone() {
        {std::lock_guard g(gpuDoneMutex);gpuDoneStop=true;}
        gpuDoneCv.notify_all();
        if(gpuDoneThread.joinable())gpuDoneThread.join();
        std::lock_guard g(gpuDoneMutex);
        if(gpuDoneMarked || gpuDoneFailed || gpuDoneRefused)RECOMP_DIRECT_LOG("RENDER_LAT2 gpu_done_marked=%llu failed=%llu refused=%llu",
            (unsigned long long)gpuDoneMarked,(unsigned long long)gpuDoneFailed,(unsigned long long)gpuDoneRefused);
    }
    bool presentNow(std::uint32_t i,std::uint32_t tag=0) {
        VkPresentInfoKHR pi{};pi.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&renderDone[i];
        pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&i;
        // v318: while the timeline is armed, a tagged frame asks for its display time (desiredPresentTime 0: no scheduling).
        namespace tl=recomp::diag::timeline;
        const bool probe=tag && tl::armed.load(std::memory_order_relaxed);
        const bool timing=probe || (tag && lagOn); // v337: display times also for the lag histograms
        // pace-caps (lane A): desiredPresentTime stays 0 unless diag-pace-dpt and a cap are on (then the frame's slot - half a refresh)
        // frame-gen (lane B): a pair's two times; fgDesired returns its second argument for every picture that is not part of a pair (integrate-v460, MERGE.md place 1)
        VkPresentTimeGOOGLE when{tag,fgDesired(tag,recomp::pace::desiredPresentNs(tag))};VkPresentTimesInfoGOOGLE times{};
        if(timing && pastTiming) {
            times.sType=VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;times.swapchainCount=1;times.pTimes=&when;pi.pNext=&times;
        }
        if(probe)tl::mark(tl::HostPresentBegin,0,tag);
        if(timing && lagOn){callTag[tag%recomp::v337::Ring]=tag;callAt[tag%recomp::v337::Ring]=recomp::v337::monotonicNs();} // v337
        VkResult pr;
        const auto t1=std::chrono::steady_clock::now();
        { // ios-slow-display: the presenter's own gap between present calls (diagnostic, at most 40 lines)
            const auto n=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1.time_since_epoch()).count());
            if(presentCallLastNs && n-presentCallLastNs>100'000'000ull && n-presentCallLastNs<2'000'000'000ull && dispLongLogs<40) {
                ++dispLongLogs;RECOMP_DIRECT_LOG("RENDER_IOS_SLOW_PRESENT_CALL gap_ms=%.1f tag=%u",double(n-presentCallLastNs)*1e-6,tag);
            }
            presentCallLastNs=n;
        }
        if(presentQueue){std::lock_guard q(presentQueueMutex);pr=queuePresent(presentQueue,&pi);}
        else {std::lock_guard q(sharedQueueMutex());pr=queuePresent(lastQueue,&pi);}
        presentNs+=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t1).count());
        recomp::pace::presentCalled(tag); // pace-caps (lane A): release -> present call (a load and a return without a cap)
        if(probe)tl::mark(tl::HostPresentEnd,0,tag);
        fgCalled(tag); // frame-gen (lane B): a pair's second picture stays out of the v337 call ring
        ++tmCalls;tmTagZero+=!tag;tmRequested+=timing && pastTiming;
        if(!(tmCalls%600))RECOMP_DIRECT_LOG("RENDER_IOS_TIMING calls=%llu tag_zero=%llu requested=%llu queries=%llu returned=%llu actual=%llu lag=%u past=%u",
            (unsigned long long)tmCalls,(unsigned long long)tmTagZero,(unsigned long long)tmRequested,(unsigned long long)tmQueries,
            (unsigned long long)tmReturned,(unsigned long long)tmActual,unsigned(lagOn),unsigned(pastTiming!=nullptr));
        if(timing && pastTiming && (pr==VK_SUCCESS || pr==VK_SUBOPTIMAL_KHR)) {
            std::array<VkPastPresentationTimingGOOGLE,8> past{};std::uint32_t n=std::uint32_t(past.size());
            const auto q=pastTiming(dev->device,swapchain,&n,past.data());
            ++tmQueries;if(q==VK_SUCCESS || q==VK_INCOMPLETE){tmReturned+=n;for(std::uint32_t k=0;k<n && k<past.size();++k)tmActual+=past[k].actualPresentTime!=0;}
            if(q==VK_SUCCESS || q==VK_INCOMPLETE)
                for(std::uint32_t k=0;k<n && k<past.size();++k) if(past[k].presentID && past[k].actualPresentTime) {
                    // arg: the driver's present margin in 100 us units (how early the frame was ready for its vsync)
                    const auto margin=std::min<std::uint64_t>(past[k].presentMargin/100000u,65535u);
                    if(probe)tl::markAt(past[k].actualPresentTime,tl::Displayed,std::uint16_t(margin),past[k].presentID);++displayTimes;
                    recomp::pace::displayedAt(past[k].presentID,past[k].actualPresentTime); // pace-caps (lane A): the vsync estimate, slot feedback
                    // ios-late-display (Claude 2026-10-18, diagnostic): a displayed interval over 40 ms with the driver's times for that frame
                    // (desired / earliest relative to actual, and the present margin): ready late (earliest after desired) or shown late
                    if(dispLast && past[k].actualPresentTime>dispLast && (past[k].actualPresentTime-dispLast>40'000'000ull ||
                       (past[k].actualPresentTime-dispLast<25'000'000ull && dispTotal>2400)) &&
                       past[k].actualPresentTime-dispLast<=2'000'000'000ull && lateDispLogs<80) {
                        ++lateDispLogs;const auto rel=[&](std::uint64_t t){return t?(double(std::int64_t(t-past[k].actualPresentTime))*1e-6):0.0;};
                        RECOMP_DIRECT_LOG("RENDER_IOS_LATE_DISPLAY interval_ms=%.2f id=%u desired_rel_ms=%.2f earliest_rel_ms=%.2f margin_ms=%.2f",
                            double(past[k].actualPresentTime-dispLast)*1e-6,past[k].presentID,rel(past[k].desiredPresentTime),
                            rel(past[k].earliestPresentTime),double(past[k].presentMargin)*1e-6);
                    }
                    noteDisplayInterval(past[k].actualPresentTime,past[k].presentID); // ios-direct-present (measurement: a few adds)
                    if(probe)tl::markAt(past[k].actualPresentTime,tl::DisplayMargin,lat2::marginUnits(past[k].presentMargin),past[k].presentID); // lat2
                    fgDisplayed(past[k].presentID,past[k].actualPresentTime); // frame-gen (lane B): pair spacing, its vsync estimate
                    if(lagOn) { // v337
                        const auto id=past[k].presentID;const auto slot=id%recomp::v337::Ring;
                        recomp::v337::noteDisplayed(id,past[k].actualPresentTime,callTag[slot]==id?callAt[slot]:0);
                    }
                }
        }
        ++presentCalls;
        if(pr==VK_SUBOPTIMAL_KHR)++suboptimal; // (v256: expected every frame, see the acquire)
        else if(pr==VK_ERROR_OUT_OF_DATE_KHR){recreate=true;++outOfDate;}
        else if(pr==VK_ERROR_SURFACE_LOST_KHR){lost=true;++surfaceLost;}
        else if(pr!=VK_SUCCESS){broken=true;gpuOn.store(false,std::memory_order_release);RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT broken=present %d",int(pr));}
        return pr==VK_SUCCESS || pr==VK_SUBOPTIMAL_KHR;
    }
    // ios-direct-present: one displayed frame at `actual` (driver clock). The interval to the previous displayed frame is the
    // previous frame's time on screen. Logged every 120 frames, then reset.
    unsigned lateDispLogs=0; // ios-late-display (diagnostic line budget)
    [[gnu::noinline]] void noteDisplayInterval(std::uint64_t actual,std::uint32_t id=0) noexcept {
        // ios-slow-display (Claude 2026-10-17, diagnostic): a displayed interval over 100 ms names how many present IDs it spans
        // (1 = one frame stayed on screen that long; more = the frames between had no display record, so the interval is not one
        // frame's time). At most 40 lines per run.
        if(id && dispLast && actual>dispLast && actual-dispLast>100'000'000ull && actual-dispLast<=2'000'000'000ull && dispLongLogs<40) {
            ++dispLongLogs;
            RECOMP_DIRECT_LOG("RENDER_IOS_SLOW_DISPLAY interval_ms=%.1f id=%u prev_id=%u spans=%d",double(actual-dispLast)*1e-6,id,dispLastId,
                dispLastId?int(id-dispLastId):-1);
        }
        if(id)dispLastId=id;
        if(dispClockLogs<3) {++dispClockLogs; // the clock check: actual against both candidate clocks (once per frame, 3 frames)
            timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);
#if defined(__APPLE__)
            const unsigned long long raw=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
            const unsigned long long raw=0;
#endif
            RECOMP_DIRECT_LOG("RENDER_IOS_DISPLAY_CLOCK actual=%llu uptime_raw=%llu monotonic=%llu",(unsigned long long)actual,raw,
                (unsigned long long)(std::uint64_t(t.tv_sec)*1000000000ull+std::uint64_t(t.tv_nsec)));
        }
        if(actual<=dispLast){if(!dispLast)dispLast=actual;return;}
        if(!dispLast){dispLast=actual;return;}
        const std::uint64_t d=actual-dispLast;dispLast=actual;
        if(d>2'000'000'000ull)return; // (a pause, not a frame)
        ++dispN;++dispTotal;dispSum+=d;dispMax=std::max(dispMax,d);dispOver50+=d>50'000'000ull;
        dispBand+=d>=30'000'000ull && d<=36'666'667ull;
        dispBand16+=d>=15'000'000ull && d<=18'333'333ull;dispOver33+=d>33'400'000ull; // (Claude 2026-10-19: the 60 fps frame-generation target)
        ++dispBins[std::min<std::uint64_t>(d/1'000'000ull,127)];
        if(dispN<120)return;
        const auto pct=[&](unsigned q){std::uint32_t acc=0;for(unsigned i=0;i<128;++i){acc+=dispBins[i];if(acc*100u>=dispN*q)return i;}return 127u;};
        RECOMP_DIRECT_LOG("RENDER_IOS_DISPLAY n=%u mean_ms=%.2f p50=%u p90=%u p99=%u max_ms=%.1f over50=%u band30=%u total=%llu band16=%u over33=%u",dispN,
            double(dispSum)/1e6/dispN,pct(50),pct(90),pct(99),double(dispMax)/1e6,dispOver50,dispBand,(unsigned long long)dispTotal,dispBand16,dispOver33);
        dispN=0;dispOver50=0;dispBand=0;dispBand16=0;dispOver33=0;dispMax=0;dispSum=0;dispBins.fill(0);
    }
    void createScalePipeline() { // v411
        VkShaderModuleCreateInfo sm{};sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;sm.codeSize=sizeof ScaleComputeSpirv;sm.pCode=ScaleComputeSpirv;
        if(vkCreateShaderModule(dev->device,&sm,nullptr,&scaleModule)!=VK_SUCCESS){scaleModule=VK_NULL_HANDLE;return;}
        std::array<VkDescriptorSetLayoutBinding,3> b{};
        for(unsigned k=0;k<3;++k){b[k].binding=k;b[k].descriptorCount=1;b[k].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
        b[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;b[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        VkDescriptorSetLayoutCreateInfo sl{};sl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;sl.bindingCount=3;sl.pBindings=b.data();
        if(vkCreateDescriptorSetLayout(dev->device,&sl,nullptr,&scaleSetLayout)!=VK_SUCCESS){scaleSetLayout=VK_NULL_HANDLE;return;}
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
        VkPipelineLayoutCreateInfo pl{};pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pl.setLayoutCount=1;pl.pSetLayouts=&scaleSetLayout;
        pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        if(vkCreatePipelineLayout(dev->device,&pl,nullptr,&scalePipelineLayout)!=VK_SUCCESS){scalePipelineLayout=VK_NULL_HANDLE;return;}
        VkComputePipelineCreateInfo cp{};cp.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;cp.layout=scalePipelineLayout;
        cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cp.stage.module=scaleModule;cp.stage.pName="main";
        if(vkCreateComputePipelines(dev->device,VK_NULL_HANDLE,1,&cp,nullptr,&scalePipeline)!=VK_SUCCESS){scalePipeline=VK_NULL_HANDLE;return;}
        std::array<VkDescriptorPoolSize,3> ps{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,MaxScaleSets},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,MaxScaleSets},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,MaxScaleSets}}};
        VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=MaxScaleSets;dp.poolSizeCount=3;dp.pPoolSizes=ps.data();
        if(vkCreateDescriptorPool(dev->device,&dp,nullptr,&scaleSetPool)!=VK_SUCCESS){scaleSetPool=VK_NULL_HANDLE;vkDestroyPipeline(dev->device,scalePipeline,nullptr);scalePipeline=VK_NULL_HANDLE;}
    }
    // v411: the tile holds the footprint of 16 output pixels while 15 * source / size + 5 <= 24 (direct_scale.comp's T).
    static bool scaleFits(std::uint32_t w,std::uint32_t h) noexcept {return w && h && 15.0*SourceWidth/w+5.0<=24.0 && 15.0*SourceHeight/h+5.0<=24.0;}
    bool ensureScale(std::uint32_t i,std::uint32_t w,std::uint32_t h) { // v411: image i's picture (created at its first compute frame)
        if(scaleFailed || !scalePipeline || !mapSetPool || i>=images.size() || !scaleFits(w,h))return false;
        if(scaleImages.size()!=images.size() || scaleW!=w || scaleH!=h) { // (the map pool's sets go back as a whole: both kinds rebuilt)
            if((!scaleImages.empty() || !mapImages.empty()) && !waitAllMine())return false;
            destroyMaps();scaleImages.resize(images.size());scaleCopySets.resize(images.size(),VK_NULL_HANDLE);scaleW=w;scaleH=h;
        }
        if(scaleCopySets[i])return true;
        const auto fail=[&](const char* what){scaleFailed=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_FILTER scale=failed %s",what);return false;};
        auto& m=scaleImages[i];
        VkImageCreateInfo ic{};ic.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ic.imageType=VK_IMAGE_TYPE_2D;ic.format=VK_FORMAT_R8G8B8A8_UNORM;
        ic.extent={w,h,1};ic.mipLevels=1;ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;
        ic.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(dev->device,&ic,nullptr,&m.image)!=VK_SUCCESS){m.image=VK_NULL_HANDLE;return fail("image");}
        VkMemoryRequirements r{};vkGetImageMemoryRequirements(dev->device,m.image,&r);
        const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(!type)return fail("memory-type");
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=*type;
        if(vkAllocateMemory(dev->device,&a,nullptr,&m.memory)!=VK_SUCCESS){m.memory=VK_NULL_HANDLE;return fail("memory");}
        if(vkBindImageMemory(dev->device,m.image,m.memory,0)!=VK_SUCCESS)return fail("bind");
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=m.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
        v.format=VK_FORMAT_R8G8B8A8_UNORM;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(vkCreateImageView(dev->device,&v,nullptr,&m.view)!=VK_SUCCESS){m.view=VK_NULL_HANDLE;return fail("view");}
        VkDescriptorSetAllocateInfo sa{};sa.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;sa.descriptorPool=mapSetPool;sa.descriptorSetCount=1;sa.pSetLayouts=&setLayout;
        VkDescriptorSet set=VK_NULL_HANDLE;
        if(vkAllocateDescriptorSets(dev->device,&sa,&set)!=VK_SUCCESS)return fail("copy-set");
        VkDescriptorImageInfo ii{sampler,m.view,VK_IMAGE_LAYOUT_GENERAL};VkDescriptorBufferInfo bi{lutBuffer.buffer,0,VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet,2> wr{};
        for(auto& x:wr){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=set;x.descriptorCount=1;}
        wr[0].dstBinding=0;wr[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr[0].pImageInfo=&ii;
        wr[1].dstBinding=1;wr[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr[1].pBufferInfo=&bi;
        vkUpdateDescriptorSets(dev->device,2,wr.data(),0,nullptr);
        scaleCopySets[i]=set;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FILTER scale_image=%u of %zu size=%ux%u",i,images.size(),w,h);
        return true;
    }
    VkDescriptorSet scaleSetFor(VkImageView view,std::uint32_t i) { // v411: the compute set reading view, writing image i's picture
        for(const auto& e:scaleSets)if(e.view==view && e.image==i)return e.set;
        if(scaleSets.size()>=MaxScaleSets){if(!waitAllMine())return VK_NULL_HANDLE;vkResetDescriptorPool(dev->device,scaleSetPool,0);scaleSets.clear();}
        VkDescriptorSetAllocateInfo sa{};sa.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;sa.descriptorPool=scaleSetPool;sa.descriptorSetCount=1;sa.pSetLayouts=&scaleSetLayout;
        VkDescriptorSet set=VK_NULL_HANDLE;
        if(vkAllocateDescriptorSets(dev->device,&sa,&set)!=VK_SUCCESS)return VK_NULL_HANDLE;
        VkDescriptorImageInfo src{sampler,view,VK_IMAGE_LAYOUT_GENERAL},dst{VK_NULL_HANDLE,scaleImages[i].view,VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo bi{lutBuffer.buffer,0,VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet,3> wr{};
        for(auto& x:wr){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=set;x.descriptorCount=1;}
        wr[0].dstBinding=0;wr[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr[0].pImageInfo=&src;
        wr[1].dstBinding=1;wr[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr[1].pBufferInfo=&bi;
        wr[2].dstBinding=2;wr[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;wr[2].pImageInfo=&dst;
        vkUpdateDescriptorSets(dev->device,3,wr.data(),0,nullptr);
        scaleSets.push_back({view,i,set});return set;
    }
    void destroyScaleImages() { // v411 (callers have waited for every slot)
        for(auto& m:scaleImages){if(m.view)vkDestroyImageView(dev->device,m.view,nullptr);if(m.image)vkDestroyImage(dev->device,m.image,nullptr);
            if(m.memory)vkFreeMemory(dev->device,m.memory,nullptr);}
        if(scaleSetPool && !scaleSets.empty())vkResetDescriptorPool(dev->device,scaleSetPool,0); // (the copy sets: destroyMaps resets their pool)
        scaleImages.clear();scaleCopySets.clear();scaleSets.clear();scaleW=scaleH=0;
    }
    void destroyPacks() { // v42x (callers have waited for every slot; the sets go back with destroyMaps' reset of mapSetPool)
        for(auto f:packFramebuffers)if(f)vkDestroyFramebuffer(dev->device,f,nullptr);
        for(auto v:packViews)if(v)vkDestroyImageView(dev->device,v,nullptr);
        for(auto& m:packImages){if(m.view)vkDestroyImageView(dev->device,m.view,nullptr);if(m.image)vkDestroyImage(dev->device,m.image,nullptr);
            if(m.memory)vkFreeMemory(dev->device,m.memory,nullptr);}
        packImages.clear();packViews.clear();packFramebuffers.clear();packSets.clear();
    }
    // v42x: image i's packed picture (created at its first packed frame): the fast map pass's target in the swapchain format
    // (the map pass's render pass and pipeline) with MUTABLE_FORMAT and an R32_UINT view, which the packed pass gathers (each
    // word = one texel's bytes in memory order). Pictures of its own: v410's (modes 1-4) keep their creation flags. False:
    // the packed mode is off for the run (fastFailed).
    bool ensurePack(std::uint32_t i) {
        if((fastFailed&2u) || !mapPass || !mapSetPool || !mapFastPipeline || !packedPipeline || i>=images.size())return false;
        if(packImages.size()!=images.size()){packImages.resize(images.size());packViews.resize(images.size(),VK_NULL_HANDLE);
            packFramebuffers.resize(images.size(),VK_NULL_HANDLE);packSets.resize(images.size(),VK_NULL_HANDLE);}
        if(packSets[i])return true;
        const auto fail=[&](const char* what){fastFailed|=2u;presentedFast.store(0,std::memory_order_relaxed);
            RECOMP_DIRECT_LOG("RENDER_DIRECT_FILTER packed=failed %s",what);return false;};
        {
            VkImageFormatProperties ip{};
            if(vkGetPhysicalDeviceImageFormatProperties(dev->physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,&ip)!=VK_SUCCESS ||
               ip.maxExtent.width<SourceWidth || ip.maxExtent.height<SourceHeight)return fail("mutable-format");
            VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(dev->physical,VK_FORMAT_R32_UINT,&fp);
            if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))return fail("r32ui-sampled");
        }
        auto& m=packImages[i];
        const std::array<VkFormat,2> viewFormats{format,VK_FORMAT_R32_UINT};
        VkImageFormatListCreateInfo fl{};fl.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;fl.viewFormatCount=2;fl.pViewFormats=viewFormats.data();
        VkImageCreateInfo ic{};ic.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ic.pNext=&fl;ic.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ic.imageType=VK_IMAGE_TYPE_2D;ic.format=format;ic.extent={SourceWidth,SourceHeight,1};ic.mipLevels=1;ic.arrayLayers=1;
        ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;ic.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(dev->device,&ic,nullptr,&m.image)!=VK_SUCCESS){m.image=VK_NULL_HANDLE;return fail("image");}
        VkMemoryRequirements r{};vkGetImageMemoryRequirements(dev->device,m.image,&r);
        const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(!type)return fail("memory-type");
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=*type;
        if(vkAllocateMemory(dev->device,&a,nullptr,&m.memory)!=VK_SUCCESS){m.memory=VK_NULL_HANDLE;return fail("memory");}
        if(vkBindImageMemory(dev->device,m.image,m.memory,0)!=VK_SUCCESS)return fail("bind");
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=m.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
        v.format=format;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(vkCreateImageView(dev->device,&v,nullptr,&m.view)!=VK_SUCCESS){m.view=VK_NULL_HANDLE;return fail("view");}
        v.format=VK_FORMAT_R32_UINT;
        if(vkCreateImageView(dev->device,&v,nullptr,&packViews[i])!=VK_SUCCESS){packViews[i]=VK_NULL_HANDLE;return fail("r32ui-view");}
        VkFramebufferCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;f.renderPass=mapPass;f.attachmentCount=1;f.pAttachments=&m.view;
        f.width=SourceWidth;f.height=SourceHeight;f.layers=1;
        if(vkCreateFramebuffer(dev->device,&f,nullptr,&packFramebuffers[i])!=VK_SUCCESS){packFramebuffers[i]=VK_NULL_HANDLE;return fail("framebuffer");}
        VkDescriptorSetAllocateInfo sa{};sa.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;sa.descriptorPool=mapSetPool;sa.descriptorSetCount=1;sa.pSetLayouts=&setLayout;
        VkDescriptorSet set=VK_NULL_HANDLE;
        if(vkAllocateDescriptorSets(dev->device,&sa,&set)!=VK_SUCCESS)return fail("set");
        VkDescriptorImageInfo ii{sampler,packViews[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo bi{lutBuffer.buffer,0,VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet,2> w{};
        for(auto& x:w){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=set;x.descriptorCount=1;}
        w[0].dstBinding=0;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&ii;
        w[1].dstBinding=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&bi;
        vkUpdateDescriptorSets(dev->device,2,w.data(),0,nullptr);
        packSets[i]=set;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FILTER packed_image=%u of %zu format=%d",i,images.size(),int(format));
        return true;
    }
    void destroyFsrImages() { // v450 (callers have waited for every slot)
        for(auto fb:fsrFramebuffers)if(fb)vkDestroyFramebuffer(dev->device,fb,nullptr);
        for(auto& m:fsrImages){if(m.view)vkDestroyImageView(dev->device,m.view,nullptr);if(m.image)vkDestroyImage(dev->device,m.image,nullptr);
            if(m.memory)vkFreeMemory(dev->device,m.memory,nullptr);}
        if(fsrSetPool && !fsrSets.empty())vkResetDescriptorPool(dev->device,fsrSetPool,0);
        fsrImages.clear();fsrFramebuffers.clear();fsrSets.clear();fsrW=fsrH=0;
    }
    // v450: image i's EASU picture (w x h = the image rect, RGBA8; created at its first FSR frame), its framebuffer (fsrPass) and
    // the RCAS pass's set reading it. False: FSR is off for the run (fsrFailed).
    bool ensureFsr(std::uint32_t i,std::uint32_t w,std::uint32_t h) {
        if(fsrFailed || !fsrPass || !fsrSetPool || !easuPipeline || !rcasPipeline || i>=images.size() || !w || !h || images.size()>MaxFsrSets)return false;
        if(fsrImages.size()!=images.size() || fsrW!=w || fsrH!=h) {
            if(!fsrImages.empty() && !waitAllMine())return false;
            destroyFsrImages();fsrImages.resize(images.size());fsrFramebuffers.resize(images.size(),VK_NULL_HANDLE);fsrSets.resize(images.size(),VK_NULL_HANDLE);
            fsrW=w;fsrH=h;
        }
        if(fsrSets[i])return true;
        const auto fail=[&](const char* what){fsrFailed=true;presentedFsr.store(0,std::memory_order_relaxed);
            RECOMP_DIRECT_LOG("RENDER_DIRECT_FSR failed=%s",what);return false;};
        auto& m=fsrImages[i];
        VkImageCreateInfo ic{};ic.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ic.imageType=VK_IMAGE_TYPE_2D;ic.format=VK_FORMAT_R8G8B8A8_UNORM;
        ic.extent={w,h,1};ic.mipLevels=1;ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;
        ic.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(dev->device,&ic,nullptr,&m.image)!=VK_SUCCESS){m.image=VK_NULL_HANDLE;return fail("image");}
        VkMemoryRequirements r{};vkGetImageMemoryRequirements(dev->device,m.image,&r);
        const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(!type)return fail("memory-type");
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=*type;
        if(vkAllocateMemory(dev->device,&a,nullptr,&m.memory)!=VK_SUCCESS){m.memory=VK_NULL_HANDLE;return fail("memory");}
        if(vkBindImageMemory(dev->device,m.image,m.memory,0)!=VK_SUCCESS)return fail("bind");
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=m.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
        v.format=VK_FORMAT_R8G8B8A8_UNORM;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(vkCreateImageView(dev->device,&v,nullptr,&m.view)!=VK_SUCCESS){m.view=VK_NULL_HANDLE;return fail("view");}
        VkFramebufferCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;f.renderPass=fsrPass;f.attachmentCount=1;f.pAttachments=&m.view;
        f.width=w;f.height=h;f.layers=1;
        if(vkCreateFramebuffer(dev->device,&f,nullptr,&fsrFramebuffers[i])!=VK_SUCCESS){fsrFramebuffers[i]=VK_NULL_HANDLE;return fail("framebuffer");}
        VkDescriptorSetAllocateInfo sa{};sa.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;sa.descriptorPool=fsrSetPool;sa.descriptorSetCount=1;sa.pSetLayouts=&setLayout;
        VkDescriptorSet set=VK_NULL_HANDLE;
        if(vkAllocateDescriptorSets(dev->device,&sa,&set)!=VK_SUCCESS)return fail("set");
        VkDescriptorImageInfo ii{sampler,m.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo bi{lutBuffer.buffer,0,VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet,2> wr{};
        for(auto& x:wr){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=set;x.descriptorCount=1;}
        wr[0].dstBinding=0;wr[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr[0].pImageInfo=&ii;
        wr[1].dstBinding=1;wr[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr[1].pBufferInfo=&bi;
        vkUpdateDescriptorSets(dev->device,2,wr.data(),0,nullptr);
        fsrSets[i]=set;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FSR picture=%u of %zu size=%ux%u bytes=%llu",i,images.size(),w,h,(unsigned long long)r.size);
        return true;
    }
    void destroyGsrSets() { // v460 (callers have waited for every slot): the sets name v410's pictures
        if(gsrSetPool && !gsrSets.empty())vkResetDescriptorPool(dev->device,gsrSetPool,0);
        gsrSets.clear();
    }
    void destroyGsrPipelines() { // v460: they follow `pass`
        for(auto& f:gsrPipeline)if(f){vkDestroyPipeline(dev->device,f,nullptr);f=VK_NULL_HANDLE;}
        gsrPipelineTried.fill(false);
    }
    // v460: everything a GSR frame of swapchain image i needs, created on first use (a run without the switch creates none of
    // it): the linear sampler and the set pool (once), the variant's shader module (once) and pipeline (once per `pass`:
    // makePassAndPipeline's fixed state on fsrLayout), v410's picture of image i and a set reading it through the linear
    // sampler. False: GSR is off for the run (the caller sets gsrFailed).
    bool ensureGsr(std::uint32_t i,unsigned variant,bool big=false) { // (lane fsr-stage-b: big = the frame reads the render-size picture)
        if(!pass || !fsrLayout || !vertex || variant>1 || i>=images.size() || images.size()>MaxGsrSets)return false;
        if(!gsrStaticTried) {
            gsrStaticTried=true;
            VkSamplerCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;s.magFilter=s.minFilter=VK_FILTER_LINEAR;
            s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;s.addressModeU=s.addressModeV=s.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            s.maxLod=0;
            if(vkCreateSampler(dev->device,&s,nullptr,&gsrSampler)!=VK_SUCCESS)gsrSampler=VK_NULL_HANDLE;
            std::array<VkDescriptorPoolSize,2> gs{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,MaxGsrSets},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,MaxGsrSets}}};
            VkDescriptorPoolCreateInfo gd{};gd.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;gd.maxSets=MaxGsrSets;gd.poolSizeCount=2;gd.pPoolSizes=gs.data();
            if(vkCreateDescriptorPool(dev->device,&gd,nullptr,&gsrSetPool)!=VK_SUCCESS)gsrSetPool=VK_NULL_HANDLE;
            RECOMP_DIRECT_LOG("RENDER_DIRECT_GSR static sampler=%u pool=%u cap=recomp-cap:gsr1",unsigned(gsrSampler!=VK_NULL_HANDLE),unsigned(gsrSetPool!=VK_NULL_HANDLE));
        }
        if(!gsrSampler || !gsrSetPool)return false;
        if(!gsrModuleTried[variant]) {
            gsrModuleTried[variant]=true;
            VkShaderModuleCreateInfo gm{};gm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            gm.codeSize=variant?sizeof GsrEdgeFragmentSpirv:sizeof GsrFragmentSpirv;gm.pCode=variant?GsrEdgeFragmentSpirv:GsrFragmentSpirv;
            if(vkCreateShaderModule(dev->device,&gm,nullptr,&gsrModule[variant])!=VK_SUCCESS)gsrModule[variant]=VK_NULL_HANDLE;
        }
        if(!gsrModule[variant])return false;
        if(!gsrPipelineTried[variant]) {
            gsrPipelineTried[variant]=true;
            std::array<VkPipelineShaderStageCreateInfo,2> st{};
            st[0].sType=st[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[0].stage=VK_SHADER_STAGE_VERTEX_BIT;st[0].module=vertex;st[0].pName="main";
            st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;st[1].module=gsrModule[variant];st[1].pName="main";
            VkPipelineVertexInputStateCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            VkPipelineInputAssemblyStateCreateInfo ia{};ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=vp.scissorCount=1;
            VkPipelineRasterizationStateCreateInfo rs{};rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState ba{};ba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|
                VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{};cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;cb.attachmentCount=1;cb.pAttachments=&ba;
            const std::array<VkDynamicState,2> dyn{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;ds.dynamicStateCount=2;ds.pDynamicStates=dyn.data();
            VkGraphicsPipelineCreateInfo gp{};gp.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;gp.stageCount=2;gp.pStages=st.data();
            gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;
            gp.pColorBlendState=&cb;gp.pDynamicState=&ds;gp.layout=fsrLayout;gp.renderPass=pass;gp.subpass=0;
            const auto t0=std::chrono::steady_clock::now();
            if(vkCreateGraphicsPipelines(dev->device,VK_NULL_HANDLE,1,&gp,nullptr,&gsrPipeline[variant])!=VK_SUCCESS)gsrPipeline[variant]=VK_NULL_HANDLE;
            RECOMP_DIRECT_LOG("RENDER_DIRECT_GSR pipeline variant=%u ok=%u ms=%.1f",variant,unsigned(gsrPipeline[variant]!=VK_NULL_HANDLE),
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count());
        }
        if(!gsrPipeline[variant])return false;
        if(big)return true; // lane fsr-stage-b: v410's 1280x720 picture and its set are not needed (ensureBig makes the render-size ones)
        if(!ensureMap(i))return false;
        if(gsrSets.size()!=images.size())gsrSets.assign(images.size(),VK_NULL_HANDLE); // (empty here: destroyMaps cleared them)
        if(gsrSets[i])return true;
        VkDescriptorSetAllocateInfo sa{};sa.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;sa.descriptorPool=gsrSetPool;sa.descriptorSetCount=1;sa.pSetLayouts=&setLayout;
        VkDescriptorSet set=VK_NULL_HANDLE;
        if(vkAllocateDescriptorSets(dev->device,&sa,&set)!=VK_SUCCESS)return false;
        VkDescriptorImageInfo ii{gsrSampler,mapImages[i].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo bi{lutBuffer.buffer,0,VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet,2> wr{};
        for(auto& x:wr){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=set;x.descriptorCount=1;}
        wr[0].dstBinding=0;wr[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr[0].pImageInfo=&ii;
        wr[1].dstBinding=1;wr[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr[1].pBufferInfo=&bi;
        vkUpdateDescriptorSets(dev->device,2,wr.data(),0,nullptr);
        gsrSets[i]=set;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_GSR set=%u of %zu",i,images.size());
        return true;
    }
    void destroyMaps() { // v410 (callers have waited for every slot)
        destroyBig(); // lane fsr-stage-b (nothing unless a render-size picture exists)
        destroyScaleImages(); // v411
        destroyPacks(); // v42x
        destroyFsrImages(); // v450
        destroyGsrSets(); // v460
        for(auto f:mapFramebuffers)if(f)vkDestroyFramebuffer(dev->device,f,nullptr);
        for(auto& m:mapImages){if(m.view)vkDestroyImageView(dev->device,m.view,nullptr);if(m.image)vkDestroyImage(dev->device,m.image,nullptr);
            if(m.memory)vkFreeMemory(dev->device,m.memory,nullptr);}
        if(mapSetPool)vkResetDescriptorPool(dev->device,mapSetPool,0); // (v411: also the copy sets)
        mapImages.clear();mapFramebuffers.clear();mapSets.clear();
    }
    // v410: image i's intermediate (created at its first filtered frame); false: use v409's single pass.
    bool ensureMap(std::uint32_t i) {
        if(mapFailed || !mapPass || !mapSetPool || i>=images.size())return false;
        if(mapImages.size()!=images.size()){mapImages.resize(images.size());mapFramebuffers.resize(images.size(),VK_NULL_HANDLE);mapSets.resize(images.size(),VK_NULL_HANDLE);}
        if(mapSets[i])return true;
        const auto fail=[&](const char* what){mapFailed=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_FILTER map=failed %s",what);return false;};
        auto& m=mapImages[i];
        VkImageCreateInfo ic{};ic.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ic.imageType=VK_IMAGE_TYPE_2D;ic.format=format;
        ic.extent={SourceWidth,SourceHeight,1};ic.mipLevels=1;ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;
        ic.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(dev->device,&ic,nullptr,&m.image)!=VK_SUCCESS){m.image=VK_NULL_HANDLE;return fail("image");}
        VkMemoryRequirements r{};vkGetImageMemoryRequirements(dev->device,m.image,&r);
        const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(!type)return fail("memory-type");
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=*type;
        if(vkAllocateMemory(dev->device,&a,nullptr,&m.memory)!=VK_SUCCESS){m.memory=VK_NULL_HANDLE;return fail("memory");}
        if(vkBindImageMemory(dev->device,m.image,m.memory,0)!=VK_SUCCESS)return fail("bind");
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=m.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
        v.format=format;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(vkCreateImageView(dev->device,&v,nullptr,&m.view)!=VK_SUCCESS){m.view=VK_NULL_HANDLE;return fail("view");}
        VkFramebufferCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;f.renderPass=mapPass;f.attachmentCount=1;f.pAttachments=&m.view;
        f.width=SourceWidth;f.height=SourceHeight;f.layers=1;
        if(vkCreateFramebuffer(dev->device,&f,nullptr,&mapFramebuffers[i])!=VK_SUCCESS){mapFramebuffers[i]=VK_NULL_HANDLE;return fail("framebuffer");}
        VkDescriptorSetAllocateInfo sa{};sa.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;sa.descriptorPool=mapSetPool;sa.descriptorSetCount=1;sa.pSetLayouts=&setLayout;
        VkDescriptorSet set=VK_NULL_HANDLE;
        if(vkAllocateDescriptorSets(dev->device,&sa,&set)!=VK_SUCCESS)return fail("set");
        VkDescriptorImageInfo ii{sampler,m.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo bi{lutBuffer.buffer,0,VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet,2> w{};
        for(auto& x:w){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=set;x.descriptorCount=1;}
        w[0].dstBinding=0;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&ii;
        w[1].dstBinding=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&bi;
        vkUpdateDescriptorSets(dev->device,2,w.data(),0,nullptr);
        mapSets[i]=set;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FILTER map_image=%u of %zu",i,images.size());
        return true;
    }
    // ---- lane fsr-stage-b: the helpers (see the members' comment). Out of line and noexcept.
    [[gnu::noinline]] void destroyBig() noexcept { // (callers have waited for every slot)
        if(bigImages.empty() && bigSets.empty())return;
        for(auto f:bigFramebuffers)if(f)vkDestroyFramebuffer(dev->device,f,nullptr);
        for(auto& bm:bigImages){if(bm.view)vkDestroyImageView(dev->device,bm.view,nullptr);if(bm.image)vkDestroyImage(dev->device,bm.image,nullptr);
            if(bm.memory)vkFreeMemory(dev->device,bm.memory,nullptr);}
        if(bigSetPool)vkResetDescriptorPool(dev->device,bigSetPool,0);
        bigImages.clear();bigFramebuffers.clear();bigSets.clear();bigGsrSets.clear();bigW=bigH=0;
    }
    // Swapchain image i's render-size map picture (src.w x src.h, the swapchain's format: the map pass's render pass and the
    // fast map pipeline), its framebuffer and its NEAREST set; with gsr also its LINEAR set (GSR's sampler exists: ensureGsr
    // ran). False: no render-size path for this frame, and (bigFailed) for none after it.
    [[gnu::noinline]] bool ensureBig(std::uint32_t i,rsb::Size src,bool gsr) noexcept {
        if(bigFailed)return false;
        const auto fail=[&](const char* what){bigFailed=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_SOURCE big=failed %s source=%ux%u",what,src.w,src.h);return false;};
        // (the map pass at 1:1 needs the fast map pipeline: mode 0 of direct_present.frag divides by 1280 and 720)
        if(!mapPass || !mapFastPipeline || !pipeline || i>=images.size() || 2*images.size()>MaxBigSets)return fail("objects");
        try {
            if(!bigSetPool) {
                if(bigPoolTried)return fail("set-pool");
                bigPoolTried=true;
                std::array<VkDescriptorPoolSize,2> bs{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,MaxBigSets},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,MaxBigSets}}};
                VkDescriptorPoolCreateInfo bd{};bd.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;bd.maxSets=MaxBigSets;bd.poolSizeCount=2;bd.pPoolSizes=bs.data();
                if(vkCreateDescriptorPool(dev->device,&bd,nullptr,&bigSetPool)!=VK_SUCCESS){bigSetPool=VK_NULL_HANDLE;return fail("set-pool");}
            }
            if(bigImages.size()!=images.size() || bigW!=src.w || bigH!=src.h) {
                if(!bigImages.empty() && !waitAllMine())return false;
                destroyBig();bigImages.resize(images.size());bigFramebuffers.resize(images.size(),VK_NULL_HANDLE);
                bigSets.resize(images.size(),VK_NULL_HANDLE);bigGsrSets.resize(images.size(),VK_NULL_HANDLE);bigW=src.w;bigH=src.h;
            }
            const auto makeSet=[&](VkSampler with,VkDescriptorSet& out) {
                VkDescriptorSetAllocateInfo sa{};sa.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;sa.descriptorPool=bigSetPool;sa.descriptorSetCount=1;sa.pSetLayouts=&setLayout;
                VkDescriptorSet set=VK_NULL_HANDLE;
                if(vkAllocateDescriptorSets(dev->device,&sa,&set)!=VK_SUCCESS)return false;
                VkDescriptorImageInfo ii{with,bigImages[i].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo bi{lutBuffer.buffer,0,VK_WHOLE_SIZE};
                std::array<VkWriteDescriptorSet,2> w{};
                for(auto& x:w){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=set;x.descriptorCount=1;}
                w[0].dstBinding=0;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&ii;
                w[1].dstBinding=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&bi;
                vkUpdateDescriptorSets(dev->device,2,w.data(),0,nullptr);
                out=set;return true;
            };
            if(!bigSets[i]) {
                auto& bm=bigImages[i];
                VkImageCreateInfo ic{};ic.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ic.imageType=VK_IMAGE_TYPE_2D;ic.format=format;
                ic.extent={src.w,src.h,1};ic.mipLevels=1;ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;
                ic.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
                if(vkCreateImage(dev->device,&ic,nullptr,&bm.image)!=VK_SUCCESS){bm.image=VK_NULL_HANDLE;return fail("image");}
                VkMemoryRequirements r{};vkGetImageMemoryRequirements(dev->device,bm.image,&r);
                const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if(!type)return fail("memory-type");
                VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=*type;
                if(vkAllocateMemory(dev->device,&a,nullptr,&bm.memory)!=VK_SUCCESS){bm.memory=VK_NULL_HANDLE;return fail("memory");}
                if(vkBindImageMemory(dev->device,bm.image,bm.memory,0)!=VK_SUCCESS)return fail("bind");
                VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=bm.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
                v.format=format;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                if(vkCreateImageView(dev->device,&v,nullptr,&bm.view)!=VK_SUCCESS){bm.view=VK_NULL_HANDLE;return fail("view");}
                VkFramebufferCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;f.renderPass=mapPass;f.attachmentCount=1;f.pAttachments=&bm.view;
                f.width=src.w;f.height=src.h;f.layers=1;
                if(vkCreateFramebuffer(dev->device,&f,nullptr,&bigFramebuffers[i])!=VK_SUCCESS){bigFramebuffers[i]=VK_NULL_HANDLE;return fail("framebuffer");}
                if(!makeSet(sampler,bigSets[i]))return fail("set");
                RECOMP_DIRECT_LOG("RENDER_DIRECT_SOURCE picture=%u of %zu size=%ux%u bytes=%llu",i,images.size(),src.w,src.h,(unsigned long long)r.size);
            }
            if(gsr && !bigGsrSets[i]) {
                if(!gsrSampler || !makeSet(gsrSampler,bigGsrSets[i]))return fail("gsr-set");
            }
        } catch(...) {return fail("exception");}
        return true;
    }
    // The presenter's own 1280x720 copy of a render-size frame (the down path), made once, before a frame's acquire. False:
    // the GPU path is off for the run (the renderer's frames come through the CPU path again, at 1280x720: stage A).
    [[gnu::noinline]] bool ensureDown() noexcept {
        if(downSet)return true;
        const auto fail=[&](const char* what){gpuAuditFailed=true;gpuOn.store(false,std::memory_order_release);
            RECOMP_DIRECT_LOG("RENDER_DIRECT_SOURCE gpu=off why=down-image %s",what);return false;};
        if(downTried)return fail("earlier");
        downTried=true;
        VkImageCreateInfo ic{};ic.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ic.imageType=VK_IMAGE_TYPE_2D;ic.format=VK_FORMAT_R8G8B8A8_UNORM;
        ic.extent={SourceWidth,SourceHeight,1};ic.mipLevels=1;ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;
        ic.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(dev->device,&ic,nullptr,&downImage.image)!=VK_SUCCESS){downImage.image=VK_NULL_HANDLE;return fail("image");}
        VkMemoryRequirements r{};vkGetImageMemoryRequirements(dev->device,downImage.image,&r);
        const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(!type)return fail("memory-type");
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=*type;
        if(vkAllocateMemory(dev->device,&a,nullptr,&downImage.memory)!=VK_SUCCESS){downImage.memory=VK_NULL_HANDLE;return fail("memory");}
        if(vkBindImageMemory(dev->device,downImage.image,downImage.memory,0)!=VK_SUCCESS)return fail("bind");
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=downImage.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
        v.format=VK_FORMAT_R8G8B8A8_UNORM;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(vkCreateImageView(dev->device,&v,nullptr,&downImage.view)!=VK_SUCCESS){downImage.view=VK_NULL_HANDLE;return fail("view");}
        std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}};
        VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=1;dp.poolSizeCount=2;dp.pPoolSizes=sizes.data();
        if(vkCreateDescriptorPool(dev->device,&dp,nullptr,&downSetPool)!=VK_SUCCESS){downSetPool=VK_NULL_HANDLE;return fail("set-pool");}
        VkDescriptorSetAllocateInfo sa{};sa.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;sa.descriptorPool=downSetPool;sa.descriptorSetCount=1;sa.pSetLayouts=&setLayout;
        VkDescriptorSet set=VK_NULL_HANDLE;
        if(vkAllocateDescriptorSets(dev->device,&sa,&set)!=VK_SUCCESS)return fail("set");
        VkDescriptorImageInfo ii{sampler,downImage.view,VK_IMAGE_LAYOUT_GENERAL};VkDescriptorBufferInfo bi{lutBuffer.buffer,0,VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet,2> w{};
        for(auto& x:w){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=set;x.descriptorCount=1;}
        w[0].dstBinding=0;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&ii;
        w[1].dstBinding=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&bi;
        vkUpdateDescriptorSets(dev->device,2,w.data(),0,nullptr);
        downSet=set;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_SOURCE down_image=1 size=%ux%u bytes=%llu",unsigned(SourceWidth),unsigned(SourceHeight),(unsigned long long)r.size);
        return true;
    }
    // The audit's source buffer holds a render-size picture (grown here, never by recordAudit, which only makes the
    // 1280x720 one). No frame's readback is in flight: every audited frame is waited for in checkAudit. A failure blocks
    // audits for the run (logged): recordAudit must never copy a larger picture into a smaller buffer.
    [[gnu::noinline]] void auditRoom(rsb::Size src) noexcept {
        const std::size_t need=rsb::auditBufferBytes(src); // (never smaller than the 1280x720 picture: a down-path or CPU frame's audit reads that)
        if(!auditable || auditBlocked || auditSource.size>=need)return;
        if(auditSource.buffer || auditSource.memory)freeBuffer(auditSource);
        if(!makeBuffer(need,VK_BUFFER_USAGE_TRANSFER_DST_BIT,auditSource,true)) {
            freeBuffer(auditSource);auditBlocked=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_SOURCE audits=off why=source-buffer source=%ux%u",src.w,src.h);
        }
    }
    // The down path: the render-size picture (GENERAL, written by commands submitted earlier) into the 1280x720 copy with one
    // LINEAR blit (stage A's blit), after everything recorded or submitted before it.
    [[gnu::noinline]] void recordDown(VkCommandBuffer cb,VkImage from,rsb::Size src) noexcept {
        const bool first=!downGeneral;downGeneral=true; // (recorded only when a frame is recorded)
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=downImage.image;
        b.oldLayout=first?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,nullptr,1,&b);
        VkImageBlit r{};r.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};r.dstSubresource=r.srcSubresource;
        r.srcOffsets[1]={std::int32_t(src.w),std::int32_t(src.h),1};r.dstOffsets[1]={std::int32_t(SourceWidth),std::int32_t(SourceHeight),1};
        vkCmdBlitImage(cb,from,VK_IMAGE_LAYOUT_GENERAL,downImage.image,VK_IMAGE_LAYOUT_GENERAL,1,&r,VK_FILTER_LINEAR);
        VkMemoryBarrier rb{};rb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;rb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        rb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,1,&rb,0,nullptr,0,nullptr);
        ++rsbDownBlits;
    }
    // The render-size map pass: each texel of the source through the tables at 1:1 into image i's render-size picture (the
    // fast map pipeline, mode 5: texelFetch at the fragment's own position, so it is right for every size).
    [[gnu::noinline]] void recordBigMap(VkCommandBuffer cb,std::uint32_t i,VkDescriptorSet set,rsb::Size src,bool identity) noexcept {
        VkRenderPassBeginInfo mp{};mp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;mp.renderPass=mapPass;mp.framebuffer=bigFramebuffers[i];
        mp.renderArea={{0,0},{src.w,src.h}};
        vkCmdBeginRenderPass(cb,&mp,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,mapFastPipeline);
        const VkViewport mv{0,0,float(src.w),float(src.h),0,1};const VkRect2D ms{{0,0},{src.w,src.h}};
        vkCmdSetViewport(cb,0,1,&mv);vkCmdSetScissor(cb,0,1,&ms);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&set,0,nullptr);
        const std::int32_t mpush[8]{0,0,std::int32_t(src.w),std::int32_t(src.h),identity?768:0,5,0,0};
        vkCmdPushConstants(cb,pipelineLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof mpush,mpush);
        vkCmdDraw(cb,3,1,0,0);
        vkCmdEndRenderPass(cb);
    }
    // RENDER_DIRECT_SOURCE: which path the render-size frames took (counts since the start; the 1:1 copy's pass time).
    [[gnu::noinline]] void rsbLog(std::uint64_t total,rsb::Size src,const Layout& l,rsb::Path now,unsigned scaler) noexcept {
        rsbLogged=src;
        unsigned made=0;for(const auto s:bigSets)made+=s!=VK_NULL_HANDLE; // (the render-size pictures that exist)
        RECOMP_DIRECT_LOG("RENDER_DIRECT_SOURCE presents=%llu source=%ux%u rect=%ux%u path=%s scaler=%u frames=down:%llu,one:%llu,scaler:%llu down_blits=%llu "
            "big_failed=%u one_failed=%u pictures=%u one_pass_us=%.1f one_pass_n=%llu one_map_us=%.1f forgets=%llu",(unsigned long long)total,src.w,src.h,l.imageWidth,l.imageHeight,
            rsb::pathName(now),scaler,(unsigned long long)rsbFrames[unsigned(rsb::Path::Down)],(unsigned long long)rsbFrames[unsigned(rsb::Path::One)],
            (unsigned long long)rsbFrames[unsigned(rsb::Path::Scaler)],(unsigned long long)rsbDownBlits,unsigned(bigFailed),unsigned(oneFailed),made,
            passMean(rsb::OneSlot),(unsigned long long)passN[rsb::OneSlot],mapMean(rsb::OneSlot),(unsigned long long)rsbForgets);
    }
    // The 1:1 copy against rsb::referenceOne of the frame's source: every byte equal (no filter, no rounding freedom).
    // A failure turns the copy off for the run (such frames then take their scaler's render-size path or the down path).
    [[gnu::noinline]] void checkOneAudit(bool identity) noexcept {
        const auto* src=static_cast<const std::uint8_t*>(auditSource.mapped);
        const auto* got=static_cast<const std::uint8_t*>(auditTarget.mapped);
        std::vector<std::uint8_t> expected;std::size_t mismatches=0,first=0;bool built=false;
        try {built=src && got && rsb::referenceOne(src,rsb::Size{auditW,auditH},identity?identityLut():gameLut,extent.width,extent.height,bgra,expected);} catch(...) {built=false;}
        if(built)for(std::size_t b=0;b<expected.size();++b)if(got[b]!=expected[b]){if(!mismatches++)first=b;}
        const bool passed=built && !mismatches;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_AUDIT kind=%s one=1 source=%ux%u pass=%u bytes=%zu mismatches=%zu first=%zu got=%u expected=%u extent=%ux%u bgra=%u",
            identity?"cpu":"gpu",auditW,auditH,unsigned(passed),expected.size(),mismatches,first,built && mismatches?unsigned(got[first]):0u,
            built && mismatches?unsigned(expected[first]):0u,extent.width,extent.height,unsigned(bgra));
        if(passed)return;
        ++auditFailures;oneFailed=true;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_SOURCE one=0 why=audit built=%u",unsigned(built));
    }
    void destroySwapchainObjects(bool keepSwapchain) {
        pendingPresents.clear();pendingTags.clear(); // v259 (images acquired from a swapchain being replaced need no present)
        destroyMaps(); // v410
        for(auto f:framebuffers)vkDestroyFramebuffer(dev->device,f,nullptr);
        for(auto v:views)vkDestroyImageView(dev->device,v,nullptr);
        for(auto s:renderDone)vkDestroySemaphore(dev->device,s,nullptr);
        if(!commands.empty())vkFreeCommandBuffers(dev->device,pool,std::uint32_t(commands.size()),commands.data());
        framebuffers.clear();views.clear();renderDone.clear();commands.clear();images.clear();imageSlot.clear();
        if(!keepSwapchain && swapchain){destroySwapchain(dev->device,swapchain,nullptr);swapchain=VK_NULL_HANDLE;}
    }
    void destroySurfaceAndWindow() {
        if(swapchain)destroySwapchainObjects(false);
        if(surface){destroySurface(dev->instance,surface,nullptr);surface=VK_NULL_HANDLE;}
        if(nw){window.release(window.user,nw);nw=nullptr;}
    }
    // Leaves nothing on the window (the app may attach its own renderer afterwards).
    void destroyAll() {
        if(!dev){if(nw && window.release){window.release(window.user,nw);nw=nullptr;}return;}
        waitAllMine();
        destroySurfaceAndWindow();
        if(pipeline){vkDestroyPipeline(dev->device,pipeline,nullptr);pipeline=VK_NULL_HANDLE;}
        if(pass){vkDestroyRenderPass(dev->device,pass,nullptr);pass=VK_NULL_HANDLE;}
        for(auto& a:acquires){if(a.sem)vkDestroySemaphore(dev->device,a.sem,nullptr);if(a.fence)vkDestroyFence(dev->device,a.fence,nullptr);}
        acquires.clear();
        if(timePool){vkDestroyQueryPool(dev->device,timePool,nullptr);timePool=VK_NULL_HANDLE;slotTimed.fill(0);} // v409
        if(midPool){vkDestroyQueryPool(dev->device,midPool,nullptr);midPool=VK_NULL_HANDLE;slotMid.fill(0);} // v42x
        if(mapPass){vkDestroyRenderPass(dev->device,mapPass,nullptr);mapPass=VK_NULL_HANDLE;} // v410
        if(scalePipeline){vkDestroyPipeline(dev->device,scalePipeline,nullptr);scalePipeline=VK_NULL_HANDLE;} // v411
        for(VkPipeline* f:{&mapFastPipeline,&gatherPipeline,&packedPipeline})if(*f){vkDestroyPipeline(dev->device,*f,nullptr);*f=VK_NULL_HANDLE;} // v42x
        for(VkPipeline* f:{&easuPipeline,&easuDirectPipeline,&rcasPipeline})if(*f){vkDestroyPipeline(dev->device,*f,nullptr);*f=VK_NULL_HANDLE;} // v450
        destroyGsrPipelines(); // v460
        if(fsrPass){vkDestroyRenderPass(dev->device,fsrPass,nullptr);fsrPass=VK_NULL_HANDLE;} // v450
        if(fsrMidPool){vkDestroyQueryPool(dev->device,fsrMidPool,nullptr);fsrMidPool=VK_NULL_HANDLE;slotFsrMid.fill(0);} // v450
        freeBuffer(auditMid); // v450
        for(auto& b:bands)freeBuffer(b); // latency-path motion probe (every slot was waited and released above)
        bands.clear();
        released_=true;gpuOn.store(false,std::memory_order_release);
    }
    bool makePassAndPipeline() {
        if(pass && passFormat==format)return true;
        if(pipeline){vkDestroyPipeline(dev->device,pipeline,nullptr);pipeline=VK_NULL_HANDLE;}
        if(pass){vkDestroyRenderPass(dev->device,pass,nullptr);pass=VK_NULL_HANDLE;}
        VkAttachmentDescription at{};at.format=format;at.samples=VK_SAMPLE_COUNT_1_BIT;at.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at.storeOp=VK_ATTACHMENT_STORE_OP_STORE;at.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;at.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;at.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sp.colorAttachmentCount=1;sp.pColorAttachments=&ref;
        VkSubpassDependency dep{};dep.srcSubpass=VK_SUBPASS_EXTERNAL;dep.dstSubpass=0; // the acquire semaphore's wait stage
        dep.srcStageMask=dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;rp.attachmentCount=1;rp.pAttachments=&at;
        rp.subpassCount=1;rp.pSubpasses=&sp;rp.dependencyCount=1;rp.pDependencies=&dep;
        if(vkCreateRenderPass(dev->device,&rp,nullptr,&pass)!=VK_SUCCESS){pass=VK_NULL_HANDLE;return false;}
        { // v410: the map pass (compatible with pass: same format and samples, so the same pipeline draws it)
            if(mapPass){vkDestroyRenderPass(dev->device,mapPass,nullptr);mapPass=VK_NULL_HANDLE;}
            VkAttachmentDescription ma=at;ma.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            std::array<VkSubpassDependency,2> md{};
            md[0].srcSubpass=VK_SUBPASS_EXTERNAL;md[0].dstSubpass=0;md[0].srcStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            md[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;md[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            md[1].srcSubpass=0;md[1].dstSubpass=VK_SUBPASS_EXTERNAL;md[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            md[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;md[1].dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            md[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            VkRenderPassCreateInfo mr=rp;mr.pAttachments=&ma;mr.dependencyCount=2;mr.pDependencies=md.data();
            if(vkCreateRenderPass(dev->device,&mr,nullptr,&mapPass)!=VK_SUCCESS)mapPass=VK_NULL_HANDLE;
        }
        std::array<VkPipelineShaderStageCreateInfo,2> st{};
        st[0].sType=st[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage=VK_SHADER_STAGE_VERTEX_BIT;st[0].module=vertex;st[0].pName="main";
        st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;st[1].module=fragment;st[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{};rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};ba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|
            VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;cb.attachmentCount=1;cb.pAttachments=&ba;
        const std::array<VkDynamicState,2> dyn{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;ds.dynamicStateCount=2;ds.pDynamicStates=dyn.data();
        VkGraphicsPipelineCreateInfo gp{};gp.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;gp.stageCount=2;gp.pStages=st.data();
        gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;
        gp.pColorBlendState=&cb;gp.pDynamicState=&ds;gp.layout=pipelineLayout;gp.renderPass=pass;gp.subpass=0;
        if(vkCreateGraphicsPipelines(dev->device,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline)!=VK_SUCCESS){pipeline=VK_NULL_HANDLE;return false;}
        { // v42x: the fast modes' pipelines (the same state, their own fragment shaders; compatible with pass and mapPass). A
          // failure leaves that mode unavailable (its frames take v410's path).
            const std::array<std::pair<VkShaderModule,VkPipeline*>,3> fast{{{mapFastModule,&mapFastPipeline},{gatherModule,&gatherPipeline},
                {packedModule,&packedPipeline}}};
            for(const auto& [module,out]:fast) {
                if(*out){vkDestroyPipeline(dev->device,*out,nullptr);*out=VK_NULL_HANDLE;}
                if(!module)continue;
                st[1].module=module;
                if(vkCreateGraphicsPipelines(dev->device,VK_NULL_HANDLE,1,&gp,nullptr,out)!=VK_SUCCESS)*out=VK_NULL_HANDLE;
            }
            st[1].module=fragment;
            RECOMP_DIRECT_LOG("RENDER_DIRECT_FILTER fast_pipelines map=%u gather=%u packed=%u",unsigned(mapFastPipeline!=VK_NULL_HANDLE),
                unsigned(gatherPipeline!=VK_NULL_HANDLE),unsigned(packedPipeline!=VK_NULL_HANDLE));
        }
        // v450: FSR's pass and pipelines follow `pass` (ensureFsrPipelines builds them again at the next FSR frame)
        for(VkPipeline* f:{&easuPipeline,&easuDirectPipeline,&rcasPipeline})if(*f){vkDestroyPipeline(dev->device,*f,nullptr);*f=VK_NULL_HANDLE;}
        if(fsrPass){vkDestroyRenderPass(dev->device,fsrPass,nullptr);fsrPass=VK_NULL_HANDLE;}
        fsrBuilt=false;
        destroyGsrPipelines(); // v460 (ensureGsr builds the wanted one again at the next GSR frame)
        passFormat=format;return true;
    }
    // v450: FSR's render pass (EASU's RGBA8 picture, then sampled by RCAS: mapPass's dependencies) and its three pipelines
    // (makePassAndPipeline's fixed state, fsrLayout), built at the first FSR frame after `pass` was made, so a run without the
    // switch creates none of them. False (or a missing pipeline): FSR is off for the run.
    bool ensureFsrPipelines() {
        if(fsrBuilt)return easuPipeline && easuDirectPipeline && rcasPipeline && fsrPass;
        fsrBuilt=true;
        if(!pass || !fsrLayout || !easuModule || !rcasModule)return false;
        VkAttachmentDescription fa{};fa.format=VK_FORMAT_R8G8B8A8_UNORM;fa.samples=VK_SAMPLE_COUNT_1_BIT;fa.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        fa.storeOp=VK_ATTACHMENT_STORE_OP_STORE;fa.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;fa.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        fa.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;fa.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sp.colorAttachmentCount=1;sp.pColorAttachments=&ref;
        std::array<VkSubpassDependency,2> fd{};
        fd[0].srcSubpass=VK_SUBPASS_EXTERNAL;fd[0].dstSubpass=0;fd[0].srcStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        fd[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;fd[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        fd[1].srcSubpass=0;fd[1].dstSubpass=VK_SUBPASS_EXTERNAL;fd[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        fd[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;fd[1].dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        fd[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo fr{};fr.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;fr.attachmentCount=1;fr.pAttachments=&fa;
        fr.subpassCount=1;fr.pSubpasses=&sp;fr.dependencyCount=2;fr.pDependencies=fd.data();
        if(vkCreateRenderPass(dev->device,&fr,nullptr,&fsrPass)!=VK_SUCCESS){fsrPass=VK_NULL_HANDLE;return false;}
        std::array<VkPipelineShaderStageCreateInfo,2> st{};
        st[0].sType=st[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage=VK_SHADER_STAGE_VERTEX_BIT;st[0].module=vertex;st[0].pName="main";
        st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;st[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{};rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};ba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|
            VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;cb.attachmentCount=1;cb.pAttachments=&ba;
        const std::array<VkDynamicState,2> dyn{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;ds.dynamicStateCount=2;ds.pDynamicStates=dyn.data();
        VkGraphicsPipelineCreateInfo gp{};gp.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;gp.stageCount=2;gp.pStages=st.data();
        gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;
        gp.pColorBlendState=&cb;gp.pDynamicState=&ds;gp.layout=fsrLayout;gp.subpass=0;
        const auto make=[&](VkShaderModule module,VkRenderPass target,VkPipeline* out) {
            st[1].module=module;gp.renderPass=target;
            if(vkCreateGraphicsPipelines(dev->device,VK_NULL_HANDLE,1,&gp,nullptr,out)!=VK_SUCCESS)*out=VK_NULL_HANDLE;
        };
        const auto t0=std::chrono::steady_clock::now();
        make(easuModule,fsrPass,&easuPipeline);make(easuModule,pass,&easuDirectPipeline);make(rcasModule,pass,&rcasPipeline);
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FSR pipelines easu=%u easu_direct=%u rcas=%u ms=%.1f",unsigned(easuPipeline!=VK_NULL_HANDLE),
            unsigned(easuDirectPipeline!=VK_NULL_HANDLE),unsigned(rcasPipeline!=VK_NULL_HANDLE),
            std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count());
        return easuPipeline && easuDirectPipeline && rcasPipeline;
    }
    // (Re)creates the surface when the app's window changed and the swapchain when it is missing or out of date.
    bool ensureSwapchain() {
        if(broken || released_)return false;
        const auto g=window.generation(window.user);
        if(!nw || g!=gen || lost) {
            if(!waitAllMine())return false;
            destroySurfaceAndWindow();
            std::uint64_t acquiredGen=0;
            nw=window.acquire(window.user,&acquiredGen);gen=acquiredGen;lost=false;
            if(!nw)return false;
#if defined(__APPLE__)
            // ios-direct-present: nw is the app's CAMetalLayer (an opaque pointer here; the app keeps it alive for the run)
            VkMetalSurfaceCreateInfoEXT si{};si.sType=VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
            si.pLayer=reinterpret_cast<const CAMetalLayer*>(nw);
#else
            VkAndroidSurfaceCreateInfoKHR si{};si.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;si.window=nw;
#endif
            if(createSurface(dev->instance,&si,nullptr,&surface)!=VK_SUCCESS){surface=VK_NULL_HANDLE;RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT surface=failed");return false;}
            VkBool32 supported=VK_FALSE;
            if(surfaceSupport(dev->physical,dev->queueFamily,surface,&supported)!=VK_SUCCESS || !supported){RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT surface=unsupported-queue");return false;}
        }
        if(swapchain && !recreate) {
            // v256: a resized window (a new current extent) is recreated for; checked every 120 presents, not per frame.
#if defined(__APPLE__)
            // (build 192) a drawable-size change is checked for at once, not at the next 120th present
            const unsigned layoutGen=recomp_ios_layout_gen.load(std::memory_order_acquire);
            const bool layoutMoved=layoutGen!=recompIosExtentLayoutGen;recompIosExtentLayoutGen=layoutGen;
            if((++extentChecks%120) && !layoutMoved)return true;
#else
            if(++extentChecks%120)return true;
#endif
            VkSurfaceCapabilitiesKHR now{};
            if(surfaceCaps(dev->physical,surface,&now)!=VK_SUCCESS)return true;
            const bool fbFlip=iosFbOnlyWanted()!=fbOnly; // (ios-fbonly: the switch flipped, so the swapchain is made again)
            if(!fbFlip && now.currentExtent.width==extent.width && now.currentExtent.height==extent.height)return true;
            ++extentChanges;
        }
        VkSurfaceCapabilitiesKHR caps{};
        if(surfaceCaps(dev->physical,surface,&caps)!=VK_SUCCESS)return false;
        if(caps.currentExtent.width==0 || caps.currentExtent.height==0)return false; // (a hidden window: nothing to show)
        std::uint32_t n=0;
        if(surfaceFormats(dev->physical,surface,&n,nullptr)!=VK_SUCCESS || !n || n>256)return false;
        std::vector<VkSurfaceFormatKHR> formats(n);
        if(surfaceFormats(dev->physical,surface,&n,formats.data())!=VK_SUCCESS)return false;
        // vulkan_smoke.cpp's choice: RGBA8 UNORM + sRGB-nonlinear when offered, otherwise the first; RGBA8/BGRA8 UNORM only.
        auto chosen=formats.front();
        if(chosen.format==VK_FORMAT_UNDEFINED){chosen.format=VK_FORMAT_R8G8B8A8_UNORM;chosen.colorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;}
        for(const auto& f:formats)if(f.format==VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR){chosen=f;break;}
        if(chosen.format!=VK_FORMAT_R8G8B8A8_UNORM && chosen.format!=VK_FORMAT_B8G8R8A8_UNORM){RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT format=unsupported %d",int(chosen.format));return false;}
        if(!(caps.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)){RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT transform=no-identity");return false;}
        VkExtent2D e=caps.currentExtent;
        if(e.width==0xFFFFFFFFu){e.width=std::clamp(1920u,caps.minImageExtent.width,caps.maxImageExtent.width);
            e.height=std::clamp(1080u,caps.minImageExtent.height,caps.maxImageExtent.height);}
        if(!layout(e.width,e.height))return false;
        std::uint32_t count=caps.minImageCount+1;if(caps.maxImageCount)count=std::min(count,caps.maxImageCount);
        // ios-fbonly (Claude 2026-10-21): without TRANSFER_SRC the swapchain's only use is the colour attachment, so MoltenVK makes
        // the CAMetalLayer framebufferOnly and Core Animation may use display-optimised drawables (hypothesis: cheaper system
        // composition; fill costs +1.2-1.4 ms of GPU per frame, c357/c358). The audit and the motion-probe band copies need
        // TRANSFER_SRC and are skipped while it is off (`auditable`). files/private-trace/ios-fbonly (or a switch list) selects it.
        fbOnly=iosFbOnlyWanted();
        const bool transferSrc=!fbOnly && (caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)!=0;
        VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for(const auto a:{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                          VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})if(caps.supportedCompositeAlpha&a){alpha=a;break;}
        if(!waitAllMine())return false;
        const VkSwapchainKHR old=swapchain;
        if(old)destroySwapchainObjects(true);
        VkSwapchainCreateInfoKHR ci{};ci.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;ci.surface=surface;ci.minImageCount=count;
        ci.imageFormat=chosen.format;ci.imageColorSpace=chosen.colorSpace;ci.imageExtent=e;ci.imageArrayLayers=1;
        ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|(transferSrc?VK_IMAGE_USAGE_TRANSFER_SRC_BIT:0);
        ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; // (as the app: Android composes it)
        ci.compositeAlpha=alpha;ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=VK_TRUE;ci.oldSwapchain=old;
        VkSwapchainKHR made=VK_NULL_HANDLE;
        const auto r=createSwapchain(dev->device,&ci,nullptr,&made);
        if(old)destroySwapchain(dev->device,old,nullptr);
        swapchain=VK_NULL_HANDLE;
        if(r!=VK_SUCCESS){RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT swapchain=failed %d",int(r));return false;}
        swapchain=made;format=chosen.format;extent=e;bgra=format==VK_FORMAT_B8G8R8A8_UNORM;auditable=transferSrc;
        RECOMP_DIRECT_LOG("RENDER_IOS_FBONLY on=%u extent=%ux%u",unsigned(fbOnly),e.width,e.height);
        paceRefresh(dev->device,swapchain); // pace-caps (lane A): the panel's refresh length, the vsync estimate's first guess (a read-only query)
        currentTransform=caps.currentTransform;
        // (from here a failure leaves a swapchain without its per-image objects: the presenter stops)
        const auto fail=[&](const char* what){broken=true;gpuOn.store(false,std::memory_order_release);RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT broken=%s",what);return false;};
        if(!makePassAndPipeline())return fail("pipeline");
        n=0;
        if(swapchainImages(dev->device,swapchain,&n,nullptr)!=VK_SUCCESS || !n || n>16)return fail("images");
        images.resize(n);
        if(swapchainImages(dev->device,swapchain,&n,images.data())!=VK_SUCCESS)return fail("images");
        views.assign(n,VK_NULL_HANDLE);framebuffers.assign(n,VK_NULL_HANDLE);renderDone.assign(n,VK_NULL_HANDLE);imageSlot.assign(n,-1);
        for(std::uint32_t i=0;i<n;++i) {
            VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=images[i];v.viewType=VK_IMAGE_VIEW_TYPE_2D;
            v.format=format;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            if(vkCreateImageView(dev->device,&v,nullptr,&views[i])!=VK_SUCCESS){views[i]=VK_NULL_HANDLE;return fail("view");}
            VkFramebufferCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;f.renderPass=pass;f.attachmentCount=1;f.pAttachments=&views[i];
            f.width=e.width;f.height=e.height;f.layers=1;
            if(vkCreateFramebuffer(dev->device,&f,nullptr,&framebuffers[i])!=VK_SUCCESS){framebuffers[i]=VK_NULL_HANDLE;return fail("framebuffer");}
            VkSemaphoreCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if(vkCreateSemaphore(dev->device,&s,nullptr,&renderDone[i])!=VK_SUCCESS){renderDone[i]=VK_NULL_HANDLE;return fail("semaphore");}
        }
        commands.assign(n,VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo ca{};ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ca.commandPool=pool;
        ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=n;
        if(vkAllocateCommandBuffers(dev->device,&ca,commands.data())!=VK_SUCCESS){commands.clear();return fail("commands");}
        while(acquires.size()<n+1) {
            Acquire a;VkSemaphoreCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkFenceCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if(vkCreateSemaphore(dev->device,&s,nullptr,&a.sem)!=VK_SUCCESS)return fail("acquire-semaphore");
            if(vkCreateFence(dev->device,&f,nullptr,&a.fence)!=VK_SUCCESS){vkDestroySemaphore(dev->device,a.sem,nullptr);return fail("fence");}
            acquires.push_back(a);
        }
        if(recreate || old)++recreated;
        recreate=false;
        return true;
    }
    // ---- frame-gen (lane B): the helpers (see the members' comment). Out of line and noexcept: presentGpu, presentLoop and
    // presentNow keep their control flow; presentLocked is not changed at all.
    [[gnu::noinline]] bool fgEnsure() noexcept {
        if(fgSet)return true;
        if(fgFailed)return false;
        const auto fail=[&](const char* what){fgFailed=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_FG failed=%s",what);return false;};
        {const rsb::Size fs=rsb::gpuSource();fgW=fs.w;fgH=fs.h;} // lane fsr-stage-b: the GPU frames' size (1280x720 unless a render scale is applied)
        for(Img* m:{&fgPrev,&fgOut}) { // 1280x720 RGBA8, GENERAL layout, as cpuImage (stage 1b adds COLOR_ATTACHMENT for the synthesis pass)
            VkImageCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;i.imageType=VK_IMAGE_TYPE_2D;i.format=VK_FORMAT_R8G8B8A8_UNORM;
            i.extent={fgW,fgH,1};i.mipLevels=1;i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;
            i.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;i.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; // (framegen-1b: + COLOR_ATTACHMENT, the synthesis pass draws fgOut)
            if(vkCreateImage(dev->device,&i,nullptr,&m->image)!=VK_SUCCESS){m->image=VK_NULL_HANDLE;return fail("image");}
            VkMemoryRequirements r{};vkGetImageMemoryRequirements(dev->device,m->image,&r);
            const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if(!type)return fail("memory-type");
            VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=*type;
            if(vkAllocateMemory(dev->device,&a,nullptr,&m->memory)!=VK_SUCCESS){m->memory=VK_NULL_HANDLE;return fail("memory");}
            if(vkBindImageMemory(dev->device,m->image,m->memory,0)!=VK_SUCCESS)return fail("bind");
            VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=m->image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
            v.format=VK_FORMAT_R8G8B8A8_UNORM;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            if(vkCreateImageView(dev->device,&v,nullptr,&m->view)!=VK_SUCCESS){m->view=VK_NULL_HANDLE;return fail("view");}
        }
        // fgOut's own set (review defect 2): one set in a pool of its own, written once
        std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}};
        VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=1;dp.poolSizeCount=2;dp.pPoolSizes=sizes.data();
        if(vkCreateDescriptorPool(dev->device,&dp,nullptr,&fgSetPool)!=VK_SUCCESS){fgSetPool=VK_NULL_HANDLE;return fail("set-pool");}
        VkDescriptorSetAllocateInfo sa{};sa.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;sa.descriptorPool=fgSetPool;sa.descriptorSetCount=1;sa.pSetLayouts=&setLayout;
        VkDescriptorSet set=VK_NULL_HANDLE;
        if(vkAllocateDescriptorSets(dev->device,&sa,&set)!=VK_SUCCESS)return fail("set");
        VkDescriptorImageInfo ii{sampler,fgOut.view,VK_IMAGE_LAYOUT_GENERAL};VkDescriptorBufferInfo bi{lutBuffer.buffer,0,VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet,2> w{};
        for(auto& x:w){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=set;x.descriptorCount=1;}
        w[0].dstBinding=0;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&ii;
        w[1].dstBinding=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&bi;
        vkUpdateDescriptorSets(dev->device,2,w.data(),0,nullptr);
        try {fgRecorder=[this](VkCommandBuffer cb){fgRecord(cb);};} catch(...) {return fail("recorder");}
        fgSet=set;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FG objects=1 size=%ux%u bytes=%u",unsigned(fgW),unsigned(fgH),unsigned(2u*rsb::bytes(rsb::Size{fgW,fgH})));
        return true;
    }
    // One whole-picture copy between two GENERAL-layout images, after everything recorded or submitted before it.
    void fgCopy(VkCommandBuffer cb,VkImage from,VkImage to) noexcept {
        VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,nullptr,0,nullptr);
        VkImageCopy c{};c.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.dstSubresource=c.srcSubresource;c.extent={fgW,fgH,1}; // (lane fsr-stage-b: the pictures' size)
        vkCmdCopyImage(cb,from,VK_IMAGE_LAYOUT_GENERAL,to,VK_IMAGE_LAYOUT_GENERAL,1,&c);
    }
    // presentLocked's `pre` recorder of a made picture (the commands in front of the unchanged presenter passes).
    void fgRecord(VkCommandBuffer cb) noexcept {
        const VkImage source=fgRecSource;const bool between=fgRecBetween;
        if(!fgGeneral) { // first use: both pictures to GENERAL (their contents are written below before anything reads them)
            std::array<VkImageMemoryBarrier,2> b{};
            for(unsigned k=0;k<2;++k){b[k].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b[k].image=k?fgOut.image:fgPrev.image;
                b[k].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b[k].newLayout=VK_IMAGE_LAYOUT_GENERAL;b[k].srcAccessMask=0;b[k].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                b[k].srcQueueFamilyIndex=b[k].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b[k].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,b.data());
            fgGeneral=true;
        }
        { // the made picture into fgOut (stage 1b: classify, resolve and synthesis instead of this copy)
            // framegen-1b: with diag-fg-between the five passes draw fgOut (true); false = no generator switch (one compare), or
            // this frame only keeps its picture (its LOW pass is recorded for the next pair): then stage 1a's copy
            if(!fgGenRecord(cb,between)){if(between)fgCopy(cb,fgPrev.image,fgOut.image);else fgCopy(cb,source,fgOut.image);}
            fgCopy(cb,source,fgPrev.image); // keep this real picture for the next pair
        }
        VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&mb,0,nullptr,0,nullptr);
        fgKept=true;
    }
    // The made picture through the unchanged presenter path, from fgOut and fgOut's own set.
    [[gnu::noinline]] bool fgPresentMade(VkQueue queue,VkImage source,bool between) noexcept {
        const auto t0=std::chrono::steady_clock::now();const auto a0=acquireNs;
        fgRecSource=source;fgRecBetween=between;
        const VkImageView realView=cachedView;const VkDescriptorSet realSet=cachedSet;
        cachedView=fgOut.view;cachedSet=fgSet;
        const bool ok=presentLocked(queue,fgOut.view,fgOut.image,false,fgRecorder,nullptr);
        cachedView=realView;cachedSet=realSet;
        const auto fgCallNs=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());
        fgMadeNs+=fgCallNs;
        fgAcquireNs+=acquireNs-a0;fgMade+=ok;
        fgGenMade(ok,fgCallNs); // framegen-1b: the slot keeper's account of a made picture; a requested audit (a compare and a return otherwise)
        return ok;
    }
    // The pair's first picture leaves pendingPresents while the second is recorded, and returns in front of it.
    void fgStash() noexcept {
        fgHeldPresents.swap(pendingPresents);fgHeldTags.swap(pendingTags);
        fgHeldSlots.clear();
        for(const auto i:fgHeldPresents)fgHeldSlots.push_back(i<imageSlot.size()?imageSlot[i]:-2);
    }
    void fgUnstash() noexcept {
        std::size_t kept=0; // (an image of a swapchain that was replaced meanwhile needs no present: its slot record is gone)
        for(std::size_t n=0;n<fgHeldPresents.size();++n) {
            const auto i=fgHeldPresents[n];
            if(i<imageSlot.size() && imageSlot[i]==fgHeldSlots[n]){fgHeldPresents[kept]=i;fgHeldTags[kept]=n<fgHeldTags.size()?fgHeldTags[n]:0u;++kept;}
        }
        fgStashDropped+=fgHeldPresents.size()-kept;
        fgHeldPresents.resize(kept);fgHeldTags.resize(kept);
        pendingPresents.insert(pendingPresents.begin(),fgHeldPresents.begin(),fgHeldPresents.end());
        pendingTags.insert(pendingTags.begin(),fgHeldTags.begin(),fgHeldTags.end());
        fgHeldPresents.clear();fgHeldTags.clear();
        if(!pendingPresents.empty())presentCv.notify_one();
    }
    // presentGpu, before the real frame's pass. No diag-fg-pair-* file: one relaxed load, a compare and a return.
    [[gnu::noinline]] void fgBefore(VkQueue queue,VkImage image,std::uint32_t tag) noexcept {
        if(!(fg.switches()&recomp::fg::SwModes)) {
            if(fgEver && (fgPlan.mode!=recomp::fg::Off || fgPrevValid)){fgPlan=recomp::fg::Plan{};fgPrevValid=false;}
            if(fgEver && fgTimed){fgTimed=false;recomp::pace::pairsTimed(false);} // pace-fg-tuning: the pacer plans untimed frames again
            if(fgEver)fgGenOff(); // framegen-1b: nothing of the generator stays planned or kept
            return;
        }
        fgEver=true;fgKept=false;fgMadeFirst=false;
        if(fgHolding)fgSuperseded=true; // the previous pair's second picture is still held: it goes out before this frame's
        if(setsUsed<fgSetsSeen)++fgPoolResets;else fgSetAllocs+=setsUsed-fgSetsSeen; // setFor()'s pool since the last frame
        fgSetsSeen=setsUsed;
        recomp::fg::Slot s{};
        if(const auto f=recomp::fg::slotSource.load(std::memory_order_relaxed))f(tag,&s);
        // framegen-1b: fgGenAllows() is true without diag-fg-between (a load and a test); with it, false while the slot keeper has fallen back to real frames only,
        // and (check c2) false for a frame whose inputs the generator refuses: such a frame is never planned as a timed pair (skip=failed, pairsTimed(false) below)
        fgPlan=fg.real(tag,recomp::fg::monotonicNs(),s,pastTiming!=nullptr && lagOn,presentThread.joinable() && !stopPresenter,fgGenAllows(tag) && tag!=0 && !fgFailed);
        if(fgPlan.timed!=fgTimed){fgTimed=fgPlan.timed;recomp::pace::pairsTimed(fgTimed);} // pace-fg-tuning: lane A's lead and trim follow (one rule for both lanes)
        if(!(++fgFrames%256) || fgFrames==1)fgLog();
        if(fgPlan.mode==recomp::fg::Off){fgPrevValid=false;return;}
        if(!fgEnsure()){fg.secondLost(tag);fgPlan=recomp::fg::Plan{};fgPrevValid=false;return;}
        // lane fsr-stage-b: the two pictures were made for one source size; a frame of another size is shown alone (never a
        // copy between images of different sizes). The size is fixed for a run, so this is a guard, not a path.
        if(const rsb::Size fs=rsb::gpuSource();fs.w!=fgW || fs.h!=fgH) {
            if(!fgSizeLogged){fgSizeLogged=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_FG refused=source-size pictures=%ux%u source=%ux%u",fgW,fgH,fs.w,fs.h);}
            fg.secondLost(tag);fgPlan=recomp::fg::Plan{};fgPrevValid=false;return;
        }
        // framegen-1b: with diag-fg-between, the generator's own objects (made once, here) and what fgGenAllows() decided for this
        // frame. False: the objects could not be made (once per run): the real frame alone. A first frame or the frame after a
        // camera cut takes the next line's path and is kept. (A frame without usable inputs does not get here: see above.)
        if(!fgGenBefore(tag)){fg.secondLost(tag);fgPlan=recomp::fg::Plan{};fgPrevValid=false;return;}
        // ahead, or between without a kept previous picture (that frame runs in the ahead order and keeps one): fgAfter
        if(fgPlan.mode!=recomp::fg::Between || !fgPrevValid)return;
        // between: the made picture first, on the frame's slot and with the frame's tag; the real frame follows as the second
        if(!fgPresentMade(queue,image,true)){fg.secondLost(tag);fgPlan=recomp::fg::Plan{};fgPrevValid=false;return;}
        ++gpuPresents; // (the made picture is a present: the real frame's pass then sees its own count, RENDER_DIRECT_PRESENT logs once)
#if defined(__APPLE__)
        iosAwaitPresented(); // ios-fg-one-acquire: the made picture's present call before the real frame's image is acquired
#else
        fgStash();
#endif
        fgMadeFirst=true;
        currentTag=fgPlan.secondId; // (the pair's second picture: its own id)
    }
    // presentGpu, after the real frame's pass (`ok`: it was recorded and handed over).
    [[gnu::noinline]] void fgAfter(VkQueue queue,VkImage image,std::uint32_t tag,bool ok) noexcept {
        if(fgPlan.mode==recomp::fg::Off)return; // (always, without a diag-fg-pair-* file)
        if(fgMadeFirst) { // between: both pictures are recorded; the made one goes back in front
            fgUnstash();
            if(!ok){fg.secondLost(tag);fgGenRealLost();} // (the made picture stands alone on the slot; framegen-1b: the slot keeper counts it as a lost picture)
        } else if(ok) { // ahead: the made picture follows the real one
#if defined(__APPLE__)
            iosAwaitPresented(); // ios-fg-one-acquire: the real picture's present call before the made picture's image is acquired
#else
            fgStash();
#endif
            ++gpuPresents; // (as above; taken back when the made picture is not presented)
            currentTag=fgPlan.secondId; // (ahead: the made picture is the pair's second and carries the id)
            const bool made=fgPresentMade(queue,image,false);
            currentTag=tag;
            if(!made){--gpuPresents;fg.secondLost(tag);}
            fgUnstash();
        } else fg.secondLost(tag);
        fgPrevValid=fgKept;
    }
    // presentLoop, before the present of image i: a held pair's second picture waits here (m released) until its call should
    // begin, or until a newer real frame, another queued present or the shutdown ends the wait. i is put out of range when its swapchain was replaced.
    [[gnu::noinline]] void fgHold(std::unique_lock<std::mutex>& l,std::uint32_t& i,std::uint32_t tag) noexcept {
        if(!fgEver)return;
        std::int64_t until=0;const std::int64_t now=recomp::fg::monotonicNs();
        if(!fg.holdUntil(tag,now,&until))return;
        const int slot=i<imageSlot.size()?imageSlot[i]:-2;
        fgHolding=true;fgSuperseded=false;
        // (ends early when a newer real frame arrives, when anything else is queued behind the held picture, or at shutdown)
        const bool released=presentCv.wait_for(l,std::chrono::nanoseconds(until-now),[&]{return stopPresenter || fgSuperseded || !pendingPresents.empty();});
        fgHolding=false;IOS_PRESENTED_NOTIFY;
        fg.holdEnded(tag,recomp::fg::monotonicNs(),released && !stopPresenter);
        if(!(i<imageSlot.size() && slot>=0 && imageSlot[i]==slot))i=~0u;
    }
    // presentNow: the desired present time (base: the value without this lane), the call's end, a display time.
    [[gnu::noinline]] std::uint64_t fgDesired(std::uint32_t tag,std::uint64_t base) noexcept {
        recomp::fgb::presentBegin(tag); // fg-burst: the present call begins (one load unless diag-fg-burst is armed)
        if(!fgEver)return base;
        if(std::this_thread::get_id()!=presentThread.get_id())++fgInline; // (a present on the frame's own thread)
        return fg.desired(tag,base,recomp::fg::monotonicNs());
    }
    [[gnu::noinline]] void fgCalled(std::uint32_t tag) noexcept {
        recomp::fgb::presentEnd(tag); // fg-burst: the present call returned (one load unless diag-fg-burst is armed)
        // a second picture's deliberate two refreshes are not display lag: it leaves the v337 call ring (its own lag: RENDER_DIRECT_FG)
        if(fgEver && fg.called(tag,recomp::fg::monotonicNs())) {
            recomp::fg::secondPresents.fetch_add(1,std::memory_order_relaxed); // v572: the frame-rate counter's shown pictures
            if(callTag[tag%recomp::v337::Ring]==tag)callTag[tag%recomp::v337::Ring]=0;
        }
    }
    [[gnu::noinline]] void fgDisplayed(std::uint32_t id,std::uint64_t actual) noexcept {
        recomp::fgb::displayed(id,actual); // fg-burst: the display time (one load unless diag-fg-burst is armed)
        if(fgEver){fg.displayed(id,std::int64_t(actual));fgKeeper.shown(id,std::int64_t(actual));} // (framegen-1b: did a made picture keep its slot)
    }
    [[gnu::noinline]] void fgLog() noexcept {
        const recomp::fg::Snapshot s=fg.take();
        const auto ms=[](std::uint64_t ns,std::uint64_t n){return n?double(ns)/1e6/double(n):0.0;};
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FG frames=%llu sw=%u real=%llu pairs=%llu between=%llu ahead=%llu skip=off:%llu,cap:%llu,rate:%llu,failed:%llu,second:%llu,drain:%llu "
            "spacing=0:%llu,1:%llu,2:%llu,3:%llu,4:%llu,5:%llu first_gap=3:%llu,4:%llu,5:%llu,6:%llu,7:%llu held=%llu hold_ms=%.2f overrun=%llu dpt_late=%llu "
            "lag_first_ms=%.2f lag_second_ms=%.2f made=%llu made_call_ms=%.3f made_acquire_ms=%.3f set_allocs=%llu pool_resets=%llu inline_presents=%llu "
            "stash_dropped=%llu id_clash=%llu gate_run=%u cap=%u grid=%u period_us=%.2f mean_phase=%.3f failed=%u lag_ref=%.2f",
            (unsigned long long)fgFrames,s.switches,(unsigned long long)s.real,(unsigned long long)s.pairs,(unsigned long long)s.between,(unsigned long long)s.ahead,
            (unsigned long long)s.skip[recomp::fg::NotWanted],(unsigned long long)s.skip[recomp::fg::NoCap],(unsigned long long)s.skip[recomp::fg::Rate],
            (unsigned long long)s.skip[recomp::fg::Failed],(unsigned long long)s.skip[recomp::fg::SecondLost],(unsigned long long)s.skip[recomp::fg::Drain],
            (unsigned long long)s.spacing[0],(unsigned long long)s.spacing[1],(unsigned long long)s.spacing[2],(unsigned long long)s.spacing[3],
            (unsigned long long)s.spacing[4],(unsigned long long)s.spacing[5],(unsigned long long)s.realGap[3],(unsigned long long)s.realGap[4],
            (unsigned long long)s.realGap[5],(unsigned long long)s.realGap[6],(unsigned long long)s.realGap[7],(unsigned long long)s.held,ms(s.holdNs,s.held),
            (unsigned long long)s.overrun,(unsigned long long)s.dptLate,ms(s.lagFirstNs,s.lagFirstN),ms(s.lagSecondNs,s.lagSecondN),
            (unsigned long long)fgMade,ms(fgMadeNs,fgMade),ms(fgAcquireNs,fgMade),(unsigned long long)fgSetAllocs,(unsigned long long)fgPoolResets,
            (unsigned long long)fgInline,(unsigned long long)fgStashDropped,(unsigned long long)s.idClash,s.gateRun,s.cap,unsigned(s.grid),s.periodNs/1e3,
            s.pairs?s.phaseSum/double(s.pairs):0.0,unsigned(fgFailed),s.lagRefresh);
        { // ios-fg-trace: the pictures recorded in the degraded mode (role:call:desired:actual, us from the pair's target)
            std::array<recomp::fg::TracePic,recomp::fg::Pairs::Trace> t;const unsigned n=fg.takeTrace(t);
            if(n) {
                char line[2048];int at=std::snprintf(line,sizeof line,"RENDER_IOS_FG_TRACE n=%u pics=",n);
                for(unsigned i=0;i<n && at<int(sizeof line)-48;++i)
                    at+=std::snprintf(line+at,sizeof line-at,"%s%c:%d:%d:%d",i?",":"",char('1'+t[i].second),t[i].callUs,t[i].desiredUs,t[i].actualUs); // (roles 1 first, 2 second, 3 lost second, 4 late second)
                RECOMP_DIRECT_LOG("%s",line);
            }
        }
        fgMade=0;fgMadeNs=0;fgAcquireNs=0;fgSetAllocs=0;fgPoolResets=0;fgInline=0;fgStashDropped=0;
#if defined(__APPLE__)
        RECOMP_DIRECT_LOG("RENDER_IOS_FG_ACQUIRE waits=%llu wait_ms=%.2f timeouts=%llu",(unsigned long long)iosWaits,
            iosWaits?double(iosWaitNs)/1e6/double(iosWaits):0.0,(unsigned long long)iosWaitTimeouts);
        iosWaits=0;iosWaitNs=0;iosWaitTimeouts=0;
#endif
        fgGenLog(); // framegen-1b: RENDER_DIRECT_FG_GEN (no line unless diag-fg-between was seen since the last one)
    }
    VkDescriptorSet setFor(VkImageView view) {
        if(view==cachedView && cachedSet)return cachedSet;
        if(setsUsed>=MaxSets){if(!waitAllMine())return VK_NULL_HANDLE;vkResetDescriptorPool(dev->device,setPool,0);setsUsed=0;}
        VkDescriptorSetAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;a.descriptorPool=setPool;a.descriptorSetCount=1;a.pSetLayouts=&setLayout;
        VkDescriptorSet set=VK_NULL_HANDLE;
        if(vkAllocateDescriptorSets(dev->device,&a,&set)!=VK_SUCCESS)return VK_NULL_HANDLE;
        ++setsUsed;
        VkDescriptorImageInfo ii{sampler,view,VK_IMAGE_LAYOUT_GENERAL};VkDescriptorBufferInfo bi{lutBuffer.buffer,0,VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet,2> w{};
        for(auto& x:w){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=set;x.descriptorCount=1;}
        w[0].dstBinding=0;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&ii;
        w[1].dstBinding=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&bi;
        vkUpdateDescriptorSets(dev->device,2,w.data(),0,nullptr);
        cachedView=view;cachedSet=set;return set;
    }
    // One displayed frame: acquire, record (pre: the CPU upload), submit behind the acquire, present; an audit frame is
    // copied back and compared with reference(). *slotOut (when given) records the acquire slot whose fence covers it.
    bool presentLocked(VkQueue queue,VkImageView view,VkImage source,bool identity,const std::function<void(VkCommandBuffer)>& pre,int* slotOut) {
        // lane fsr-stage-b: a GPU frame's source has the run's render size (1280x720 without a scale: `native` false and every
        // line marked fsr-stage-b below is v463's expression). CPU frames (identity) are always 1280x720.
        const rsb::Size src=identity?rsb::Size{}:rsb::gpuSource();
        const bool native=!rsb::isDefault(src);
        if(native && !ensureDown())return false; // (before the acquire: after it a frame must be drawn)
        if(native)auditRoom(src);
        if(!ensureSwapchain())return false;
        // v259: presents the present thread has not taken yet go first (this thread holds m, which the present thread needs:
        // an acquire waiting for one of those images would otherwise wait for its timeout).
        while(!pendingPresents.empty()){const auto j=pendingPresents.front();pendingPresents.pop_front();const auto t=popTag();if(j<renderDone.size())presentNow(j,t);}
        if(!swapchain)return false;
        const unsigned k=nextAcquire;
        if(!waitAcquireSlot(k))return false;
        auto& a=acquires[k];
        std::uint32_t i=0;
        const auto t0=std::chrono::steady_clock::now();
        const auto r=acquireImage(dev->device,swapchain,AcquireTimeout,a.sem,VK_NULL_HANDLE,&i);
        const auto acquiredNs=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());
        acquireNs+=acquiredNs;
        if(acquiredNs>100'000'000ull) // ios-gpu-hang: an acquire that waited on the drawable pool (c291: a 993 ms frame before the GPU timeouts)
            RECOMP_DIRECT_LOG("RENDER_IOS_SLOW_ACQUIRE ms=%.1f result=%d image=%u pending=%zu",double(acquiredNs)/1e6,int(r),i,pendingPresents.size());
        if(r==VK_ERROR_OUT_OF_DATE_KHR){recreate=true;++outOfDate;return false;}
        if(r==VK_ERROR_SURFACE_LOST_KHR){lost=true;++surfaceLost;return false;}
        if(r==VK_TIMEOUT || r==VK_NOT_READY){++timeouts;return false;}
        // v256: SUBOPTIMAL is expected: the identity transform on a rotated panel (as the app presents) is suboptimal on
        // every frame (v255 recreated the swapchain per frame for it: ~6-10 ms per present). The extent is checked below.
        if(r==VK_SUBOPTIMAL_KHR)++suboptimal;
        else if(r!=VK_SUCCESS){broken=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT broken=acquire %d",int(r));return false;}
        if(i>=images.size()){broken=true;return false;}
        // (from here a failure leaves the acquire semaphore signaled: the presenter stops)
        if(imageSlot[i]>=0 && !waitAcquireSlot(unsigned(imageSlot[i]))){broken=true;return false;}
        VkDescriptorSet set=setFor(view); // (lane fsr-stage-b: not const, see `down`)
        if(!set){broken=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT broken=descriptor");return false;}
        pollFilter(); // v409
        const bool audit=auditable && !auditBlocked && wantAudit(identity); // (lane fsr-stage-b: auditBlocked, see auditRoom; false without a scale)
        if(audit)auditFilter=filter;
#if defined(__APPLE__)
        if(audit)recompIosAuditLayoutGen=recomp_ios_layout_gen.load(std::memory_order_acquire); // (build 192, see checkAudit)
#endif
        // (Claude 2026-10-17, ios-gpu-ts) GPU timestamps on one present in 16: in MoltenVK any timestamp in a command buffer adds a
        // stage-counter fence wait (c133/c134: the renderer's per-frame timestamps cost 1-5 ms of display)
        static unsigned timedTick=0;const bool timed=timePool && !audit && k<MaxTimed && (timedTick++&15u)==0;
        // latency-path motion probe: armed, tagged, not an audit frame (the audit owns those transitions), a TRANSFER_SRC swapchain
        const bool band=motionOn && auditable && !audit && currentTag && recomp::diag::timeline::armed.load(std::memory_order_relaxed) &&
            extent.height>=BandRows && ensureBand(k);
        const auto l=*layout(extent.width,extent.height);
        // v450: FSR 1 (EASU + RCAS: v410's picture of image i, then its own EASU picture; EASU only: v410's picture) unless it
        // failed or lacks its objects (then the other modes below, as without the switch)
        // v460: Snapdragon GSR 1 (one pass from v410's picture of image i through the linear sampler) wins over FSR while its
        // switch exists, unless it failed or lacks its objects (then FSR / the other modes, as without the switch). On a GSR
        // frame fsr is 0 through FSR's own checks and gsr1::Mode after them. gsrWant false: fsr = v458's expression.
        const unsigned gsrVariant=gsrEdge?1u:0u;
        // lane fsr-stage-b (present_source.h; all false / v463's values for a 1280x720 source). one: the picture already has the
        // image rect's size, so it is copied 1:1 and no scaler runs (their objects are not made for such a frame); bigTry: the
        // frame's FSR / GSR pass, if it has one, reads the render-size map picture.
        bool one=rsb::wantsOne(src,l.imageWidth,l.imageHeight,bigFailed,oneFailed);
        if(one && !ensureBig(i,src,false))one=false; // (bigFailed is set: this frame and the later ones take the down path)
        const bool bigTry=rsb::wantsBig(src,l.imageWidth,l.imageHeight,bigFailed,one);
        const bool gsrAsk=gsrWant && !one;const unsigned fsrAsk=one?0u:fsrWant; // (not the members: fsrW is the EASU picture's width)
        if(gsrAsk && !gsrFailed && !ensureGsr(i,gsrVariant,bigTry)){gsrFailed=true;presentedGsr.store(0,std::memory_order_relaxed);filterPolls=0; // (the next present polls: the presented* flags then name the scaler that took over)
            RECOMP_DIRECT_LOG("RENDER_DIRECT_GSR failed=objects");}
        unsigned fsr=gsr1::frameScaler(gsrAsk,gsrFailed,fsrAsk,fsrFailed); // (v458: fsrWant && !fsrFailed?fsrWant:0u)
        bool gsr=fsr==gsr1::Mode;
        if(gsr)fsr=0;
        if(fsr && !ensureFsrPipelines()) {
            fsrFailed=true;fsr=0;presentedFsr.store(0,std::memory_order_relaxed);RECOMP_DIRECT_LOG("RENDER_DIRECT_FSR failed=pipelines");
        }
        if(fsr && !bigTry && !ensureMap(i)){fsrFailed=true;fsr=0;presentedFsr.store(0,std::memory_order_relaxed);RECOMP_DIRECT_LOG("RENDER_DIRECT_FSR failed=map");}
        if(fsr==1 && !ensureFsr(i,l.imageWidth,l.imageHeight))fsr=0; // (ensureFsr sets fsrFailed)
        // lane fsr-stage-b: the render-size picture of image i for this frame's scaler. If it cannot be made, this one frame is
        // shown without FSR / GSR through the down path (their 1280x720 pictures were not made for it); from the next frame on
        // bigFailed sends every frame down the down path with its scaler on the 1280x720 copy.
        bool big=false;
        if(bigTry && (fsr || gsr)){big=ensureBig(i,src,gsr);if(!big){fsr=0;gsr=false;}}
        const bool down=native && !one && !big;
        const rsb::Size scalerSrc=rsb::scalerSource(src,big); // (what FSR's and GSR's constants are computed for)
        const VkImage nativeImage=source;
        if(down){view=downImage.view;source=downImage.image;set=downSet;} // v463's passes below read the 1280x720 copy (recordDown fills it)
        if(fsr && (fsrCon.width!=l.imageWidth || fsrCon.height!=l.imageHeight || fsrCon.stops!=fsrStops || fsrCon.srcWidth!=scalerSrc.w || fsrCon.srcHeight!=scalerSrc.h))
            fsrCon=fsr1::constants(l.imageWidth,l.imageHeight,fsrStops,scalerSrc.w,scalerSrc.h); // (AMD's FsrEasuCon / FsrRcasCon, fsr1_cpu.h)
        if(gsr) { // v460: from here a GSR frame is `fsr == gsr1::Mode` (no fast / compute mode, v410's map pass, pass slot 9)
            fsr=gsr1::Mode;++gsrFrames;
            if(gsrCon.width!=l.imageWidth || gsrCon.height!=l.imageHeight || gsrCon.threshold255!=gsrThreshold || gsrCon.sharpness!=gsrSharpness ||
               gsrCon.edgeDirection!=gsrEdge || gsrCon.srcWidth!=scalerSrc.w || gsrCon.srcHeight!=scalerSrc.h)
                gsrCon=gsr1::constants(l.imageWidth,l.imageHeight,gsrThreshold,gsrSharpness,gsrEdge,scalerSrc.w,scalerSrc.h);
        }
        // v42x: a fast cubic mode (1 gather: v410's picture; 2 packed: its own picture) unless it failed or lacks its objects
        unsigned fast=!fsr && !one && filter==Filter::Cubic && fastWant && !(fastFailed&fastWant)?fastWant:0u;
        if(fast==1 && (!mapFastPipeline || !gatherPipeline))fast=0;
        if(fast==2 && !ensurePack(i))fast=0;
        // v411: the compute path (its sets before recording: scaleSetFor may wait for the slots); else v410's map pass
        const bool tryCompute=!fsr && !one && filter!=Filter::Nearest && computeOn && !fast && ensureScale(i,l.imageWidth,l.imageHeight);
        const VkDescriptorSet scaleSet=tryCompute?scaleSetFor(view,i):VK_NULL_HANDLE;
        const bool useCompute=scaleSet!=VK_NULL_HANDLE;
        const bool useMap=(fsr || filter!=Filter::Nearest) && !one && !big && !useCompute && fast!=2 && ensureMap(i); // v410 (v42x: also mode 4's picture; v450: FSR's input)
        if(fast==1 && !useMap)fast=0; // (v409's single pass)
        if(audit){auditCompute=useCompute;auditFast=fast;auditFsr=fsr;if(fsr)auditFsrCon=fsrCon;}
        if(audit && gsr)auditGsrCon=gsrCon; // v460
        if(audit){auditOne=one;const rsb::Size as=rsb::auditSource(src,one,big);auditW=as.w;auditH=as.h;} // lane fsr-stage-b (false, 1280x720 without a scale)
        if(fsr && !gsr)++fsrFrames; // (v460: a GSR frame is not an FSR frame)
        // v450: the map pass of an FSR frame uses the fast map pipeline (mode 5: the same bytes as v410's map pass) when it exists
        const bool fastMap=fast || (fsr && mapFastPipeline);
        const bool fsrMid=timed && fsrMidPool!=VK_NULL_HANDLE && fsr==1; // v450: EASU's end is timed
        // v42x: the reference's own fp32 scale for the fast shaders (push constant words 6 and 7; the others ignore them)
        const float scale[2]{float(SourceWidth)/float(l.imageWidth),float(SourceHeight)/float(l.imageHeight)};
        std::int32_t scaleBits[2]{};std::memcpy(scaleBits,scale,sizeof scaleBits);
        const bool mid=timed && midPool!=VK_NULL_HANDLE && (useMap || fast==2 || big || one); // v42x: the map pass's end is timed (lane fsr-stage-b: the render-size one too)
#ifdef RECOMP_THOR_EDITION
        // hud2 (v575): a real GPU frame (no `pre`: not a frame-generation picture, not a CPU frame), not an audit frame: the second
        // screen's HUD image (timeout 0; none free = no HUD picture this frame). hud3 (v585): drawn from the game's radar target
        // and the player's state, with the display tables (lutBuffer) of this presenter; the main swapchain image is not read.
        const hud2::Frame hud=!pre && !audit && hud2::screen().wanted()?hud2::screen().prepare(dev->instance,dev->physical,dev->device,
            dev->queueFamily,lutBuffer.buffer,[](void* p){return static_cast<Presenter*>(p)->waitAllMine();},this):hud2::Frame{};
#endif /* RECOMP_THOR_EDITION */
        const VkCommandBuffer cb=commands[i];
        VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        bool ok=vkResetCommandBuffer(cb,0)==VK_SUCCESS && vkBeginCommandBuffer(cb,&bi)==VK_SUCCESS;
        if(ok) {
            if(pre)pre(cb);
            if(timed){vkCmdResetQueryPool(cb,timePool,2*k,2);vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timePool,2*k);}
            if(mid)vkCmdResetQueryPool(cb,midPool,k,1); // v42x
            if(fsrMid)vkCmdResetQueryPool(cb,fsrMidPool,k,1); // v450
            { // ios-gpu-burden-present (Claude 2026-10-21; MEASUREMENT ONLY): files/private-trace/diag-gpu-burden-present (access(), so the
              // iOS A/B switches drive it) records 6 copies of an 8 MB scratch buffer into every present's command buffer. Question: is
              // the presenter's / frame generator's GPU time (~3 ms per real frame) on the frame's critical path, as the game's own
              // chunks are (c419), or off it like the system compositor's (c429/c430)? Both buffers are ours: the picture is unchanged.
                static unsigned polls=0;static bool on=false;
                if(!(polls++%60))on=recomp_ios_switches_access((std::string(Switches)+"diag-gpu-burden-present").c_str(),F_OK)==0;
                static Buffer bsrc,bdst;static bool failed=false;
                if(on && !failed && !bsrc.buffer && !(makeBuffer(8u<<20,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,bsrc) &&
                   makeBuffer(8u<<20,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,bdst)))failed=true;
                if(on && !failed && bsrc.buffer && bdst.buffer){const VkBufferCopy rg{0,0,8u<<20};for(int k=0;k<6;++k)vkCmdCopyBuffer(cb,bsrc.buffer,bdst.buffer,1,&rg);}
                static unsigned long long logged=0;if(on && !(logged++%600))RECOMP_DIRECT_LOG("RENDER_DIRECT_GPU_BURDEN_PRESENT on=1 failed=%u",unsigned(failed));
            }
            { // ios-gpu-burden-side (Claude 2026-10-21; MEASUREMENT ONLY): with RECOMP_IOS_SIDE_QUEUE and files/private-trace/
              // diag-gpu-burden-side, the same 6 x 8 MB copies per present as diag-gpu-burden-present, but submitted to the side queue
              // (another Metal command queue) with no dependency on the frame. Overlaps the game (frames unchanged, like the system
              // compositor) or not (late rises, as c437)? Decides whether moving the generator/presenter to a second queue can pay.
                static unsigned polls=0;static bool on=false;
                if(!(polls++%60))on=recomp_ios_switches_access((std::string(Switches)+"diag-gpu-burden-side").c_str(),F_OK)==0;
                static const auto side=probeSideQueue(dev->device);
                static VkCommandPool pool=VK_NULL_HANDLE;static VkCommandBuffer cbs[3]{};static VkFence fences[3]{};static Buffer s0,s1;static bool ready=false,failed=false;
                static unsigned long long submitted=0,skipped=0;
                if(on && side.first && !ready && !failed) {
                    VkCommandPoolCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pi.queueFamilyIndex=side.second;
                    bool ok=vkCreateCommandPool(dev->device,&pi,nullptr,&pool)==VK_SUCCESS;
                    VkCommandBufferAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=3;
                    ok=ok && vkAllocateCommandBuffers(dev->device,&ai,cbs)==VK_SUCCESS;
                    ok=ok && makeBuffer(8u<<20,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,s0) && makeBuffer(8u<<20,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,s1);
                    for(int k=0;ok && k<3;++k) {
                        VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
                        ok=vkCreateFence(dev->device,&fi,nullptr,&fences[k])==VK_SUCCESS;
                        VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                        ok=ok && vkBeginCommandBuffer(cbs[k],&bi)==VK_SUCCESS;
                        const VkBufferCopy rg{0,0,8u<<20};for(int j=0;ok && j<6;++j)vkCmdCopyBuffer(cbs[k],s0.buffer,s1.buffer,1,&rg);
                        ok=ok && vkEndCommandBuffer(cbs[k])==VK_SUCCESS;
                    }
                    ready=ok;failed=!ok;
                    RECOMP_DIRECT_LOG("RENDER_DIRECT_GPU_BURDEN_SIDE ready=%u family=%u",unsigned(ok),side.second);
                }
                if(on && ready) {
                    int k=-1;for(int j=0;j<3;++j)if(vkGetFenceStatus(dev->device,fences[j])==VK_SUCCESS){k=j;break;}
                    if(k<0)++skipped;
                    else {vkResetFences(dev->device,1,&fences[k]);VkSubmitInfo si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;si.commandBufferCount=1;si.pCommandBuffers=&cbs[k];
                          if(vkQueueSubmit(side.first,1,&si,fences[k])==VK_SUCCESS)++submitted;}
                    if(!((submitted+skipped)%600))RECOMP_DIRECT_LOG("RENDER_DIRECT_GPU_BURDEN_SIDE on=1 submitted=%llu skipped=%llu",submitted,skipped);
                }
            }
            if(down)recordDown(cb,nativeImage,src); // lane fsr-stage-b: the render-size picture into the 1280x720 copy (inside the timed span)
            if(useCompute) { // v411: the source (written by earlier commands, any stage) -> the picture -> the swapchain pass
                VkMemoryBarrier g{};g.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;g.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;g.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                VkImageMemoryBarrier ib{};ib.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;ib.image=scaleImages[i].image;
                ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;ib.srcAccessMask=0;ib.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&g,0,nullptr,1,&ib);
                vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,scalePipeline);
                vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,scalePipelineLayout,0,1,&scaleSet,0,nullptr);
                const std::int32_t cpush[4]{std::int32_t(l.imageWidth),std::int32_t(l.imageHeight),identity?768:0,std::int32_t(filter)};
                vkCmdPushConstants(cb,scalePipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof cpush,cpush);
                vkCmdDispatch(cb,(l.imageWidth+15)/16,(l.imageHeight+15)/16,1);
                ib.oldLayout=ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;ib.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;ib.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&ib);
            }
            if(useMap || fast==2) { // v410: the map pass (mode 0 at 1:1 = the tables per texel); v42x: the fast modes' map pass (mode 5)
                VkRenderPassBeginInfo mp{};mp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;mp.renderPass=mapPass;
                mp.framebuffer=fast==2?packFramebuffers[i]:mapFramebuffers[i];
                mp.renderArea={{0,0},{SourceWidth,SourceHeight}};
                vkCmdBeginRenderPass(cb,&mp,VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,fastMap?mapFastPipeline:pipeline);
                const VkViewport mv{0,0,float(SourceWidth),float(SourceHeight),0,1};const VkRect2D ms{{0,0},{SourceWidth,SourceHeight}};
                vkCmdSetViewport(cb,0,1,&mv);vkCmdSetScissor(cb,0,1,&ms);
                vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&set,0,nullptr);
                const std::int32_t mpush[8]{0,0,std::int32_t(SourceWidth),std::int32_t(SourceHeight),identity?768:0,fastMap?5:0,scaleBits[0],scaleBits[1]};
                vkCmdPushConstants(cb,pipelineLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof mpush,mpush);
                vkCmdDraw(cb,3,1,0,0);
                vkCmdEndRenderPass(cb);
                if(mid)vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,midPool,k); // v42x
            }
            if(big || one) { // lane fsr-stage-b: the tables at 1:1 into image i's render-size picture
                recordBigMap(cb,i,set,src,identity);
                if(mid)vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,midPool,k);
            }
            if(fsr==1) { // v450: EASU of v410's picture into image i's own picture (the image rect's size; RCAS reads it next)
                VkRenderPassBeginInfo ep{};ep.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;ep.renderPass=fsrPass;ep.framebuffer=fsrFramebuffers[i];
                ep.renderArea={{0,0},{l.imageWidth,l.imageHeight}};
                vkCmdBeginRenderPass(cb,&ep,VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,easuPipeline);
                const VkViewport ev{0,0,float(l.imageWidth),float(l.imageHeight),0,1};const VkRect2D es{{0,0},{l.imageWidth,l.imageHeight}};
                vkCmdSetViewport(cb,0,1,&ev);vkCmdSetScissor(cb,0,1,&es);
                vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,fsrLayout,0,1,big?&bigSets[i]:&mapSets[i],0,nullptr); // (lane fsr-stage-b: the render-size picture)
                std::uint32_t epush[20]{0,0,l.imageWidth,l.imageHeight};std::memcpy(epush+4,fsrCon.easu,sizeof fsrCon.easu);
                vkCmdPushConstants(cb,fsrLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof epush,epush);
                vkCmdDraw(cb,3,1,0,0);
                vkCmdEndRenderPass(cb);
                if(fsrMid)vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,fsrMidPool,k);
            }
            VkRenderPassBeginInfo rp{};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;rp.renderPass=pass;rp.framebuffer=framebuffers[i];
            rp.renderArea={{0,0},extent};
            vkCmdBeginRenderPass(cb,&rp,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,gsr?gsrPipeline[gsrVariant]: // (v460: GSR's own)
                fsr==1?rcasPipeline:fsr==2?easuDirectPipeline: // (v450: FSR's own)
                fast==1?gatherPipeline:fast==2?packedPipeline:pipeline); // (v42x: the fast modes' own)
            const VkViewport vp{0,0,float(extent.width),float(extent.height),0,1};const VkRect2D sc{{0,0},extent};
            vkCmdSetViewport(cb,0,1,&vp);vkCmdSetScissor(cb,0,1,&sc);
            if(gsr) { // v460: GSR of v410's picture (linear sampler) inside the rect (black outside, alpha 1); push = rect, viewport, params
                vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,fsrLayout,0,1,big?&bigGsrSets[i]:&gsrSets[i],0,nullptr); // (lane fsr-stage-b)
                std::uint32_t gpush[12]{l.left,l.top,l.imageWidth,l.imageHeight};
                std::memcpy(gpush+4,gsrCon.viewport,sizeof gsrCon.viewport);std::memcpy(gpush+8,gsrCon.params,sizeof gsrCon.params);
                vkCmdPushConstants(cb,fsrLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof gpush,gpush);
            } else if(fsr) { // v450: RCAS of image i's EASU picture, or EASU of v410's picture, inside the rect (black outside, alpha 1)
                vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,fsrLayout,0,1,fsr==1?&fsrSets[i]:big?&bigSets[i]:&mapSets[i],0,nullptr); // (lane fsr-stage-b)
                std::uint32_t fpush[20]{l.left,l.top,l.imageWidth,l.imageHeight};
                if(fsr==1)std::memcpy(fpush+4,fsrCon.rcas,sizeof fsrCon.rcas);else std::memcpy(fpush+4,fsrCon.easu,sizeof fsrCon.easu);
                vkCmdPushConstants(cb,fsrLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof fpush,fpush);
            } else {
                vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,
                    one?&bigSets[i]:fast==2?&packSets[i]:useCompute?&scaleCopySets[i]:useMap?&mapSets[i]:&set,0,nullptr);
                // (v42x: mode 4 gather, 6 / 7 packed from an RGBA8 / BGRA8 picture; words 6-7 the scale)
                // (lane fsr-stage-b: `one` = mode 3, the 1:1 copy of a rect-sized, already mapped picture: here the render-size map picture)
                const std::int32_t push[8]{std::int32_t(l.left),std::int32_t(l.top),std::int32_t(l.imageWidth),std::int32_t(l.imageHeight),
                    one || useMap || fast==2?1536:identity?768:0,one?3:fast==1?4:fast==2?(bgra?7:6):useCompute?3:std::int32_t(filter),scaleBits[0],scaleBits[1]};
                vkCmdPushConstants(cb,pipelineLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,push);
            }
            vkCmdDraw(cb,3,1,0,0);
            vkCmdEndRenderPass(cb);
            if(timed)vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timePool,2*k+1);
            if(audit)ok=recordAudit(cb,images[i],source);
            if(audit && fsr==1)ok=ok && recordFsrAudit(cb,fsrImages[i].image,l.imageWidth,l.imageHeight); // v450: + EASU's picture
            if(band)recordBand(cb,images[i],k); // latency-path motion probe
#ifdef RECOMP_THOR_EDITION
            if(hud.on)hud2::screen().record(cb,hud); // hud3 (v585): the HUD render pass into the second screen's image
            if(hud2::screen().hideWanted())hud2::screen().recordHide(cb,framebuffers[i],format,l); // hud2 (v576, hide=1): after every copy of this picture
#endif /* RECOMP_THOR_EDITION */
            // Later commands in submission order (the next frame's writes of the source) wait for this pass.
            VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,nullptr,0,nullptr);
            ok=ok && vkEndCommandBuffer(cb)==VK_SUCCESS;
        }
#ifndef RECOMP_THOR_EDITION
        if(!ok){broken=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT broken=record");return false;}
        const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;si.waitSemaphoreCount=1;si.pWaitSemaphores=&a.sem;si.pWaitDstStageMask=&waitStage;
        si.commandBufferCount=1;si.pCommandBuffers=&cb;si.signalSemaphoreCount=1;si.pSignalSemaphores=&renderDone[i];
#else /* RECOMP_THOR_EDITION */
        if(!ok){broken=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT broken=record");if(hud.on)hud2::screen().submitted(hud,false,queue,VK_NULL_HANDLE,nullptr);return false;}
        // hud2 (v575): with a HUD image, the submit also waits for its acquire and signals its present semaphore (hud3 v585: the
        // acquire is waited at COLOR_ATTACHMENT_OUTPUT, where the HUD render pass first touches the image; v575 copied: TRANSFER)
        const VkPipelineStageFlags waitStages[2]{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        const VkSemaphore waits[2]{a.sem,hud.acquired},signals[2]{renderDone[i],hud.done};
        VkSubmitInfo si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;si.waitSemaphoreCount=hud.on?2:1;si.pWaitSemaphores=waits;si.pWaitDstStageMask=waitStages;
        si.commandBufferCount=1;si.pCommandBuffers=&cb;si.signalSemaphoreCount=hud.on?2:1;si.pSignalSemaphores=signals;
#endif /* RECOMP_THOR_EDITION */
        vkResetFences(dev->device,1,&a.fence);
        VkResult sr;
        {GenericSubmissionScope generic;sr=lockedQueueSubmit(queue,1,&si,a.fence);} // (not a native ticket, see vulkan_queue_lock.h)
#ifdef RECOMP_THOR_EDITION
#if defined(RECOMP_DUO_EDITION)
        // ios-duo (2026-10-24, run claude-duo-01: SIGSEGV in MVKPresentableSwapchainImage::getCAMetalDrawable while encoding a render
        // pass, after ~1,000 lower-screen presents): MoltenVK encodes each VkQueue's submissions on its own dispatch thread, so a lower-
        // screen present on presentQueue could release an image's drawable while `queue`'s thread still encoded a pass on that image
        // (Android encodes in vkQueueSubmit). The lower screen presents on the queue that draws it (sharedQueueMutex). Runs 02-05: no crash.
        if(hud.on)hud2::screen().submitted(hud,sr==VK_SUCCESS,queue,queue,nullptr);
#else
        if(hud.on)hud2::screen().submitted(hud,sr==VK_SUCCESS,queue,presentQueue?presentQueue:queue,presentQueue?&presentQueueMutex:nullptr); // hud2 (v575)
#endif
#endif /* RECOMP_THOR_EDITION */
        if(sr!=VK_SUCCESS){broken=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT broken=submit %d",int(sr));return false;}
        a.submitted=true;imageSlot[i]=int(k);if(slotOut)*slotOut=int(k);
        if(timed){slotTimed[k]=std::uint8_t((one?rsb::OneSlot:passSlot(unsigned(filter),useCompute,fast,fsr))+1u);slotMid[k]=std::uint8_t(mid); // v409 (v412: passSlot; v42x: + fast, mid)
            slotFsrMid[k]=std::uint8_t(fsrMid);} // (v450: + fsr, EASU's end)
        if(currentTag && recomp::diag::timeline::armed.load(std::memory_order_relaxed)) { // latency-path (lat2, measurement only)
            recomp::diag::timeline::mark(recomp::diag::timeline::PresenterSubmitted,0,currentTag);
            if(gpuDoneOn)watchGpuDone(k,currentTag,timed,band?static_cast<const std::uint8_t*>(bands[k].mapped):nullptr);
        }
        nextAcquire=(k+1)%unsigned(acquires.size());
        lastQueue=queue;
        bool shown=true;
        if(presentThread.joinable() && !stopPresenter){pendingPresents.push_back(i);pendingTags.push_back(currentTag);presentCv.notify_one();} // v259: presented there
        else shown=presentNow(i,currentTag);
        if(audit)checkAudit(k,identity);
        const auto total=cpuPresents+gpuPresents+1;
        if(!(total%256) || total<=2)
            RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT presents=%llu gpu=%llu cpu=%llu gpu_dropped=%llu cpu_dropped=%llu timeouts=%llu out_of_date=%llu "
                "suboptimal=%llu surface_lost=%llu recreated=%llu extent_changes=%llu present_calls=%llu audits=%llu audit_failures=%llu acquire_ms=%.3f present_ms=%.3f gpu_call_ms=%.3f "
                "cpu_call_ms=%.3f extent=%ux%u filter=%u compute=%u fast=%u filter_changes=%llu pass_us=%.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f "
                "pass_n=%llu/%llu/%llu/%llu/%llu/%llu/%llu map_us=%.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f",(unsigned long long)total,(unsigned long long)gpuPresents,(unsigned long long)cpuPresents,
                (unsigned long long)gpuDropped,(unsigned long long)cpuDropped,(unsigned long long)timeouts,(unsigned long long)outOfDate,
                (unsigned long long)suboptimal,(unsigned long long)surfaceLost,(unsigned long long)recreated,(unsigned long long)extentChanges,
                (unsigned long long)presentCalls,(unsigned long long)audits,
                (unsigned long long)auditFailures,acquireNs/1e6/double(total),presentCalls?presentNs/1e6/double(presentCalls):0.0,
                gpuPresents?gpuNs/1e6/double(gpuPresents):0.0,cpuPresents?cpuNs/1e6/double(cpuPresents):0.0,extent.width,extent.height,
                unsigned(filter),unsigned(computeOn),fastWant && !(fastFailed&fastWant)?fastWant:0u,(unsigned long long)filterChanges,
                passMean(0),passMean(1),passMean(2),passMean(3),passMean(4),passMean(5),passMean(6),
                (unsigned long long)passN[0],(unsigned long long)passN[1],(unsigned long long)passN[2],(unsigned long long)passN[3],(unsigned long long)passN[4],
                (unsigned long long)passN[5],(unsigned long long)passN[6],mapMean(0),mapMean(1),mapMean(2),mapMean(3),mapMean(4),mapMean(5),mapMean(6));
        if((!(total%256) || total<=2) && (fsrWant || fsrFrames || passN[7] || passN[8])) // v450 (its own line: RENDER_DIRECT_PRESENT keeps its format)
            RECOMP_DIRECT_LOG("RENDER_DIRECT_FSR presents=%llu want=%u failed=%u stops=%.3f frames=%llu pictures=%zu size=%ux%u pass_us=%.1f/%.1f pass_n=%llu/%llu "
                "map_us=%.1f/%.1f easu_end_us=%.1f n_easu=%llu",(unsigned long long)total,fsrWant,unsigned(fsrFailed),double(fsrStops),
                (unsigned long long)fsrFrames,fsrImages.size(),fsrW,fsrH,passMean(7),passMean(8),(unsigned long long)passN[7],(unsigned long long)passN[8],
                mapMean(7),mapMean(8),easuN[7]?easuNs[7]/1e3/double(easuN[7]):0.0,(unsigned long long)easuN[7]);
        if((!(total%256) || total<=2) && (gsrWant || gsrFrames)) // v460 (its own line; pass_us = the whole presenter pass of GSR frames, slot 9)
            RECOMP_DIRECT_LOG("RENDER_DIRECT_GSR presents=%llu want=%u failed=%u edge_direction=%u threshold=%.3f sharpness=%.3f frames=%llu pass_us=%.1f "
                "pass_n=%llu map_us=%.1f",(unsigned long long)total,unsigned(gsrWant),unsigned(gsrFailed),unsigned(gsrEdge),double(gsrThreshold),
                double(gsrSharpness),(unsigned long long)gsrFrames,passMean(9),(unsigned long long)passN[9],mapMean(9));
        if(native) { // lane fsr-stage-b: its own line, only in a run with a render scale
            const rsb::Path p=rsb::path(src,one,big);++rsbFrames[unsigned(p)];
            if(rsbLogged!=src || !(total%256))rsbLog(total,src,l,p,fsr);
        }
        if(!(total%256)){passNs.fill(0);passN.fill(0);mapNs.fill(0);mapN.fill(0);easuNs.fill(0);easuN.fill(0);} // (pass_us: the last 256 presents)
        return shown;
    }
    double passMean(unsigned m) const noexcept {return passN[m]?passNs[m]/1e3/double(passN[m]):0.0;} // v411 (us)
    double mapMean(unsigned m) const noexcept {return mapN[m]?mapNs[m]/1e3/double(mapN[m]):0.0;} // v42x (us)
    void pollFilter() { // v409
        if(filterPolls++%120)return;
        const std::string sw(Switches);
        // lane fsr-sharpness: the decision is settings::poll (scaler_settings.h, host-tested). With no files/settings/scaler.txt and
        // no files/settings/fsr-sharpness.txt it asks the same switch files in the same order as v458 did here and returns v458's
        // want / wantCompute / fast / fsr / stops; scaler.txt (fsr1 | cubic | nearest | gsr1) replaces the three scaler-choosing
        // switches, fsr-sharpness.txt (0 .. 4 stops) replaces diag-v450-fsr1-stops.
        const settings::Poll sp=settings::poll(settings::State{filterFailed,fsrFailed},
            [&](const char* name){
#if defined(__APPLE__)
                // ios-scale-1to1 (Claude, 2026-10-16): the swapchain is the game's own 1280x720 on iOS, so an upscaler only sharpens
                // (RCAS) and costs GPU time (~1-2 ms/frame of FSR1 passes): at 1:1 the scaler switch files count as absent, i.e.
                // the plain copy (the game's exact pixels). A files/settings/scaler.txt word still decides (settings::poll reads it
                // first); files/private-trace/ios-scale-keep restores the switch files' choice (A/B).
                { // ios-screen-native (Claude 2026-10-21; MEASUREMENT ONLY, RECOMP_IOS_SCREEN_FIT=native): the plain bilinear scale into the
                  // native-size swapchain (c426: FSR cost ~4.9 ms per present there); the scaler switch files count as absent, linear present.
                    static const bool native=[]{const char* e=std::getenv("RECOMP_IOS_SCREEN_FIT");return e && (!std::strcmp(e,"native") || !std::strcmp(e,"native1"));}();
                    if(native) {
                        if(!std::strcmp(name,"diag-v409-scale-linear"))return true;
                        if(!std::strcmp(name,"diag-v450-scale-fsr1") || !std::strcmp(name,"diag-v409-scale-cubic") ||
                           !std::strcmp(name,"diag-v411-scale-compute") || !std::strcmp(name,"diag-v460-scale-gsr1"))return false;
                    }
                }
                // (Claude 2026-10-21, ios-drawable-half: a swapchain of the render-scaled frame's own size is 1:1 as well)
                const unsigned rsk=unsigned(std::clamp(recomp_ios_render_sixteenths.load(std::memory_order_relaxed),1,16));
                if(((extent.width==1280 && extent.height==720) || (extent.width*16u==1280u*rsk && extent.height*16u==720u*rsk)) && (!std::strcmp(name,"diag-v450-scale-fsr1") || !std::strcmp(name,"diag-v409-scale-cubic") ||
                   !std::strcmp(name,"diag-v409-scale-linear") || !std::strcmp(name,"diag-v411-scale-compute")) &&
                   recomp_ios_switches_access((sw+"ios-scale-keep").c_str(),F_OK)!=0)return false;
#endif
                return recomp_ios_switches_access((sw+name).c_str(),F_OK)==0;},
            [&](settings::Text which,char* buf,std::size_t cap)->int {
                const std::string path=std::string(settings::SettingsDir)+(which==settings::Text::SettingsScaler?settings::ScalerFile:settings::SharpnessFile);
                FILE* f=std::fopen(path.c_str(),"r");
                if(!f)return -1;
                const std::size_t n=std::fread(buf,1,cap,f);std::fclose(f);return int(n);
            },
            [&](float& out) { // v458's reading of diag-v450-fsr1-stops, unchanged (its text; absent or unreadable: the default)
                bool ok=false;
                if(FILE* sf=std::fopen((sw+"diag-v450-fsr1-stops").c_str(),"r")) {
                    float v=0;if(std::fscanf(sf,"%f",&v)==1 && v>=0.0f && v<=4.0f){out=v;ok=true;}
                    std::fclose(sf);
                }
                return ok;
            });
        const Filter want=sp.want;const bool wantCompute=sp.wantCompute;const unsigned fast=sp.fast,fsr=sp.fsr;const float stops=sp.stops;
        const unsigned ignored=(sp.scalerIgnored?1u:0u)|(sp.stopsIgnored?2u:0u); // a setting file is present but not valid
#if defined(__APPLE__)
        // (2026-10-21) the drawable-half default stands aside while the player's scaler.txt picks an upscaler (Main.mm)
        recomp_ios_settings_upscaler.store((sp.scaler==settings::Scaler::Cubic || sp.scaler==settings::Scaler::Fsr1 || sp.scaler==settings::Scaler::Gsr1)?1:0,std::memory_order_relaxed);
#endif
        if(sp.scaler!=settingsScaler || sp.stopsFromSettings!=settingsStops || ignored!=settingsIgnored) { // (never true while both files stay absent)
            settingsScaler=sp.scaler;settingsStops=sp.stopsFromSettings;settingsIgnored=ignored;
            RECOMP_DIRECT_LOG("RENDER_DIRECT_SETTINGS scaler=%s sharpness_file=%u stops=%.3f gsr1_asked=%u gsr1_built=1 scaler_ignored=%u sharpness_ignored=%u",
                settings::scalerName(sp.scaler),unsigned(sp.stopsFromSettings),double(stops),unsigned(sp.gsr1),ignored&1u,ignored>>1);
        }
        // GSR1-HOOK (merged, v461 + lane fsr-sharpness): sp.gsr1 is true when files/settings/scaler.txt says gsr1; the GSR lines
        // below take the frame then (sp.fsr / sp.want stay the fsr1 word's: FSR 1 is what runs if GSR failed its audit).
        const bool stopsChanged=fsr==1 && stops!=fsrStops;
        // A sharpness change audits a GPU frame again (v458) only when the value v458's rule gives (diagnostic file or default)
        // changed. Changes that come from fsr-sharpness.txt - a new value, or the file removed or made invalid so the rule's
        // unchanged value returns - do not: the settings screen may move it repeatedly, each audit reads two full pictures back
        // and runs the CPU reference (a visible stall), and only one push constant changes. With no fsr-sharpness.txt ruleStops
        // equals fsrStops at every poll, so stopsAudit is v458's stopsChanged. The mode's first entry is audited as before;
        // diag-direct-audit still audits on request.
        bool stopsAudit=false;
        if(!sp.stopsFromSettings){stopsAudit=fsr==1 && stops!=ruleStops;ruleStops=stops;}
        fsrWant=fsr;fsrStops=stops;
        // v460: Snapdragon GSR 1 (wins over FSR and every other mode while its switch exists; diag-v460-gsr1-edge-direction: the
        // edge-direction shader), its edge threshold (1/255 units) and sharpness from their text files (absent, unreadable or
        // out of range: Qualcomm's defaults); diag-v460-gsr1-keep: a failed audit is logged but GSR stays on
        // (merged with lane fsr-sharpness: files/settings/scaler.txt decides when it holds a valid word; the diagnostic switch otherwise)
        const bool gsr=!gsrFailed && (sp.scaler!=settings::Scaler::Unset?sp.gsr1:recomp_ios_switches_access((sw+"diag-v460-scale-gsr1").c_str(),F_OK)==0);
        const bool gsrEdgeWant=gsr && recomp_ios_switches_access((sw+"diag-v460-gsr1-edge-direction").c_str(),F_OK)==0;
        float gsrT=gsr1::DefaultThreshold255,gsrS=gsr1::DefaultSharpness;
        if(gsr) {
            const auto tuning=[&](const char* name,float lo,float hi,float fallback) {
                float v=0;bool have=false;
                if(FILE* tf=std::fopen((sw+name).c_str(),"r")){have=std::fscanf(tf,"%f",&v)==1;std::fclose(tf);}
                return gsr1::tuned(have,v,lo,hi,fallback);
            };
            gsrT=tuning("diag-v460-gsr1-threshold",gsr1::MinThreshold255,gsr1::MaxThreshold255,gsr1::DefaultThreshold255);
            gsrS=tuning("diag-v460-gsr1-sharpness",gsr1::MinSharpness,gsr1::MaxSharpness,gsr1::DefaultSharpness);
            gsrKeep=recomp_ios_switches_access((sw+"diag-v460-gsr1-keep").c_str(),F_OK)==0;
        }
        // (a tuning change audits again; gsrThreshold / gsrSharpness are those of the last poll WITH the switch, kept while it is
        // absent, so switching GSR on again with the same tuning files is not a change)
        const bool gsrChanged=gsr && (gsrT!=gsrThreshold || gsrS!=gsrSharpness);
        const bool gsrWas=gsrWant;
        gsrWant=gsr;gsrEdge=gsrEdgeWant;
        if(gsr){gsrThreshold=gsrT;gsrSharpness=gsrS;}
        presentedGsr.store(gsr?1u+unsigned(gsrEdgeWant):0u,std::memory_order_relaxed);
        // v411: the audit key is mode + 3 for the compute path (a first entry into each kind audits a GPU frame); v42x: 6 gather, 7 packed;
        // v450: 8 FSR, 9 EASU only (and a sharpness change audits again)
        // v460: 10 GSR, 11 GSR with edge direction (and a tuning change audits again). With gsr false every expression below is v458's.
        const unsigned key=gsr?10u+unsigned(gsrEdgeWant):fsr?7u+fsr:fast?5u+fast:unsigned(want)+(wantCompute && want!=Filter::Nearest?3u:0u);
        fastWant=fast;presentedFast.store(!fsr && !gsr && fast && !(fastFailed&fast)?fast:0u,std::memory_order_relaxed);
        presentedFsr.store(gsr?0u:fsr,std::memory_order_relaxed);
        if(want==filter && key==filterKey && !stopsChanged && !gsrChanged){computeOn=wantCompute;presentedCompute.store(unsigned(wantCompute && !fsr && !gsr),std::memory_order_relaxed);
            presentedFilter.store(fsr || gsr?3u:unsigned(want),std::memory_order_relaxed);return;}
        filter=want;computeOn=wantCompute;filterKey=key;++filterChanges;presentedFilter.store(fsr || gsr?3u:unsigned(want),std::memory_order_relaxed);
        presentedCompute.store(unsigned(wantCompute && !fsr && !gsr),std::memory_order_relaxed);
        const unsigned bit=1u<<key;
        const bool auditNext=(fsr || gsr || want!=Filter::Nearest) && (!(filterAudited&bit) || stopsAudit || gsrChanged);
        if(auditNext){filterAudited|=bit;gpuAuditsLeft=std::max(gpuAuditsLeft,1u);}
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FILTER mode=%u compute=%u fast=%u changes=%llu audit_next=%u fsr=%u stops=%.3f",unsigned(want),unsigned(wantCompute),fast,
            (unsigned long long)filterChanges,unsigned(auditNext),fsr,double(stops)); // (v450: fsr / stops appended)
        if(gsr || gsrWas) // v460 (its own line: RENDER_DIRECT_FILTER keeps its format)
            RECOMP_DIRECT_LOG("RENDER_DIRECT_GSR mode=%u edge_direction=%u threshold=%.3f sharpness=%.3f keep=%u audit_next=%u",unsigned(gsr),
                unsigned(gsrEdgeWant),double(gsrT),double(gsrS),unsigned(gsrKeep),unsigned(auditNext));
    }
    bool wantAudit(bool identity) {
        if(!(++auditChecks%300) && access((std::string(Switches)+"diag-direct-audit").c_str(),F_OK)==0){cpuAuditsLeft=std::max(cpuAuditsLeft,1u);
            gpuAuditsLeft=std::max(gpuAuditsLeft,1u);}
        auto& left=identity?cpuAuditsLeft:gpuAuditsLeft;
        if(!left)return false;
        --left;return true;
    }
    bool recordAudit(VkCommandBuffer cb,VkImage target,VkImage source) {
        const VkDeviceSize targetBytes=VkDeviceSize(extent.width)*extent.height*4;
        if(auditTarget.size<targetBytes){freeBuffer(auditTarget);if(!makeBuffer(targetBytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,auditTarget,true))return false;}
        if(!auditSource.buffer && !makeBuffer(SourceBytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,auditSource,true))return false;
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=target;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent={extent.width,extent.height,1};
        vkCmdCopyImageToBuffer(cb,target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,auditTarget.buffer,1,&c);
        c.imageExtent={auditW,auditH,1}; // (lane fsr-stage-b: the audited frame's source; 1280x720 unless it is a render-size frame, whose buffer auditRoom made)
        vkCmdCopyImageToBuffer(cb,source,VK_IMAGE_LAYOUT_GENERAL,auditSource.buffer,1,&c);
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=0;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkMemoryBarrier h{};h.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;h.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;h.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&h,0,nullptr,0,nullptr);
        return true;
    }
    // v450: an FSR frame's EASU picture (in SHADER_READ_ONLY_OPTIMAL after RCAS read it) -> auditMid (the next frame's EASU pass
    // starts it from UNDEFINED: no transition back).
    bool recordFsrAudit(VkCommandBuffer cb,VkImage picture,std::uint32_t w,std::uint32_t h) {
        const VkDeviceSize bytes=VkDeviceSize(w)*h*4;
        if(auditMid.size<bytes){freeBuffer(auditMid);if(!makeBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,auditMid,true))return false;}
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=picture;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent={w,h,1};
        vkCmdCopyImageToBuffer(cb,picture,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,auditMid.buffer,1,&c);
        VkMemoryBarrier hb{};hb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;hb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;hb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&hb,0,nullptr,0,nullptr);
        return true;
    }
    // v450: an FSR frame against fsr1_reference.h. EASU + RCAS: (A) the GPU's EASU picture against easuPicture() of the frame's
    // mapped source, (B) the window against RCAS of the GPU's own picture; EASU only: the window against easuPicture() in the
    // rect. Each <= 1 per colour channel, alpha and border exact (fsr1::compare; EASU's luma-tie pixels within their 2x2 clamp
    // bounds +/- 1, counted as tie_range). A failure turns FSR off for the run.
    void checkFsrAudit(bool identity) {
        const auto t0=std::chrono::steady_clock::now();
        const auto* src=static_cast<const std::uint8_t*>(auditSource.mapped);
        const auto* got=static_cast<const std::uint8_t*>(auditTarget.mapped);
        const auto* mid=static_cast<const std::uint8_t*>(auditMid.mapped);
        const fsr1::Constants& c=auditFsrCon;
        std::vector<std::uint8_t> mapped,window;fsr1::Expected picture,easuWindow;
        bool built=src && got;
        fsr1::Diff a{},b{};
        if(built){fsr1::mappedPicture(src,identity?identityLut():gameLut,mapped,std::size_t{c.srcWidth}*c.srcHeight);fsr1::easuPicture(mapped.data(),c,picture);} // (lane fsr-stage-b: the source size)
        if(built && auditFsr==1) {
            built=mid && auditMid.size>=picture.value.size();
            if(built){a=fsr1::compare(mid,picture);built=fsr1::windowFromPicture(mid,c,true,extent.width,extent.height,bgra,window);}
            if(built)b=fsr1::compare(got,window);
        } else if(built) {
            built=fsr1::windowFromEasu(picture,c,extent.width,extent.height,bgra,easuWindow);
            if(built)b=fsr1::compare(got,easuWindow);
        }
        const bool pass=built && !a.mismatches && !b.mismatches;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_AUDIT kind=%s filter=3 compute=0 fast=0 fsr=%u stops=%.3f pass=%u bytes=%zu mismatches=%zu off_by_one=%zu tie_range=%zu worst=%u "
            "first=%zu got=%u expected=%u picture_bytes=%zu picture_mismatches=%zu picture_off_by_one=%zu picture_tie_range=%zu picture_worst=%u tie_pixels=%zu "
            "extent=%ux%u rect=%ux%u bgra=%u ref_ms=%.1f",
            identity?"cpu":"gpu",auditFsr,double(c.stops),unsigned(pass),b.bytes,b.mismatches,b.offByOne,b.tieRange,b.worst,b.first,b.got,b.expected,a.bytes,
            a.mismatches,a.offByOne,a.tieRange,a.worst,picture.ties,extent.width,extent.height,c.width,c.height,unsigned(bgra),
            std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count());
        if(pass)return;
        ++auditFailures;
        fsrFailed=true;presentedFsr.store(0,std::memory_order_relaxed);presentedFilter.store(unsigned(filter),std::memory_order_relaxed);
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FSR fsr=0 why=audit built=%u",unsigned(built));
    }
    // v460: a GSR frame against gsr1_reference.h: the window against gsr1::window() of the frame's mapped source. Colour bytes
    // within 1, or within the reference's range where GSR's result legitimately depends on the GPU's arithmetic (counted as
    // soft; see gsr1_reference.h); alpha exact, the border's colour within 1. A failure turns GSR off for the run (unless diag-v460-gsr1-keep).
    void checkGsrAudit(bool identity) {
        const auto t0=std::chrono::steady_clock::now();
        const auto* src=static_cast<const std::uint8_t*>(auditSource.mapped);
        const auto* got=static_cast<const std::uint8_t*>(auditTarget.mapped);
        const gsr1::Constants& c=auditGsrCon;
        std::vector<std::uint8_t> mapped;gsr1::Expected expected;gsr1::Diff d{};
        bool built=src && got;
        if(built){gsr1::mappedPicture(src,identity?identityLut():gameLut,mapped,std::size_t{c.srcWidth}*c.srcHeight);built=gsr1::window(mapped.data(),c,extent.width,extent.height,bgra,expected);} // (lane fsr-stage-b: the source size)
        if(built)d=gsr1::compare(got,expected);
        const bool pass=built && !d.mismatches;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_AUDIT kind=%s filter=3 compute=0 fast=0 fsr=0 gsr=%u threshold=%.3f sharpness=%.3f pass=%u bytes=%zu mismatches=%zu "
            "off_by_one=%zu soft=%zu worst=%u first=%zu got=%u expected=%u sharpened=%zu uncertain=%zu extent=%ux%u rect=%ux%u bgra=%u ref_ms=%.1f",
            identity?"cpu":"gpu",1u+unsigned(c.edgeDirection),double(c.threshold255),double(c.sharpness),unsigned(pass),d.bytes,d.mismatches,d.offByOne,
            d.soft,d.worst,d.first,d.got,d.expected,expected.sharpened,expected.uncertain,extent.width,extent.height,c.width,c.height,unsigned(bgra),
            std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count());
        if(pass)return;
        ++auditFailures;
        if(gsrKeep){RECOMP_DIRECT_LOG("RENDER_DIRECT_GSR gsr=1 why=audit-failed-kept built=%u",unsigned(built));return;}
        gsrFailed=true;presentedGsr.store(0,std::memory_order_relaxed);
        filterPolls=0; // (the next present polls: presentedFsr / presentedFilter / presentedFast / presentedCompute then name the scaler that took over)
        RECOMP_DIRECT_LOG("RENDER_DIRECT_GSR gsr=0 why=audit built=%u",unsigned(built));
    }
    void checkAudit(unsigned k,bool identity) {
        if(!waitAcquireSlot(k))return;
#if defined(__APPLE__)
        // (build 192) a drawable-size change between the audit's record and now: the frame may have been drawn into a drawable of
        // the other size (MoltenVK follows the layer at once), so the comparison says nothing about the pass. Not a failure:
        // the audit is discarded and another one is armed (c449: one such frame turned the GPU presenter off for the run).
        if(recomp_ios_layout_gen.load(std::memory_order_acquire)!=recompIosAuditLayoutGen) {
            RECOMP_DIRECT_LOG("RENDER_DIRECT_AUDIT kind=%s discarded=1 why=layout-changed",identity?"cpu":"gpu");
            ++(identity?cpuAuditsLeft:gpuAuditsLeft);return;
        }
#endif
        ++audits;
        if(auditOne){checkOneAudit(identity);return;} // lane fsr-stage-b
        if(auditFsr==gsr1::Mode){checkGsrAudit(identity);return;} // v460
        if(auditFsr){checkFsrAudit(identity);return;} // v450
        std::vector<std::uint8_t> expected;
        const auto* src=static_cast<const std::uint8_t*>(auditSource.mapped);
        const auto* got=static_cast<const std::uint8_t*>(auditTarget.mapped);
        if(auditFilter!=Filter::Nearest) { // v409: <= 1 per colour channel, the border and alpha exact
            const auto t0=std::chrono::steady_clock::now();
            const bool built=referenceFiltered(src,identity?identityLut():gameLut,extent.width,extent.height,bgra,auditFilter,expected);
            std::size_t mismatches=0,near=0,first=0;unsigned worst=0;
            if(built)for(std::size_t b=0;b<expected.size();++b) {
                const unsigned d=unsigned(std::abs(int(got[b])-int(expected[b])));
                if(!d)continue;
                worst=std::max(worst,d);
                if(d==1 && b%4!=3)++near;else if(!mismatches++)first=b;
            }
            const bool pass=built && !mismatches;
            RECOMP_DIRECT_LOG("RENDER_DIRECT_AUDIT kind=%s filter=%u compute=%u fast=%u pass=%u bytes=%zu mismatches=%zu off_by_one=%zu worst=%u first=%zu got=%u expected=%u "
                "extent=%ux%u bgra=%u ref_ms=%.1f",identity?"cpu":"gpu",unsigned(auditFilter),unsigned(auditCompute),auditFast,unsigned(pass),expected.size(),mismatches,near,worst,first,
                built && mismatches?unsigned(got[first]):0u,built && mismatches?unsigned(expected[first]):0u,extent.width,extent.height,unsigned(bgra),
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count());
            if(pass)return;
            ++auditFailures;
            if(auditFast){fastFailed|=auditFast;presentedFast.store(0,std::memory_order_relaxed); // v42x: that mode off, v410's path
                RECOMP_DIRECT_LOG("RENDER_DIRECT_FILTER fast=0 kind=%s why=audit",auditFast==2?"packed":"gather");return;}
            if(auditCompute){scaleFailed=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_FILTER compute=0 why=audit");return;} // v411
            filterFailed=true;filter=Filter::Nearest;presentedFilter.store(0,std::memory_order_relaxed);
            RECOMP_DIRECT_LOG("RENDER_DIRECT_FILTER mode=0 why=audit");
            return;
        }
        const bool built=reference(src,identity?identityLut():gameLut,extent.width,extent.height,bgra,expected);
        std::size_t mismatches=0,first=0;
        if(built)for(std::size_t b=0;b<expected.size();++b)if(got[b]!=expected[b]){if(!mismatches++)first=b;}
        const bool pass=built && !mismatches;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_AUDIT kind=%s pass=%u bytes=%zu mismatches=%zu first=%zu got=%u expected=%u extent=%ux%u bgra=%u",identity?"cpu":"gpu",
            unsigned(pass),expected.size(),mismatches,first,built && mismatches?unsigned(got[first]):0u,built && mismatches?unsigned(expected[first]):0u,
            extent.width,extent.height,unsigned(bgra));
        if(pass)return;
        ++auditFailures;
        if(!identity){gpuAuditFailed=true;gpuOn.store(false,std::memory_order_release);RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT gpu=off why=audit");}
        else {RECOMP_DIRECT_LOG("RENDER_DIRECT_PRESENT released=1 why=cpu-audit");destroyAll();}
    }
    // ---- frame-gen (lane B), stage 1a: the pair dry run (fg_pair.h; local-analysis/agents/frame-gen/DESIGN-stage1.md).
    // While a diag-fg-pair-* file exists and the gate is open, a GPU frame is shown as two pictures two refreshes apart:
    // between = [the kept previous real picture, this real picture], ahead = [this real picture, the same picture again]
    // (stage 1b replaces only fgRecord's first copy by the generator's passes). The made picture is presented by CALLING the
    // unchanged presentLocked() with fgOut as its source and fgRecord as its `pre` recorder. Three things keep that call from
    // waiting (all under m, all in the out-of-line helpers below):
    //  - fgOut has a descriptor set of its OWN (fgSetPool, written once), put into setFor()'s one-entry cache around the call
    //    and taken out again: setFor() never misses, so its pool is never reset (a reset waits for every submitted pass);
    //  - the pair's first picture is taken off pendingPresents while the second is recorded (fgStash / fgUnstash): presentLocked
    //    would otherwise present it on the frame's own thread, and vkQueuePresentKHR waits for GPU work on this driver;
    //  - a pair is only planned when the made picture's objects exist (fgEnsure, once).
    // The first picture carries the frame's tag, the second an id of its own (fg_pair.h secondIdFor). Spacing: desired present
    // times on both pictures when lane A's slot is known and display timing works; else (or with diag-fg-hold) the present
    // thread waits before the second call (fgHold: a timed wait on presentCv with m released; no spinning).
    // Lifetime: fgPrev / fgOut / fgSetPool live as long as cpuImage and setPool do (never destroyed before the process ends).
    recomp::fg::Pairs fg;recomp::fg::Plan fgPlan;
    Img fgPrev,fgOut;VkDescriptorPool fgSetPool=VK_NULL_HANDLE;VkDescriptorSet fgSet=VK_NULL_HANDLE;
    std::function<void(VkCommandBuffer)> fgRecorder;VkImage fgRecSource=VK_NULL_HANDLE;
    bool fgEver=false,fgFailed=false,fgGeneral=false,fgPrevValid=false,fgKept=false,fgRecBetween=false,fgMadeFirst=false;
    bool fgHolding=false,fgSuperseded=false; // (the present thread's hold; under m)
    bool fgTimed=false; // pace-fg-tuning: the pacer was told that the pairs' first pictures carry a desired present time
    std::deque<std::uint32_t> fgHeldPresents,fgHeldTags;std::vector<int> fgHeldSlots;
    unsigned fgSetsSeen=0;std::uint64_t fgFrames=0,fgSetAllocs=0,fgPoolResets=0,fgMade=0,fgMadeNs=0,fgAcquireNs=0,fgStashDropped=0,fgInline=0;
#include "fg_gen_presenter.inc" // framegen-1b: the generator (last: the members above keep v465's offsets)
};
#else
// Host builds (tests, the replay harness): no window, never claimed.
class Presenter {
public:
    void setWindow(const Window&){}
    bool hasWindow(){return false;}
    void setDeviceSource(std::function<ProbeDevice*()>){}
    bool claim(){return false;}
    bool released(){return true;}
    bool usable(){return false;}
    void giveBack(){}
    bool gpuWanted(){return false;}
    bool gpuReady() const noexcept {return false;}
    void setLut(const Lut&){}
    bool presentCpu(const std::uint8_t*,std::size_t,unsigned,unsigned){return false;}
    bool presentGpu(VkQueue,VkImage,VkImageView,std::uint32_t=0){return false;}
    void forgetSource(VkImageView) noexcept {} // lane fsr-stage-b (review)
    void fgSwitches(unsigned) noexcept {} // frame-gen (lane B)
    unsigned fgSwitchWord() const noexcept {return 0;} // framegen-1b
    void fgGenFrame(const recomp::fgg::FrameIn&) noexcept {} // framegen-1b
    void fgGenTune(float,unsigned,unsigned) noexcept {} // framegen-1b
    void shutdown(){}
};
#endif
// One presenter per process (an inline function's static: one object across the library's translation units).
inline Presenter& presenter(){static Presenter p;return p;}
}
