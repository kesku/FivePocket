#pragma once
// pace-caps (lane A, even pacing; 2026-10-04). Optional frame-rate caps that give EQUAL on-screen frame times on the Thor's
// ~120 Hz panel: 40 fps (3 refreshes per frame), 30 (4), 24 (5), and an automatic mode that picks among 60 / 40 / 30 / 24.
// Switch files (files/private-trace/, polled by the renderer every 60 presents: generic_renderer.cpp paceWindow()):
//   diag-pace-40 / diag-pace-30 / diag-pace-24   a fixed cap (several present: the lowest rate wins)
//   diag-pace-auto                               the automatic cap (wins over the fixed ones)
//   diag-pace-dpt     (modifier) also ask the compositor not to show a paced frame before its slot (VkPresentTimeGOOGLE
//                     desiredPresentTime = the slot's vsync - half a refresh)
//   diag-pace-lowlat  (modifier) a second hold after the present, so the next frame is kicked later (less input latency)
//   diag-pace-nolend  (modifier) the hold does not lend the prime core to main (see PrimeLend below); for the A/B
// No cap file present = no wait anywhere, no desired present time, every decision exactly as before (the display-time samples
// below are still folded into the vsync estimate so a cap switched on mid-run is aligned at once; nothing reads it meanwhile).
//
// What it does (DESIGN.md has the measurements behind every constant):
//  - The vsync grid (phase and period) is estimated from VK_GOOGLE_display_timing's actualPresentTime of our own frames
//    (direct_present.h feeds displayedAt()). The Thor's panel runs at 119.09 Hz (8.397 ms), not 8.333: the period is tracked.
//    Its first guess is the refresh length the swapchain reports (setRefresh; 8.333 ms when it reports none), so a panel in
//    another mode (60 / 90 Hz) is paced in whole refreshes of ITS grid.
//  - A frame's slot is the first vsync that is (a) at least n refreshes after the previous frame's slot and (b) still reachable
//    (now + lead + the Vulkan worker's backlog allowance). The render thread is held at the top of its present hook until slot -
//    lead, while the worker finishes the frame's draws (hold(): the caller parks the task through the executor's own delay, as
//    the game's Sleep does; no lock is held and nothing is spun). A late frame takes the first vsync it can make and the comb
//    re-anchors there: the missed slot is skipped, two frames never share a slot, nothing queues behind a late frame. The same
//    when the park itself returns too late for the slot. Main is throttled by the game's own kick acknowledgement (it parks on
//    the render thread's acknowledgement semaphore while the render thread is held).
//  - While the render thread is held, main may use the prime core until its kick, as it does without a cap while the render
//    thread waits for that kick (PrimeLend; the standing diag-v245-swap protocol, used through its own calls).
//  - lead = latch lead (the measured window of the compositor) + the measured release -> present-call time + a small trim that
//    follows the display feedback (a frame shown after / before its slot moves it by 0.1 ms).
//  - lowlat: after the present, the render thread is held again until (next release - the recent kick -> arrival time with a
//    margin), so main's next frame starts later and its picture is fresher when its slot comes.
//  - auto: starts from the frame time itself (the frames run unheld until two demand samples exist). Afterwards only late slots
//    step the cap down; main's CPU + run-queue time per frame (x 1.19 = the frame time main needs, measurable while main is
//    throttled) says when a step up is worth a trial. A failed trial doubles a hold-off.
// Pure logic: every time is passed in (nanoseconds on CLOCK_MONOTONIC, the clock of actualPresentTime), so the host test drives
// it with a simulated clock. One leaf mutex (never held across a wait or a call out).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <time.h>

