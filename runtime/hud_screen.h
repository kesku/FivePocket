#pragma once
// hud2 (2026-10-22, v575): the second screen's HUD. The app (debug source set: PrivateTraceActivity + SecondScreen.java) opens an
// android.app.Presentation on a secondary display when files/settings/second-screen.txt says "hud", and hands its window to
// the library through recomp_set_hud_window_v1 (private_trace_api.h; the same acquire / release / generation contract as V6's
// window). Without that export call, or without a window, nothing here runs: direct_present.h asks wanted() (one relaxed
// atomic load) per real frame.
//
// hud3 (v585, the owner: "you didn't just move the ui element you cut and pasted that portion of the screen"): nothing is
// copied from the main picture any more. The second screen draws its own HUD at its own resolution:
//  - the radar from the game's own radar render target: Renderer::resolve (generic_renderer.cpp, v585Radar) calls
//    captureRadar() right after the game resolves that target (EDRAM 1440, pitch 560, k_8_8_8_8, 512x512, caller 824126fc;
//    hud_screen_layout.h) and copies RadarCopyW x RadarCopyH guest pixels of it into this module's radar image, in the
//    renderer's command buffer: the map and its blips on a transparent margin, before the game composites them over the
//    world. Every frame that has no such resolve (pause menu, cutscene, loading) leaves the last radar, shown dimmed;
//  - the health / armour bars and the five wanted stars as shapes (signed distances, antialiased) from the player's state read
//    from guest memory (hud_game_state.h);
//  - drawList (hud_screen_layout.h) = up to 16 quads, one push-constant block each, one render pass (hud_screen.vert / .frag).
// Per real GPU frame (not a frame-generation picture, not a CPU frame, not an audit frame), when a HUD window exists:
//  prepare()   (presentLocked, Presenter::m held, before recording) reads the player's state and the radar's age; a picture
//              equal to the last one shown (contentKey) is not drawn again (skipped_same); else acquires a HUD swapchain
//              image with timeout 0; no free image, or the present thread still busy: the frame is skipped (counted), so the
//              main path never waits for the second screen.
//  record()    after the main pass, in the same command buffer: the HUD render pass (clear black, the draws), timed with its own
//              two timestamps (pass_us). The main swapchain image is not touched.
//  the submit  waits for the HUD acquire semaphore too (COLOR_ATTACHMENT_OUTPUT) and signals the HUD image's own semaphore.
//  submitted() an empty submission with the HUD image's fence follows (it signals after all earlier work on the queue), and the
//              image goes to the HUD thread: it waits for that fence first, so its vkQueuePresentKHR does not wait for GPU work
//              while it holds the present queue's lock (present_us_max in the log line is the hold time).
// Ordering of the radar image: the capture is recorded in the renderer's command buffer, submitted to the same queue before
// presentGpu (which records the HUD pass) - queue submission order plus a barrier on each side of the copy; the presenter's
// pass ends with a full memory barrier, so the next frame's capture waits for this frame's reads.
// Window replaced / gone: the next prepare() waits for the main presenter's slots (its fences cover every HUD pass) and for the
// HUD thread, then destroys / re-creates the HUD swapchain. Any Vulkan failure turns the HUD off for the run (RENDER_HUD2 off).
//
// v576, the owner's switch "hide=1" in files/settings/second-screen.txt (off by default): while the HUD swapchain exists, every
// picture presentLocked shows on the main screen (real, frame-generation and CPU pictures alike) gets the game's HUD
// rectangles (hud_screen_layout.h minimapRect / starsRect) cleared to black (recordHide: one render pass per rectangle on the
// main presenter's own framebuffer, renderArea = the rectangle, loadOp CLEAR). It changes the main picture, never game content.
// No second screen (window gone, HUD off): nothing is hidden.
#include "hud_screen_layout.h"
#include "hud_replay.h" // hud5 (v604): MapMode, parseMapMode
#if defined(RECOMP_THOR_EDITION)
#include "wheel_screen_layout.h" // thor-wheel (v601/v605)
#include "resume_layout.h" // thor-maptap (v661 / v663): the Resume row's layout, colours and label
#include "loading_layout.h" // thor-char (v685): the Loading screen during a character switch
#if defined(RECOMP_THOR_EDITION)
#include "quick_save.h" // v820
#endif
#if defined(RECOMP_THOR_EDITION) && defined(__ANDROID__)
#include "map_tap.h" // v820 (thor-radio): the Quick Save button tells the pad machine when it is shown (quick_save.h)
#endif
#include <algorithm>
#endif
#include "direct_present_reference.h" // Layout
#include "vulkan_queue_lock.h"
#include <vulkan/vulkan.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
// ios-duo (2026-10-24): the iOS Duo edition's lower screen is the bottom half of a portrait screen, a second CAMetalLayer the app hands
// over through recomp_set_hud_window_v1 (Main.mm, RECOMP_DUO_EDITION); the surface is VK_EXT_metal_surface, as direct_present.h makes
// the main one on iOS. Everything else is the Thor's code. Host builds (macOS, no RECOMP_DUO_EDITION) keep the stub below.
#if defined(__ANDROID__) || (defined(__APPLE__) && defined(RECOMP_DUO_EDITION))
#define RECOMP_HUD2_REAL 1
#endif
#if defined(RECOMP_HUD2_REAL)
#include <android/log.h> // (iOS: platform/apple/include/android/log.h, the port's log shim)
#include <pthread.h>
#if defined(__APPLE__)
#include <vulkan/vulkan_metal.h>
#else
#include <vulkan/vulkan_android.h>
#endif
#include "recomp_app_dir.h"
#include "hud_screen_spirv.h"
#endif
struct ANativeWindow;
namespace recomp::gpu {
struct Window; // direct_present.h
}
namespace recomp::gpu::hud2 {
struct Frame {bool on=false;std::uint32_t image=0;VkSemaphore acquired=VK_NULL_HANDLE,done=VK_NULL_HANDLE;};
// hud4 (v597): the renderer's replica of the game's minimap box (hud_replay_renderer.inc): its shadow overlay image (GENERAL, written
// in the renderer's command buffer before this frame's HUD pass is submitted; the renderer never destroys a published image while
// the process runs), its size in texels, the box in its texels, the scale (sixteenths) and a serial (one per replay).
struct Replica {VkImage image=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;VkFormat format=VK_FORMAT_UNDEFINED;std::uint32_t width=0,height=0;
    float box[4]{};int k=16;std::uint64_t serial=0;
    bool stars=false;float starBox[4]{}; // hud5 (v604): the game's stars in the overlay's star band this frame; the crop shown (texels)
    float map[4]{},bars[4]{}; // hud6 (v618): the map inside the box and the bar strip below it (texels)
    float interior[4]{},interiorAt[2]{}; // hud7 (v620): map = the screen-sized map band; the box interior's texels and its screen place
    float frame[4]{}; // hud11 (v635): the game's frame alone (the frame band's texels; all 0 = none this replay)
    float backdrop[4]{}; // v638 (look=game): the main frame's pixels under the box this frame (texels; all 0 = none)
    float flash[4]{};}; // hud15 (v695): the damage flash alone (the flash band's map rectangle, texels; all 0 = none this replay)
struct Extent {std::uint32_t width=1240,height=1080;};
#if defined(RECOMP_THOR_EDITION)
// thor-wheel (v601): the renderer's replay of the weapon wheel (wheel_replay_renderer.inc), published as a Replica is: its overlay image,
// size in texels, the ring's square and the stats panel's rectangle in its texels, the scale (sixteenths), a serial per replay.
struct Wheel {VkImage image=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;VkFormat format=VK_FORMAT_UNDEFINED;std::uint32_t width=0,height=0;
    float ring[4]{},stats[4]{};int k=16;std::uint64_t serial=0;
    bool radio=false; // v651 (thor-radio): the radio wheel's replay (shown only with the setting "Radio wheel: Lower screen"; no stats panel)
    bool character=false; // thor-char: the character wheel's replay (ring = the wheel's texels, stats = the stats panel's; charDrawList)
    bool phone=false;bool phoneTurn=false;}; // v815; v822: phoneTurn = the screen movie holds a landscape app (shown turned back)
    // (v815, thor-radio: the game's phone screen - ring = its texels; phoneDrawList)
// v818 (thor-radio, mission text): the renderer's replay of the game's help box (wheel_replay.h TextArea at TextK) - its image, size and
// the area's texels, a serial per replay.
struct TextImg {VkImage image=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;VkFormat format=VK_FORMAT_UNDEFINED;std::uint32_t width=0,height=0;
    float box[4]{};std::uint64_t serial=0;};
#endif
struct HudWindow {
    void* user=nullptr;
    ANativeWindow* (*acquire)(void*,std::uint64_t*) noexcept=nullptr;
    void (*release)(void*,ANativeWindow*) noexcept=nullptr;
    std::uint64_t (*generation)(void*) noexcept=nullptr;
};
#if defined(RECOMP_HUD2_REAL)
#define RECOMP_HUD2_LOG(...) __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",__VA_ARGS__)
class Screen {
public:
    // From recomp_set_hud_window_v1 (before the game starts; once).
    void setWindow(const HudWindow& w) noexcept {
        std::lock_guard l(hm);
        if(!w.acquire || !w.release || !w.generation)return;
        window=w;callbacks.store(true,std::memory_order_release);
        RECOMP_HUD2_LOG("RENDER_HUD2 callbacks=1");
    }
    // Per real frame, Presenter::m held: false = nothing to do (no callbacks, or off for the run).
#if defined(RECOMP_DUO_EDITION)
    // ios-duo (2026-10-24): the Thor's lower screen always exists; the Duo's is the bottom half of a portrait screen, and the player's
    // fullscreen toggle takes it away (recomp_duo_set_lower_screen_v1, Main.mm). shown=false makes wanted() false, so the game's own
    // minimap box and wheels stay on the main frame and nothing is prepared for the lower screen, exactly as without callbacks.
    void setShown(bool on) noexcept {if(shown.exchange(on,std::memory_order_relaxed)!=on)RECOMP_HUD2_LOG("RENDER_DUO lower=%d",int(on));}
    bool wanted() const noexcept {return callbacks.load(std::memory_order_relaxed) && !off.load(std::memory_order_relaxed) && shown.load(std::memory_order_relaxed);}
    std::atomic<bool> shown{true};
    // ios-duo: the lower screen's map rectangle in its pixels (layout3's map, as RENDER_HUD3 logs it), for the app's tap zone
    // (recomp_duo_lower_map_rect_v1); all 0 before a layout.
    std::atomic<int> duoMapRect[4]{};
#else
    bool wanted() const noexcept {return callbacks.load(std::memory_order_relaxed) && !off.load(std::memory_order_relaxed);}
#endif
    // hud4 (v597): whether the renderer replays the minimap for this screen (files/settings/second-screen.txt replica=0: no)
    bool replicaWanted() const noexcept {return wanted() && replicaOn.load(std::memory_order_relaxed);}
    // hud5 (v604): files/settings/minimap.txt as read when the HUD window was set up ("bottom": the renderer leaves the game's minimap
    // box off the main frame; "top" never reaches the library: the app then opens no second screen).
    hudrep::MapMode mapMode() const noexcept {return hudrep::MapMode(mapModeV.load(std::memory_order_relaxed));}
#if defined(RECOMP_THOR_EDITION)
    // v638: second-screen.txt look=game (the renderer then captures the main frame's pixels under the box for the lower screen)
    bool lookGame() const noexcept {return wanted() && lookGameV.load(std::memory_order_relaxed);}
#endif
    // hud7 (v620): every Nth game frame is replayed for this screen (second-screen.txt rate=1..4, default 2)
    unsigned replayRate() const noexcept {return rateV.load(std::memory_order_relaxed);}
    // hud3 (v585): the renderer, right after it resolved the game's radar target into `source` (GENERAL layout, its resolve
    // copy recorded before this call with the renderer's own barrier), in its command buffer `cmd`. sourceWidth / Height: the
    // Vulkan image's texels; scale: its texels per guest pixel (the render scale). Nothing while the HUD has no radar image.
    [[gnu::noinline]] void captureRadar(VkCommandBuffer cmd,VkImage source,std::uint32_t sourceWidth,std::uint32_t sourceHeight,float scale,
                                        std::uint32_t caller) noexcept {
        const VkImage dst=radarPublished.load(std::memory_order_acquire);
        if(!dst || off.load(std::memory_order_relaxed) || !(scale>0.0f))return;
        const auto w=std::min({std::uint32_t(std::ceil(float(hud::RadarCopyW)*scale)),sourceWidth,hud::RadarCapacity});
        const auto h=std::min({std::uint32_t(std::ceil(float(hud::RadarCopyH)*scale)),sourceHeight,hud::RadarCapacity});
        if(!w || !h)return;
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=dst;b.oldLayout=b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; // (the last HUD pass's reads first)
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={w,h,1};
        vkCmdCopyImage(cmd,source,VK_IMAGE_LAYOUT_GENERAL,dst,VK_IMAGE_LAYOUT_GENERAL,1,&c);
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
        std::uint32_t bits;std::memcpy(&bits,&scale,4);radarScaleBits.store(bits,std::memory_order_relaxed);
        if(radarSerial.fetch_add(1,std::memory_order_release)==0)
            RECOMP_HUD2_LOG("RENDER_HUD3 radar first=1 source=%ux%u copied=%ux%u scale=%.3f caller=%08x",sourceWidth,sourceHeight,w,h,double(scale),caller);
    }
    // hud4 (v597): the renderer thread, after a replay recorded into its command buffer (before that buffer's submit, which precedes
    // the presenter's submit of the HUD pass that samples it).
    void publishReplica(const Replica& r) noexcept {std::lock_guard l(rm);replica=r;}
    // hud4: the second screen's size (the renderer chooses its replay scale from it); 1240x1080 until a HUD swapchain exists.
    Extent extentHint() const noexcept {return {hintW.load(std::memory_order_relaxed),hintH.load(std::memory_order_relaxed)};}
    // Presenter::m held. waitMain(user) waits for every submitted main-presenter pass (true when they all finished).
    // lut: the presenter's display tables (the main screen's gamma; a storage buffer of 1536 words, [0,768) the game's).
    [[gnu::noinline]] Frame prepare(VkInstance instance,VkPhysicalDevice physical,VkDevice device,std::uint32_t family,
                                    VkBuffer lut,bool (*waitMain)(void*),void* waitUser) noexcept {
        std::unique_lock l(hm);
        if(off.load(std::memory_order_relaxed))return {};
        dev=device;inst=instance;phys=physical;queueFamily=family;
        if(!loaded && !load())return turnOff(l,"functions",waitMain,waitUser);
        if(!lut)return turnOff(l,"no-lut",waitMain,waitUser);
        const std::uint64_t g=window.generation(window.user);
        if(g!=gen || lost || recreate || lut!=lutBuffer) {
            if(!dropSwapchain(l,waitMain,waitUser))return turnOff(l,"wait",waitMain,waitUser);
            gen=g;lost=false;recreate=false;lutBuffer=lut;
            if(!g)return {}; // (no window: nothing until the app attaches one)
            const char* why=makeSwapchain();
            if(why) {
                if(why[0]=='-'){RECOMP_HUD2_LOG("RENDER_HUD2 wait why=%s",why+1);dropSwapchain(l,waitMain,waitUser);return {};} // (retried at the next generation change)
                return turnOff(l,why,waitMain,waitUser);
            }
        }
        if(!swapchain)return {};
        ++frames;
        // hud3: this frame's picture (the player's state, the radar's age); the same picture as the one shown is not drawn again
        const std::uint64_t serial=radarSerial.load(std::memory_order_acquire);
        if(serial!=seenSerial){seenSerial=serial;radar.age=0;}else if(radar.age<1000000u)++radar.age;
        radar.ever=serial!=0;
        {const std::uint32_t bits=radarScaleBits.load(std::memory_order_relaxed);std::memcpy(&radar.scale,&bits,4);if(!(radar.scale>0.0f))radar.scale=1.0f;}
        const std::uint8_t* base=hud::guestBase().load(std::memory_order_acquire);
        game=base?hud::readGameState(hud::GuestMemory{base}):hud::GameState{};
        {std::lock_guard rl(rm);rep=replica;} // hud4: the newest replica
        if(rep.serial!=seenReplica){seenReplica=rep.serial;replicaInfo.age=0;}else if(replicaInfo.age<1000000u)++replicaInfo.age;
        replicaInfo.ever=rep.serial!=0 && rep.view && rep.width && rep.height;
        // the replica when one exists and is current, or when the radar capture is not current either (a menu: the last replica, dimmed);
        // the radar capture of v596 (hud3) while replays fail but the game draws its radar (and in the synchronous renderer)
        replicaMode=settings.replica && replicaInfo.ever && (hud::replicaLive(replicaInfo) || !hud::radarLive(radar));
        if(replicaMode) {
            for(unsigned i=0;i<4;++i){replicaInfo.box[i]=rep.box[i];replicaInfo.starBox[i]=rep.starBox[i];replicaInfo.map[i]=rep.map[i];replicaInfo.bars[i]=rep.bars[i];replicaInfo.interior[i]=rep.interior[i];}
            replicaInfo.interiorAt[0]=rep.interiorAt[0];replicaInfo.interiorAt[1]=rep.interiorAt[1];
            for(unsigned i=0;i<4;++i){replicaInfo.frame[i]=rep.frame[i];replicaInfo.backdrop[i]=rep.backdrop[i];replicaInfo.flash[i]=rep.flash[i];} // hud11, v638, hud15
            replicaInfo.stars=rep.stars && hud::replicaLive(replicaInfo); // (hud5: a stale replica shows no stars: a menu, a cutscene)
            replicaInfo.invW=1.0f/float(rep.width);replicaInfo.invH=1.0f/float(rep.height);
        }
        frameKey=replicaMode?hud::replicaKey(settings,game,replicaInfo,rep.serial):hud::contentKey(settings,game,radar,serial);
        hudHidden=hud::hudHidden(settings,radar,replicaInfo); // v632: the game shows no HUD (cutscene, menu, load): a black lower screen
#if defined(RECOMP_THOR_EDITION)
        resumeSnapshot();
        resumeMode=!resumeFailed && resumeOn.load(std::memory_order_relaxed) && mirrorView && mirrorW && mirrorH; // (v667: for resumeLeave)
        resumeLeave(hudHidden); // thor-maptap (v667): the HUD at once after a pause screen
        if(!hudHidden && replicaMode)frameKey=hud::replicaKey(settings,game,replicaInfo,rep.serial); // (the key of what is shown now)
#endif
        if(hudHidden)frameKey=hud::HiddenKey;
#if defined(RECOMP_THOR_EDITION)
        textFrame(frameKey); // v818: the help box over the map (its replays change the key)
        wheelFrame(frameKey); // thor-wheel (v601): the wheel replica, while current, is this frame's picture
        resumeFrame(frameKey); // thor-maptap (v661): a pause screen = the Resume button
        saveFrame(frameKey); // v820: the Quick Save button over the map (shown / pressed change the key)
#endif
        if(shownValid && frameKey==shownKey){++skippedSame;return {};}
        if(pending.size()+(busy?1u:0u)>=1u){++skippedBusy;return {};} // (one HUD picture at a time in the present thread)
        if(spare.empty()){++skippedNoImage;return {};}
        const VkSemaphore s=spare.back();
        std::uint32_t j=0;
        const auto t0=std::chrono::steady_clock::now();
        const VkResult r=acquireImage(dev,swapchain,0,s,VK_NULL_HANDLE,&j);
        const auto ns=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());
        acquireNsMax=std::max(acquireNsMax,ns);acquireNs+=ns;++acquireCalls;
        if(r==VK_NOT_READY || r==VK_TIMEOUT){++skippedNoImage;return {};}
        if(r==VK_ERROR_OUT_OF_DATE_KHR){recreate=true;++outOfDate;return {};}
        if(r==VK_ERROR_SURFACE_LOST_KHR){lost=true;return {};}
        if((r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR) || j>=images.size()){RECOMP_HUD2_LOG("RENDER_HUD2 acquire=%d",int(r));return turnOff(l,"acquire",waitMain,waitUser);}
        spare.pop_back();
        if(imageAcquired[j])spare.push_back(imageAcquired[j]); // (its last wait finished: image j came back from the display)
        imageAcquired[j]=s;
        return {true,j,s,done[j]};
    }
    // After the main pass (the main swapchain image is not used): the HUD render pass into HUD image f.image.
    [[gnu::noinline]] void record(VkCommandBuffer cb,const Frame& f) noexcept {
        std::lock_guard g(hm);
        if(!radarInitialized) { // (first HUD frame: the radar image leaves UNDEFINED, cleared; published after this submit)
            VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=radarImage;b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            const VkClearColorValue zero{{0,0,0,0}};const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(cb,radarImage,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&all);
            b.oldLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
            radarInitRecorded=true;
        }
        if(timePool){vkCmdResetQueryPool(cb,timePool,2*f.image,2);vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timePool,2*f.image);}
        hud::DrawList list;VkDescriptorSet useSet=set;
#if defined(RECOMP_THOR_EDITION)
        if(wheelRecord(cb,list,useSet)){} else // thor-wheel (v601): the weapon wheel instead of the HUD
        if(resumeRecord(cb,list,useSet)){} else // thor-maptap (v661): the Resume button while the game is paused
#endif
        if(hudHidden){useSet=set;++hiddenPasses;} else // v632: no HUD in the game = no HUD here: the pass only clears to black
        if(replicaMode && (useSet=replicaSet(rep.view))!=VK_NULL_HANDLE) { // hud4 (v597): the game's own minimap box
            // the renderer's replay (an earlier submission on this queue) wrote it as a colour attachment
            VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&mb,0,nullptr,0,nullptr);
            // hud7 (v620): the map's whole width at full strength over the whole screen, the box's own draws over it, the bar strip along the
            // bottom, the game's stars small at the top right
            list=hud::replicaDrawList11(hud::replicaLayout11(extent.width,extent.height,replicaInfo),settings,replicaInfo);++replicaPasses; // (hud11 v635)
        } else {useSet=set;list=hud::drawList(layout,settings,game,radar);}
