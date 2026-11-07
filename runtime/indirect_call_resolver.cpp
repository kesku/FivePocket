#include "poll_sleep_v266.h" // v270 (the clock A/B flag)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <android/log.h>
#include <algorithm>
#include <vector>
#include <array>
#include "indirect_call_resolver.h"
#include "bot_vision_scan.h" // bot-vision (2026-10-03): RENDER_DIAG_NEAR, read-only entity pool scan
#include "recomp_app_dir.h" // data-paths: the app's files directory.
#include "guest_execution_guard.h"
#include "ppc_config.h"
#include "ppc_context.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <atomic>
extern "C" std::atomic<int> recomp_diag_loading_screen_shutdown;
std::atomic<int> recomp_diag_loading_screen_shutdown{0};
// v216 A/B: false while files/private-trace/diag-v216-baseline exists (the renderer applies it every 60 presents): no
// fast-form entries (hits and fills use the index form, v215's path through the slow function).
extern "C" std::atomic<bool> recomp_indirect_fast;
std::atomic<bool> recomp_indirect_fast{true};
extern "C" std::atomic<bool> recomp_v217_baseline; // v217 A/B (guest_import_registry.cpp)

namespace {
// Attempt v12: direct-mapped cache in front of the binary search over ~95K mappings (every virtual call paid ~17
// dependent loads). One 64-bit atomic per entry with the guest address in the high half, so concurrent fills can never
// produce a torn guest/function pair. v216: two forms, told apart by bit 0 of the low half:
//   bit 0 = 1: (the host function's offset from recomp_host_guest_call_indirect, in 4-byte units, signed 31 bits) << 1
//              | 1, for a function without a native probe: the entry point calls it with no mapping-table or probe-slot
//              load and no frame of its own (v215 house profile: the resolver was 2.2% of the render thread, 3% of main,
//              2-4.6% of the scene workers, mostly those two cache misses and the telemetry code's 2.7 KB frame);
//   bit 0 = 0: (mapping index + 1) << 1, for a function with a probe (the slow path runs the probe/telemetry code).
// A probe registration clears its handler's entry; a fill re-checks the probe slot after storing (either order ends
// with no fast entry for a probed handler). Resets clear the cache.
std::array<std::atomic<std::uint64_t>,16384> indirectCache{};
// calls (package E, indirect_dispatch_patch.py): the 2-way set-associative cache (bit 0 of recomp_calls_dispatch).
constexpr std::uint32_t IndirectSets2 = 32768;
alignas(64) std::array<std::atomic<std::uint64_t>,2 * IndirectSets2> indirectCache2{};
inline std::atomic<std::uint64_t>* indirectSet2(std::uint32_t guestTarget) noexcept {
    return &indirectCache2[((guestTarget >> 2) & (IndirectSets2 - 1)) * 2];
}
// A fast-form entry for guestTarget (the same value just stored in the v216 cache) goes to the way already holding its
// tag, else an empty way, else a pseudo-random way (the virtual counter's low bits; a miss takes far longer than one
// tick). A fill only ever writes ITS OWN entry (never copies another way), so v216's probe protocol carries over
// unchanged: the filler re-checks its own probe slot after the release store. Concurrent fills may overwrite each other:
// every entry is self-validating (its tag), so a lost one is only a later miss.
inline unsigned replaceWay2() noexcept {
#if defined(__aarch64__)
    std::uint64_t t; asm volatile("mrs %0, cntvct_el0" : "=r"(t)); return unsigned(t ^ (t >> 7)) & 1u;
#else
    static std::atomic<unsigned> n{0}; return n.fetch_add(1, std::memory_order_relaxed) & 1u;
#endif
}
inline void fill2(std::uint32_t guestTarget, std::uint64_t entry) noexcept {
    auto* set = indirectSet2(guestTarget);
    const auto e0 = set[0].load(std::memory_order_relaxed), e1 = set[1].load(std::memory_order_relaxed);
    unsigned way;
    if ((e0 >> 32) == guestTarget) way = 0;
    else if ((e1 >> 32) == guestTarget) way = 1;
    else if (!e0) way = 0;
    else if (!e1) way = 1;
    else way = replaceWay2();
    set[way].store(entry, std::memory_order_release);
}
inline void clear2(std::uint32_t guestTarget) noexcept { // both ways of the set (v216 clears its slot unconditionally too)
    auto* set = indirectSet2(guestTarget);
    set[0].store(0, std::memory_order_release); set[1].store(0, std::memory_order_release);
}
void clearIndirectCache() noexcept {for(auto& e:indirectCache)e.store(0,std::memory_order_release);for(auto& e:indirectCache2)e.store(0,std::memory_order_release);} // (calls: both caches)
// Lanes audit L2: sized by initializeGuestIndirectCallResolver before any guest thread starts and
// released by reset after they all joined (defined with the probe below).
void sizeNativeProbeSlots(std::size_t count) noexcept;
bool jumpTableRepairEnabled() noexcept; // v101, defined with recomp_jump_table_repair below
}

