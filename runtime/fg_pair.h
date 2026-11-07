#pragma once
// frame-gen (lane B, frame generation; 2026-10-04), stage 1a: the PAIR logic. Our own code.
//
// While the option is on and its gate is open, every real frame is shown as TWO pictures two refreshes apart on the ~120 Hz
// panel: "between" = [the made picture, the real frame], "ahead" = [the real frame, the made picture]. In stage 1a the made
// picture is trivial (between: the previous real picture; ahead: the same real picture again), so the screen looks exactly as
// with the 30 fps cap alone; what runs for real is everything else: the gate, the second acquire, the second scaler chain, the
// two present calls and their timing. Stage 1b replaces only the recorder of the made picture (DESIGN-stage1.md).
// Switch files (files/private-trace/, polled by generic_renderer.cpp fgWindow() every 60 presents; none present = no decision
// anywhere differs from the build without this lane):
//   diag-fg-pair-between / diag-fg-pair-ahead   the two orders (both present: between)
//   diag-fg-hold    (modifier) space the pair with a timed wait of the present thread instead of desired present times
//   diag-fg-nocap   (modifier, diagnostic) do not require lane A's 30 fps cap (pairs of 2+1 refreshes then occur)
//
// The gate (review defect 1: the first rule, "last interval 3.5-6.5 refreshes, 8 in a row, works uncapped", was open for 6-27 %
// of the measured uncapped driving frames): pairs need a real frame of 4 refreshes, which only lane A's 30 fps cap gives. So
//   (1) the frame's slot must come from lane A's pacer with 4 refreshes per frame (slotSource; no pacer or another cap: closed,
//       reason "cap"), unless diag-fg-nocap;
//   (2) the last GateRun real intervals were all GateLo..GateHi refreshes (under the cap the lower bound never binds; the
//       upper one closes the gate for GateRun frames after a stall).
// tests/fg_pair_test.cpp replays the measured v458 driving and house interval sequences through this file.
//
// Which picture carries the frame's tag: always the pair's FIRST picture (it is the one on the frame's slot, so lane A's slot
// feedback, the v337 lag rings and the latency probe keep meaning "the frame reached the screen"); the second picture carries
// an id of its own that can never evict a pending real frame from the 64-entry rings (review: (tag + 32) | bit 31 shared a ring
// slot with a real frame within three frames in 7.5 % of frames).
//
// Pure: every time is passed in (nanoseconds on CLOCK_MONOTONIC, the clock of actualPresentTime); no lock of its own (the
// presenter calls everything except setSwitches() / switches() under its mutex).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <time.h>