#if defined(RECOMP_THOR_EDITION)
        saveShowNow=saveShow && useSet==replicaSetHandle && replicaMode && !hudHidden && saveUpload(cb); // v820 (the label image: before the pass)
#endif
        const VkClearValue black{{{hud::Background.r,hud::Background.g,hud::Background.b,1.0f}}};
        VkRenderPassBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;bi.renderPass=renderPass;bi.framebuffer=framebuffers[f.image];
        bi.renderArea={{0,0},extent};bi.clearValueCount=1;bi.pClearValues=&black;
        vkCmdBeginRenderPass(cb,&bi,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        const VkViewport vp{0,0,float(extent.width),float(extent.height),0,1};const VkRect2D sc{{0,0},extent};
        vkCmdSetViewport(cb,0,1,&vp);vkCmdSetScissor(cb,0,1,&sc);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&useSet,0,nullptr);
        for(unsigned k=0;k<list.n;++k) {
            float push[24];std::memcpy(push,&list.d[k],sizeof(hud::Draw));
            push[20]=float(extent.width);push[21]=float(extent.height);push[22]=push[23]=1.0f/float(hud::RadarCapacity);
            vkCmdPushConstants(cb,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,push);
            vkCmdDraw(cb,4,1,0,0);
        }
#if defined(RECOMP_THOR_EDITION)
        if(useSet==replicaSetHandle && replicaMode && !hudHidden)textRecord(cb,float(extent.width),float(extent.height)); // v818: the help box over the map
        if(saveShowNow)saveRecord(cb,float(extent.width),float(extent.height)); // v820: the Quick Save button over the map
