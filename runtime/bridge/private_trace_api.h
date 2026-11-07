#pragma once
#include <cstdint>
#include "native_gamepad_state.h"
// Private embedding ABI, version1. This is NOT JNI and creates no window.
// The host must supply stable argv strings, dedicated worker/process lifetime,
// bounded supervision and private inputs. Output still uses the trace protocol.
// At most ONE setup/execution attempt per loaded library lifetime; calls cannot
// resume a stopped game or run a second title. 2=invalid/setup failure,
// 3=already attempted. Normal guarded trace outcome remains in its protocol.
extern "C" __attribute__((visibility("default")))
int recomp_run_private_trace_v1(int argc,char** argv) noexcept;

// Version2 adds a synchronous STOPPED-image consumer, not live VdSwap support.
// Original guest writers have joined before this callback. Pixels are borrowed
// opaque1280x720 RGBA8 and valid only for the callback's duration: copy or finish
// all native reads before returning. The host must retain user data throughout
// the run. 1=consumer accepted,0=rejected; neither is a guest/vblank completion.
// Callbacks must not throw, reenter the runner or resume guest execution.
struct RecompPrivateTraceCallbacksV2 {
    std::uint32_t structureBytes;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    void* user;
    int (*stoppedRgba)(void*,const std::uint8_t*,std::uint32_t,std::uint32_t,std::uint32_t) noexcept;
};
extern "C" __attribute__((visibility("default")))
int recomp_run_private_trace_v2(int argc,char** argv,const RecompPrivateTraceCallbacksV2* callbacks) noexcept;

// Version3 is a SEPARATE live, ordered native-command consumer. The callback
// runs synchronously on the serialized guest executor, with ALL OTHER guest
// writers parked and the registry allocation lock held. It must not reenter,
// throw, resume guests or retain the borrowed RGBA after return. The host keeps
// user/window lifetime valid throughout the call. 1 means actual GPU-audited
// upload AND QueuePresent accepted, not display/vblank/identifier completion.
// 0 means no accepted presentation. Only one frame is currently supported.
// Requires explicit --experimental-native-swap-commands. Shares the v1/v2
// once-gate; malformed callback configuration does not consume that gate.
struct RecompPrivateTraceCallbacksV3 {
    std::uint32_t structureBytes;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    void* user;
    int (*orderedRgba)(void*,const std::uint8_t*,std::uint32_t,std::uint32_t,std::uint32_t) noexcept;
};
extern "C" __attribute__((visibility("default")))
int recomp_run_private_trace_v3(int argc,char** argv,const RecompPrivateTraceCallbacksV3* callbacks) noexcept;

// Version4 extends the SAME writer-parking/borrowed-pixel contract to at most
// Ordered display updates, numbered 0..UINT32_MAX-1 (UINT32_MAX is reserved
// to prevent wraparound). Updates may recolor an existing
// frame; the sequence is not a generated-game-frame counter. No gaps or replays.
// Invalid shape/sequence returns 0 without submitting an attempt. A valid
// attempted update returning 0 permanently closes the update stream before
// QueuePresent acceptance. A failure after acceptance must terminate, never
// report 0. No physical vblank is implied. Shares the v1/v2/v3 once-gate.
struct RecompPrivateTraceCallbacksV4 {
    std::uint32_t structureBytes;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    void* user;
    int (*orderedRgba)(void*,std::uint32_t,const std::uint8_t*,std::uint32_t,std::uint32_t,std::uint32_t) noexcept;
};
extern "C" __attribute__((visibility("default")))
int recomp_run_private_trace_v4(int argc,char** argv,const RecompPrivateTraceCallbacksV4* callbacks) noexcept;

// V5 retains the V4 ordered display contract and adds explicit native input.
// The snapshot callback may run on any guest thread; it must be thread-safe,
// non-reentrant and retain no guest pointers. 1=connected, 0=disconnected,
// other=failed query. Only enabled with --native-input. V1-V4 are unchanged.
struct RecompPrivateTraceCallbacksV5 {
    std::uint32_t structureBytes,version,reserved0,reserved1;
    RecompPrivateTraceCallbacksV4 display;
    void* inputUser;
    int (*gamepadState)(void*,std::uint32_t,recomp::input::GamepadState*) noexcept;
    int (*gamepadCapabilities)(void*,std::uint32_t,recomp::input::GamepadState*) noexcept;
};
extern "C" __attribute__((visibility("default")))
int recomp_run_private_trace_v5(int argc,char** argv,const RecompPrivateTraceCallbacksV5* callbacks) noexcept;

// V6 (attempt v255, direct present) retains V5 and adds the app's window. When the library's direct present is on (its own
// switch, files/private-trace/diag-direct-present) it presents every frame to that window itself and calls
// input.display.orderedRgba only after it gave the window back (then the app's renderer attaches as for V5); otherwise it
// behaves exactly as V5. acquireWindow returns a new reference to the current window (or null) and that window's
// generation; releaseWindow drops a reference acquireWindow returned; windowGeneration changes whenever the app's window
// is replaced or detached. All three are thread-safe and may be called on any thread. V1-V5 are unchanged.
struct ANativeWindow;
struct RecompPrivateTraceCallbacksV6 {
    std::uint32_t structureBytes,version,reserved0,reserved1;
    RecompPrivateTraceCallbacksV5 input;
    void* windowUser;
    ANativeWindow* (*acquireWindow)(void*,std::uint64_t*) noexcept;
    void (*releaseWindow)(void*,ANativeWindow*) noexcept;
    std::uint64_t (*windowGeneration)(void*) noexcept;
};
extern "C" __attribute__((visibility("default")))
int recomp_run_private_trace_v6(int argc,char** argv,const RecompPrivateTraceCallbacksV6* callbacks) noexcept;