namespace recomp::pace {

constexpr unsigned SwCap40=1u,SwCap30=2u,SwCap24=4u,SwAuto=8u,SwDpt=16u,SwLowLat=32u,SwNoLend=64u,SwShortBacklog=128u,SwCaps=SwCap40|SwCap30|SwCap24|SwAuto;

// Constants (ns unless noted). DESIGN.md section 7 lists the measurement each one comes from.
struct K {
    static constexpr std::int64_t NominalPeriod=8'333'333;     // the grid's first guess when the swapchain reports no refresh length
    static constexpr std::int64_t RefreshMin=2'000'000,RefreshMax=50'000'000; // a reported refresh length outside is not believed
    static constexpr unsigned GridMinSamples=4;                // display times needed before a slot is aligned
    static constexpr std::int64_t GridStale=1'500'000'000;     // no display time for this long: spacing only
    static constexpr unsigned GridMaxGap=240;                  // refreshes between two samples beyond which the phase restarts
    static constexpr std::int64_t TimeAhead=100'000'000,TimeBehind=2'000'000'000; // a display time this far from now is not one
    static constexpr std::int64_t LatchLead=9'800'000;         // present call returned -> the slot's vsync
    static constexpr std::int64_t DptExtra=1'200'000;          // more lead with diag-pace-dpt (the compositor cannot show it early)
    static constexpr std::int64_t PathInit=2'300'000,PathMin=500'000,PathMax=8'000'000; // release -> present call returned
    static constexpr std::int64_t TrimStep=100'000,TrimMin=-1'500'000,TrimMax=2'500'000,TrimLeak=256; // (an on-slot frame takes 1/256 back)
    static constexpr std::int64_t Backlog=7'000'000;           // a frame must arrive this long before its release: the Vulkan worker's
                                                               // remaining draws of the frame finish during the hold (p90 6.9 ms)
    static constexpr std::int64_t MinWait=200'000;             // shorter holds are not worth a park
    static constexpr std::int64_t OverTol=4'300'000;           // a park that returned this late has more likely lost its slot than not
    static constexpr std::int64_t IdleReset=250'000'000;       // a gap between presents after which the comb starts over
    static constexpr std::int64_t SnapTol=300'000;             // a time this close after a vsync still counts as that vsync
    static constexpr std::int64_t LendMinHold=1'000'000;       // shorter holds do not lend the prime core (v407's own threshold)
    static constexpr std::int64_t LendFresh=20'000'000;        // the hook's arm is cancelled only this soon after the hold it follows
    static constexpr unsigned LowLatMinSamples=8;
    static constexpr std::int64_t LowLatMargin=1'000'000;      // + 4 x the mean deviation of kick -> arrival
    static constexpr unsigned AutoSampleFrames=8;              // main's schedstat is read every this many presents
    static constexpr double AutoKappa=1.19;                    // frame time / (main's CPU + run-queue wait), measured 1.14-1.28
    static constexpr double AutoAlpha=0.3;                     // weight of a new demand sample
    static constexpr double AutoUpFrac=0.85,AutoProbeFrac=0.93; // demand / slot length: step up soon below / after a long dwell below
    static constexpr unsigned AutoLateLimit=7,AutoTrialLates=3,AutoFirstMin=8; // late frames: of the last 64 / of the last 32 on trial
    static constexpr std::int64_t AutoSettle=1'000'000'000,AutoUpDwell=4'000'000'000,AutoProbeDwell=20'000'000'000,
        AutoTrial=10'000'000'000,AutoHoldoffMin=4'000'000'000,AutoHoldoffMax=128'000'000'000,AutoRelax=30'000'000'000,
        AutoBusyGap=1'000'000'000,AutoFirstMax=200'000'000;
    static constexpr unsigned Ring=32,DispBins=8;
    static constexpr unsigned FrameRecs=64;                    // per-frame records kept per window (RENDER_DIAG_PACE_FR)
};
// pace-fg-tuning (v460's first device run): the desired present time that puts a picture on the vsync `vsync`. The Thor's
// compositor shows a timed picture at the first vsync LATER THAN desired - one refresh: v460 asked for vsync - half a refresh
// and 7,029 of 7,152 second pictures (given slot + 1.5 refreshes) came one refresh after the first instead of two (the other
// 123 followed a first picture that was itself a refresh early: 102 first pictures given slot - 0.5 were shown at slot - 1).
// The middle of the window that selects `vsync` is vsync + half a refresh.
// One formula for both lanes (fg_pair.h repeats it; tests/tuning_test.cpp compares the two).
inline std::int64_t desiredFor(std::int64_t vsync,double period) noexcept {return vsync+std::int64_t(period/2);}
// The automatic mode's caps, highest first: nominal frame lengths of 60 / 40 / 30 / 24 fps.
constexpr std::array<std::int64_t,4> CapLength{16'666'667,25'000'000,33'333'333,41'666'667};
constexpr std::array<unsigned,4> CapFps{60,40,30,24};
// Refreshes per frame of a cap on a grid of `period`: the fewest whole refreshes that are not shorter than the cap's frame
// (120 Hz: 2 / 3 / 4 / 5; the 0.1 allows for a panel slightly slower than nominal, e.g. 119.09 Hz).
inline unsigned refreshesFor(std::int64_t length,double period) noexcept {
    const double r=std::ceil(double(length)/period-0.1);
    return r<1.0?1u:unsigned(r);
}
// ios-direct-present (Claude, 2026-10-16): on Darwin CLOCK_MONOTONIC counts sleep, while MoltenVK's actualPresentTime /
// desiredPresentTime (MTLDrawable presentedTime / presentAtTime) and libc++'s steady_clock (the executor's park) are
// mach_absolute_time, i.e. CLOCK_UPTIME_RAW. One clock for the pacer, the display times and the park, as on Android.
#if defined(__APPLE__)
inline std::int64_t monotonicNs() noexcept {return std::int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));}
#else
inline std::int64_t monotonicNs() noexcept {timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return std::int64_t(t.tv_sec)*1000000000ll+t.tv_nsec;}
#endif