namespace recomp::fg {

constexpr unsigned SwBetween=1u,SwAhead=2u,SwHold=4u,SwNoCap=8u,SwDrain=1024u,SwTraceAll=2048u,SwModes=SwBetween|SwAhead; // (SwTraceAll: ios-fg-trace-all, bit 11) // (SwDrain: bit 10; 16 and 32 are fg_gen.h's SwGenBetween / SwShow in the same word)
enum Mode : unsigned {Off=0,Between=1,Ahead=2};
// Why a real frame got no second picture (counted per reason; RENDER_DIRECT_FG skip=).
enum Reason : unsigned {Shown=0,NotWanted,NoCap,Rate,Failed,SecondLost,Drain,Reasons};

struct K {
#if defined(__APPLE__)
    // iOS (Claude 2026-10-17, owner: "enable framegen on the iPad"): the iPad mini's panel runs at 60 Hz, so lane A's 30 fps cap
    // is 2 refreshes per real frame and the two pictures of a pair are 1 refresh apart; the gate keeps the same TIMES as the
    // ~120 Hz original (2.5..6.5 refreshes of 8.33 ms = 1.25..3.25 of 16.67 ms). Phase (I - Apart) / I = 0.5 at I = 2.
    static constexpr double GateLo=1.25,GateHi=3.25;        // refreshes per real frame
    static constexpr unsigned GateRun=8;                    // real intervals in range, in a row, before the gate opens
    static constexpr unsigned CapRefreshes=2;               // lane A's 30 fps cap on the 16.67 ms grid
    static constexpr unsigned Apart=1;                      // refreshes between the two pictures of a pair
    static constexpr double NominalPeriod=16'666'667.0;     // the grid's first guess; tracked within +-5 %
#else
    static constexpr double GateLo=2.5,GateHi=6.5;          // refreshes per real frame (replay: tests/fg_pair_test.cpp)
    static constexpr unsigned GateRun=8;                    // real intervals in range, in a row, before the gate opens
    static constexpr unsigned CapRefreshes=4;               // lane A's 30 fps cap on the 8.397 ms grid
    static constexpr unsigned Apart=2;                      // refreshes between the two pictures of a pair
    static constexpr double NominalPeriod=8'333'333.0;      // the grid's first guess; tracked within +-5 % (the Thor: 8.397 ms)
#endif
    static constexpr std::int64_t LatchLead=9'800'000;      // present call returned -> its vsync (lane A's measured window)
    static constexpr std::int64_t FirstLead=6'600'000;      // present call begins -> first vsync it can make (review/display-cadence.txt: right for 89 % of frames)
    static constexpr std::int64_t CallInit=1'100'000;       // vkQueuePresentKHR, p50 (v458 run); then measured
    static constexpr std::int64_t HoldMax=30'000'000;       // a hold never lasts longer (a broken estimate must not park a picture)
    static constexpr float PhaseLo=0.33f,PhaseHi=0.67f;     // between: (I - 2) / I for I = 3..6 refreshes; ahead: 2 / I
    static constexpr unsigned Ring=64;                      // the tag rings of lag_v337.h and pace_caps.h
    static constexpr unsigned Recent=4,SpacingBins=6;
    // queue drain (Claude 2026-10-20, ios-fg-drain; c310 after a resume, c305 under load, and c291's last three windows before
    // its GPU hang): a hiccup can leave the present queue one picture deeper. Pairs then feed exactly one picture per refresh, so
    // it never drains: the first picture's lag (present call -> display) sits a refresh higher (~4 instead of ~3 refreshes),
    // ~20 % of the made pictures are dropped and every drawable is held. When the smoothed lag exceeds DrainLag refreshes, one
    // real frame gets no made picture (it shows for two refreshes, as with the cap alone) and the queue loses one picture.
    // OPT-IN (ios-fg-drain): c312 drained 17 times without lowering the lag (3.69-3.71) or ending the 80/20 stretch, so the
    // extra refresh is not queue depth; off by default, the smoothed lag (lag_ref=) stays logged as the detector.
    static constexpr double DrainLag=3.6;                   // refreshes, smoothed over ~8 first pictures
    static constexpr unsigned DrainCool=16;                 // real frames before the next drain may happen
};

#if defined(__APPLE__)
// iOS (Claude 2026-10-17; c177/c178b GPU timeouts): lane A's pacer (pace_caps.h) and MoltenVK's display times use
// mach_absolute_time = CLOCK_UPTIME_RAW; CLOCK_MONOTONIC also counts the time the device slept since boot, so mixing the two
// put a pair's desired present time far in the future (drawables held -> the next acquire never completes -> GPU watchdog).
inline std::int64_t monotonicNs() noexcept {return std::int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));}
#else
inline std::int64_t monotonicNs() noexcept {timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return std::int64_t(t.tv_sec)*1000000000ll+t.tv_nsec;}
#endif
// pace-fg-tuning (v460's first device run): the desired present time that puts a picture on the vsync `vsync`. The Thor's
// compositor shows a timed picture at the first vsync LATER THAN desired - one refresh: v460 asked for vsync - half a refresh
// and 7,029 of 7,152 second pictures came one refresh after the first instead of two (the other 123 followed a first picture
// that was itself a refresh early). The middle of the window that selects `vsync` is vsync + half a refresh. The same formula
// as lane A's recomp::pace::desiredFor (pace_caps.h; this file stays free of that include; tests/tuning_test.cpp compares them).
inline std::int64_t desiredFor(std::int64_t vsync,double period) noexcept {return vsync+std::int64_t(period/2);}

// Lane A's plan of a frame: the vsync it is paced for, the cap in refreshes per frame and the period estimate. All zero: unknown.
struct Slot {std::int64_t target=0;unsigned refreshes=0;double period=0;};
// Set once by the merged build (MERGE.md: six lines beside lane A's pacer). Null: no pacer in this build.
using SlotFn=bool(*)(std::uint32_t tag,Slot* out) noexcept;
inline std::atomic<SlotFn> slotSource{nullptr};
// v572: the pairs' second pictures handed to the present call (the presenter counts them; the frame-rate counter, fps_overlay.h,
// reads the increase per window to show the displayed rate).
inline std::atomic<std::uint64_t> secondPresents{0};

