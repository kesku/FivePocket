#pragma once
// world-load (2026-10-04; the user's decision: "yes, add the sliders that default to the original").
// Optional sliders for how much world the game simulates and draws. Every slider defaults to the ORIGINAL game: with the
// settings file absent (or every value 100) and no diag-wl-* switch file created during this launch (one that was already
// there when the game started counts for nothing: LEFTOVER SWITCH FILES below), this code reads and writes no guest memory
// except one read-only sample per 60 presents for the log line, scales nothing and changes no guest function's result.
//
// WHAT IS SCALED (addresses, writers, readers and frame order: local-analysis/agents/world-load/locate/KNOBS.md, confirmed by
// world-load/verify; every address below was derived from our own recompiled code, never from game source). The game's scripts
// already lower these per-frame multipliers through natives that keep the LOWER of the current value and their argument; the
// game resets each of them to 1.0 once per frame on its main thread (update list callbacks #33 sub_8223F728 and #34
// sub_82533758), then the scripts run, then the population code of the next frame reads them:
//   vehicles   0x8380B2BC  random / ambient vehicle density   (set_random_vehicle_density_multiplier_this_frame)
//   parked     0x8380B2C0  parked vehicle density              (set_parked_vehicle_density_multiplier_this_frame)
//   peds       0x837B8754  pedestrian density                  (set_ped_density_multiplier_this_frame)
//   scenario   0x837B8758 and 0x837B875C  scenario pedestrian density, both arguments (set_scenario_ped_density_..._this_frame)
//   lod        0x839E5258 + 0  the root of the level-of-detail scale (1.0 from the game's init, no other writer; the per-frame
//              update multiplies it into the scale it computes, a script's override included)
//
// HOW A VALUE MAPS: a percentage P becomes the multiplier P / 100 as an IEEE single (50 -> 0.5 = 0x3F000000), stored big-endian
// with ONE aligned 32-bit store. 100 = nothing is stored. Density sliders are clamped to 25..100, lod to 70..100 (world-load/
// verify: never 0 - the engine has separate branches for exactly 0 and for <= 0; the floors are its recommended ranges).
//
// WHERE IT IS APPLIED: world_load_hooks.inc wraps the two reset callbacks. After the game's own vehicle reset (#34, which runs
// directly after the pedestrian reset #33) each density word is replaced ONLY IF it holds exactly 1.0 (the reset's value). Every
// later script call then yields min(user, script) by the game's own rule, so a user value can never raise a value the game or a
// script lowered: a mission's 0 stays 0. The LOD root is persistent: it is replaced only when it holds exactly 1.0 or the value
// this code stored last, and 1.0 is put back when the slider returns to 100. Everything runs on the thread that runs the
// game's update list (its main thread): the same thread as the resets, the script natives and the readers, so there is no
// asynchronous writer. The renderer's 60-present window poll (window(), another thread) only publishes one atomic word.
//
// LOD IS LOCKED: world-load/verify confirmed the root but did NOT call it safe to scale (the same scale feeds vehicle AI
// ranges, pedestrian ranges and other distance tests, and no script writes the root). The lod value of the settings file is
// therefore honoured only while files/private-trace/diag-wl-lod-unlock counts (a device-verification switch, created during
// this launch); without it the LOD root is never touched. Unlock by default only after a device run has shown it safe.
//
// INTERFACE (read every 60 presents by window(), on the renderer's thread):
//   files/settings/world-load.txt   key=value tokens separated by blanks or line ends, values 0..100 (percent):
//        vehicles | traffic,  peds | pedestrians,  parked | parked_cars,  scenario | scenario_pedestrians,  lod | draw_distance
//      (both spellings are accepted: the second one is what the app's settings screen writes, RuntimeMapping.java).
//      Absent file, unreadable file, a file longer than 512 bytes, a missing key, an invalid value or 100 = that slider at the
//      original. A key given twice: the last one counts. Unknown keys are ignored.
//   files/private-trace/diag-wl-test            A/B aid: all four density sliders at 50 %
//   files/private-trace/diag-wl-test-min        A/B aid: all four density sliders at the floor (25 %)
//   files/private-trace/diag-wl-test-vehicles / -peds / -parked / -scenario      A/B aid: that one slider at 50 %
//   files/private-trace/diag-wl-test-lod        A/B aid: lod at 80 % (needs no unlock file)
//      While a diag-wl-test* file COUNTS the settings file is ignored: each slider is the lowest value such a test file
//      gives it, else 100. Remove the files and the settings file (or the original) is back at the next poll.
//
// LEFTOVER SWITCH FILES COUNT FOR NOTHING (review, 2026-10-04): the diag-wl-* files outlive the game process, and a killed A/B
// script leaves its switch behind (tools/iter/ab_toggle.sh, drive_ab.sh and alternate_drives.sh remove theirs only at their
// normal end). So each of the eight diag-wl-* files counts only after THIS process has seen it ABSENT at an earlier poll: a
// file that is already there when the game starts was not made for this launch. It is ignored (the settings file, or the
// original, stays in effect; the lod lock stays locked) and reported every window (RENDER_DIAG_WL leftover=<bits>) until it
// has been removed once; created again after that, it is a switch of this run. Consequences for whoever runs a test:
// create a diag-wl-* file only after the launch's first RENDER_DIAG_WL line (ab_toggle.sh does: it removes, waits, then
// creates); a whole-launch setting belongs in the settings file, which the app's screen shows and its Restore original removes.
// A switch left behind while the game keeps running stays in effect until that process ends (src=test in every window).
//
// LOG (per 60 presents): RENDER_GENERIC_V210Z wl_test=.. (1 = that file counted for the whole window that just ended)
// wl_vehicles=.. wl_peds=.. wl_parked=.. wl_scenario=.. wl_lod=.. (the percentages in effect during that window), then
// RENDER_DIAG_WL (leftover = the ignored leftover files at this poll, as bits: 1 test, 2 test-min, 4 test-vehicles,
// 8 test-peds, 16 test-parked, 32 test-scenario, 64 test-lod, 128 lod-unlock; that window's counters: writes / w_* = guest
// stores, lower_* = frames in which the game's own value ended lower than the user's, kept = frames it ended as the user's;
// hook = entries of the apply helper: 0 while the game runs its update list means the strong hook is NOT linked in - the
// link's redirect script still binds the wrapper to the body).
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include "recomp_app_dir.h" // data-paths: the app's files directory.
#if defined(__APPLE__)
// ios-draw-distance (Claude, 2026-10-20): the iPad launcher's saved Draw Distance in percent (100 = Original or no saved choice),
// defined by generic_renderer.cpp from the launcher word. The owner made it a player option ("Original by default, down to
// 70 %"), so it needs no diag-wl-lod-unlock; the lower of it and an unlocked settings-file lod counts; a test switch decides alone.
extern "C" unsigned recomp_ios_wl_launcher_lod() noexcept;
// ios-traffic (Claude, 2026-10-20): the launcher's saved Traffic Density in percent (100 = Original), for the random and
// parked vehicle sliders; the lower of it and the settings file counts; a test switch decides alone.
extern "C" unsigned recomp_ios_wl_launcher_traffic() noexcept;
#endif
#ifndef RECOMP_WL_FILES_DIR // the app's files directory (the one place this header names the package)
#define RECOMP_WL_PATH(rel) RECOMP_APP_PATH(rel) // data-paths (host tests: recomp_app_dir.h)
#endif