#endif
        vkCmdEndRenderPass(cb);
        if(timePool)vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timePool,2*f.image+1);
        timed[f.image]=timePool!=VK_NULL_HANDLE;draws+=list.n;
    }
    // After the frame's submit. ok false: the submit failed (the main presenter stops; the HUD goes off without waiting).
    // presentQueue / presentMutex: where and under which lock the main presenter presents (its own queue, or the shared one).
    [[gnu::noinline]] void submitted(const Frame& f,bool ok,VkQueue queue,VkQueue presentQueue,std::mutex* presentMutex) noexcept {
        std::unique_lock l(hm);
        if(!ok){off.store(true,std::memory_order_relaxed);RECOMP_HUD2_LOG("RENDER_HUD2 off why=submit");return;}
        if(radarInitRecorded && !radarInitialized){radarInitialized=true;radarPublished.store(radarImage,std::memory_order_release);} // (later captures: after this submit)
        shownKey=frameKey;shownValid=true;
        vkResetFences(dev,1,&fences[f.image]);
        VkResult r;
        {GenericSubmissionScope generic;r=lockedQueueSubmit(queue,0,nullptr,fences[f.image]);} // (signals after this frame's pass)
        if(r!=VK_SUCCESS){RECOMP_HUD2_LOG("RENDER_HUD2 fence-submit=%d",int(r));fenceOk[f.image]=false;}else fenceOk[f.image]=true;
        presentQ=presentQueue;presentLock=presentMutex;
        if(!thread.joinable()) {
            try {thread=std::thread([this]{loop();});} catch(...) {off.store(true,std::memory_order_relaxed);RECOMP_HUD2_LOG("RENDER_HUD2 off why=thread");return;}
        }
        pending.push_back(f.image);++copies;
        cv.notify_one();
    }
    // v576: whether this picture's HUD rectangles are blacked on the main screen (one relaxed load when off).
    bool hideWanted() const noexcept {return hideOn.load(std::memory_order_relaxed) && !off.load(std::memory_order_relaxed);}
    // v576: after every copy of the main swapchain image this frame (its framebuffer fb, made with the main pass for format;
    // the image is in PRESENT_SRC). Clears the game's HUD rectangles on the main picture to black.
    [[gnu::noinline]] void recordHide(VkCommandBuffer cb,VkFramebuffer fb,VkFormat mainFormat,const direct::Layout& l) noexcept {
        std::lock_guard g(hm);
        if(!hideOn.load(std::memory_order_relaxed) || !dev || !fb)return;
        if(!hidePass || hideFormat!=mainFormat) {
            if(hidePass){vkDestroyRenderPass(dev,hidePass,nullptr);hidePass=VK_NULL_HANDLE;} // (a format change: the previous frames finished, the main swapchain was re-made)
            VkAttachmentDescription at{};at.format=mainFormat;at.samples=VK_SAMPLE_COUNT_1_BIT;at.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
            at.storeOp=VK_ATTACHMENT_STORE_OP_STORE;at.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;at.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            at.initialLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;at.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sp{};sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sp.colorAttachmentCount=1;sp.pColorAttachments=&ref;
            // after the pass, the audit / band copies (TRANSFER reads) and the main pass's writes of the same image
            VkSubpassDependency dep{};dep.srcSubpass=VK_SUBPASS_EXTERNAL;dep.dstSubpass=0;
            dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
            dep.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;
            dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo rp{};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;rp.attachmentCount=1;rp.pAttachments=&at;
            rp.subpassCount=1;rp.pSubpasses=&sp;rp.dependencyCount=1;rp.pDependencies=&dep;
            if(vkCreateRenderPass(dev,&rp,nullptr,&hidePass)!=VK_SUCCESS){hidePass=VK_NULL_HANDLE;hideOn.store(false,std::memory_order_relaxed);
                RECOMP_HUD2_LOG("RENDER_HUD2 hide=0 why=render-pass");return;}
            hideFormat=mainFormat;
        }
        hud::RectI r[2];unsigned n=0;
        if(settings.minimap)r[n++]=hud::onPicture(mapRect,l.left,l.top,l.imageWidth,l.imageHeight);
        if(settings.stars)r[n++]=hud::onPicture(starsRect,l.left,l.top,l.imageWidth,l.imageHeight);
        const VkClearValue black{{{0.0f,0.0f,0.0f,1.0f}}};
        for(unsigned k=0;k<n;++k) {
            if(r[k].empty())continue;
            VkRenderPassBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;bi.renderPass=hidePass;bi.framebuffer=fb;
            bi.renderArea={{r[k].x0,r[k].y0},{std::uint32_t(r[k].w()),std::uint32_t(r[k].h())}};bi.clearValueCount=1;bi.pClearValues=&black;
            vkCmdBeginRenderPass(cb,&bi,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdEndRenderPass(cb);
        }
        if(!(hidden++%1024))RECOMP_HUD2_LOG("RENDER_HUD2 hide=1 pictures=%llu rects=%u",(unsigned long long)hidden,n);
    }
    void shutdown() noexcept {
        {std::lock_guard l(hm);stop=true;}
        cv.notify_all();
        if(thread.joinable())thread.join();
    }
private:
    std::mutex hm;std::condition_variable cv;std::thread thread;std::deque<std::uint32_t> pending;bool busy=false,stop=false;
    std::atomic<bool> callbacks{false},off{false},hideOn{false}; // (v576: hideOn = hide=1 and a HUD swapchain)
    VkRenderPass hidePass=VK_NULL_HANDLE;VkFormat hideFormat=VK_FORMAT_UNDEFINED;std::uint64_t hidden=0;
    HudWindow window{};
    VkDevice dev=VK_NULL_HANDLE;VkInstance inst=VK_NULL_HANDLE;VkPhysicalDevice phys=VK_NULL_HANDLE;std::uint32_t queueFamily=0;
    bool loaded=false,lost=false,recreate=false;std::uint64_t gen=0;
    ANativeWindow* nw=nullptr;VkSurfaceKHR surface=VK_NULL_HANDLE;VkSwapchainKHR swapchain=VK_NULL_HANDLE;VkExtent2D extent{};VkFormat format{};
    std::vector<VkImage> images;std::vector<VkImageView> views;std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> done,imageAcquired,spare;std::vector<VkFence> fences;std::vector<bool> fenceOk,timed;
    VkQueryPool timePool=VK_NULL_HANDLE;double tsNs=1.0;std::uint64_t tsMask=~0ull;
    hud::Settings settings;hud::RectF mapRect{},starsRect{};
    VkQueue presentQ=VK_NULL_HANDLE;std::mutex* presentLock=nullptr;
    std::uint64_t frames=0,copies=0,presented=0,skippedBusy=0,skippedNoImage=0,skippedSame=0,outOfDate=0,presentFails=0,fenceWaitFails=0,draws=0;
    std::uint64_t acquireNs=0,acquireNsMax=0,acquireCalls=0,presentNs=0,presentNsMax=0,passNs=0,passNsMax=0,passN=0;
    // hud3 (v585): the layout, this frame's state and the drawing objects. The radar image, its memory / view, the sampler,
    // the descriptor objects, the shaders and the pipeline layout live for the process (made once: the renderer's capture may
    // name the image at any time after radarPublished); the render pass, pipeline, views and framebuffers follow the swapchain.
    hud::Layout3 layout{};hud::GameState game{};hud::RadarInfo radar{};std::uint64_t seenSerial=0,frameKey=0,shownKey=0;bool shownValid=false;
    // hud4 (v597): the published replica (rm), this frame's copy and state, the descriptor sets that name a replica view (made when
    // the published view changes; a replaced one is freed at the next swapchain change, after the waits)
    std::mutex rm;Replica replica{},rep{};hud::ReplicaInfo replicaInfo{};std::uint64_t seenReplica=0,replicaPasses=0,hiddenPasses=0;bool replicaMode=false,hudHidden=false;
    std::atomic<std::uint32_t> hintW{1240},hintH{1080};std::atomic<bool> replicaOn{true};
    std::atomic<std::uint8_t> mapModeV{std::uint8_t(hudrep::MapMode::Both)}; // hud5
#if defined(RECOMP_THOR_EDITION)
    std::atomic<bool> lookGameV{false}; // v638
#endif
    std::atomic<unsigned> rateV{hudrep::DefaultRate}; // hud7
    VkImageView replicaSetView=VK_NULL_HANDLE;VkDescriptorSet replicaSetHandle=VK_NULL_HANDLE;std::vector<VkDescriptorSet> replicaRetired;
    std::atomic<VkImage> radarPublished{VK_NULL_HANDLE};std::atomic<std::uint64_t> radarSerial{0};std::atomic<std::uint32_t> radarScaleBits{0x3F800000u};
    VkImage radarImage=VK_NULL_HANDLE;VkDeviceMemory radarMemory=VK_NULL_HANDLE;VkImageView radarView=VK_NULL_HANDLE;bool radarInitRecorded=false,radarInitialized=false;
    VkSampler sampler=VK_NULL_HANDLE;VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;VkDescriptorPool pool=VK_NULL_HANDLE;VkDescriptorSet set=VK_NULL_HANDLE;
    static constexpr std::uint32_t PoolSets=16; // hud4: the radar set + replica sets
    VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;VkShaderModule vs=VK_NULL_HANDLE,fs=VK_NULL_HANDLE;bool statics=false;
    VkRenderPass renderPass=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;VkBuffer lutBuffer=VK_NULL_HANDLE;
#if defined(__APPLE__)
    PFN_vkCreateMetalSurfaceEXT createSurface=nullptr; // ios-duo
#else
    PFN_vkCreateAndroidSurfaceKHR createSurface=nullptr;
#endif
    PFN_vkDestroySurfaceKHR destroySurface=nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surfaceSupport=nullptr;PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surfaceCaps=nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR surfaceFormats=nullptr;PFN_vkCreateSwapchainKHR createSwapchain=nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain=nullptr;PFN_vkGetSwapchainImagesKHR swapchainImages=nullptr;
    PFN_vkAcquireNextImageKHR acquireImage=nullptr;PFN_vkQueuePresentKHR queuePresent=nullptr;

    bool load() noexcept {
        const auto i=[&](const char* n){return vkGetInstanceProcAddr(inst,n);};
        const auto d=[&](const char* n){return vkGetDeviceProcAddr(dev,n);};
#if defined(__APPLE__)
        createSurface=reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(i("vkCreateMetalSurfaceEXT")); // ios-duo (device.cpp enables the extension)
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
        loaded=createSurface && destroySurface && surfaceSupport && surfaceCaps && surfaceFormats && createSwapchain && destroySwapchain &&
            swapchainImages && acquireImage && queuePresent;
        return loaded;
    }
    Frame turnOff(std::unique_lock<std::mutex>& l,const char* why,bool (*waitMain)(void*),void* waitUser) noexcept {
        off.store(true,std::memory_order_relaxed);
        RECOMP_HUD2_LOG("RENDER_HUD2 off why=%s",why);
        dropSwapchain(l,waitMain,waitUser);
        return {};
    }
    void readSettings() noexcept {
        char text[hud::SettingsCap+1]{};std::size_t n=0;bool present=false;
        if(FILE* f=std::fopen(RECOMP_APP_PATH("/settings/second-screen.txt"),"rb")){present=true;n=std::fread(text,1,hud::SettingsCap,f);std::fclose(f);}
        settings=hud::parseSettings(text,n);
        if(!settings.on)settings=hud::Settings{true}; // (the app opened the window, so it read "hud"; the options keep their defaults)
        replicaOn.store(settings.replica,std::memory_order_relaxed); // hud4
        rateV.store(settings.rate,std::memory_order_relaxed); // hud7
        { // hud5 (v604): the Minimap screens setting (the Thor edition app's settings row)
            char m[64]{};std::size_t mn=0;
            if(FILE* f=std::fopen(RECOMP_APP_PATH("/settings/minimap.txt"),"rb")){mn=std::fread(m,1,sizeof m,f);std::fclose(f);}
            const auto mode=hudrep::parseMapMode(m,mn);mapModeV.store(std::uint8_t(mode),std::memory_order_relaxed);
            RECOMP_HUD2_LOG("RENDER_HUD5 minimap=%s",mode==hudrep::MapMode::Bottom?"bottom":mode==hudrep::MapMode::Top?"top":"both");
        }
        mapRect=hud::minimapRect(settings.insetPercent);starsRect=hud::starsRect(settings.insetPercent);
        RECOMP_HUD2_LOG("RENDER_HUD2 settings file=%u inset=%.2f map=%u stars=%u hide=%u replica=%u ignored=%u",unsigned(present),double(settings.insetPercent),
            unsigned(settings.minimap),unsigned(settings.stars),unsigned(settings.hide),unsigned(settings.replica),settings.ignored);
    }
    // Under hm. Waits for the HUD thread's queue and present, then for the main presenter's passes; destroys the swapchain,
    // its objects, the surface and the window reference (the process-long hud3 objects stay).
    bool dropSwapchain(std::unique_lock<std::mutex>& l,bool (*waitMain)(void*),void* waitUser) noexcept {
        if(!swapchain && !surface && !nw && !renderPass)return true;
        cv.wait(l,[&]{return pending.empty() && !busy;});
        bool ok=waitMain(waitUser);
        for(std::size_t j=0;j<fences.size();++j)if(fenceOk[j] && vkWaitForFences(dev,1,&fences[j],VK_TRUE,2'000'000'000ull)!=VK_SUCCESS)ok=false;
        for(auto s:done)if(s)vkDestroySemaphore(dev,s,nullptr);
        for(auto s:imageAcquired)if(s)vkDestroySemaphore(dev,s,nullptr);
        for(auto s:spare)if(s)vkDestroySemaphore(dev,s,nullptr);
        for(auto f:fences)if(f)vkDestroyFence(dev,f,nullptr);
        for(auto f:framebuffers)if(f)vkDestroyFramebuffer(dev,f,nullptr);
        for(auto v:views)if(v)vkDestroyImageView(dev,v,nullptr);
        done.clear();imageAcquired.clear();spare.clear();fences.clear();fenceOk.clear();timed.clear();images.clear();views.clear();framebuffers.clear();
        if(pipeline){vkDestroyPipeline(dev,pipeline,nullptr);pipeline=VK_NULL_HANDLE;}
        if(renderPass){vkDestroyRenderPass(dev,renderPass,nullptr);renderPass=VK_NULL_HANDLE;}
        if(timePool){vkDestroyQueryPool(dev,timePool,nullptr);timePool=VK_NULL_HANDLE;}
        hideOn.store(false,std::memory_order_relaxed); // (v576: nothing hidden without the second screen)
#if defined(RECOMP_THOR_EDITION)
        wheelOn.store(false,std::memory_order_relaxed); // (thor-wheel: no lower screen, the main screen keeps its wheel)
        radioOn.store(false,std::memory_order_relaxed); // (thor-radio v651: and its radio wheel)
        charOn.store(false,std::memory_order_relaxed); // (thor-char: and its character wheel)
        phoneOn.store(false,std::memory_order_relaxed); // (v815: and the phone)
        textOn.store(false,std::memory_order_relaxed); // (v818: and the help box)
        saveOn.store(false,std::memory_order_relaxed);saveOff(); // (v820: and the Quick Save button)
#endif
        if(swapchain){destroySwapchain(dev,swapchain,nullptr);swapchain=VK_NULL_HANDLE;}
        if(surface){destroySurface(inst,surface,nullptr);surface=VK_NULL_HANDLE;}
        if(nw){window.release(window.user,nw);nw=nullptr;}
        shownValid=false;
        RECOMP_HUD2_LOG("RENDER_HUD2 window=0 copies=%llu presented=%llu",(unsigned long long)copies,(unsigned long long)presented);
        return ok;
    }
    std::optional<std::uint32_t> memoryType(std::uint32_t bits,VkMemoryPropertyFlags want) const noexcept {
        VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(phys,&mp);
        for(std::uint32_t i=0;i<mp.memoryTypeCount;++i)if((bits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&want)==want)return i;
        return std::nullopt;
    }
    // hud3: the objects that live for the process (once; null = made, else why not).
    const char* makeStatics() noexcept {
        if(statics)return nullptr;
        VkImageCreateInfo ii{};ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent={hud::RadarCapacity,hud::RadarCapacity,1};ii.mipLevels=1;ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
        ii.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(!radarImage && vkCreateImage(dev,&ii,nullptr,&radarImage)!=VK_SUCCESS){radarImage=VK_NULL_HANDLE;return "radar-image";}
        if(!radarMemory) {
            VkMemoryRequirements r{};vkGetImageMemoryRequirements(dev,radarImage,&r);
            const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if(!type)return "radar-memory-type";
            VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=r.size;ai.memoryTypeIndex=*type;
            if(vkAllocateMemory(dev,&ai,nullptr,&radarMemory)!=VK_SUCCESS){radarMemory=VK_NULL_HANDLE;return "radar-memory";}
            if(vkBindImageMemory(dev,radarImage,radarMemory,0)!=VK_SUCCESS)return "radar-bind";
        }
        if(!radarView) {
            VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=radarImage;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
            vi.format=VK_FORMAT_R8G8B8A8_UNORM;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            if(vkCreateImageView(dev,&vi,nullptr,&radarView)!=VK_SUCCESS){radarView=VK_NULL_HANDLE;return "radar-view";}
        }
        if(!sampler) {
            VkSamplerCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;si.magFilter=si.minFilter=VK_FILTER_LINEAR;
            si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.maxLod=0;
            if(vkCreateSampler(dev,&si,nullptr,&sampler)!=VK_SUCCESS){sampler=VK_NULL_HANDLE;return "sampler";}
        }
        if(!setLayout) {
            VkDescriptorSetLayoutBinding b[3]{};
            b[0].binding=0;b[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b[0].descriptorCount=1;b[0].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding=1;b[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;b[1].descriptorCount=1;b[1].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
            b[2].binding=2;b[2].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b[2].descriptorCount=1;b[2].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT; // hud4: the replica
            VkDescriptorSetLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;li.bindingCount=3;li.pBindings=b;
            if(vkCreateDescriptorSetLayout(dev,&li,nullptr,&setLayout)!=VK_SUCCESS){setLayout=VK_NULL_HANDLE;return "set-layout";}
        }
        if(!pool) {
            const VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*PoolSets},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,PoolSets}};
            VkDescriptorPoolCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pi.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            pi.maxSets=PoolSets;pi.poolSizeCount=2;pi.pPoolSizes=sizes; // (hud4: + the replica sets)
            if(vkCreateDescriptorPool(dev,&pi,nullptr,&pool)!=VK_SUCCESS){pool=VK_NULL_HANDLE;return "pool";}
            VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;
            if(vkAllocateDescriptorSets(dev,&ai,&set)!=VK_SUCCESS){set=VK_NULL_HANDLE;return "set";}
        }
        if(!pipelineLayout) {
            const VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,96};
            VkPipelineLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;li.setLayoutCount=1;li.pSetLayouts=&setLayout;
            li.pushConstantRangeCount=1;li.pPushConstantRanges=&pr;
            if(vkCreatePipelineLayout(dev,&li,nullptr,&pipelineLayout)!=VK_SUCCESS){pipelineLayout=VK_NULL_HANDLE;return "pipeline-layout";}
        }
        const auto module=[&](const std::uint32_t* code,std::size_t bytes,VkShaderModule& m) {
            if(m)return true;
            VkShaderModuleCreateInfo mi{};mi.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;mi.codeSize=bytes;mi.pCode=code;
            if(vkCreateShaderModule(dev,&mi,nullptr,&m)!=VK_SUCCESS){m=VK_NULL_HANDLE;return false;}
            return true;
        };
        if(!module(HudVertexSpirv,sizeof HudVertexSpirv,vs) || !module(HudFragmentSpirv,sizeof HudFragmentSpirv,fs))return "shaders";
        statics=true;
        return nullptr;
    }
    // hud3: the descriptor set names the radar image and the presenter's tables (written while nothing uses it: after the waits).
    void writeSet(VkDescriptorSet target=VK_NULL_HANDLE,VkImageView replicaView=VK_NULL_HANDLE) noexcept {
        if(!target)target=set;
        const VkDescriptorImageInfo ii{sampler,radarView,VK_IMAGE_LAYOUT_GENERAL};const VkDescriptorBufferInfo bi{lutBuffer,0,VK_WHOLE_SIZE};
        const VkDescriptorImageInfo ri{sampler,replicaView?replicaView:radarView,VK_IMAGE_LAYOUT_GENERAL}; // (hud4: binding 2; the radar as a placeholder)
        VkWriteDescriptorSet w[3]{};
        for(auto& x:w){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=target;x.descriptorCount=1;}
        w[0].dstBinding=0;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&ii;
        w[1].dstBinding=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&bi;
        w[2].dstBinding=2;w[2].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[2].pImageInfo=&ri;
        vkUpdateDescriptorSets(dev,3,w,0,nullptr);
    }
    // hud4: under hm, in record(): the set that names `view` (a new one when the published view changed; the replaced one may still
    // be read by a HUD pass in flight and is freed at the next swapchain change). Null = none (pool exhausted): the v596 picture.
    VkDescriptorSet replicaSet(VkImageView view) noexcept {
        if(!view)return VK_NULL_HANDLE;
        if(view==replicaSetView && replicaSetHandle)return replicaSetHandle;
        VkDescriptorSet s=VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;
        if(vkAllocateDescriptorSets(dev,&ai,&s)!=VK_SUCCESS)return VK_NULL_HANDLE;
        writeSet(s,view);
        if(replicaSetHandle)replicaRetired.push_back(replicaSetHandle);
        replicaSetHandle=s;replicaSetView=view;
        RECOMP_HUD2_LOG("RENDER_HUD4 replica_set=1 view=%p sets_retired=%zu",static_cast<void*>(view),replicaRetired.size());
        return s;
    }
    // hud3: the render pass (clear, PRESENT_SRC at the end) and the pipeline for the swapchain's format.
    const char* makePipeline() noexcept {
        VkAttachmentDescription at{};at.format=format;at.samples=VK_SAMPLE_COUNT_1_BIT;at.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;at.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        at.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;at.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;at.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sp.colorAttachmentCount=1;sp.pColorAttachments=&ref;
        // the acquire semaphore is waited at COLOR_ATTACHMENT_OUTPUT: the layout transition is behind it
        VkSubpassDependency dep{};dep.srcSubpass=VK_SUBPASS_EXTERNAL;dep.dstSubpass=0;dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep.srcAccessMask=0;dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;rp.attachmentCount=1;rp.pAttachments=&at;rp.subpassCount=1;rp.pSubpasses=&sp;
        rp.dependencyCount=1;rp.pDependencies=&dep;
        if(vkCreateRenderPass(dev,&rp,nullptr,&renderPass)!=VK_SUCCESS){renderPass=VK_NULL_HANDLE;return "render-pass";}
        VkPipelineShaderStageCreateInfo st[2]{};
        for(unsigned i=0;i<2;++i){st[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;
            st[i].module=i?fs:vs;st[i].pName="main";}
        VkPipelineVertexInputStateCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=1;vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{};rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;rs.polygonMode=VK_POLYGON_MODE_FILL;
        rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};ba.blendEnable=VK_TRUE;ba.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;ba.colorBlendOp=VK_BLEND_OP_ADD;ba.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;ba.alphaBlendOp=VK_BLEND_OP_ADD;
        ba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;cb.attachmentCount=1;cb.pAttachments=&ba;
        const VkDynamicState dyn[2]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;ds.dynamicStateCount=2;ds.pDynamicStates=dyn;
        VkGraphicsPipelineCreateInfo gi{};gi.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;gi.stageCount=2;gi.pStages=st;gi.pVertexInputState=&vi;
        gi.pInputAssemblyState=&ia;gi.pViewportState=&vp;gi.pRasterizationState=&rs;gi.pMultisampleState=&ms;gi.pColorBlendState=&cb;gi.pDynamicState=&ds;
        gi.layout=pipelineLayout;gi.renderPass=renderPass;gi.subpass=0;
        if(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gi,nullptr,&pipeline)!=VK_SUCCESS){pipeline=VK_NULL_HANDLE;return "pipeline";}
        return nullptr;
    }
    // Under hm, nothing of a swapchain left. Null = made; "-why" = not now (no window yet, a hidden window); else = off.
    const char* makeSwapchain() noexcept {
        if(const char* why=makeStatics())return why;
        writeSet();
        // hud4: every replica set (nothing reads them now: dropSwapchain waited); the next replica frame makes one with the new tables
        for(auto s:replicaRetired)vkFreeDescriptorSets(dev,pool,1,&s);
        replicaRetired.clear();
        if(replicaSetHandle){vkFreeDescriptorSets(dev,pool,1,&replicaSetHandle);replicaSetHandle=VK_NULL_HANDLE;replicaSetView=VK_NULL_HANDLE;}
        std::uint64_t g=0;
        nw=window.acquire(window.user,&g);
        if(!nw)return "-no-window";
        if(g!=gen){gen=g;}
        readSettings();
#if defined(__APPLE__)
        // ios-duo: nw is the app's lower-screen CAMetalLayer (opaque here; the app keeps it alive while its generation lasts)
        VkMetalSurfaceCreateInfoEXT si{};si.sType=VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;si.pLayer=reinterpret_cast<const CAMetalLayer*>(nw);
#else
        VkAndroidSurfaceCreateInfoKHR si{};si.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;si.window=nw;
#endif
        if(createSurface(inst,&si,nullptr,&surface)!=VK_SUCCESS){surface=VK_NULL_HANDLE;return "surface";}
        VkBool32 supported=VK_FALSE;
        if(surfaceSupport(phys,queueFamily,surface,&supported)!=VK_SUCCESS || !supported)return "surface-queue";
        VkSurfaceCapabilitiesKHR caps{};
        if(surfaceCaps(phys,surface,&caps)!=VK_SUCCESS)return "caps";
        if(caps.currentExtent.width==0 || caps.currentExtent.height==0)return "-hidden";
        if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))return "no-color-attachment";
        if(!(caps.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))return "no-identity";
        std::uint32_t n=0;
        if(surfaceFormats(phys,surface,&n,nullptr)!=VK_SUCCESS || !n || n>256)return "formats";
        std::vector<VkSurfaceFormatKHR> formats(n);
        if(surfaceFormats(phys,surface,&n,formats.data())!=VK_SUCCESS)return "formats";
        auto chosen=formats.front();
        if(chosen.format==VK_FORMAT_UNDEFINED){chosen.format=VK_FORMAT_R8G8B8A8_UNORM;chosen.colorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;}
        for(const auto& f:formats)if(f.format==VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR){chosen=f;break;}
        if(chosen.format!=VK_FORMAT_R8G8B8A8_UNORM && chosen.format!=VK_FORMAT_B8G8R8A8_UNORM)return "format"; // (UNORM: the shader writes display values, as the main presenter)
        VkExtent2D e=caps.currentExtent;
        if(e.width==0xFFFFFFFFu){e.width=std::clamp(1080u,caps.minImageExtent.width,caps.maxImageExtent.width);
            e.height=std::clamp(1240u,caps.minImageExtent.height,caps.maxImageExtent.height);}
        std::uint32_t count=caps.minImageCount+1;if(caps.maxImageCount)count=std::min(count,caps.maxImageCount);
        VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for(const auto a:{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                          VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})if(caps.supportedCompositeAlpha&a){alpha=a;break;}
        VkSwapchainCreateInfoKHR ci{};ci.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;ci.surface=surface;ci.minImageCount=count;
        ci.imageFormat=chosen.format;ci.imageColorSpace=chosen.colorSpace;ci.imageExtent=e;ci.imageArrayLayers=1;
        ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;ci.compositeAlpha=alpha;ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=VK_TRUE;
        if(createSwapchain(dev,&ci,nullptr,&swapchain)!=VK_SUCCESS){swapchain=VK_NULL_HANDLE;return "swapchain";}
        extent=e;format=chosen.format;
        n=0;
        if(swapchainImages(dev,swapchain,&n,nullptr)!=VK_SUCCESS || !n || n>16)return "images";
        images.resize(n);
        if(swapchainImages(dev,swapchain,&n,images.data())!=VK_SUCCESS)return "images";
        done.assign(n,VK_NULL_HANDLE);imageAcquired.assign(n,VK_NULL_HANDLE);fences.assign(n,VK_NULL_HANDLE);fenceOk.assign(n,false);timed.assign(n,false);
        views.assign(n,VK_NULL_HANDLE);framebuffers.assign(n,VK_NULL_HANDLE);
        if(const char* why=makePipeline())return why;
        VkSemaphoreCreateInfo sc{};sc.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fc{};fc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        for(std::uint32_t j=0;j<n;++j) {
            if(vkCreateSemaphore(dev,&sc,nullptr,&done[j])!=VK_SUCCESS){done[j]=VK_NULL_HANDLE;return "semaphore";}
            if(vkCreateFence(dev,&fc,nullptr,&fences[j])!=VK_SUCCESS){fences[j]=VK_NULL_HANDLE;return "fence";}
            VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=images[j];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
            vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            if(vkCreateImageView(dev,&vi,nullptr,&views[j])!=VK_SUCCESS){views[j]=VK_NULL_HANDLE;return "view";}
            VkFramebufferCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fi.renderPass=renderPass;fi.attachmentCount=1;fi.pAttachments=&views[j];
            fi.width=e.width;fi.height=e.height;fi.layers=1;
            if(vkCreateFramebuffer(dev,&fi,nullptr,&framebuffers[j])!=VK_SUCCESS){framebuffers[j]=VK_NULL_HANDLE;return "framebuffer";}
        }
        for(std::uint32_t j=0;j<=n;++j){VkSemaphore s=VK_NULL_HANDLE;if(vkCreateSemaphore(dev,&sc,nullptr,&s)!=VK_SUCCESS)return "semaphore";spare.push_back(s);}
        // pass_us: two timestamps per HUD image (none when the queue family has no timestamps)
        std::uint32_t qn=0;vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,nullptr);
        std::vector<VkQueueFamilyProperties> qp(qn);vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,qp.data());
        VkPhysicalDeviceProperties pp{};vkGetPhysicalDeviceProperties(phys,&pp);
        const std::uint32_t bits=queueFamily<qn?qp[queueFamily].timestampValidBits:0;
        if(bits && pp.limits.timestampPeriod>0) {
            tsNs=pp.limits.timestampPeriod;tsMask=bits>=64?~0ull:((1ull<<bits)-1);
            VkQueryPoolCreateInfo qi{};qi.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2*n;
            if(vkCreateQueryPool(dev,&qi,nullptr,&timePool)!=VK_SUCCESS)timePool=VK_NULL_HANDLE;
        }
        layout=hud::layout3(e.width,e.height,settings);
        hintW.store(e.width,std::memory_order_relaxed);hintH.store(e.height,std::memory_order_relaxed); // hud4
        if(!layout.ok)return "layout";
        hideOn.store(settings.hide,std::memory_order_relaxed); // v576