namespace recomp::bootstrap {
namespace {

std::atomic_size_t validatedMappingCount{0};
std::mutex rejectedCallMutex;
std::optional<InvalidGuestIndirectCall> rejectedCall;

bool configuredCodeRange(std::uint64_t& begin, std::uint64_t& end) noexcept {
    begin = static_cast<std::uint64_t>(PPC_CODE_BASE);
    const auto size = static_cast<std::uint64_t>(PPC_CODE_SIZE);
    constexpr std::uint64_t GuestAddressSpaceSize = 1ULL << 32U;
    if (size == 0 || begin >= GuestAddressSpaceSize
            || size > GuestAddressSpaceSize - begin) {
        return false;
    }
    end = begin + size;
    return true;
}

} // namespace

bool initializeGuestIndirectCallResolver(
        std::size_t expectedMappingCount,
        std::size_t& outputMappingCount) noexcept {
    validatedMappingCount.store(0, std::memory_order_release);clearIndirectCache(); // (v216)
    { std::lock_guard lock(rejectedCallMutex); rejectedCall.reset(); }
    outputMappingCount = 0;
    if (expectedMappingCount == 0
            || expectedMappingCount > MaximumGuestFunctionMappings) {
        return false;
    }

    std::uint64_t codeBegin = 0;
    std::uint64_t codeEnd = 0;
    if (!configuredCodeRange(codeBegin, codeEnd)) {
        return false;
    }
    std::uint64_t previousAddress = 0;
    for (std::size_t index = 0; index < expectedMappingCount; ++index) {
        const auto& mapping = PPCFuncMappings[index];
        const auto address = static_cast<std::uint64_t>(mapping.guest);
        if (mapping.host == nullptr
                || address < codeBegin
                || address >= codeEnd
                || address > std::numeric_limits<std::uint32_t>::max()
                || (address & 3U) != 0
                || (index != 0 && address <= previousAddress)) {
            return false;
        }
        previousAddress = address;
    }
    const auto& sentinel = PPCFuncMappings[expectedMappingCount];
    if (sentinel.guest != 0 || sentinel.host != nullptr) {
        return false;
    }

    outputMappingCount = expectedMappingCount;
    (void)jumpTableRepairEnabled(); // logs RENDER_DIAG_JUMP_TABLE_REPAIR enabled=
    sizeNativeProbeSlots(expectedMappingCount); // Before any worker: published by thread start.
    validatedMappingCount.store(expectedMappingCount, std::memory_order_release);
    return true;
}

void resetGuestIndirectCallResolver() noexcept {
    validatedMappingCount.store(0, std::memory_order_release);clearIndirectCache(); // (v216)
    sizeNativeProbeSlots(0); // After every guest thread joined.
    std::lock_guard lock(rejectedCallMutex); rejectedCall.reset();
}

std::optional<InvalidGuestIndirectCall> stoppedGuestIndirectCall() {
    std::lock_guard lock(rejectedCallMutex); return rejectedCall;
}

bool hasExactGuestFunction(std::uint32_t guestTarget) noexcept {
    const auto count = validatedMappingCount.load(std::memory_order_acquire);
    std::size_t first = 0, last = count;
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        if (PPCFuncMappings[middle].guest < guestTarget) { first = middle + 1; }
        else { last = middle; }
    }
    return first < count && PPCFuncMappings[first].guest == guestTarget
            && PPCFuncMappings[first].host != nullptr;
}

} // namespace recomp::bootstrap

// v101: jump tables the recompiler emitted as a bare indirect branch although every target lies inside
// the dispatching function (sub_82221FB0 table 82222298 stopped the prologue at the garage shootout
// with invalid-indirect-target 8222235C; sub_831A0348 table 831A04F4 is the same idiom). The repaired
// generated code asks here before jumping to its local labels. files/private-trace/diag-no-jump-table-repair
// at start restores v99 (the resolver refuses the target and the game stops).
namespace {
std::atomic<unsigned> jumpTableRepairHits{0};
bool jumpTableRepairEnabled() noexcept {
    static const bool enabled = [] {
        const bool on = ::access(RECOMP_APP_PATH("/private-trace/diag-no-jump-table-repair"), F_OK) != 0;
        __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_DIAG_JUMP_TABLE_REPAIR enabled=%d sites=2", on ? 1 : 0);
        return on;
    }();
    return enabled;
}
} // namespace

extern "C" bool recomp_jump_table_repair(std::uint32_t site, std::uint32_t target) noexcept {
    if (!jumpTableRepairEnabled()) return false;
    const auto hits = jumpTableRepairHits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hits <= 16 || (hits & (hits - 1)) == 0) {
        __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_DIAG_JUMP_TABLE_REPAIR site=%08x target=%08x hits=%u",
                            site, target, hits);
    }
    return true;
}

