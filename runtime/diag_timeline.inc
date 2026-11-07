#if defined(__APPLE__)
#include <pthread.h>
#endif
// Attempt v159: the timeline probe (diag_timeline.h), included once by guest_cooperative_executor.cpp.
#include "diag_timeline.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#if !defined(RECOMP_DIAG_SWITCH_DIR)
#define RECOMP_DIAG_SWITCH(name) RECOMP_APP_PATH("/private-trace/" name) // data-paths (host tests: recomp_app_dir.h)
#endif
namespace recomp::diag::timeline {
thread_local std::uint32_t waitObject = 0; // v195
namespace {
struct Rec { std::uint64_t ns; std::uint32_t tid, token; std::uint16_t kind, arg; std::uint32_t extra; };
constexpr std::uint32_t Capacity = 1u << 20; // v264: ~15 s outdoors (65536 held ~0.8 s: 23 frames)
std::array<Rec, Capacity> ring{};
std::atomic<std::uint32_t> next{0}, written{0};
std::atomic<bool> dumping{false};
std::atomic<long long> nextCheckMs{0};
std::atomic<unsigned> dumps{0};
std::uint64_t startNs = 0;
thread_local std::uint32_t threadToken = 0, threadTid = 0;
std::uint64_t now() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void dump() noexcept {
    char path[200];
    std::snprintf(path, sizeof path, RECOMP_DIAG_SWITCH("timeline-%d-%u.bin"), int(::getpid()), dumps.fetch_add(1) + 1);
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        std::uint32_t header[8]{0x314C5452u /*RTL1*/, Capacity, std::uint32_t(sizeof(Rec)), 0, std::uint32_t(startNs), std::uint32_t(startNs >> 32), 0, 0};
        (void)!::write(fd, header, sizeof header);
        (void)!::write(fd, ring.data(), sizeof(Rec) * Capacity);
        ::close(fd);
    }
    ::unlink(RECOMP_DIAG_SWITCH("diag-timeline"));
}
}
std::atomic<bool> armed{false};
void setThreadToken(std::uint32_t token) noexcept { threadToken = token; }
void record(Kind kind, std::uint16_t arg, std::uint32_t extra) noexcept { recordAt(now(), kind, arg, extra); }
void recordAt(std::uint64_t ns, Kind kind, std::uint16_t arg, std::uint32_t extra) noexcept {
    const auto i = next.fetch_add(1, std::memory_order_relaxed);
    if (i >= Capacity) { armed.store(false, std::memory_order_relaxed); return; }
#if defined(__APPLE__)
    // RTL1 has a 32-bit field. Use the process-local Mach thread port name;
    // do not truncate Apple's 64-bit kernel thread ID. Diagnostic identity only.
    if (!threadTid) threadTid = std::uint32_t(pthread_mach_thread_np(pthread_self()));
#if defined(APPLE_TIMELINE_MUTANT_ZERO_TID)
    threadTid = 0;
#endif
#else
    if (!threadTid) threadTid = std::uint32_t(::syscall(SYS_gettid));
#endif
    ring[i] = Rec{ns, threadTid, threadToken, std::uint16_t(kind), arg, extra};
    if (written.fetch_add(1, std::memory_order_acq_rel) + 1 == Capacity) { // the last record: write the ring once
        armed.store(false, std::memory_order_relaxed);
        bool expected = false;
        if (dumping.compare_exchange_strong(expected, true)) { dump(); dumping.store(false); }
    }
}
void tick() noexcept {
    const long long ms = (long long)(now() / 1000000);
    auto due = nextCheckMs.load(std::memory_order_relaxed);
    if (ms < due || !nextCheckMs.compare_exchange_strong(due, ms + 1000, std::memory_order_relaxed)) return;
    if (armed.load(std::memory_order_relaxed) || dumping.load()) return;
    if (next.load() >= Capacity && written.load() < Capacity) return; // records still being completed
    if (::access(RECOMP_DIAG_SWITCH("diag-timeline"), F_OK) != 0) return;
    next.store(0); written.store(0); startNs = now();
    armed.store(true, std::memory_order_release);
}
}