#if defined(RECOMP_THOR_EDITION)
        wheelOn.store(!settings.wheelMain,std::memory_order_relaxed); // thor-wheel (v601): the setting "Weapon wheel: Lower screen" (default)
        radioOn.store(!settings.radioMain,std::memory_order_relaxed); // thor-radio (v651): the setting "Radio wheel: Lower screen" (default)
        charOn.store(!settings.characterMain,std::memory_order_relaxed); // thor-char: the setting "Character wheel: Lower screen" (default)
        phoneOn.store(!settings.phoneMain,std::memory_order_relaxed); // v815: the phone on the lower screen (default; phone=main = the game's own)
        textOn.store(settings.textOn,std::memory_order_relaxed); // v818 / v836: the help box on the lower screen only with text=on (default: none)
        saveOn.store(!settings.saveOff,std::memory_order_relaxed); // v820: the Quick Save button (default; save=off = none)
        lookGameV.store(settings.lookGame,std::memory_order_relaxed); // v638
#endif
        shownValid=false;
#if defined(RECOMP_DUO_EDITION)
        duoMapRect[0].store(int(layout.map.x0));duoMapRect[1].store(int(layout.map.y0));duoMapRect[2].store(int(layout.map.x1));duoMapRect[3].store(int(layout.map.y1));