// Attempt v10 probe: script native calls. The title registers each native (Jenkins hash of the
// lowercase name, handler) through sub_83438FE8; handlers take r3 = scrNativeCallContext* and write
// their result to *(ctx+0). Every native call reaches this resolver through the script VM's indirect
// call, so one hook sees them all. Counters are relaxed atomics: diagnostics, not synchronization.
namespace {
struct NativeProbe {
    // Written before the owning nativeSlot entry is published (release), read after it (acquire).
    std::atomic<std::uint32_t> hash{0},handler{0};
    std::atomic<unsigned long long> calls{0},reported{0};
    std::atomic<std::uint32_t> last{0},arg0{0};std::atomic<unsigned> changes{0};
};
std::array<NativeProbe,8192> nativeProbe;std::atomic<unsigned> nativeProbeCount{0};
// Mapping index -> probe slot + 1 (0: not a native). Lanes audit L2: formerly a std::vector that the
// first registration assign()ed while every other permit domain read it on each indirect call (size
// and data seen mid-reallocation; an unpublished element used as an index into nativeProbe). Now a
// fixed array of atomics sized before guest threads start; entries are published with release.
std::unique_ptr<std::atomic<std::uint16_t>[]> nativeSlot;std::size_t nativeSlotCount=0;
void sizeNativeProbeSlots(std::size_t count) noexcept {
    nativeSlot.reset();nativeSlotCount=0;
    if(!count)return;
    try {nativeSlot.reset(new std::atomic<std::uint16_t>[count]);} catch(...) {return;} // Probe off, calls unaffected.
    for(std::size_t i=0;i<count;++i)nativeSlot[i].store(0,std::memory_order_relaxed);
    nativeSlotCount=count;
}
std::atomic<std::uint32_t> playerPed{0};std::array<std::atomic<std::uint32_t>,3> playerXyz{};
std::atomic<unsigned long long> playerReads{0},playerMoves{0};
// Attempt v153: play telemetry for the play bot (files/private-trace/diag-telemetry, checked once a second): the player's
// state as the mission scripts read it, and the coordinates/areas the scripts test entities against (objectives), logged
// as RENDER_DIAG_TELEMETRY every 100 ms. Read-only: arguments are copied before the call, results after it.
namespace telemetry {
constexpr std::uint32_t Health=0x8e3222b7u,Wanted=0xbdcdd163u,InVehicle=0x3b0171eeu,Cutscene=0xa3a78392u,Faded=0x9caa05fau,
    Control=0x618857f2u,AtCoord=0xd749b606u,InAngledArea=0x883622fau;
// v155: text-label natives seen in the prologue (label string in the first or second argument): objective and help
// texts, and is_ped_injured (the scripts poll every ped they track: crew and enemies).
constexpr std::uint32_t Text1=0xf42c43c7u,Text2=0xb3cec06fu,Text3=0x48262edau,Text4=0xcbf91d2au,PedInjured=0x2530a087u;
bool text(std::uint32_t h){return h==Text1||h==Text2||h==Text3||h==Text4;}
bool wanted(std::uint32_t h){return h==Health||h==Wanted||h==InVehicle||h==Cutscene||h==Faded||h==Control||h==AtCoord||h==InAngledArea||
    text(h)||h==PedInjured;}
struct Label {std::uint32_t hash=0;char text[28]{};long long lastMs=0;};
std::array<Label,6> labels{};
// Peds by handle (direct-mapped; relaxed atomics: telemetry, a torn entry is harmless).
struct PedSlot {std::atomic<std::uint32_t> handle{0},x{0},y{0},z{0},injured{2};std::atomic<long long> coordsMs{0},injuredMs{0};};
std::array<PedSlot,64> peds{};
PedSlot& pedSlot(std::uint32_t h){return peds[(h*0x9E3779B1u>>26)&63];}
void notePedCoords(std::uint32_t h,const std::uint32_t* xyz,long long now) {
    auto& p=pedSlot(h);p.handle.store(h,std::memory_order_relaxed);p.x.store(xyz[0],std::memory_order_relaxed);
    p.y.store(xyz[1],std::memory_order_relaxed);p.z.store(xyz[2],std::memory_order_relaxed);p.coordsMs.store(now,std::memory_order_relaxed);
}
std::atomic<int> hp{-1},stars{-1},veh{-1},cut{-1},fade{-1},ctl{-1};
struct Target {char kind=0;std::uint32_t entity=0;float f[7]{};std::uint32_t result=0;long long lastMs=0;unsigned count=0;};
std::mutex tableMutex;std::array<Target,16> table{};
std::atomic<long long> nextLogMs{0},nextSwitchMs{0};std::atomic<bool> enabled{false};
// bot-vision (2026-10-03): files/private-trace/diag-bot-vision (checked once a second by logNear, which runs only while
// diag-telemetry is on): the 16 nearest vehicles within 80 m and the 12 nearest on-foot peds within 40 m, read from the
// game's entity pools, logged as RENDER_DIAG_NEAR just before each RENDER_DIAG_TELEMETRY line (bot_vision_scan.h). Off = no
// guest reads.
std::atomic<bool> vision{false};std::atomic<long long> nextVisionSwitchMs{0};
// v270: the ARM generic timer (no vDSO call): probed natives call this at every call (v266 main profile: clock_gettime under
// indirectCallSlow 1.1% of main's samples). Only differences of these values are used, and the ms= of the telemetry line.
long long nowMs(){
#if defined(__aarch64__)
    if(recomp_v266::pollSleep().clockBaseline.load(std::memory_order_relaxed)) // v270 A/B: v269's steady_clock
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    static const std::uint64_t perMs=[]{std::uint64_t f=0;asm volatile("mrs %0, cntfrq_el0":"=r"(f));return f>=1000?f/1000:19200;}();
    std::uint64_t t;asm volatile("mrs %0, cntvct_el0":"=r"(t));
    return static_cast<long long>(t/perMs);
#else
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
float asFloat(std::uint32_t w){float v;std::memcpy(&v,&w,4);return v;}
bool on(long long now) {
    auto next=nextSwitchMs.load(std::memory_order_relaxed);
    if(now>=next && nextSwitchMs.compare_exchange_strong(next,now+1000,std::memory_order_relaxed))
        enabled.store(::access(RECOMP_APP_PATH("/private-trace/diag-telemetry"),F_OK)==0,std::memory_order_relaxed);
    return enabled.load(std::memory_order_relaxed);
}
void remember(char kind,std::uint32_t entity,const float* f,unsigned n,std::uint32_t result,long long now) {
    std::lock_guard lock(tableMutex);
    Target* slot=nullptr;Target* oldest=&table[0];
    for(auto& t:table) {
        if(t.kind==kind && t.entity==entity && std::fabs(t.f[0]-f[0])<0.05f && std::fabs(t.f[1]-f[1])<0.05f){slot=&t;break;}
        if(t.lastMs<oldest->lastMs)oldest=&t;
    }
    if(!slot){slot=oldest;*slot=Target{};slot->kind=kind;slot->entity=entity;}
    for(unsigned k=0;k<n && k<7;++k)slot->f[k]=f[k];
    slot->result=result;slot->lastMs=now;++slot->count;
}
void record(std::uint32_t hash,const std::uint32_t* a,std::uint32_t result,std::uint32_t player) {
    const auto now=nowMs();
    if(!on(now))return;
    switch(hash) {
    case Health: if(a[0]==player)hp.store(int(result),std::memory_order_relaxed);break;
    case Wanted: stars.store(int(result),std::memory_order_relaxed);break;
    case InVehicle: if(a[0]==player)veh.store(result?1:0,std::memory_order_relaxed);break;
    case Cutscene: cut.store(result?1:0,std::memory_order_relaxed);break;
    case Faded: fade.store(result?1:0,std::memory_order_relaxed);break;
    case Control: ctl.store(result?1:0,std::memory_order_relaxed);break;
    case AtCoord: {const float f[6]{asFloat(a[1]),asFloat(a[2]),asFloat(a[3]),asFloat(a[4]),asFloat(a[5]),asFloat(a[6])};
        remember('a',a[0],f,6,result,now);break;}
    case InAngledArea: {const float f[7]{asFloat(a[1]),asFloat(a[2]),asFloat(a[3]),asFloat(a[4]),asFloat(a[5]),asFloat(a[6]),asFloat(a[7])};
        remember('r',a[0],f,7,result,now);break;}
    case PedInjured: {
        if(!a[0] || a[0]==player)break;
        auto& p=pedSlot(a[0]);
        if(p.handle.load(std::memory_order_relaxed)!=a[0]){p.handle.store(a[0],std::memory_order_relaxed);p.coordsMs.store(0,std::memory_order_relaxed);}
        p.injured.store(result?1u:0u,std::memory_order_relaxed);p.injuredMs.store(now,std::memory_order_relaxed);break;}
    }
}
// v155: a label argument (read by the caller before the native ran; `bytes` = the guest string).
void noteLabel(std::uint32_t hash,const char* label) {
    std::lock_guard lock(tableMutex);
    const auto now=nowMs();
    Label* slot=&labels[0];
    for(auto& l:labels){if(l.hash==hash && !std::strcmp(l.text,label)){slot=&l;break;} if(l.lastMs<slot->lastMs)slot=&l;}
    slot->hash=hash;std::snprintf(slot->text,sizeof slot->text,"%s",label);slot->lastMs=now;
}
// bot-vision: the switch (once a second) and, while it is on, one read-only pool scan + its log line. Called unconditionally
// (a direct call, no branch) from maybeLog, which is inlined into indirectCallSlow: a new branch there changed
// indirectCallSlow's IR-PGO CFG hash and clang discarded its round5-v422 profile ("function control flow change detected");
// with the call alone the hash, and so the profile, stay as in v431. (Not [[gnu::cold]]: a call to a cold function made the
// rest of maybeLog's block cold and telemetry::remember stopped being inlined into indirectCallSlow; without it the
// function differs from v431 only by keeping `base` live: +15 of 2438 instructions, register renaming.)
[[gnu::noinline]] void logNear(std::uint32_t player,const std::uint8_t* base,long long now) {
    auto next=nextVisionSwitchMs.load(std::memory_order_relaxed);
    if(now>=next && nextVisionSwitchMs.compare_exchange_strong(next,now+1000,std::memory_order_relaxed))
        vision.store(::access(RECOMP_APP_PATH("/private-trace/diag-bot-vision"),F_OK)==0,
                     std::memory_order_relaxed);
    if(!vision.load(std::memory_order_relaxed))return; // off: no guest reads
#if defined(__aarch64__)
    static const std::uint64_t perMs=[]{std::uint64_t f=0;asm volatile("mrs %0, cntfrq_el0":"=r"(f));return f>=1000?f/1000:19200;}();
    const auto ticks=[]{std::uint64_t t;asm volatile("isb\n\tmrs %0, cntvct_el0":"=r"(t)::"memory");return t;};
#else
    constexpr std::uint64_t perMs=1000000;
    const auto ticks=[]{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());};
#endif
    const auto t0=ticks();
    bot_vision::Snapshot snap; // (~0.8 KB; this cold frame only)
    bot_vision::scan(base,player,80.f,40.f,snap); // vehicles within 80 m (oncoming and cross traffic early), peds within 40 m
    const auto us=static_cast<unsigned>((ticks()-t0)*1000/perMs);
    char line[1600];
    bot_vision::format(snap,now,us,line,sizeof line);
    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","%s",line);
}
void maybeLog(std::uint32_t player,const std::array<std::atomic<std::uint32_t>,3>& xyz,const std::uint8_t* base) {
    const auto now=nowMs();
    if(!enabled.load(std::memory_order_relaxed))return;
    auto next=nextLogMs.load(std::memory_order_relaxed);
    if(now<next || !nextLogMs.compare_exchange_strong(next,now+100,std::memory_order_relaxed))return;
    logNear(player,base,now); // (bot-vision: before this tick's telemetry line; no branch here, see logNear)
    char targets[1400];std::size_t used=0;targets[0]=0;unsigned shown=0;
    char texts[240];std::size_t textUsed=0;texts[0]=0;
    char pedText[900];std::size_t pedUsed=0;pedText[0]=0;unsigned pedShown=0;
    for(const auto& p:peds) { // v155: tracked peds with a position seen in the last 2 s
        const auto h=p.handle.load(std::memory_order_relaxed);const auto at=p.coordsMs.load(std::memory_order_relaxed);
        if(!h || h==player || !at || now-at>2000 || pedShown>=16 || pedUsed+60>sizeof pedText)continue;
        const int n=std::snprintf(pedText+pedUsed,sizeof pedText-pedUsed,"%08x:%.1f,%.1f,%.1f:%u:%lld;",h,
            asFloat(p.x.load(std::memory_order_relaxed)),asFloat(p.y.load(std::memory_order_relaxed)),asFloat(p.z.load(std::memory_order_relaxed)),
            p.injured.load(std::memory_order_relaxed),now-at);
        if(n>0)pedUsed+=std::size_t(n);++pedShown;
    }
    {
        std::lock_guard lock(tableMutex);
        for(const auto& l:labels) {
            if(!l.hash || now-l.lastMs>30000 || textUsed+48>sizeof texts)continue;
            const int n=std::snprintf(texts+textUsed,sizeof texts-textUsed,"%08x:%s:%lld;",l.hash,l.text,now-l.lastMs);
            if(n>0)textUsed+=std::size_t(n);
        }
        for(const auto& t:table) {
            if(!t.kind || now-t.lastMs>1500 || shown>=10 || used+130>sizeof targets)continue;
            const int n=t.kind=='a'
                ? std::snprintf(targets+used,sizeof targets-used,"%c%s:%08x:%.1f,%.1f,%.1f:%.1f,%.1f:%u:%lld:%u;",t.kind,
                    t.entity==player?"P":"",t.entity,t.f[0],t.f[1],t.f[2],t.f[3],t.f[4],t.result,now-t.lastMs,t.count)
                : std::snprintf(targets+used,sizeof targets-used,"%c%s:%08x:%.1f,%.1f,%.1f:%.1f,%.1f,%.1f:%.1f:%u:%lld:%u;",t.kind,
                    t.entity==player?"P":"",t.entity,t.f[0],t.f[1],t.f[2],t.f[3],t.f[4],t.f[5],t.f[6],t.result,now-t.lastMs,t.count);
            if(n>0)used+=std::size_t(n);++shown;
        }
    }
    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
        "RENDER_DIAG_TELEMETRY ms=%lld ped=%08x pos=%.2f,%.2f,%.2f hp=%d wanted=%d veh=%d cut=%d fade=%d ctl=%d targets=%s text=%s peds=%s",
        now,player,asFloat(xyz[0].load(std::memory_order_relaxed)),asFloat(xyz[1].load(std::memory_order_relaxed)),
        asFloat(xyz[2].load(std::memory_order_relaxed)),hp.load(),stars.load(),veh.load(),cut.load(),fade.load(),ctl.load(),targets,texts,pedText);
}
} // namespace telemetry
std::uint32_t beWord(const std::uint8_t* base,std::uint32_t a) {
    const auto* p=base+a;return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
}
}
// v273: every registered script native was probed, so every script native call took this slow path (probe lookup,
// telemetry tests, call counting) and never got a fast cache entry. With the trim on (default; diag-v273-probe-all at the
// renderer's A/B windows turns it off), only the natives the telemetry reads stay probed: the wanted() set, player_ped_id,
// get_entity_coords and shutdown_loading_screen; the others take the fast path. The RENDER_DIAG_NATIVE counts (a
// diagnostic dump) then cover the probed natives only. A change clears the indirect cache, so entries re-form either way.
std::atomic<bool> probeTrim{true};
bool probeNeeded(unsigned slot1) {
    const auto h=nativeProbe[slot1-1].hash.load(std::memory_order_relaxed);
    return telemetry::wanted(h) || h==0xfa92e226u || h==0x1647f1cbu || h==0xa2826d17u;
}
inline unsigned activeProbe(std::size_t first) {
    const unsigned slot1=first<nativeSlotCount?nativeSlot[first].load(std::memory_order_acquire):0u;
    if(!slot1)return 0u;
#if defined(RECOMP_V273_MUTANT_TRIM_ALL) // test mutant: the telemetry's natives lose their probes too
    if(probeTrim.load(std::memory_order_relaxed))return 0u;
#endif
    return !probeTrim.load(std::memory_order_relaxed) || probeNeeded(slot1)?slot1:0u;
}
extern "C" void recomp_v273_set_probe_trim(bool on) {
    if(probeTrim.exchange(on,std::memory_order_acq_rel)!=on)clearIndirectCache();
}
extern "C" void recomp_trainer_note_native(std::uint32_t hash,std::uint32_t handler) noexcept; // (trainer_glue.inc)
extern "C" void recomp_trainer_after_native(std::uint32_t hash,std::uint64_t callerLr,PPCContext& c,std::uint8_t* base) noexcept; // (trainer_glue.inc)
extern "C" void recomp_native_probe_register(std::uint32_t hash,std::uint32_t handler) {
    recomp_trainer_note_native(hash,handler); // trainer (iPad 237): the handlers of the natives it calls, by hash
    const auto count=std::min(recomp::bootstrap::validatedMappingCount.load(std::memory_order_acquire),nativeSlotCount);
    std::size_t first=0,last=count;
    while(first<last){const auto m=first+(last-first)/2;if(PPCFuncMappings[m].guest<handler)first=m+1;else last=m;}
    if(first>=count || PPCFuncMappings[first].guest!=handler || nativeSlot[first].load(std::memory_order_acquire))return;
    const auto slot=nativeProbeCount.fetch_add(1,std::memory_order_relaxed);
    if(slot>=nativeProbe.size())return;
    nativeProbe[slot].hash.store(hash,std::memory_order_relaxed);nativeProbe[slot].handler.store(handler,std::memory_order_relaxed);
    std::uint16_t unpublished=0; // First registration of a handler wins; a lost race wastes one probe slot.
    (void)nativeSlot[first].compare_exchange_strong(unpublished,static_cast<std::uint16_t>(slot+1),
        std::memory_order_release,std::memory_order_relaxed);
    indirectCache[(handler>>2)&(indirectCache.size()-1)].store(0,std::memory_order_release); // v216: no fast entry for a probed handler
#if !defined(RECOMP_CALLS_MUTANT_NO_PROBE_CLEAR) // test mutant: the 2-way set keeps the handler's entry
    clear2(handler); // calls: and none in the 2-way cache
#endif
}
extern "C" void recomp_native_probe_dump() {
    const auto count=std::min<unsigned>(nativeProbeCount.load(std::memory_order_relaxed),nativeProbe.size());
    std::vector<std::pair<unsigned long long,unsigned>> recent;unsigned long long total=0;
    for(unsigned i=0;i<count;++i){const auto c=nativeProbe[i].calls.load(std::memory_order_relaxed);
        const auto w=c-nativeProbe[i].reported.exchange(c,std::memory_order_relaxed);total+=w;if(w)recent.emplace_back(w,i);}
    std::sort(recent.begin(),recent.end(),[](auto& a,auto& b){return a.first>b.first;});
    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_NATIVES registered=%u window_calls=%llu distinct=%zu",
        count,total,recent.size());
    { // Attempt v12: player position (floats, big-endian words as the guest stores them).
        const auto f=[](std::uint32_t w){float v;std::memcpy(&v,&w,4);return v;};
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_PLAYER ped=%08x x=%.2f y=%.2f z=%.2f reads=%llu moves=%llu",
            playerPed.load(std::memory_order_relaxed),f(playerXyz[0].load()),f(playerXyz[1].load()),f(playerXyz[2].load()),
            playerReads.load(std::memory_order_relaxed),playerMoves.load(std::memory_order_relaxed));
    }
    for(std::size_t k=0;k<recent.size() && k<600;++k){const auto& e=nativeProbe[recent[k].second];
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_NATIVE hash=%08x handler=%08x window=%llu total=%llu last=%08x changes=%u arg0=%08x",
            e.hash.load(std::memory_order_relaxed),e.handler.load(std::memory_order_relaxed),recent[k].first,e.calls.load(std::memory_order_relaxed),e.last.load(std::memory_order_relaxed),
            e.changes.load(std::memory_order_relaxed),e.arg0.load(std::memory_order_relaxed));}
}