// One 60-present window's counters and the state at its end (RENDER_DIAG_PACE).
struct Snapshot {
    unsigned switches=0,fps=0,refreshes=0,autoIndex=0;
    unsigned slots=0,held=0,late=0,unpaced=0,noGrid=0,refused=0,clamped=0,overslept=0,badTimes=0; // frames seen / held / late for their slot / ...
    unsigned onSlot=0,early=0,tailLate=0;                                  // display feedback against the planned slot
    unsigned held2=0,autoUp=0,autoDown=0;
    unsigned lends=0,mainLeft=0,cancels=0,lends2=0,mainLeft2=0;            // the prime core lent for a hold / main left it by itself / ...
    std::uint64_t waitNs=0,waitMaxNs=0,overshootNs=0,overshootMaxNs=0,wait2Ns=0;
    std::array<unsigned,K::DispBins> disp{};                               // refreshes between consecutive displayed frames
    std::int64_t leadNs=0,pathNs=0,trimNs=0,kickNs=0,kickDevNs=0,demandNs=0,holdoffNs=0,firstNs=0,refreshNs=0;
    double periodNs=0;bool grid=false;
    // pace-fg-tuning: the lead trim of frames whose first picture carries a desired present time (trimNs: the other frames'),
    // frames planned as timed, and one record per displayed paced frame (in display-feedback order): off = refreshes from the
    // slot (0 on it, -1 early), begin / end = the slot minus the present call's begin / return (us), timed.
    std::int64_t trimTimedNs=0;unsigned timedFrames=0;
    struct Rec {std::int8_t off=0;bool timed=false;std::int32_t beginUs=0,endUs=0;};
    std::array<Rec,K::FrameRecs> recs{};unsigned recN=0;
};

class Pacer {
public:
    // ---- the renderer's switch poll (one thread)
    void setSwitches(unsigned bits) noexcept {
        const unsigned old=sw.exchange(bits,std::memory_order_relaxed);
        // The automatic cap and the demand estimate start over whenever they are switched on, also after a stretch with no cap
        // file at all (a 13 s A/B block): what they knew belongs to another scene.
        if(((bits&SwAuto) && !(old&SwAuto)) || ((bits&SwCaps) && !(old&SwCaps)))gen.fetch_add(1,std::memory_order_relaxed);
    }
    unsigned switches() const noexcept {return sw.load(std::memory_order_relaxed);}
    bool on() const noexcept {return (sw.load(std::memory_order_relaxed)&SwCaps)!=0;}