// One real interval after another: true while pairs may be shown.
struct Gate {
    unsigned run=0;
    bool step(double refreshes) noexcept {
        run=(refreshes>=K::GateLo && refreshes<=K::GateHi)?std::min(run+1u,K::GateRun):0u;
        return run>=K::GateRun;
    }
    bool open() const noexcept {return run>=K::GateRun;}
};
// (1) of the gate: lane A's cap is the 4-refresh one, or the diagnostic switch waives it.
constexpr bool capAllows(unsigned capRefreshes,unsigned sw) noexcept {return capRefreshes==K::CapRefreshes || (sw&SwNoCap)!=0;}
// Where the made picture lies. between: its share of the way from the previous real frame to this one when it is shown `Apart`
// refreshes before this one; ahead: how far past this frame, in real intervals, a picture shown `Apart` refreshes later lies.
inline float phaseFor(double interval) noexcept {
    return interval>0?std::clamp(float((interval-double(K::Apart))/interval),K::PhaseLo,K::PhaseHi):0.5f;
}
inline float stepFor(double interval) noexcept {
    return interval>0?std::clamp(float(double(K::Apart)/interval),K::PhaseLo,K::PhaseHi):0.5f;
}
// The second picture's id: bit 31 opposite to the frame tag's, a ring slot (id % Ring) that none of `recent` real tags uses
// (they may still be waiting for their display time in a ring), never 0.
inline std::uint32_t secondIdFor(std::uint32_t tag,std::uint32_t sequence,const std::array<std::uint32_t,K::Recent>& recent) noexcept {
    unsigned slot=(tag+K::Ring/2)%K::Ring;
    for(unsigned tries=0;tries<=K::Recent;++tries) {
        bool taken=tag%K::Ring==slot;
        for(const auto r:recent)taken=taken || (r && r%K::Ring==slot);
        if(!taken)break;
        slot=(slot+1)%K::Ring;
    }
    return ((tag^0x80000000u)&0x80000000u)|((sequence&0x1FFFFFFu)<<6)|slot;
}

struct Plan {
    Mode mode=Off;Reason reason=NotWanted;
    std::uint32_t secondId=0;      // the pair's second picture (0: no pair)
    bool hold=false;               // the present thread spaces the pair (no desired present times)
    bool timed=false;              // pace-fg-tuning: both pictures carry a desired present time (lane A then plans the frame as timed)
    double interval=0;             // this real frame's interval in refreshes
    float phase=0.5f,step=0.5f;    // stage 1b's inputs (logged in 1a)
};

// One RENDER_DIRECT_FG line's counters (since the last take()) and the state at its end.
struct Snapshot {
    unsigned switches=0,gateRun=0,cap=0;bool grid=false;double periodNs=0;
    std::uint64_t real=0,pairs=0,between=0,ahead=0,held=0,overrun=0,idClash=0;
    std::array<std::uint64_t,Reasons> skip{};
    std::array<std::uint64_t,K::SpacingBins> spacing{}; // refreshes from a pair's first picture to its second (last bin: more)
    std::array<std::uint64_t,K::SpacingBins+2> realGap{}; // refreshes between consecutive FIRST pictures (last bin: more)
    std::uint64_t holdNs=0,lagFirstNs=0,lagFirstN=0,lagSecondNs=0,lagSecondN=0,dptLate=0;
    double phaseSum=0,lagRefresh=0;
};

// ios-fg-trace (Claude 2026-10-20; measurement only): one displayed picture of a pair while the smoothed first-picture lag
// is above K::DrainLag (the degraded mode; blocker packet 2026-10-20-ipad-framegen-hang). Times relative to the pair's
// target (lane A's slot) in microseconds; desired = what desired() returned for it (0: none).
struct TracePic {std::uint32_t id=0;std::uint8_t second=0;std::int32_t callUs=0,desiredUs=0,actualUs=0;};
class Pairs {
public:
    // ---- any thread
    void setSwitches(unsigned bits) noexcept {sw.store(bits,std::memory_order_relaxed);}
    unsigned switches() const noexcept {return sw.load(std::memory_order_relaxed);}

