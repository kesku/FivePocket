#pragma once
// Attempt v337 (diagnostic, goal stage 5): display lag per frame from VK_GOOGLE_display_timing while
// files/private-trace/diag-v337-lag exists at the presenter's start. Two 1 ms-bin histograms, filled by the presenter thread
// and read (and reset) by the renderer's 60-present RENDER_GENERIC_TIMING line: qd = the render thread queued the frame's
// present -> the frame was displayed; cd = the presenter's vkQueuePresentKHR call -> displayed. Clock: CLOCK_MONOTONIC (the
// clock of actualPresentTime on Android). Frames are matched by their tag (the present's async-queue sequence) in 64-entry rings.
#include <array>
#include <atomic>
#include <cstdint>
#include <time.h>
namespace recomp::v337 {
#if defined(__APPLE__) // ios-direct-present: the clock of MoltenVK's actualPresentTime (see pace_caps.h)
inline std::uint64_t monotonicNs() noexcept {return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);}
#else
inline std::uint64_t monotonicNs() noexcept {timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return std::uint64_t(t.tv_sec)*1000000000ull+std::uint64_t(t.tv_nsec);}
#endif
constexpr unsigned Bins=128,Ring=64;
inline std::array<std::atomic<std::uint32_t>,Bins> qd{},cd{};
inline std::array<std::atomic<std::uint32_t>,Ring> queuedTag{};
inline std::array<std::atomic<std::uint64_t>,Ring> queuedAt{};
// render thread: the frame's present was queued now
inline void noteQueued(std::uint32_t tag) noexcept {
    if(!tag)return;
    queuedAt[tag%Ring].store(monotonicNs(),std::memory_order_relaxed);queuedTag[tag%Ring].store(tag,std::memory_order_release);
}
inline void add(std::array<std::atomic<std::uint32_t>,Bins>& h,std::uint64_t from,std::uint64_t to) noexcept {
    if(!from || to<from)return;
    const auto ms=(to-from)/1000000ull;h[ms<Bins?ms:Bins-1].fetch_add(1,std::memory_order_relaxed);
}
// presenter thread: frame `tag` was displayed at `displayed` (its present call was at `called`, 0 = unknown)
inline void noteDisplayed(std::uint32_t tag,std::uint64_t displayed,std::uint64_t called) noexcept {
    if(queuedTag[tag%Ring].load(std::memory_order_acquire)==tag)add(qd,queuedAt[tag%Ring].load(std::memory_order_relaxed),displayed);
    add(cd,called,displayed);
}
// the median bin's centre in ms (the histogram is reset), -1 without samples
inline double takeMedian(std::array<std::atomic<std::uint32_t>,Bins>& h) noexcept {
    std::array<std::uint32_t,Bins> c{};std::uint64_t n=0;
    for(unsigned i=0;i<Bins;++i){c[i]=h[i].exchange(0,std::memory_order_relaxed);n+=c[i];}
    if(!n)return -1.0;
    std::uint64_t acc=0;for(unsigned i=0;i<Bins;++i){acc+=c[i];if(2*acc>=n)return double(i)+0.5;}
    return -1.0;
}
}