    // ---- present thread, when a swapchain is created: the panel's refresh length as VK_GOOGLE_display_timing reports it
    // (0: unknown). A value in another class than the current first guess restarts the vsync estimate there.
    void setRefresh(std::uint64_t ns) noexcept {
        std::lock_guard l(m);
        refreshNs=std::int64_t(std::min<std::uint64_t>(ns,1'000'000'000u));
        const double want=(refreshNs>=K::RefreshMin && refreshNs<=K::RefreshMax)?double(refreshNs):double(K::NominalPeriod);
        if(std::fabs(want-nominal)<=0.02*nominal)return;
        nominal=want;period=want;gridSamples=0;outliers=0;resetPlan();
    }

    // ---- render thread, top of the present hook. Returns the time to hold the thread until (0: no hold).
    std::int64_t arrive(std::int64_t now) noexcept {
        const unsigned s=sw.load(std::memory_order_relaxed);
        if(!(s&SwCaps))return 0;
        std::lock_guard l(m);
        if(lastArrive && now-lastArrive>K::IdleReset)resetPlan();
        if(const unsigned g=gen.load(std::memory_order_relaxed);g!=seenGen){seenGen=g;resetAuto();}
        if(ackAt && now>ackAt) { // the previous hand-back -> this arrival: main's kick + the render thread's frame
            const std::int64_t g=std::min<std::int64_t>(now-ackAt,100'000'000);
            if(!kickSamples){kickMean=g;kickDev=g/4;}
            else {kickDev+=(std::llabs(g-kickMean)-kickDev)/4;kickMean+=(g-kickMean)/8;}
            ++kickSamples;
        }
        ackAt=0;
        if((s&SwAuto) && !au.started) { // unheld frames: their own interval is the frame time (the first two still carry the old cap)
            if(const std::int64_t d=au.firstAt?now-au.firstAt:0;d>0 && d<K::AutoFirstMax) {
                if(au.firstSkip<2)++au.firstSkip;
                else if(au.firstN<au.first.size())au.first[au.firstN++]=std::int32_t(d/1000);
            }
            au.firstAt=now;
        }
        lastArrive=now;++frames;++c.slots;
        const double P=period;
        unsigned n=0;unsigned fps=0;
        if(s&SwAuto) {
            if(au.started){n=refreshesFor(CapLength[au.index],P);fps=CapFps[au.index];}
        } else {
            const unsigned i=(s&SwCap24)?3u:(s&SwCap30)?2u:1u;
            n=refreshesFor(CapLength[i],P);fps=CapFps[i];
        }
        curFps=fps;curN=n;
        const bool bound=lastBound;lastBound=false;
        cur=Plan{};cur.arrive=now;cur.release=now;cur.n=n;curPending=true;curPendingView.store(true,std::memory_order_relaxed);
        if(!n){++c.unpaced;lastTarget=0;lastRelease=0;lateBits<<=1;return 0;} // (auto before its first estimate)
        cur.timed=timedNow(s);c.timedFrames+=cur.timed; // pace-fg-tuning: this frame's first picture will carry a desired time
        const std::int64_t lead=leadNs(cur.timed);
        const std::int64_t span=std::llround(double(n)*P);
        const bool grid=gridOk(now);
        std::int64_t target=0,release=now;bool late=false;
        if(grid) {
            // (Claude 2026-10-18, ios-pace-backlog2: 2 ms instead of the Thor-measured 7 ms; c204: lone 3-refresh frames on iOS)
            const std::int64_t backlog=(s&SwShortBacklog)?2'000'000:K::Backlog;
            const std::int64_t earliest=now+lead+backlog;
            const std::int64_t next=lastTarget?lastTarget+span:0;
            target=vsyncAtOrAfter(std::max(next,earliest)-K::SnapTol);
            late=next && target-vsyncNearest(next)>std::int64_t(P/2);
            release=std::max(now,target-lead);
        } else {
            ++c.noGrid;
            if(lastRelease) { // no display phase: equal spacing on our own clock
                release=lastRelease+span;
                if(release<now){late=now-release>std::int64_t(P/2);release=now;}
            }
        }
        if(!bound) { // the previous frame did not go through the queued present: nothing to pace against yet
            release=now;late=false;
            if(grid)target=vsyncAtOrAfter(now+lead+K::Backlog-K::SnapTol);
        }
        if(release-now>span+2*std::int64_t(P)) { // (cannot happen with a sane comb: never sleep on a broken one)
            ++c.clamped;release=now;target=grid?vsyncAtOrAfter(now+lead+K::Backlog-K::SnapTol):0;
        }
        lateBits=(lateBits<<1)|(late?1u:0u);
        if(late)++c.late;
        lastTarget=target;lastRelease=release;
        cur.target=target;cur.release=release;cur.late=late;
        cur.waited=release-now>=K::MinWait;
        if(!cur.waited){cur.release=now;lastRelease=now;return 0;}
        ++c.held;
        return release;
    }
    // ---- render thread, after the hold arrive() asked for (parked false: the executor refused, nothing was waited).
    // lent / mainLeft: the prime core was lent to main for the hold / main had left it by itself when the hold ended.
    void released(std::int64_t now,bool parked,bool lent=false,bool mainLeft=false) noexcept {
        std::lock_guard l(m);
        c.lends+=lent;c.mainLeft+=lent && mainLeft;
        const std::int64_t waited=now>cur.arrive?now-cur.arrive:0;
        c.waitNs+=std::uint64_t(waited);c.waitMaxNs=std::max(c.waitMaxNs,std::uint64_t(waited));
        if(!parked){++c.refused;cur.release=now;cur.waited=false;lastRelease=now;return;}
        const std::int64_t over=now>cur.release?now-cur.release:0;
        c.overshootNs+=std::uint64_t(over);c.overshootMaxNs=std::max(c.overshootMaxNs,std::uint64_t(over));
        if(over>K::OverTol) { // the park itself came back too late for the slot (the thread waited for its permit behind a world
            ++c.overslept;    // stop): as a late frame, it takes the first vsync it can make with a full lead and the comb follows
            if(cur.target){cur.target=vsyncAtOrAfter(now+leadNs(cur.timed)-K::SnapTol);lastTarget=cur.target;}
            cur.release=now;lastRelease=now;
        }
    }
    // ---- render thread, when the frame's present is queued: the plan belongs to this tag from here on.
    void queued(std::uint32_t tag) noexcept {
        if(!curPendingView.load(std::memory_order_relaxed))return; // (no cap: nothing planned, no lock)
        std::lock_guard l(m);
        if(!curPending)return;
        curPending=false;curPendingView.store(false,std::memory_order_relaxed);lastBound=true;
        auto& r=ring[ringNext++%K::Ring]; // (the newest plans, whatever their tags: a tag is the renderer's queue sequence)
        if(!r.live)++ringLive;
        r.tag=tag;r.target=cur.target;r.release=cur.release;r.waited=cur.waited;r.live=true;
        r.n=cur.n; // frame-gen (lane B): the cap in refreshes this plan was made under (integrate-v460)
        r.timed=cur.timed;r.begin=0;r.end=0; // pace-fg-tuning
    }
    // ---- render thread, after the present (top of the presentation callback). Returns the lowlat hold's end (0: none).
    // cancelled: the hook's arm of the prime-core swap was cancelled for this frame (PrimeLend).
    std::int64_t afterPresent(std::int64_t now,bool cancelled=false) noexcept {
        const unsigned s=sw.load(std::memory_order_relaxed);
        if(!(s&SwCaps))return 0;
        std::lock_guard l(m);
        c.cancels+=cancelled;
        std::int64_t until=0;
        if((s&SwLowLat) && lastBound && cur.target && cur.n && kickSamples>=K::LowLatMinSamples) {
            const std::int64_t span=std::llround(double(cur.n)*period);
            until=cur.target+span-leadNs(cur.timed)-K::Backlog-(kickMean+4*kickDev+K::LowLatMargin); // (always less than a slot from now)
            if(until-now<K::MinWait)until=0;
        }
        ackAt=now;
        if(until)++c.held2;
        return until;
    }
    void afterReleased(std::int64_t now,bool parked,bool lent=false,bool mainLeft=false) noexcept {
        std::lock_guard l(m);
        c.lends2+=lent;c.mainLeft2+=lent && mainLeft;
        if(now>ackAt){c.wait2Ns+=std::uint64_t(now-ackAt);ackAt=now;}
        if(!parked)++c.refused;
    }
    // ---- render thread (any cap): main's cumulative CPU + run-queue time. True when the caller should sample now. The estimate
    // is kept under a fixed cap too (demand_ms in the log: what the automatic mode would see there); only auto acts on it.
    bool wantsMainBusy() noexcept {return (sw.load(std::memory_order_relaxed)&SwCaps) && !(busyTick++%K::AutoSampleFrames);}
    void mainBusy(std::int64_t now,std::uint64_t busyNs) noexcept {
        std::lock_guard l(m);
        if(const unsigned g=gen.load(std::memory_order_relaxed);g!=seenGen){seenGen=g;resetAuto();}
        const bool usable=au.busyAt && now-au.busyAt<K::AutoBusyGap && busyNs>=au.busy && frames>au.busyFrames;
        if(usable) {
            const double perFrame=double(busyNs-au.busy)/double(frames-au.busyFrames);
            const double d=K::AutoKappa*perFrame;
            au.demand=au.samples?au.demand+(d-au.demand)*K::AutoAlpha:d;
            ++au.samples;
            if(sw.load(std::memory_order_relaxed)&SwAuto)decide(now);
        }
        au.busy=busyNs;au.busyAt=now;au.busyFrames=frames;
    }
    // frame-gen (lane B; integrate-v460, frame-gen/MERGE.md place 4): the plan of a queued frame (its slot, the cap in refreshes
    // per frame, the period estimate). False: no plan for this tag (no cap, or the frame was not paced). Called only while a
    // diag-fg-pair-* file is present (Presenter::fgBefore). Found by tag as desired() / presented() find it.
    bool slot(std::uint32_t tag,std::int64_t* target,unsigned* refreshes,double* periodNs) noexcept {
        std::lock_guard l(m);
        const Bound* r=find(tag);
        if(!r)return false;
        *target=r->target;*refreshes=r->n;*periodNs=period;return true;
    }
    // ---- present thread: the desired present time of a paced frame (0 unless diag-pace-dpt and the tag is planned).
    // pace-fg-tuning: `now` (0: not given) is the moment the present call begins, kept for the per-frame record under any cap.
    std::uint64_t desired(std::uint32_t tag,std::int64_t now=0) noexcept {
        const unsigned s=sw.load(std::memory_order_relaxed);
        if(!(s&SwCaps))return 0;
        std::lock_guard l(m);
        Bound* r=find(tag);
        if(!r)return 0;
        if(now)r->begin=now;
        if(!(s&SwDpt) || !r->target)return 0;
        return std::uint64_t(desiredFor(r->target,period));
    }
    // pace-fg-tuning: the pair path (frame-gen) says whether it gives the frames' first pictures a desired present time. While
    // it does, the pacer plans frames as under diag-pace-dpt (the extra lead, the timed trim): one lead rule for both lanes, and
    // a pairs phase leaves the trim of untimed frames alone. Presenter::fgBefore, only once a diag-fg-pair-* file was seen.
    void pairsTimed(bool on) noexcept {extTimed.store(on,std::memory_order_relaxed);}
    // ---- present thread: vkQueuePresentKHR of `tag` returned at `now`.
    void presented(std::uint32_t tag,std::int64_t now) noexcept {
        std::lock_guard l(m);
        Bound* r=find(tag);
        if(!r)return;
        r->end=now; // pace-fg-tuning (the per-frame record)
        if(!r->waited)return;
        const std::int64_t path=now-r->release;
        if(path<=0 || path>40'000'000)return;
        pathNs+=(std::clamp(path,K::PathMin,K::PathMax)-pathNs)/8;
    }
    // ---- present thread, at `now`: `tag` was displayed at `actual` (VK_GOOGLE_display_timing). Always called (see the header).
    void displayed(std::uint32_t tag,std::uint64_t actualNs,std::int64_t now) noexcept {
        std::lock_guard l(m);
        if(actualNs>std::uint64_t(now+K::TimeAhead) || std::int64_t(actualNs)<now-K::TimeBehind){++c.badTimes;return;} // (not a display time of ours)
        const std::int64_t actual=std::int64_t(actualNs);
        gridSample(actual);
        if(lastDisplayed && actual>lastDisplayed) {
            const auto k=std::llround(double(actual-lastDisplayed)/period);
            ++c.disp[std::size_t(std::clamp<long long>(k,0,K::DispBins-1))];
        }
        lastDisplayed=actual;
        Bound* r=find(tag);
        if(!r)return;
        r->live=false;--ringLive;
        if(!r->target)return;
        const auto e=std::llround(double(actual-r->target)/period);
        std::int64_t& trim=r->timed?trimTimedNs:trimNs; // pace-fg-tuning: timed and untimed frames learn separately
        if(e==0){++c.onSlot;trim-=trim/K::TrimLeak;}
        else if(e<0){++c.early;if(r->waited)trim=std::max(trim-K::TrimStep,K::TrimMin);}
        else {++c.tailLate;if(r->waited)trim=std::min(trim+K::TrimStep,K::TrimMax);}
        if(c.recN<K::FrameRecs) { // pace-fg-tuning: the per-frame record (what the window counters cannot say)
            const auto us=[&](std::int64_t t){return t?std::int32_t(std::clamp<std::int64_t>((r->target-t)/1000,-999'999,999'999)):0;};
            c.recs[c.recN++]=Snapshot::Rec{std::int8_t(std::clamp<long long>(e,-9,9)),r->timed,us(r->begin),us(r->end)};
        }
    }
    // ---- the renderer's 60-present window: the counters since the last call and the state now.
    Snapshot take() noexcept {
        std::lock_guard l(m);
        Snapshot s=c;c=Snapshot{};
        s.switches=sw.load(std::memory_order_relaxed);s.fps=curFps;s.refreshes=curN;s.autoIndex=au.index;
        const bool timed=timedNow(s.switches);
        s.leadNs=leadNs(timed);s.pathNs=pathNs;s.trimNs=trimNs;s.trimTimedNs=trimTimedNs;s.kickNs=kickMean;s.kickDevNs=kickDev;
        s.demandNs=std::int64_t(au.demand);s.holdoffNs=au.holdoff;s.firstNs=std::int64_t(au.firstNeed);s.refreshNs=refreshNs;
        s.periodNs=period;s.grid=gridSamples>=K::GridMinSamples;
        return s;
    }

private:
    struct Plan {std::int64_t arrive=0,release=0,target=0;unsigned n=0;bool waited=false,late=false,timed=false;};
    struct Bound {std::uint32_t tag=0;std::int64_t target=0,release=0;bool waited=false,live=false;unsigned n=0; // n: frame-gen (lane B), integrate-v460
                  bool timed=false;std::int64_t begin=0,end=0;}; // pace-fg-tuning: planned as timed; the present call's begin / return
    struct Auto {
        bool started=false;unsigned index=3,samples=0;
        double demand=0,firstNeed=0;std::uint64_t busy=0;std::int64_t busyAt=0;std::uint64_t busyFrames=0;
        std::int64_t switchedAt=0,upSince=0,probeSince=0,holdoff=K::AutoHoldoffMin,holdoffUntil=0,upAt=0,relaxAt=0;
        std::array<std::int32_t,32> first{};unsigned firstN=0,firstSkip=0;std::int64_t firstAt=0; // the unheld frames' intervals (us)
    };

    std::atomic<unsigned> sw{0},gen{0};
    std::atomic<bool> extTimed{false}; // pace-fg-tuning: the pair path times the frames' first pictures
    std::mutex m;
    // the vsync grid: `phase` is the estimated time of one vsync, `period` the refresh length, `nominal` its first guess
    double nominal=double(K::NominalPeriod),period=double(K::NominalPeriod);
    std::int64_t phase=0,lastSample=0,refreshNs=0;unsigned gridSamples=0,outliers=0;
    // the comb
    std::int64_t lastArrive=0,lastTarget=0,lastRelease=0,lastDisplayed=0;bool lastBound=false,curPending=false;
    std::atomic<bool> curPendingView{false};
    Plan cur;std::array<Bound,K::Ring> ring{};unsigned ringNext=0,ringLive=0;
    unsigned curFps=0,curN=0,seenGen=0;std::uint64_t lateBits=0,frames=0; // lateBits: the last 64 frames, newest in bit 0
    std::int64_t pathNs=K::PathInit,trimNs=0,trimTimedNs=0; // trimTimedNs: pace-fg-tuning (frames planned as timed)
    // lowlat: the previous hand-back and the kick -> arrival statistics
    std::int64_t ackAt=0,kickMean=0,kickDev=0;unsigned kickSamples=0;
    Auto au;unsigned busyTick=0;
    Snapshot c;

    // pace-fg-tuning: a frame is planned as timed when its first picture will carry a desired present time: diag-pace-dpt, or
    // the pair path (pairsTimed). Timed frames get the extra lead (the compositor cannot show them early) and their own trim.
    bool timedNow(unsigned s) const noexcept {return (s&SwCaps) && ((s&SwDpt) || extTimed.load(std::memory_order_relaxed));}
    std::int64_t leadNs(bool timed) const noexcept {
        return K::LatchLead+pathNs+(timed?trimTimedNs+K::DptExtra:trimNs);
    }
    bool gridOk(std::int64_t now) const noexcept {return gridSamples>=K::GridMinSamples && now-lastSample<K::GridStale;}
    std::int64_t vsyncAtOrAfter(std::int64_t t) const noexcept {return phase+std::int64_t(std::ceil(double(t-phase)/period)*period);}
    std::int64_t vsyncNearest(std::int64_t t) const noexcept {return phase+std::int64_t(std::llround(double(t-phase)/period)*period);}
    Bound* find(std::uint32_t tag) noexcept {
        if(ringLive)for(auto& r:ring)if(r.live && r.tag==tag)return &r;
        return nullptr;
    }
    void resetPlan() noexcept {lastTarget=0;lastRelease=0;lastBound=false;ackAt=0;lateBits=0;cur=Plan{};}
    void resetAuto() noexcept {au=Auto{};}
    // An alpha-beta tracker: the first samples are trusted (gains 1/n), later ones move the phase by 1/4 and the period by 2 %
    // of their error. A sample far off the grid is ignored; three in a row (the display changed) restart the phase.
    void gridSample(std::int64_t d) noexcept {
        if(!gridSamples || d-lastSample>std::int64_t(double(K::GridMaxGap)*period)){phase=d;lastSample=d;gridSamples=1;outliers=0;return;}
        if(d<=lastSample)return;
        const double k=std::round(double(d-phase)/period);
        if(k<1.0){lastSample=d;return;}
        const double predicted=double(phase)+k*period,error=double(d)-predicted;
        if(std::fabs(error)>0.30*period) {
            if(++outliers>=3){phase=d;gridSamples=1;outliers=0;}
            lastSample=d;return;
        }
        outliers=0;
        const double a=std::max(0.25,1.0/double(gridSamples)),b=std::max(0.02,0.5/double(gridSamples));
        phase=std::int64_t(predicted+a*error);
        period=std::clamp(period+b*error/k,0.95*nominal,1.05*nominal);
        lastSample=d;++gridSamples;
    }
    // The automatic cap (under m, at every demand sample). Whether a cap HOLDS is decided by the slots alone (late frames): the
    // demand estimate may be off by several per cent under a cap (DESIGN.md section 5), so it never steps the cap down; it only
    // says when a step up is worth a trial.
    void decide(std::int64_t now) noexcept {
        const auto length=[&](unsigned i){return double(refreshesFor(CapLength[i],period))*period;};
        if(!au.started) {
            if(au.samples<2)return;
            double need=au.demand;
            if(au.firstN>=K::AutoFirstMin) { // the frames ran unheld until now: the median of their intervals is the frame time itself
                auto v=au.first;const auto mid=v.begin()+au.firstN/2;
                std::nth_element(v.begin(),mid,v.begin()+au.firstN);need=double(*mid)*1e3;
            }
            au.firstNeed=need;au.index=3;
            for(unsigned i=0;i<4;++i)if(need<=K::AutoUpFrac*length(i)){au.index=i;break;}
            au.started=true;au.switchedAt=now;return;
        }
        if(now-au.switchedAt<K::AutoSettle)return;
        const auto count=[](std::uint64_t b){unsigned n=0;for(;b;b&=b-1)++n;return n;};
        // Step down: 7 of the last 64 frames were late for their slot (one frame in nine, however they are spread), or 3 of the
        // last 32 while a step up is on trial (its first 10 s). The next step up waits for the hold-off, which doubles when this
        // step down undoes a step up that had not yet held for 30 s.
        const bool trial=au.upAt && now-au.upAt<K::AutoTrial;
        if(au.index<3 && (count(lateBits)>=K::AutoLateLimit || (trial && count(lateBits&0xffffffffu)>=K::AutoTrialLates))) {
            if(au.upAt && now-au.upAt<K::AutoRelax)au.holdoff=std::min(au.holdoff*2,K::AutoHoldoffMax);
            au.holdoffUntil=now+au.holdoff;
            ++au.index;++c.autoDown;au.switchedAt=now;au.upSince=0;au.probeSince=0;au.upAt=0;lateBits=0;return;
        }
        // Step up, on trial: to the highest cap the estimate fits with the full margin for 4 s; or by one cap when the estimate
        // has been under that cap's slot by the small margin for 20 s (the estimate may read high). Both wait for the hold-off.
        if(au.index>0) {
            unsigned best=au.index;
            for(unsigned i=0;i<au.index;++i)if(au.demand<K::AutoUpFrac*length(i)){best=i;break;}
            au.upSince=best<au.index?(au.upSince?au.upSince:now):0;
            au.probeSince=au.demand<K::AutoProbeFrac*length(au.index-1)?(au.probeSince?au.probeSince:now):0;
            unsigned to=au.index;
            if(au.upSince && now-au.upSince>=K::AutoUpDwell)to=best;
            else if(au.probeSince && now-au.probeSince>=K::AutoProbeDwell)to=au.index-1;
            if(to<au.index && now>=au.holdoffUntil) {
                au.index=to;++c.autoUp;au.switchedAt=now;au.upSince=0;au.probeSince=0;au.upAt=now;au.relaxAt=now;lateBits=0;return;
            }
        }
        // a step up that held for AutoRelax earns half of the hold-off back (and again every AutoRelax it goes on holding)
        if(au.holdoff>K::AutoHoldoffMin && au.upAt && now-au.relaxAt>=K::AutoRelax){au.holdoff/=2;au.relaxAt=now;}
    }
};

// The standing prime-core lending (prime_swap_v245.h, switch diag-v245-swap) under a cap. Without a cap the render thread's
// wait for main's kick lends cpu7 to main for ~10-12 ms of every frame. Under a cap the render thread spends that time in the
// pacer's hold, which is a delay (the executor makes no swap for it), and when the hold ends main has usually kicked already:
// the armed swap then lands on the 1.2 ms wait for the second kick. So the hold itself lends, through the protocol's own calls:
//   begin   arm + beforePark: main gets {7} if the swap switch is on (beforePark's own test; no call otherwise)
//   end     release(false): the render thread takes cpu7 back if main still has it, exactly as when its wait completes;
//           true when main had already left by itself (it does at its kick: wakeGuestWaiters -> release(true))
//   cancelArm   after the hook armed the swap for the render thread's next wait: when main left during the hold only the
//           1.2 ms wait is left to lend, which is not worth two affinity calls and two migrations of main: the arm is taken
//           back. When main had not left, the arm stays and the wait for the kick lends again, as without a cap.
// diag-pace-lowlat's second hold lends the same way; it is that frame's armed swap (begin consumes the hook's arm), so no
// further swap follows it in the frame.
// Swap = recomp_v245::PrimeSwap (a template so this header needs no include of it and the host test drives the real one).
// Render thread only.
template<class Swap> class PrimeLend {
public:
    void newFrame() noexcept {leftAt=0;}
    bool begin(Swap& s,std::int64_t holdNs) noexcept {
        if(holdNs<K::LendMinHold)return false;
        const auto before=s.swaps.load(std::memory_order_relaxed);
        byMain=s.releasesByMain.load(std::memory_order_relaxed);
        s.arm();s.beforePark();
        return s.swaps.load(std::memory_order_relaxed)!=before; // (only this thread swaps)
    }
    bool end(Swap& s) noexcept {
        s.release(false);
        return s.releasesByMain.load(std::memory_order_relaxed)!=byMain; // (final once release returned: main counts before it clears the swap)
    }
    void held(bool lent,bool mainLeft,std::int64_t now) noexcept {leftAt=lent && mainLeft?now:0;}
    bool cancelArm(Swap& s,std::int64_t now) noexcept {
        const bool fresh=leftAt && now-leftAt<=K::LendFresh;
        leftAt=0;
        if(fresh)s.armed.store(false,std::memory_order_relaxed);
        return fresh;
    }
private:
    std::int64_t leftAt=0;unsigned long long byMain=0;
};

// The render thread's two holds, as the runtime runs them (renderer_boundary_trace.cpp paceHold; the host tests call the same
// function with a simulated park, and with the real executor's). phase 0: top of the present hook. phase 1: top of the
// presentation callback (the frame's present is queued). now(): CLOCK_MONOTONIC ns. ready(): what must run on this thread
// before it may park (the runtime delivers a pending deferred interrupt there, BEFORE main is moved onto this thread's core);
// false: the thread cannot be parked. park(until): parks the thread until `until`; false when it could not (nothing was
// waited); it may throw (the executor's cancellation): the lend is then returned while unwinding.
template<class Swap,class Now,class Ready,class Park>
void hold(Pacer& p,PrimeLend<Swap>& lend,Swap& swap,unsigned phase,Now&& now,Ready&& ready,Park&& park) {
    struct Scope { // the end of a hold's lend, on every way out
        PrimeLend<Swap>& lend;Swap& swap;bool lent;bool left=false;
        void finish() noexcept {if(lent && !done)left=lend.end(swap);done=true;}
        ~Scope(){finish();}
        bool done=false;
    };
    const bool lendOn=!(p.switches()&SwNoLend);
    const std::int64_t t0=now();
    if(phase==0) {
        lend.newFrame();
        const std::int64_t until=p.arrive(t0);
        if(!until)return;
        const bool can=ready();
        Scope scope{lend,swap,can && lendOn && lend.begin(swap,until-now())};
        const bool parked=can && park(until);
        scope.finish();
        const std::int64_t t1=now();
        lend.held(scope.lent,scope.left,t1);
        p.released(t1,parked,scope.lent,scope.left);
        return;
    }
    const bool cancelled=lendOn && lend.cancelArm(swap,t0);
    const std::int64_t until=p.afterPresent(t0,cancelled);
    if(!until)return;
    const bool can=ready();
    Scope scope{lend,swap,can && lendOn && !cancelled && lend.begin(swap,until-now())};
    const bool parked=can && park(until);
    scope.finish();
    p.afterReleased(now(),parked,scope.lent,scope.left);
}

// One pacer per process (an inline function's static: one object across the library's translation units).
inline Pacer& pacer() noexcept {static Pacer p;return p;}
// The hooks of the other files: out of line and noexcept, so their callers keep their control flow (PGO records).
[[gnu::noinline]] inline void queued(std::uint32_t tag) noexcept {pacer().queued(tag);}
[[gnu::noinline]] inline std::uint64_t desiredPresentNs(std::uint32_t tag) noexcept {return tag && pacer().on()?pacer().desired(tag,monotonicNs()):0;}
[[gnu::noinline]] inline void pairsTimed(bool on) noexcept {pacer().pairsTimed(on);} // pace-fg-tuning (Presenter::fgBefore)
[[gnu::noinline]] inline void presentCalled(std::uint32_t tag) noexcept {if(tag && pacer().on())pacer().presented(tag,monotonicNs());}
[[gnu::noinline]] inline void displayedAt(std::uint32_t tag,std::uint64_t actualNs) noexcept {pacer().displayed(tag,actualNs,monotonicNs());}
[[gnu::noinline]] inline void refreshIs(std::uint64_t ns) noexcept {pacer().setRefresh(ns);}

} // namespace recomp::pace