    // ---- under the presenter's mutex from here on.
    // A real frame arrives at the presenter (before any of its pictures is presented). `timed`: display times and desired
    // present times are available; `thread`: a present thread exists (a hold is possible); `usable`: the made picture's objects
    // exist and have not failed.
    Plan real(std::uint32_t tag,std::int64_t now,const Slot& s,bool timed,bool thread,bool usable) noexcept {
        const unsigned w=switches();
        Plan p;
        const double P=s.period>0?s.period:period;
        // the interval: between the two frames' slots when lane A planned both (exact refreshes), else on our own clock
        double interval=0;
        if(lastNow) interval=(s.target && lastTarget)?double(s.target-lastTarget)/P:double(now-lastNow)/P;
        const bool open=lastNow?gate.step(interval):false;
        lastNow=now;lastTarget=s.target;lastCap=s.refreshes;
        p.interval=interval;p.phase=phaseFor(interval);p.step=stepFor(interval);
        ++c.real;
        recent[recentAt++%K::Recent]=tag;
        for(auto& r:pairs)if(r.live && r.second==tag){r.live=false;++c.idClash;} // (a real tag equal to a live second id: that pair is forgotten)
        if(drainCool)--drainCool; // (ios-fg-drain)
        if(!(w&SwModes))p.reason=NotWanted;
        else if(!capAllows(s.refreshes,w))p.reason=NoCap;
        else if(!open)p.reason=Rate;
        else if(!usable)p.reason=Failed;
        else if((w&SwDrain) && !drainCool && lagRefresh>K::DrainLag) {
            p.reason=Drain;drainCool=K::DrainCool;lagRefresh-=1.0; // (one picture less in the queue; the lag is measured again)
        }
        else {
            p.mode=(w&SwBetween)?Between:Ahead;p.reason=Shown;
            p.secondId=secondIdFor(tag,sequence++,recent);
            // desired present times need the frame's slot and display timing; otherwise (or on request) the present thread holds
            const bool dpt=timed && s.target && !(w&SwHold);
            p.hold=!dpt && thread;p.timed=dpt;
            auto& r=pairs[pairAt++%pairs.size()];
            r=Pair{};r.live=true;r.first=tag;r.second=p.secondId;r.dpt=dpt;r.hold=p.hold;r.period=P;
            r.target=dpt?s.target:0;
        }
        if(p.mode==Off)++c.skip[p.reason];
        else {++c.pairs;++(p.mode==Between?c.between:c.ahead);c.phaseSum+=double(p.mode==Between?p.phase:p.step);}
        return p;
    }
    // The planned pair's second picture was not presented (its acquire or its pass failed): the frame is a single picture.
    void secondLost(std::uint32_t tag) noexcept {
        for(auto& r:pairs)if(r.live && r.first==tag){r.live=false;++c.skip[SecondLost];} // (pairs counts planned pairs)
    }
    // ---- present thread (under the presenter's mutex). The desired present time of `id`; `base` is what the build would pass
    // without this lane (0, or lane A's value for the frame's tag). Also the moment the picture's present call begins.
    std::uint64_t desired(std::uint32_t id,std::uint64_t base,std::int64_t now) noexcept {
        if(!anyLive())return base;
        for(auto& r:pairs) {
            if(!r.live)continue;
            if(r.first==id) {
                r.firstCall=now;
                // both pictures of a timed pair carry a time: the first cannot be shown before the frame's slot
                return r.dpt?std::max<std::uint64_t>(base,std::uint64_t(desiredFor(r.target,r.period))):base;
            }
            if(r.second==id) {
                r.secondCall=now;
                return r.dpt?std::uint64_t(desiredFor(r.target+std::int64_t(double(K::Apart)*r.period),r.period)):0;
            }
        }
        return base;
    }
    // The present call of `id` returned at `now`. True when `id` is a pair's second picture (the caller then keeps it out of
    // the v337 call ring: its deliberate two refreshes are not display lag).
    bool called(std::uint32_t id,std::int64_t now) noexcept {
        if(!anyLive())return false;
        for(auto& r:pairs)if(r.live && r.second==id) {
            if(r.secondCall && now>r.secondCall)callNs+=(std::clamp<std::int64_t>(now-r.secondCall,100'000,8'000'000)-callNs)/8;
            return true;
        }
        return false;
    }
    // Before the present call of `id`: when it is a held pair's second picture, the time its call should begin (false: present now).
    bool holdUntil(std::uint32_t id,std::int64_t now,std::int64_t* until) noexcept {
        if(!anyLive())return false;
        for(auto& r:pairs)if(r.live && r.hold && r.second==id && r.firstCall) {
            const std::int64_t apart=std::int64_t(double(K::Apart)*r.period);
            // with a grid: the first picture's vsync + Apart refreshes, minus the latch lead and the call itself;
            // without: Apart refreshes after the first call began (equal spacing on our own clock)
            std::int64_t t=gridOk(now)?vsyncAtOrAfter(r.firstCall+K::FirstLead)+apart-K::LatchLead-callNs:r.firstCall+apart;
            t=std::min(t,now+K::HoldMax);
            if(t<=now)return false;
            *until=t;++c.held;r.holdFrom=now;return true;
        }
        return false;
    }
    // The hold of `id` ended at `now`; `superseded`: a newer real frame arrived first (the picture goes out at once, late).
    void holdEnded(std::uint32_t id,std::int64_t now,bool superseded) noexcept {
        for(auto& r:pairs)if(r.live && r.second==id && r.holdFrom){c.holdNs+=std::uint64_t(std::max<std::int64_t>(0,now-r.holdFrom));r.holdFrom=0;}
        if(superseded)++c.overrun;
    }
    // ios-fg-trace: the recorded pictures since the last call (at most Trace).
    static constexpr unsigned Trace=48;
    unsigned takeTrace(std::array<TracePic,Trace>& out) noexcept {const unsigned n=traceN;out=trace;traceN=0;return n;}
    // `id` reached the screen at `actual` (VK_GOOGLE_display_timing).
    void displayed(std::uint32_t id,std::int64_t actual) noexcept {
        gridSample(actual);
        if(lagRefresh>K::DrainLag && traceN<Trace) // (ios-fg-trace)
            for(const auto& r:pairs)if(r.live && (r.first==id || r.second==id) && r.target) {
                const bool sec=r.second==id;const std::int64_t call=sec?r.secondCall:r.firstCall;
                const auto us=[&](std::int64_t t){return t?std::int32_t(std::clamp<std::int64_t>((t-r.target)/1000,-999'999,999'999)):0;};
                const std::int64_t want=r.dpt?(sec?desiredFor(r.target+std::int64_t(double(K::Apart)*r.period),r.period):desiredFor(r.target,r.period)):0;
                trace[traceN++]=TracePic{id,std::uint8_t(sec),us(call),us(want),us(actual)};break;
            }
        if(!anyLive() && !lastFirstShown)return;
        // ios-fg-trace-all (Claude 2026-10-21; diagnosis of the 50 ms intervals, c388): a first picture reaching the screen while an
        // OLDER pair's second picture has not: that second is recorded as lost (role 3: its call and desired times, actual 0), with
        // the first that passed it (role 1); a second displayed after that is recorded again as late (role 4). Tracing only.
        const bool traceAll=(switches()&SwTraceAll)!=0;
        if(traceAll)for(auto& r:pairs)if(r.live && r.first==id) {
            for(auto& o:pairs)if(o.live && &o!=&r && o.firstShown && o.second && !o.lostTraced && o.target<r.target && traceN+2<=Trace) {
                o.lostTraced=true;
                const auto us=[&](std::int64_t t,std::int64_t base){return t?std::int32_t(std::clamp<std::int64_t>((t-base)/1000,-999'999,999'999)):0;};
                const std::int64_t want=o.dpt?desiredFor(o.target+std::int64_t(double(K::Apart)*o.period),o.period):0;
                trace[traceN++]=TracePic{o.second,2,us(o.secondCall,o.target),us(want,o.target),us(r.target,o.target)}; // (actual: the next pair's target, from this one's)
                trace[traceN++]=TracePic{id,0,us(r.firstCall,r.target),us(r.dpt?desiredFor(r.target,r.period):0,r.target),us(actual,r.target)};
            }
            break;
        }
        for(auto& r:pairs) {
            if(!r.live)continue;
            if(traceAll && r.second==id && r.lostTraced && traceN<Trace) { // (a second recorded lost that arrived after all)
                const std::int64_t want=r.dpt?desiredFor(r.target+std::int64_t(double(K::Apart)*r.period),r.period):0;
                trace[traceN++]=TracePic{id,3,std::int32_t((r.secondCall-r.target)/1000),std::int32_t((want-r.target)/1000),std::int32_t((actual-r.target)/1000)};
            }
            if(r.first==id) {
                r.firstShown=actual;
                if(r.firstCall && actual>r.firstCall) {
                    c.lagFirstNs+=std::uint64_t(actual-r.firstCall);++c.lagFirstN;
                    lagRefresh+=(double(actual-r.firstCall)/r.period-lagRefresh)/8.0; // (ios-fg-drain)
                }
                if(r.dpt && std::llround(double(actual-r.target)/r.period)>0)++c.dptLate;
                if(lastFirstShown && actual>lastFirstShown)
                    ++c.realGap[std::size_t(std::clamp<long long>(std::llround(double(actual-lastFirstShown)/period),0,K::SpacingBins+1))];
                lastFirstShown=actual;
            } else if(r.second==id) {
                if(r.firstShown && actual>=r.firstShown)
                    ++c.spacing[std::size_t(std::clamp<long long>(std::llround(double(actual-r.firstShown)/r.period),0,K::SpacingBins-1))];
                if(r.secondCall && actual>r.secondCall){c.lagSecondNs+=std::uint64_t(actual-r.secondCall);++c.lagSecondN;}
                r.live=false;
            }
        }
    }
    // The pairs are over (the switch went away, the swapchain was replaced): nothing stays planned.
    void reset() noexcept {for(auto& r:pairs)r=Pair{};lastFirstShown=0;}
    Snapshot take() noexcept {
        Snapshot s=c;c=Snapshot{};
        s.switches=switches();s.gateRun=gate.run;s.cap=lastCap;s.grid=gridSamples>=4;s.periodNs=period;s.lagRefresh=lagRefresh;
        return s;
    }
    bool gateOpen() const noexcept {return gate.open();}

private:
    struct Pair {
        bool live=false,dpt=false,hold=false,lostTraced=false;std::uint32_t first=0,second=0; // (lostTraced: ios-fg-trace-all)
        std::int64_t target=0,firstCall=0,secondCall=0,firstShown=0,holdFrom=0;double period=K::NominalPeriod;
    };
    std::atomic<unsigned> sw{0};
    Gate gate;
    std::int64_t lastNow=0,lastTarget=0,lastFirstShown=0,callNs=K::CallInit;unsigned lastCap=0;
    double lagRefresh=0;unsigned drainCool=0; // (ios-fg-drain)
    std::array<TracePic,Trace> trace{};unsigned traceN=0; // (ios-fg-trace)
    std::array<std::uint32_t,K::Recent> recent{};unsigned recentAt=0;std::uint32_t sequence=1;
    std::array<Pair,4> pairs{};unsigned pairAt=0;
    // our own vsync estimate (hold mode and the spacing counters; lane A's is used whenever its slot is known)
    double period=K::NominalPeriod;std::int64_t phase=0,lastSample=0;unsigned gridSamples=0;
    Snapshot c;

    bool anyLive() const noexcept {for(const auto& r:pairs)if(r.live)return true;return false;}
    bool gridOk(std::int64_t now) const noexcept {return gridSamples>=4 && now-lastSample<1'500'000'000;}
    std::int64_t vsyncAtOrAfter(std::int64_t t) const noexcept {return phase+std::int64_t(std::ceil(double(t-phase)/period)*period);}
    // Display times lie on the panel's grid: the newest one is the phase; the period follows whole-refresh gaps slowly.
    void gridSample(std::int64_t d) noexcept {
        if(!gridSamples || d-lastSample>2'000'000'000){phase=d;lastSample=d;gridSamples=1;return;}
        if(d<=lastSample)return;
        const double k=std::round(double(d-lastSample)/period);
        if(k>=1.0) {
            const double per=double(d-lastSample)/k;
            if(std::fabs(per-period)<0.3*period/k) {
                period=std::clamp(period+(per-period)*std::max(0.05,1.0/double(gridSamples)),0.95*K::NominalPeriod,1.05*K::NominalPeriod);
                ++gridSamples;
            }
        }
        phase=d;lastSample=d;
    }
};

} // namespace recomp::fg