namespace recomp::wl {
enum Slider : unsigned { Vehicles, Peds, Parked, Scenario, Lod, SliderCount };
// The guest words (big-endian floats in the title image's data).
constexpr std::uint32_t VehicleDensity = 0x8380B2BCu, ParkedDensity = 0x8380B2C0u, PedDensity = 0x837B8754u,
    ScenarioDensity0 = 0x837B8758u, ScenarioDensity1 = 0x837B875Cu, LodRoot = 0x839E5258u;
constexpr unsigned DensityWords = 5;
constexpr std::uint32_t WordAddress[DensityWords]{VehicleDensity, ParkedDensity, PedDensity, ScenarioDensity0, ScenarioDensity1};
constexpr Slider WordSlider[DensityWords]{Vehicles, Parked, Peds, Scenario, Scenario};
// Read-only words for the log line (KNOBS.md section 7): the effective vehicle density product (float), live ambient and
// parked vehicle counts, the two desired pedestrian counts, the scenario base (ints), the effective LOD scale (float).
constexpr std::uint32_t SampleAddress[]{0x8380B370u, 0x83A5DD4Cu, 0x83A5DD54u, 0x8394B7CCu, 0x8394B85Cu, 0x8394B7C8u, 0x839E5284u};
constexpr unsigned SampleWords = sizeof SampleAddress / sizeof SampleAddress[0];
constexpr std::uint32_t One = 0x3F800000u; // 1.0f
constexpr unsigned DensityFloor = 25, LodFloor = 70, TestDensity = 50, TestLod = 80, MaxFileBytes = 512;
static_assert(DensityFloor > 0 && LodFloor > 0 && TestDensity >= DensityFloor && TestLod >= LodFloor, "never 0, tests inside the ranges");

enum Switch : unsigned {
    SwTest = 1u, SwTestMin = 2u, SwTestVehicles = 4u, SwTestPeds = 8u, SwTestParked = 16u, SwTestScenario = 32u, SwTestLod = 64u,
    SwLodUnlock = 128u, SwPedsHalf = 256u, SwLod85 = 512u, SwAnyTest = 127u, SwAll = 1023u
};

// ---- logging -------------------------------------------------------------------------------------------------------------
[[gnu::format(printf, 1, 2)]] inline void logLine(const char* format, ...) noexcept {
    va_list a;
    va_start(a, format);
#if defined(__ANDROID__)
    __android_log_vprint(ANDROID_LOG_INFO, "RecompRenderer", format, a);
#else
    std::vfprintf(stderr, format, a);
    std::fputc('\n', stderr);
#endif
    va_end(a);
}

// ---- the settings file ---------------------------------------------------------------------------------------------------
struct FileSettings {
    std::uint8_t percent[SliderCount]{100, 100, 100, 100, 100};
    unsigned bad = 0, clamped = 0; // invalid values of known keys; values raised to a floor
};

inline int sliderOf(const char* key, std::size_t size) noexcept {
    struct Name { const char* text; Slider slider; };
    static constexpr Name names[]{{"vehicles", Vehicles}, {"traffic", Vehicles}, {"peds", Peds}, {"pedestrians", Peds},
        {"parked", Parked}, {"parked_cars", Parked}, {"scenario", Scenario}, {"scenario_pedestrians", Scenario},
        {"lod", Lod}, {"draw_distance", Lod}};
    for (const auto& n : names)
        if (std::strlen(n.text) == size && !std::memcmp(n.text, key, size)) return int(n.slider);
    return -1;
}

inline FileSettings parse(const char* text, std::size_t size) noexcept {
    FileSettings s;
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t i = 0;
    while (i < size) {
        while (i < size && blank(text[i])) ++i;
        const std::size_t start = i;
        while (i < size && !blank(text[i])) ++i;
        if (i == start) break;
        std::size_t eq = start;
        while (eq < i && text[eq] != '=') ++eq;
        const int slider = eq < i ? sliderOf(text + start, eq - start) : -1;
        if (slider < 0) continue; // no known key: ignored
        const std::size_t digits = i - eq - 1;
        bool valid = digits >= 1 && digits <= 3;
        unsigned value = 0;
        for (std::size_t k = eq + 1; valid && k < i; ++k) {
            if (text[k] < '0' || text[k] > '9') valid = false;
            else value = value * 10 + unsigned(text[k] - '0');
        }
        if (!valid || value > 100) { s.percent[slider] = 100; ++s.bad; continue; } // not a percentage: that slider at the original
        const unsigned floor = slider == int(Lod) ? LodFloor : DensityFloor;
        if (value < floor) { value = floor; ++s.clamped; } // the verified range (never 0)
        s.percent[slider] = std::uint8_t(value);
    }
    return s;
}

enum FileState : unsigned { FileAbsent, FileOk, FileUnreadable };

// One open / read / close of a small regular file; anything else (a directory, a FIFO, a read error, more than MaxFileBytes)
// counts as unreadable = every slider at the original. Creates nothing.
inline FileState readSettings(const char* path, FileSettings& out) noexcept {
    out = FileSettings{};
#if defined(__APPLE__) // ios-wl-text (Claude, 2026-10-20; iOS test runs only): the launch environment's text stands for the settings
    // file (the iOS shell cannot write the app's files); without the variable the file is read as before
    if (const char* text = std::getenv("RECOMP_IOS_WL_TEXT")) {
        const std::size_t size = std::strlen(text);
        if (size > MaxFileBytes) return FileUnreadable;
        out = parse(text, size);
        return FileOk;
    }
#endif
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? FileAbsent : FileUnreadable;
    char text[MaxFileBytes + 1];
    std::size_t used = 0;
    struct stat st {};
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    while (ok && used < sizeof text) {
        const ssize_t n = ::read(fd, text + used, sizeof text - used);
        if (n > 0) used += std::size_t(n);
        else if (n == 0) break;
        else if (errno != EINTR) ok = false;
    }
    ::close(fd);
    if (!ok || used > MaxFileBytes) return FileUnreadable;
    out = parse(text, used);
    return FileOk;
}

// The switch files as one poll finds them. absent = the lookup said "no such file"; a lookup that failed for another reason
// (an unreadable directory, an I/O error) is neither: such a file is not in effect, and it was not seen absent either.
struct Seen { unsigned present = 0, absent = 0; };

inline Seen pollSwitches() noexcept {
    struct File { const char* path; unsigned bit; };
    static constexpr File files[]{
        {RECOMP_WL_PATH("/private-trace/diag-wl-test"), SwTest},
        {RECOMP_WL_PATH("/private-trace/diag-wl-test-min"), SwTestMin},
        {RECOMP_WL_PATH("/private-trace/diag-wl-test-vehicles"), SwTestVehicles},
        {RECOMP_WL_PATH("/private-trace/diag-wl-test-peds"), SwTestPeds},
        {RECOMP_WL_PATH("/private-trace/diag-wl-test-parked"), SwTestParked},
        {RECOMP_WL_PATH("/private-trace/diag-wl-test-scenario"), SwTestScenario},
        {RECOMP_WL_PATH("/private-trace/diag-wl-test-lod"), SwTestLod},
        {RECOMP_WL_PATH("/private-trace/diag-wl-lod-unlock"), SwLodUnlock},
        // ios-wl-peds50 (Claude 2026-10-20, A/B aid): pedestrians and scenario peds at 50 % ON TOP of the settings file (unlike
        // the diag-wl-test* files it does not replace them), to measure their CPU cost in the now CPU-limited drive
        {RECOMP_WL_PATH("/private-trace/ios-wl-peds50"), SwPedsHalf},
        // ios-wl-lod85 (Claude 2026-10-22, A/B aid): draw distance (lod) at 85 % ON TOP of the settings file (the densities stay as
        // the file sets them), for an in-run A/B of lod 85 against the play settings' 70 (the owner: "the draw distance is atrocious")
        {RECOMP_WL_PATH("/private-trace/ios-wl-lod85"), SwLod85}};
    Seen seen;
    for (const auto& f : files) {
        if (::access(f.path, F_OK) == 0) seen.present |= f.bit;
        else if (errno == ENOENT || errno == ENOTDIR) seen.absent |= f.bit;
    }
    return seen;
}

// Launch scope (see LEFTOVER SWITCH FILES in the header comment). seenAbsent: the files this process has seen absent so far
// (0 at its start). live = the present files that count; leftover = the present files that were never seen absent.
struct Scoped { unsigned live, leftover; };
inline Scoped launchScope(const Seen& seen, unsigned& seenAbsent) noexcept {
    const Scoped scoped{seen.present & seenAbsent, seen.present & ~seenAbsent};
    seenAbsent |= seen.absent & SwAll;
    return scoped;
}

// The percentages in effect for a settings file and a set of switch files.
struct Effective { std::uint8_t percent[SliderCount]{100, 100, 100, 100, 100}; };

inline Effective resolve(const FileSettings& file, unsigned switches) noexcept {
    Effective e;
    if (switches & SwAnyTest) { // a test file: the settings file is ignored
        const auto lower = [&](Slider s, unsigned value) { if (value < e.percent[s]) e.percent[s] = std::uint8_t(value); };
        const auto densities = [&](unsigned value) { lower(Vehicles, value); lower(Peds, value); lower(Parked, value); lower(Scenario, value); };
        if (switches & SwTest) densities(TestDensity);
        if (switches & SwTestMin) densities(DensityFloor);
        if (switches & SwTestVehicles) lower(Vehicles, TestDensity);
        if (switches & SwTestPeds) lower(Peds, TestDensity);
        if (switches & SwTestParked) lower(Parked, TestDensity);
        if (switches & SwTestScenario) lower(Scenario, TestDensity);
        if (switches & SwTestLod) lower(Lod, TestLod);
        return e;
    }
    e.percent[Vehicles] = file.percent[Vehicles];
    e.percent[Peds] = file.percent[Peds];
    e.percent[Parked] = file.percent[Parked];
    e.percent[Scenario] = file.percent[Scenario];
    if (switches & SwLodUnlock) e.percent[Lod] = file.percent[Lod]; // locked: see the header comment
    if (switches & SwPedsHalf) // (ios-wl-peds50)
        for (const Slider s : {Peds, Scenario}) if (e.percent[s] > TestDensity) e.percent[s] = std::uint8_t(TestDensity);
    if (switches & SwLod85) e.percent[Lod] = std::uint8_t(85); // (ios-wl-lod85)
    return e;
}

// ---- shared state ----------------------------------------------------------------------------------------------------------
// control: published by window() (the renderer's thread), read by the hook (the game's main thread). Slider s occupies bits
// 7 s .. 7 s + 6 as (100 - percent); SampleBit asks the hook for one read-only sample. 0 = every slider at the original and
// nothing to do. Everything else in State is written by the hook's thread only and read by window() for the log line.
constexpr std::uint64_t SampleBit = 1ull << 63;
inline std::uint64_t pack(const Effective& e) noexcept {
    std::uint64_t control = 0;
    for (unsigned s = 0; s < SliderCount; ++s) control |= std::uint64_t(100u - e.percent[s]) << (7 * s);
    return control;
}
inline unsigned percentOf(std::uint64_t control, Slider s) noexcept { return 100u - unsigned((control >> (7 * unsigned(s))) & 127u); }

struct State {
    std::atomic<std::uint64_t> control{0};
    std::atomic<std::uint32_t> armed{0};                 // nonzero while a density word of the last apply holds a value of ours
    std::atomic<std::uint32_t> wrote[DensityWords]{};    // the bits stored into each density word by the last apply (0 = none)
    std::atomic<std::uint32_t> lodHeld{0};               // the bits this code stored in the LOD root and still owns (0 = none)
    std::atomic<std::uint64_t> entries{0}, observes{0}, samples{0}, lodForeign{0}, lodLost{0};
    std::atomic<std::uint64_t> writes[DensityWords + 1]{}; // guest stores per density word, then the LOD root
    std::atomic<std::uint64_t> kept[DensityWords]{};     // frames whose final value was ours
    std::atomic<std::uint64_t> lower[DensityWords]{};    // frames in which the game's own (script) value ended lower than ours
    std::atomic<std::uint64_t> other[DensityWords]{};    // frames whose final value was neither (an unknown writer: expected 0)
    std::atomic<std::uint64_t> notOne[DensityWords]{};   // applies skipped because the word did not hold 1.0 after the reset
    std::atomic<std::uint32_t> sample[SampleWords]{}, sampleWord[DensityWords + 1]{};
};
inline State shared;

inline void bump(std::atomic<std::uint64_t>& counter) noexcept { // single writer (the hook's thread): no atomic read-modify-write
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// ---- guest memory (aligned 32-bit big-endian words of the title image's data) --------------------------------------------------
inline std::uint32_t guestWord(const std::uint8_t* base, std::uint32_t address) noexcept {
    return __builtin_bswap32(__atomic_load_n(reinterpret_cast<const std::uint32_t*>(base + address), __ATOMIC_RELAXED));
}
// ONE aligned 32-bit store (never byte by byte: no reader may see a torn float).
inline void setGuestWord(std::uint8_t* base, std::uint32_t address, std::uint32_t value) noexcept {
    __atomic_store_n(reinterpret_cast<std::uint32_t*>(base + address), __builtin_bswap32(value), __ATOMIC_RELAXED);
}
inline std::uint32_t multiplierBits(unsigned percent) noexcept {
    const float multiplier = float(percent) / 100.0f;
    std::uint32_t bits;
    std::memcpy(&bits, &multiplier, 4);
    return bits;
}
inline float asFloat(std::uint32_t bits) noexcept { float f; std::memcpy(&f, &bits, 4); return f; }

// ---- the hook's side (world_load_hooks.inc; the thread that runs the game's update list) -------------------------------------
// Before a reset: what the previous frame ended with in a word this code had stored into (read-only).
inline void observeWord(const std::uint8_t* base, unsigned word) noexcept {
    const std::uint32_t ours = shared.wrote[word].load(std::memory_order_relaxed);
    if (!ours) return;
    const std::uint32_t now = guestWord(base, WordAddress[word]);
    if (now == ours) bump(shared.kept[word]);
    else bump(asFloat(now) < asFloat(ours) ? shared.lower[word] : shared.other[word]);
}
[[gnu::noinline]] inline void beforePedReset(const std::uint8_t* base) noexcept {
    bump(shared.observes);
    if (!shared.armed.load(std::memory_order_relaxed)) return; // nothing of ours anywhere: no guest access
    observeWord(base, 2); observeWord(base, 3); observeWord(base, 4);
}
[[gnu::noinline]] inline void beforeVehicleReset(const std::uint8_t* base) noexcept {
    if (!shared.armed.load(std::memory_order_relaxed)) return; // nothing of ours anywhere: no guest access
    observeWord(base, 0); observeWord(base, 1);
}

[[gnu::noinline]] inline void applySliders(std::uint8_t* base, std::uint64_t control) noexcept {
    State& s = shared;
    std::uint32_t armed = 0;
    for (unsigned word = 0; word < DensityWords; ++word) {
        const unsigned percent = percentOf(control, WordSlider[word]);
        std::uint32_t stored = 0;
        if (percent < 100u) {
            // Only an exact 1.0 (the value the game's reset just stored) is replaced: a value the game or a script lowered is
            // never raised, and the scripts' later min() keeps the lower of the user's value and their own.
            if (guestWord(base, WordAddress[word]) == One) {
                stored = multiplierBits(percent);
                setGuestWord(base, WordAddress[word], stored);
                bump(s.writes[word]);
            } else {
                bump(s.notOne[word]);
            }
        }
        s.wrote[word].store(stored, std::memory_order_relaxed);
        armed |= stored;
    }
    s.armed.store(armed, std::memory_order_relaxed);
    // The LOD root is persistent (the game's init stores 1.0 and nothing else writes it): replace only 1.0 or our own last
    // value; put 1.0 back when the slider returns to 100; a root the game rewrote is the game's again.
    const unsigned lodPercent = percentOf(control, Lod);
    const std::uint32_t want = lodPercent < 100u ? multiplierBits(lodPercent) : One;
    std::uint32_t held = s.lodHeld.load(std::memory_order_relaxed);
    if (held || want != One) {
        const std::uint32_t now = guestWord(base, LodRoot);
        if (held && now != held) { held = 0; bump(s.lodLost); }
        const std::uint32_t replaceable = held ? held : One;
        if (now == replaceable) {
            if (want != now) { setGuestWord(base, LodRoot, want); bump(s.writes[DensityWords]); }
            held = want == One ? 0u : want;
        } else if (want != One) {
            bump(s.lodForeign); // neither 1.0 nor ours (0.0 before the game's init, or an unknown writer): left alone
        }
        s.lodHeld.store(held, std::memory_order_relaxed);
    }
    if (control & SampleBit) { // read-only: the words the log line reports
        for (unsigned i = 0; i < SampleWords; ++i) s.sample[i].store(guestWord(base, SampleAddress[i]), std::memory_order_relaxed);
        for (unsigned word = 0; word < DensityWords; ++word) s.sampleWord[word].store(guestWord(base, WordAddress[word]), std::memory_order_relaxed);
        s.sampleWord[DensityWords].store(guestWord(base, LodRoot), std::memory_order_relaxed);
        bump(s.samples);
        std::uint64_t expected = control; // (a word published meanwhile keeps its own request)
        s.control.compare_exchange_strong(expected, control & ~SampleBit, std::memory_order_relaxed);
    }
}

// After the game's own vehicle reset (update #34). Everything at the original: one counter, one load, return.
[[gnu::noinline]] inline void afterVehicleReset(std::uint8_t* base) noexcept {
    bump(shared.entries);
    const std::uint64_t control = shared.control.load(std::memory_order_relaxed);
    if (!control) return;
    applySliders(base, control);
}

// ---- the renderer's 60-present window poll -------------------------------------------------------------------------------------
[[gnu::noinline]] inline void window() noexcept {
    struct Totals { std::uint64_t entries, observes, samples, lodForeign, lodLost, writes[DensityWords + 1], kept[DensityWords], lower[DensityWords], other, notOne; };
    struct Poll { unsigned live = 0, seenAbsent = 0; Effective effective{}; Totals last{}; bool launcher = false; };
    static Poll poll;
    State& s = shared;
    // the next window's setting (a switch file left by an earlier run counts for nothing: launchScope)
    const Scoped scoped = launchScope(pollSwitches(), poll.seenAbsent);
    const unsigned live = scoped.live;
    FileSettings parsed;
    const FileState file = readSettings(RECOMP_WL_PATH("/settings/world-load.txt"), parsed);
    Effective next = resolve(parsed, live);
    bool launcherLod = false; // (ios-draw-distance / ios-traffic: a slider of this window came from the launcher)
#if defined(__APPLE__)
    if (!(live & SwAnyTest)) {
        unsigned launcher = recomp_ios_wl_launcher_lod();
        if (launcher < LodFloor) launcher = LodFloor;
        if (launcher < next.percent[Lod]) { next.percent[Lod] = std::uint8_t(launcher); launcherLod = true; }
        unsigned traffic = recomp_ios_wl_launcher_traffic();
        if (traffic < DensityFloor) traffic = DensityFloor;
        for (const Slider s : {Vehicles, Parked})
            if (traffic < next.percent[s]) { next.percent[s] = std::uint8_t(traffic); launcherLod = true; }
    }
#endif
    s.control.store(pack(next) | SampleBit, std::memory_order_relaxed);
    // the window that just ended
    const unsigned whole = poll.live & live; // (a file that counted at both ends)
    const Effective& was = poll.effective;
    logLine("RENDER_GENERIC_V210Z wl_test=%u wl_test_min=%u wl_test_vehicles=%u wl_test_peds=%u wl_test_parked=%u wl_test_scenario=%u "
            "wl_test_lod=%u wl_lod_unlock=%u wl_vehicles=%u wl_peds=%u wl_parked=%u wl_scenario=%u wl_lod=%u",
            unsigned((whole & SwTest) != 0), unsigned((whole & SwTestMin) != 0), unsigned((whole & SwTestVehicles) != 0),
            unsigned((whole & SwTestPeds) != 0), unsigned((whole & SwTestParked) != 0), unsigned((whole & SwTestScenario) != 0),
            unsigned((whole & SwTestLod) != 0), unsigned((whole & SwLodUnlock) != 0), unsigned(was.percent[Vehicles]),
            unsigned(was.percent[Peds]), unsigned(was.percent[Parked]), unsigned(was.percent[Scenario]), unsigned(was.percent[Lod]));
    const auto get = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    Totals now{};
    now.entries = get(s.entries); now.observes = get(s.observes); now.samples = get(s.samples);
    now.lodForeign = get(s.lodForeign); now.lodLost = get(s.lodLost);
    for (unsigned i = 0; i <= DensityWords; ++i) now.writes[i] = get(s.writes[i]);
    for (unsigned i = 0; i < DensityWords; ++i) {
        now.kept[i] = get(s.kept[i]); now.lower[i] = get(s.lower[i]);
        now.other += get(s.other[i]); now.notOne += get(s.notOne[i]);
    }
    const Totals& l = poll.last;
    const auto d = [](std::uint64_t a, std::uint64_t b) { return static_cast<unsigned long long>(a - b); };
    unsigned long long writes = 0, kept = 0;
    for (unsigned i = 0; i <= DensityWords; ++i) writes += d(now.writes[i], l.writes[i]);
    for (unsigned i = 0; i < DensityWords; ++i) kept += d(now.kept[i], l.kept[i]);
    const bool original = pack(was) == 0;
    const auto word = [&](unsigned i) { return double(asFloat(s.sampleWord[i].load(std::memory_order_relaxed))); };
    const auto sampled = [&](unsigned i) { return s.sample[i].load(std::memory_order_relaxed); };
    // src / the five percentages: the window that just ended; file / bad / clamped: the settings file as read at this poll;
    // leftover: the ignored leftover switch files at this poll (bits as in enum Switch); counters: that window; now_* / root:
    // the control words right after the apply of the sampled frame; eff_veh .. lod_eff: the game's own words (read-only) in
    // that frame.
    logLine("RENDER_DIAG_WL src=%s file=%s leftover=%u vehicles=%u peds=%u parked=%u scenario=%u lod=%u hook=%llu obs=%llu writes=%llu "
            "w_vehicles=%llu w_parked=%llu w_peds=%llu w_scen0=%llu w_scen1=%llu w_lod=%llu kept=%llu "
            "lower_vehicles=%llu lower_parked=%llu lower_peds=%llu lower_scen0=%llu lower_scen1=%llu other=%llu not_one=%llu "
            "lod_foreign=%llu lod_lost=%llu lod_held=%.2f bad=%u clamped=%u sampled=%llu "
            "now_vehicles=%.3f now_parked=%.3f now_peds=%.3f now_scen0=%.3f now_scen1=%.3f root=%.3f "
            "eff_veh=%.4f veh_live=%d parked_live=%d ped_want=%d ped_want2=%d scen_base=%d lod_eff=%.4f",
            original ? "original" : (poll.live & SwAnyTest) ? "test" : poll.launcher ? "launcher" : "file",
            file == FileOk ? "ok" : file == FileAbsent ? "absent" : "unreadable", scoped.leftover,
            unsigned(was.percent[Vehicles]), unsigned(was.percent[Peds]), unsigned(was.percent[Parked]), unsigned(was.percent[Scenario]),
            unsigned(was.percent[Lod]), d(now.entries, l.entries), d(now.observes, l.observes), writes,
            d(now.writes[0], l.writes[0]), d(now.writes[1], l.writes[1]), d(now.writes[2], l.writes[2]), d(now.writes[3], l.writes[3]),
            d(now.writes[4], l.writes[4]), d(now.writes[5], l.writes[5]), kept,
            d(now.lower[0], l.lower[0]), d(now.lower[1], l.lower[1]), d(now.lower[2], l.lower[2]), d(now.lower[3], l.lower[3]),
            d(now.lower[4], l.lower[4]), d(now.other, l.other), d(now.notOne, l.notOne),
            d(now.lodForeign, l.lodForeign), d(now.lodLost, l.lodLost), double(asFloat(s.lodHeld.load(std::memory_order_relaxed))),
            parsed.bad, parsed.clamped, d(now.samples, l.samples),
            word(0), word(1), word(2), word(3), word(4), word(5), double(asFloat(sampled(0))), int(sampled(1)), int(sampled(2)),
            int(sampled(3)), int(sampled(4)), int(sampled(5)), double(asFloat(sampled(6))));
    poll.live = live;
    poll.effective = next;
    poll.launcher = launcherLod;
    poll.last = now;
}
} // namespace recomp::wl
