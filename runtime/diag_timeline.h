#pragma once
// Attempt v159: timeline probe. A lock-free ring of timestamped events (host thread id, guest token, kind, argument)
// recorded at the points where guest threads wait or hand work over: permit waits, guest-wait parks, delays, graphics
// submissions, renderer drains, GPU waits, exclusive spans, presents. Armed by files/private-trace/diag-timeline (checked
// once a second by recordTimelineTick()); when armed, the next 1048576 events (v264; 65536 before) are recorded, then the ring is written once
// to files/private-trace/timeline-<pid>-<n>.bin and the probe disarms (the switch file is removed). Off: one relaxed
// atomic load per event site. Format: 32-byte header "RTL1", count, clock ns at start; then records of
// {uint64 ns, uint32 tid, uint32 token, uint16 kind, uint16 arg, uint32 extra}.
#include <atomic>
#include <cstdint>

namespace recomp::diag::timeline {
enum Kind : std::uint16_t {
    PermitWaitBegin=1, PermitWaitEnd=2,       // arg: permit reason (rotation, safepoint, wait return, delay return, start)
    ParkBegin=3, ParkEnd=4,                   // parallelWait path, not necessarily OS sleep (arg 1 = predicate notified, 2 = poll, 3 = deadline)
    DelayBegin=5, DelayEnd=6,                 // parallelDelay
    SubmitBegin=7, SubmitEnd=8,               // graphics submission handler (extra: WPTR word)
    DrainBegin=9, DrainEnd=10,                // renderer queue drain (arg: reason 0 barrier, 1 present, 2 submit, 3 other)
    GpuWaitBegin=11, GpuWaitEnd=12,           // lockedQueueWaitIdle / fence waits (arg: site)
    ExclusiveBegin=13, ExclusiveEnd=14,       // outermost exclusive ownership, including the wait for safepoints
    Present=15,                               // present hook (a frame boundary)
    WorkerItemBegin=16, WorkerItemEnd=17,     // renderer thread executes a queued item
    Signal=20,                                // v195: a guest wait object signalled (arg 1 handle, 2 dispatcher, 3 CS; extra: object)
    PollPhase=21,                             // v201: a Sleep(0) poll phase began (yields <= 50 us apart); extra: the PREVIOUS
                                              // phase's length in us (its start is this thread's previous PollPhase event)
    HandoffWorkerCreated=22,                  // v218: tile/scene IRQ handoff: the submitter created the IRQ worker (extra: its token)
    HandoffWorkerStart=23,                    // v218: the IRQ worker's body began (on the worker's host thread)
    HandoffWorkerEnd=24,                      // v218: the IRQ worker's body returned (after markGuestThreadReturned)
    HandoffReturned=25,                       // v218: the submitter's runNativeWorkerNext returned (arg 1 = returned)
    // v318: the input-to-display latency probe (tools/iter/timeline_latency.py). A frame is tied together by `extra` = the low
    // 32 bits of its present's async-queue sequence.
    InputEdge=26,                             // v318: a synthetic (diag-stick) step took effect; ns = the step's start on the device clock
    InputPoll=27,                             // v318: the first input read returning a changed state (arg 1 synthetic, 2 physical)
    PresentQueued=28,                         // v318: the render thread queued the frame's present (extra: tag; latency-path: arg =
                                              // the items queued before it and not yet executed by the Vulkan worker, max 65535)
    HostPresentBegin=29, HostPresentEnd=30,   // v318: vkQueuePresentKHR of the frame (extra: tag)
    Displayed=31,                             // v318: VK_GOOGLE_display_timing actualPresentTime (ns) of the frame (extra: tag)
    // latency-path package ("lat2", 2026-10-03; tools/iter/present_path.py): measurement only, recorded while armed.
    InputRead=32,                             // every user-0 pad read (arg: latency_lat2.h readFlags; extra: the guest link
                                              // register of the XamInputGetState call, i.e. its call site)
    WorkerPresentBegin=33,                    // the Vulkan worker starts the frame's queued present item (extra: tag)
    FrameSubmitted=34,                        // the frame's last renderer command buffer was submitted (direct path; extra: tag)
    PresenterSubmitted=35,                    // the presenter pass was submitted behind the acquire (extra: tag)
    PresenterGpuDone=36,                      // the presenter pass's fence seen signalled by the watcher (files/private-trace/
                                              // diag-lat-gpu-done; extra: tag; arg: the pass's GPU time in 10 us units, 0xFFFF unknown)
    DisplayMargin=37,                         // ns = actualPresentTime; arg = the SIGNED presentMargin (compositor latch - render
                                              // complete) in 10 us units as int16 (0x8000 = beyond +-327 ms); extra: tag
    WorkerPresentEnd=38,                      // the worker's present item returned (extra: tag)
    FrameShift=39,                            // diag-lat-motion: the displayed frame's horizontal shift vs the previous watched
                                              // frame (arg: latency_lat2.h shiftArg; extra: tag; ns: when the watcher measured it)
};
extern std::atomic<bool> armed;
// v195: the guest object the calling thread is about to park on (set by the registry around its wait handler; the
// executor stamps it into ParkBegin's extra). 0 = unknown.
extern thread_local std::uint32_t waitObject;
void record(Kind kind, std::uint16_t arg = 0, std::uint32_t extra = 0) noexcept;
// An explicit timestamp in the event's source clock domain. Driver timestamps
// (Displayed/DisplayMargin) need not share the CPU steady-clock epoch.
void recordAt(std::uint64_t ns, Kind kind, std::uint16_t arg = 0, std::uint32_t extra = 0) noexcept;
inline void mark(Kind kind, std::uint16_t arg = 0, std::uint32_t extra = 0) noexcept {
    if (armed.load(std::memory_order_relaxed)) record(kind, arg, extra);
}
inline void markAt(std::uint64_t ns, Kind kind, std::uint16_t arg = 0, std::uint32_t extra = 0) noexcept {
    if (armed.load(std::memory_order_relaxed)) recordAt(ns, kind, arg, extra);
}
// Checks the switch file at most once a second (call from a frequent, non-critical site: the present hook).
void tick() noexcept;
// The guest token of the calling host thread, set by the executor when a task binds (0 = host thread).
void setThreadToken(std::uint32_t token) noexcept;
}