#endif
        RECOMP_HUD2_LOG("RENDER_HUD3 window=1 generation=%llu extent=%ux%u format=%d images=%u transform=%u timed=%u frame=%.0f,%.0f..%.0f,%.0f "
            "map=%.0f,%.0f..%.0f,%.0f bars=%.0f..%.0f stars=%.0f,%.0f..%.0f,%.0f",(unsigned long long)gen,e.width,e.height,int(format),n,
            unsigned(caps.currentTransform),unsigned(timePool!=VK_NULL_HANDLE),double(layout.frame.x0),double(layout.frame.y0),double(layout.frame.x1),
            double(layout.frame.y1),double(layout.map.x0),double(layout.map.y0),double(layout.map.x1),double(layout.map.y1),double(layout.health.y0),
            double(layout.health.y1),double(layout.stars[0].x0),double(layout.stars[0].y0),double(layout.stars[4].x1),double(layout.stars[4].y1));
#if defined(RECOMP_THOR_EDITION)
        // thor-radio (v654): the setting words as zero-terminated strings in the library - the app's CapabilityScanner offers the Weapon
        // wheel / Radio wheel rows only for a build that contains them so; parseSettings' wordIs("wheel=main") compiles to immediate
        // compares (v605-v653: no "wheel=main\0" in any build, the row never available without capabilities-override.txt)
        RECOMP_HUD2_LOG("RENDER_HUD3 words weapon=%s radio=%s character=%s phone=%s",settings.wheelMain?"wheel=main":"lower",settings.radioMain?"radio=main":"lower",
            settings.characterMain?"character=main":"lower",settings.phoneMain?"phone=main":"lower"); // (thor-char: + character=main, also for the app's CapabilityScanner)
