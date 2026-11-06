#pragma once
// v218 (goal step 1, hot-path logging): per-event diagnostic lines on the submission / tile-IRQ / query paths (~830
// lines/s in the v217 house: SUBMIT 256/s, KICK 193/s, RETIRED 59/s, five lines per tile IRQ at 44/s, several of
// them inside the IRQ's exclusive span) are sampled: the first 64 of each site, then every 16th; callers always log
// failures and refusals, and exact totals go to periodic lines. files/private-trace/diag-v218-baseline (applied by
// the renderer every 60 presents) restores per-event logging (the in-run A/B).
//
// quiet-log (2026-10-04): files/private-trace/diag-quiet-log, polled at the renderer's 60-present window boundary
// (quietLogWindow; RENDER_GENERIC_V210Z quiet_log=1 when it was on for the whole window that just ended), cached in
// `quiet` and read with one relaxed load in the log call, before any formatting by liblog. While it is on:
//  - quietLog / rendererLogQuiet / executorLogQuiet lines (the high-rate types nothing reads; local-analysis/agents/
//    quiet-log/policy.json) are not logged;
//  - quietLogSampled / rendererLogSampled lines (the v218-sampled sites: SUBMIT, KICK, RETIRED, TILE_*, QUERY_*) keep
//    every 16th of the sampled lines per call site (every 256th event; iterate.py's Watch keeps its SUBMIT / KICK
//    heartbeat, ~2 lines/s each), and every line whose `keep` argument is set (the site's failure / refusal condition);
//  - quietLogThin lines (RENDER_DIAG_FILE_READ) keep the first 64 of the run, then every 16th.
// Never gated: failure, refusal, stop, marker and window-summary lines. Each gated call replaces one log call (same
// arguments; the sampled sites add their failure condition as `keep`), is out of line and, like __android_log_print, not
// noexcept: the call sites keep their CFG and IR-PGO records (checked with -Wbackend-plugin against round6-v437).
#include "play_build.h" // play-config-2 (v527). sampled() itself is unchanged: a change to its inlined body moved pre-inlining decisions (stale records in trace_runner).
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include "recomp_app_dir.h" // data-paths: the app's files directory.
namespace recomp::diag {
inline std::atomic<bool> logAll{false};
inline bool sampled(std::atomic<unsigned long long>& count, unsigned long long every = 16) noexcept {
    const auto k = count.fetch_add(1, std::memory_order_relaxed) + 1;
    return k <= 64 || k % every == 0 || logAll.load(std::memory_order_relaxed);
}

#if defined(__APPLE__)
inline bool appleQuietDefault() noexcept {
    static const bool quietByDefault=[] {
        const char* verbose=std::getenv("RECOMP_VERBOSE_DIAGNOSTICS");
        return !(verbose && verbose[0]=='1' && verbose[1]=='\0');
    }();
    return quietByDefault;
}
// Apple forwards diagnostics through synchronous stdio; opt in to verbose
// diagnostics before launch rather than putting that I/O on every frame.
inline std::atomic<bool> quiet{RECOMP_PLAY_QUIET_AT_START || appleQuietDefault()};
#else
inline std::atomic<bool> quiet{RECOMP_PLAY_QUIET_AT_START};
#endif
inline bool quietSkip() noexcept { return quiet.load(std::memory_order_relaxed); }
#include "play_log.h" // play-config-2 (v527): the play build's diag-play-loud switch and its counted quietSkip (nothing without -DRECOMP_PLAY_BUILD=1)
#if defined(RECOMP_QUIET_LOG_TEST_SINK) // host test: every line that would reach liblog
void RECOMP_QUIET_LOG_TEST_SINK(int priority, const char* tag, const char* format, va_list arguments);
#endif
// Not noexcept, like __android_log_vprint (it never throws): the quietLog* functions then are not inferred nounwind and a
// call that replaces __android_log_print stays an invoke where it was one (a noexcept version turned registryMaybeLog's
// invokes into calls: a CFG change, its PGO record discarded).
inline void quietLogEmit(int priority, const char* tag, const char* format, va_list arguments) {
#if defined(RECOMP_QUIET_LOG_TEST_SINK)
    RECOMP_QUIET_LOG_TEST_SINK(priority, tag, format, arguments);
#elif defined(__ANDROID__)
    __android_log_vprint(priority, tag, format, arguments);
#else
    (void)priority; (void)tag;
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
#endif
}
// One sampled call site's quiet-mode counter, keyed by its format literal (each site has its own).
inline bool quietSiteDue(const char* format) noexcept {
    struct Site { std::atomic<const char*> format{nullptr}; std::atomic<unsigned long long> n{0}; };
    static Site sites[32];
    for (auto& s : sites) {
        const char* f = s.format.load(std::memory_order_acquire);
        if (!f) {
            const char* expected = nullptr;
            f = s.format.compare_exchange_strong(expected, format, std::memory_order_acq_rel) ? format : expected;
        }
        if (f == format) return s.n.fetch_add(1, std::memory_order_relaxed) % 16 == 0;
    }
    return true; // (more than 32 sites: logged as without the switch)
}

// __android_log_print for a line type nothing reads: not logged while quiet.
[[gnu::noinline]] [[gnu::format(printf, 3, 4)]] inline int quietLog(int priority, const char* tag, const char* format, ...) {
    if (quietSkip()) return 0;
    va_list a;
    va_start(a, format);
    quietLogEmit(priority, tag, format, a);
    va_end(a);
    return 1;
}
// __android_log_print at a v218-sampled site: while quiet, every 16th of this site's lines unless `keep` (its failure /
// refusal condition, logged every time as before).
[[gnu::noinline]] [[gnu::format(printf, 4, 5)]] inline int quietLogSampled(int keep, int priority, const char* tag, const char* format, ...) {
    if (!keep && quietSkip() && !quietSiteDue(format)) return 0;
    va_list a;
    va_start(a, format);
    quietLogEmit(priority, tag, format, a);
    va_end(a);
    return 1;
}
// __android_log_print for a line type tools read only in bulk (RENDER_DIAG_FILE_READ: world-read counts): every line; while
// quiet the first 64 of the run, then every 16th (one counter for its call sites).
[[gnu::noinline]] [[gnu::format(printf, 3, 4)]] inline int quietLogThin(int priority, const char* tag, const char* format, ...) {
    static std::atomic<unsigned long long> count{0};
    const auto k = count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (quietSkip() && k > 64 && k % 16 != 0) return 0;
    va_list a;
    va_start(a, format);
    quietLogEmit(priority, tag, format, a);
    va_end(a);
    return 1;
}
[[gnu::format(printf, 3, 4)]] inline void quietLogAlways(int priority, const char* tag, const char* format, ...) noexcept {
    va_list a;
    va_start(a, format);
    quietLogEmit(priority, tag, format, a);
    va_end(a);
}
// The 60-present window poll (generic_renderer.cpp, after awWindow()): the switch for the next window, then its line for
// the window that just ended (1 = the file was present at both ends; ab_windows.py / ab_joint.py merge V210Z lines).
#if defined(RECOMP_QUIET_LOG_TEST_FILE) // host test: the switch file's presence
bool RECOMP_QUIET_LOG_TEST_FILE() noexcept;
#endif
[[gnu::noinline]] inline void quietLogWindow() noexcept {
#if defined(__APPLE__)
    static bool file = RECOMP_PLAY_QUIET_AT_START || appleQuietDefault();
#else
    static bool file = false;
#endif
    const bool window = file;
#if defined(RECOMP_QUIET_LOG_TEST_FILE)
    file = RECOMP_QUIET_LOG_TEST_FILE(); RECOMP_PLAY_ONLY(file |= playWindowQuiet();)
#else
    file = ::access(RECOMP_APP_PATH("/private-trace/diag-quiet-log"), F_OK) == 0; RECOMP_PLAY_ONLY(file |= playWindowQuiet();) // play-config-2: quiet unless diag-play-loud (play_log.h)
#endif
#if defined(__APPLE__)
    file |= appleQuietDefault();
#endif
    quiet.store(file, std::memory_order_relaxed);
    quietLogAlways(4 /* ANDROID_LOG_INFO */, "RecompRenderer", "RENDER_GENERIC_V210Z quiet_log=%u", unsigned(window & file));
}
}