extern "C" void recomp_host_guest_call_indirect(std::uint32_t guestTarget,PPCContext& context,std::uint8_t* base);
namespace {
// v216: everything but a fast cache hit (binary search, fills, invalid targets, native probes and telemetry).
[[gnu::noinline]] void indirectCallSlow(
        std::uint32_t guestTarget,
        PPCContext& context,
        std::uint8_t* base) {
    const auto count = recomp::bootstrap::validatedMappingCount.load(
            std::memory_order_acquire);
    std::size_t first = 0;
    auto& cacheEntry = indirectCache[(guestTarget >> 2) & (indirectCache.size() - 1)];
    const auto cached = cacheEntry.load(std::memory_order_acquire);
    const auto low = static_cast<std::uint32_t>(cached);
    if ((cached >> 32) == guestTarget && !(low & 1u) && (low >> 1) && (low >> 1) - 1 < count) { // (the index form)
        first = static_cast<std::size_t>((low >> 1) - 1);
        // v217: an index entry filled while the v216 A/B disabled fast entries (or before a probe was removed) stays in
        // the slow form forever; rewrite it in the fast form once fast entries are on and the function has no probe
        // (the same fill rule as a miss below, including the probe re-check after the store).
        if (recomp_indirect_fast.load(std::memory_order_relaxed) && !recomp_v217_baseline.load(std::memory_order_relaxed) &&
            PPCFuncMappings[first].guest == guestTarget && PPCFuncMappings[first].host != nullptr &&
            !activeProbe(first)) { // (v273: an active probe)
            const auto delta = reinterpret_cast<std::intptr_t>(PPCFuncMappings[first].host) -
                               reinterpret_cast<std::intptr_t>(&recomp_host_guest_call_indirect);
            if (!(delta & 3) && delta / 4 >= -(std::intptr_t{1} << 30) && delta / 4 < (std::intptr_t{1} << 30)) {
                std::uint64_t expected = cached;
                const std::uint64_t entry = (std::uint64_t{guestTarget} << 32) |
                    ((static_cast<std::uint32_t>(static_cast<std::int32_t>(delta / 4)) << 1) | 1u);
                if (cacheEntry.compare_exchange_strong(expected, entry, std::memory_order_acq_rel)) {
                    fill2(guestTarget, entry); // calls
                    if (activeProbe(first)) {
                        cacheEntry.store(0, std::memory_order_release); // a probe registered meanwhile
                        clear2(guestTarget); // calls
                    }
                }
            }
        }
    } else if ((cached >> 32) == guestTarget && (low & 1u) && false) { // (a fast-form entry while the A/B disables it: search)
        first = static_cast<std::size_t>((low >> 1) - 1);
    } else {
    std::size_t last = count;
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        if (PPCFuncMappings[middle].guest < guestTarget) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    if (first < count && PPCFuncMappings[first].guest == guestTarget && PPCFuncMappings[first].host != nullptr) {
        const bool probe = activeProbe(first) != 0; // v273
        const auto delta = reinterpret_cast<std::intptr_t>(PPCFuncMappings[first].host) -
                           reinterpret_cast<std::intptr_t>(&recomp_host_guest_call_indirect);
        const bool fits = !(delta & 3) && delta / 4 >= -(std::intptr_t{1} << 30) && delta / 4 < (std::intptr_t{1} << 30);
        const bool fast = !probe && fits && recomp_indirect_fast.load(std::memory_order_relaxed);
        const std::uint64_t entry = (std::uint64_t{guestTarget} << 32) | (fast
            ? ((static_cast<std::uint32_t>(static_cast<std::int32_t>(delta / 4)) << 1) | 1u)
            : static_cast<std::uint32_t>((first + 1) << 1));
        cacheEntry.store(entry, std::memory_order_release);
        if (fast) fill2(guestTarget, entry); // calls
        if (fast && activeProbe(first)) {
            cacheEntry.store(0, std::memory_order_release); // a probe registered meanwhile
            clear2(guestTarget); // calls
        }
    }
    }
    if (first >= count
            || PPCFuncMappings[first].guest != guestTarget
            || PPCFuncMappings[first].host == nullptr) {
        {
            std::lock_guard lock(recomp::bootstrap::rejectedCallMutex);
            if (!recomp::bootstrap::rejectedCall)
                recomp::bootstrap::rejectedCall = recomp::bootstrap::InvalidGuestIndirectCall{
                    guestTarget,static_cast<std::uint32_t>(context.lr),context.r1.u32,
                    {context.r3.u32,context.r4.u32,context.r5.u32,context.r6.u32,
                     context.r7.u32,context.r11.u32,context.r26.u32,context.r31.u32}};
        }
        recomp::bootstrap::GuestExecutionGuard::stopForInvalidIndirectTarget();
    }
    if(const unsigned probe=activeProbe(first)) { // v273
        auto& e=nativeProbe[probe-1];
        const auto hash=e.hash.load(std::memory_order_relaxed);
        const auto cxt=context.r3.u32;
        // Attempt v12: the player's own position. RAGE writes a native's result over its argument
        // area, so the entity handle is read before the call; player_ped_id supplies the handle.
        const auto plausibleArg=[](std::uint32_t a){return !(a&3) && a>=0x10000u && a<=0xFFFF0000u;};
        std::uint32_t entityArg=0;
        if(hash==0x1647f1cbu && plausibleArg(cxt)){const auto args=beWord(base,cxt+8);if(plausibleArg(args))entityArg=beWord(base,args);}
        std::uint32_t telemetryArgs[8]{};bool telemetryCall=false; // v153: arguments before the call may overwrite them
        if(telemetry::wanted(hash) && telemetry::enabled.load(std::memory_order_relaxed) && plausibleArg(cxt)) {
            const auto args=beWord(base,cxt+8);
            if(plausibleArg(args)){for(unsigned k=0;k<8;++k)telemetryArgs[k]=beWord(base,args+4*k);telemetryCall=true;}
        }
        char telemetryLabel[28]{};
        if(telemetryCall && telemetry::text(hash)) { // v155: a label string in the first or second argument
            for(unsigned k=0;k<2 && !telemetryLabel[0];++k) {
                const auto a=telemetryArgs[k];
                if(!plausibleArg(a) && !(a>=0x10000u && a<=0xFFFF0000u))continue;
                unsigned n=0;bool ok=true;
                for(;n<27;++n){const char c=char(base[a+n]);if(!c)break;
                    if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_')){ok=false;break;}telemetryLabel[n]=c;}
                if(!ok || n<3 || n>=27)telemetryLabel[0]=0; else telemetryLabel[n]=0;
            }
        }
        const auto callerLr=context.lr; // trainer (iPad 237, trainer-mods v952): the script VM's native call is the safe point
        PPCFuncMappings[first].host(context, base);
        recomp_trainer_after_native(hash,callerLr,context,base); // (a plain call: no new branch in this function)
        if(telemetryLabel[0])telemetry::noteLabel(hash,telemetryLabel);
        if(plausibleArg(cxt)) {
            if(telemetryCall){const auto ret=beWord(base,cxt);
                telemetry::record(hash,telemetryArgs,plausibleArg(ret)?beWord(base,ret):0u,playerPed.load(std::memory_order_relaxed));}
            else if(telemetry::wanted(hash))(void)telemetry::on(telemetry::nowMs()); // (re)checks the switch once a second
            telemetry::maybeLog(playerPed.load(std::memory_order_relaxed),playerXyz,base);
        }
        if(plausibleArg(cxt)) {
            if(hash==0xfa92e226u){const auto ret=beWord(base,cxt);if(plausibleArg(ret))playerPed.store(beWord(base,ret),std::memory_order_relaxed);}
            else if(hash==0x1647f1cbu && entityArg && entityArg==playerPed.load(std::memory_order_relaxed)) {
                const auto ret=beWord(base,cxt);
                if(plausibleArg(ret)){
                    const std::uint32_t xyz[3]{beWord(base,ret),beWord(base,ret+4),beWord(base,ret+8)};
                    if(xyz[0]!=playerXyz[0].load(std::memory_order_relaxed) || xyz[1]!=playerXyz[1].load(std::memory_order_relaxed))
                        playerMoves.fetch_add(1,std::memory_order_relaxed);
                    for(unsigned k=0;k<3;++k)playerXyz[k].store(xyz[k],std::memory_order_relaxed);
                    playerReads.fetch_add(1,std::memory_order_relaxed);
                }
            }
            else if(hash==0x1647f1cbu && entityArg && telemetry::enabled.load(std::memory_order_relaxed)) { // v155: other peds
                auto& slot=telemetry::pedSlot(entityArg);
                if(slot.handle.load(std::memory_order_relaxed)==entityArg) { // only peds the scripts poll with is_ped_injured
                    const auto ret=beWord(base,cxt);
                    if(plausibleArg(ret)){const std::uint32_t xyz[3]{beWord(base,ret),beWord(base,ret+4),beWord(base,ret+8)};
                        telemetry::notePedCoords(entityArg,xyz,telemetry::nowMs());}
                }
            }
        }
        e.calls.fetch_add(1,std::memory_order_relaxed);
        // Attempt v17: shutdown_loading_screen (Jenkins hash a2826d17) marks the start of gameplay; the
        // generic renderer takes over from the native paths there (renderer_boundary_trace.cpp).
        if(hash==0xa2826d17u)recomp_diag_loading_screen_shutdown.store(1,std::memory_order_release);
        // Handlers are also called directly from C++ with other r3 values: read only aligned guest
        // pointers that cannot run past the 4 GB reservation (v10's first run faulted at base+4 GB).
        const auto plausible=[](std::uint32_t a){return !(a&3) && a>=0x10000u && a<=0xFFFF0000u;};
        if(plausible(cxt)) {
            const auto ret=beWord(base,cxt),args=beWord(base,cxt+8);
            bool changed=false;std::uint32_t result=0;
            if(plausible(ret)) {
                result=beWord(base,ret);
                changed=e.last.exchange(result,std::memory_order_relaxed)!=result;
                if(changed)e.changes.fetch_add(1,std::memory_order_relaxed);
            }
            std::uint32_t a[3]{};
            if(plausible(args))for(unsigned k=0;k<3;++k)a[k]=beWord(base,args+4*k);
            e.arg0.store(a[0],std::memory_order_relaxed);
            // v303 (diagnostic; these two are probed only while diag-v273-probe-all probes every native): the game's clock
            // (get_game_timer, ms) and timestep (get_frame_time, float s) against the steady clock every 5 s, for the
            // frame-limiter game-speed check (tools/iter/speed_and_drive.sh, game_speed.py).
            if(hash==0xa4ea0691u || hash==0x96374262u) {
                static std::atomic<std::uint32_t> gameTimer{0},frameTime{0};static std::atomic<long long> nextMs{0};
                (hash==0xa4ea0691u?gameTimer:frameTime).store(result,std::memory_order_relaxed);
                const long long now=telemetry::nowMs();auto next=nextMs.load(std::memory_order_relaxed);
                if(hash==0xa4ea0691u && plausible(ret) && now>=next && nextMs.compare_exchange_strong(next,now+5000,std::memory_order_relaxed))
                    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_GAME_TIME steady_ms=%lld game_timer=%u frame_time=%08x",
                        now,result,frameTime.load(std::memory_order_relaxed));
            }
            // Attempt v11: the first 4 calls of each native and its first 16 result changes, with
            // arguments and any C-string argument, so script requests (cutscenes, scripts, map
            // sections, models) and loading transitions are visible in order.
            const auto calls=e.calls.load(std::memory_order_relaxed);
            const auto changes=e.changes.load(std::memory_order_relaxed);
            if(calls<=4 || (changed && changes<=16)) {
                char text[48]{};
                for(unsigned k=0;k<3 && !text[0];++k) {
                    const auto p=a[k];
                    if(p<0x10000u || p>0xFFFF0000u)continue;
                    unsigned len=0;
                    while(len<40){const auto c=base[p+len];if(c<32 || c>126)break;++len;}
                    if(len>=3 && base[p+len]==0){std::memcpy(text,base+p,len);text[len]=0;}
                }
                __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_NATIVE_EVENT kind=%s hash=%08x call=%llu result=%08x changes=%u a0=%08x a1=%08x a2=%08x str=%s",
                    calls<=4?"call":"change",hash,calls,result,changes,a[0],a[1],a[2],text);
            }
        }
        return;
    }
    PPCFuncMappings[first].host(context, base);
}
} // namespace
// calls (package E): bit 0 = the 2-way cache, relaxed loads; bit 1 = v216's cache with a relaxed load; 0 = v407.
extern "C" std::atomic<unsigned> recomp_calls_dispatch;
std::atomic<unsigned> recomp_calls_dispatch{0};
extern "C" void recomp_host_guest_call_indirect(std::uint32_t guestTarget,PPCContext& context,std::uint8_t* base) {
    using Host = decltype(PPCFuncMappings[0].host);
#if defined(RECOMP_CALLS_DISPATCH_FIXED)
    constexpr unsigned mode = RECOMP_CALLS_DISPATCH_FIXED;
#else
    const unsigned mode = recomp_calls_dispatch.load(std::memory_order_relaxed);
#endif
    if (mode & 1u) {
        const auto* set = indirectSet2(guestTarget);
        auto e = set[0].load(std::memory_order_relaxed);
#if !defined(RECOMP_CALLS_MUTANT_ONE_WAY) // test mutant: way 1 never read
        if ((e >> 32) != guestTarget) e = set[1].load(std::memory_order_relaxed);
#endif
#if defined(RECOMP_CALLS_MUTANT_NO_TAG) // test mutant: the tag is not compared
        if ((e & 1u) && recomp_indirect_fast.load(std::memory_order_relaxed)) {
#else
        if ((e >> 32) == guestTarget && (e & 1u) && recomp_indirect_fast.load(std::memory_order_relaxed)) {
#endif
            const auto units = static_cast<std::int32_t>(static_cast<std::uint32_t>(e)) >> 1;
            const auto host = reinterpret_cast<Host>(reinterpret_cast<std::intptr_t>(&recomp_host_guest_call_indirect) + std::intptr_t{units} * 4);
            return host(context, base);
        }
        return indirectCallSlow(guestTarget, context, base);
    }
    const auto e = (mode & 2u) ? indirectCache[(guestTarget >> 2) & (indirectCache.size() - 1)].load(std::memory_order_relaxed)
                               : indirectCache[(guestTarget >> 2) & (indirectCache.size() - 1)].load(std::memory_order_acquire);
    if ((e >> 32) == guestTarget && (e & 1u) && recomp_indirect_fast.load(std::memory_order_relaxed)) { // v216: the fast form
        const auto units = static_cast<std::int32_t>(static_cast<std::uint32_t>(e)) >> 1;
        const auto host = reinterpret_cast<Host>(reinterpret_cast<std::intptr_t>(&recomp_host_guest_call_indirect) + std::intptr_t{units} * 4);
        return host(context, base);
    }
    indirectCallSlow(guestTarget, context, base);
}

// trainer (iPad 237, 2026-10-25; local-analysis/trainer-mods v952): the safe point, the native calls and the app API.
#include "trainer_glue.inc"