#endif
        return nullptr;
    }
    void loop() noexcept {
#if defined(__APPLE__)
        pthread_setname_np("recomp-hud2"); // (ios-duo: Darwin names the calling thread only)
#else
        pthread_setname_np(pthread_self(),"recomp-hud2");
#endif
        std::unique_lock l(hm);
        for(;;) {
            cv.wait(l,[&]{return stop || !pending.empty();});
            if(stop)return;
            const std::uint32_t j=pending.front();pending.pop_front();busy=true;
            const VkFence fence=j<fences.size() && fenceOk[j]?fences[j]:VK_NULL_HANDLE;const VkSwapchainKHR sc=swapchain;
            const VkSemaphore wait=done[j];const VkQueue q=presentQ;std::mutex* lock=presentLock;const bool wasTimed=timed[j];
            l.unlock();
            // the frame's pass first (its own fence): the present below then does not wait for GPU work under the queue lock
            if(fence && vkWaitForFences(dev,1,&fence,VK_TRUE,2'000'000'000ull)!=VK_SUCCESS)++fenceWaitFails;
            std::uint64_t t[2]{};
            const bool gotTime=wasTimed && fence && timePool &&
                vkGetQueryPoolResults(dev,timePool,2*j,2,sizeof t,t,sizeof t[0],VK_QUERY_RESULT_64_BIT)==VK_SUCCESS;
            VkPresentInfoKHR pi{};pi.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&wait;
            pi.swapchainCount=1;pi.pSwapchains=&sc;pi.pImageIndices=&j;
            VkResult r;
            const auto t0=std::chrono::steady_clock::now();
            if(lock){std::lock_guard g(*lock);r=queuePresent(q,&pi);}
            else {std::lock_guard g(sharedQueueMutex());r=queuePresent(q,&pi);}
            const auto ns=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());
            l.lock();
            busy=false;presentNs+=ns;presentNsMax=std::max(presentNsMax,ns);
            if(gotTime){const auto d=std::uint64_t(double(((t[1]&tsMask)-(t[0]&tsMask))&tsMask)*tsNs);passNs+=d;passNsMax=std::max(passNsMax,d);++passN;}
            if(r==VK_SUCCESS || r==VK_SUBOPTIMAL_KHR)++presented;
            else {++presentFails;if(r==VK_ERROR_OUT_OF_DATE_KHR)recreate=true;else if(r==VK_ERROR_SURFACE_LOST_KHR)lost=true;
                else if(presentFails<=4)RECOMP_HUD2_LOG("RENDER_HUD2 present=%d",int(r));}
            if(!(presented%256) || presented<=2) {
                RECOMP_HUD2_LOG("RENDER_HUD2 frames=%llu copies=%llu presented=%llu skipped_busy=%llu skipped_no_image=%llu skipped_same=%llu out_of_date=%llu "
                    "present_fails=%llu fence_wait_fails=%llu pass_us=%.1f pass_us_max=%.1f acquire_us=%.1f acquire_us_max=%.1f present_us=%.1f "
                    "present_us_max=%.1f draws=%llu radar_captures=%llu radar_age=%u ped=%u hp=%.0f/%.0f armour=%.0f wanted=%d replica=%u replica_age=%u replica_serial=%llu replica_passes=%llu hidden_passes=%llu",
                    (unsigned long long)frames,(unsigned long long)copies,(unsigned long long)presented,(unsigned long long)skippedBusy,
                    (unsigned long long)skippedNoImage,(unsigned long long)skippedSame,(unsigned long long)outOfDate,(unsigned long long)presentFails,
                    (unsigned long long)fenceWaitFails,passN?passNs/1e3/double(passN):0.0,passNsMax/1e3,acquireCalls?acquireNs/1e3/double(acquireCalls):0.0,
                    acquireNsMax/1e3,presented?presentNs/1e3/double(presented):0.0,presentNsMax/1e3,(unsigned long long)draws,
                    (unsigned long long)radarSerial.load(std::memory_order_relaxed),radar.age,unsigned(game.ped),double(game.health),double(game.healthMax),
                    double(game.armour),game.wanted,unsigned(replicaMode),replicaInfo.age,(unsigned long long)rep.serial,(unsigned long long)replicaPasses,(unsigned long long)hiddenPasses);
                if(presented>2){passNs=passN=passNsMax=0;acquireNs=acquireCalls=acquireNsMax=0;presentNs=presentNsMax=0;} // (per 256 presents)
            }
            cv.notify_all();
        }
    }
#if defined(RECOMP_THOR_EDITION)
#include "wheel_screen.inc" // thor-wheel (v601): the lower screen's weapon wheel (last: its access labels end the class)
#include "resume_screen.inc" // thor-maptap (v661): the lower screen's Resume Game button on a pause screen
#include "text_screen.inc" // v818 (thor-radio): the game's help box over the lower screen's map
#include "quick_save_screen.inc" // v820 (thor-radio): the Quick Save button over the lower screen's map
#endif
};
#else
// Host builds: never on (hud4's renderer harness: RECOMP_HUD4_TEST_WANTED = the second screen wants its HUD, so the renderer replays).
class Screen {
public:
    void setWindow(const HudWindow&) noexcept {}
#if defined(RECOMP_HUD4_TEST_WANTED)
    bool wanted() const noexcept {return true;}
#else
    bool wanted() const noexcept {return false;}
#endif
    bool replicaWanted() const noexcept {return wanted();}
#if defined(RECOMP_HUD5_TEST_BOTTOM) // hud5's renderer harness: the "Bottom screen only" mode
    hudrep::MapMode mapMode() const noexcept {return hudrep::MapMode::Bottom;}
#else
    hudrep::MapMode mapMode() const noexcept {return hudrep::MapMode::Both;}
#endif
#if defined(RECOMP_THOR_EDITION) // v638: the renderer harness's RECOMP_HUD12_TEST_LOOKGAME = look=game
#if defined(RECOMP_HUD12_TEST_LOOKGAME)
    bool lookGame() const noexcept {return true;}
#else
    bool lookGame() const noexcept {return false;}
#endif
#endif
#if defined(RECOMP_HUD7_TEST_RATE)
    unsigned replayRate() const noexcept {return RECOMP_HUD7_TEST_RATE;}
#else
    unsigned replayRate() const noexcept {return 1;} // (the host harness replays every frame unless a test asks)
#endif
#if defined(RECOMP_THOR_EDITION) // thor-wheel (v601): the renderer harness's RECOMP_WHEEL_TEST_WANTED = the lower screen takes the wheel
#if defined(RECOMP_WHEEL_TEST_WANTED)
    bool wheelWanted() const noexcept {return true;}
#else
    bool wheelWanted() const noexcept {return false;}
#endif
#if defined(RECOMP_RADIO_TEST_WANTED) // thor-radio (v651): the same for the radio wheel
    bool radioWanted() const noexcept {return true;}
#else
    bool radioWanted() const noexcept {return false;}
#endif
#if defined(RECOMP_CHAR_TEST_WANTED) // thor-char: the same for the character wheel
    bool charWanted() const noexcept {return true;}
#else
    bool charWanted() const noexcept {return false;}
#endif
    bool phoneWanted() const noexcept {return false;} // v815 (the host harness: no phone)
    bool textWanted() const noexcept {return false;} // v818
    void publishText(const TextImg&) noexcept {}
    void publishWheel(const Wheel&) noexcept {}
    void publishResume(bool,bool,bool=true) noexcept {} // thor-maptap (v661 .. v667; v811: the pill flag)
    void publishLoading(bool,bool=false) noexcept {} // thor-char (v685; v693: tint)
    void mirrorSource(VkImageView,std::uint32_t,std::uint32_t) noexcept {} // thor-maptap (v667)
#endif
    void captureRadar(VkCommandBuffer,VkImage,std::uint32_t,std::uint32_t,float,std::uint32_t) noexcept {}
    void publishReplica(const Replica&) noexcept {}
    Extent extentHint() const noexcept {return {};}
    Frame prepare(VkInstance,VkPhysicalDevice,VkDevice,std::uint32_t,VkBuffer,bool (*)(void*),void*) noexcept {return {};}
    void record(VkCommandBuffer,const Frame&) noexcept {}
    void submitted(const Frame&,bool,VkQueue,VkQueue,std::mutex*) noexcept {}
    bool hideWanted() const noexcept {return false;}
    void recordHide(VkCommandBuffer,VkFramebuffer,VkFormat,const direct::Layout&) noexcept {}
    void shutdown() noexcept {}
};
#endif
// One per process (an inline function's static: one object across the library's translation units).
inline Screen& screen() noexcept {static Screen s;return s;}
}
