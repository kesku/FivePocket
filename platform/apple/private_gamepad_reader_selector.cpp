// SPDX-License-Identifier: GPL-3.0-or-later
#include "private_gamepad_reader_selector.h"
#include "private_gamepad_observer_lifecycle.h"
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace recomp::input::delivery::embedding {
namespace {
[[noreturn]] void lifetimeFailure(const char* message) noexcept {
    std::fputs(message,stderr);std::fputc('\n',stderr);std::fflush(stderr);std::abort();
}
}
__attribute__((noinline)) NativeReader selectNativeGamepadReader(
    const RecompPrivateTraceCallbacksV5* input,NativeReader legacy) noexcept {
    if(!dedicatedObserverInstalled())return legacy;
    if(!dedicatedObserverRunning() || !input || !input->gamepadState)
        lifetimeFailure("PRIVATE_GAMEPAD_READER_LIFETIME FAIL invalid_agent_binding");
    // Capture the callback/context, not a pointer to the caller's stack table.
    // Context lifetime is still owned by the app through its joined run.
    const auto read=input->gamepadState;const auto context=input->inputUser;
    return [read,context](std::uint32_t user,recomp::input::GamepadState& state) {
        // Covers BOTH XamInputGetState and XamUserGetDeviceContext. Never
        // dereference an app context after finish/clear. Unexpected late use
        // is a loud invariant failure, not a silent fabricated disconnection.
        if(!dedicatedObserverRunning())
            lifetimeFailure("PRIVATE_GAMEPAD_READER_LIFETIME FAIL bound_reader_after_run");
        return read(context,user,&state);
    };
}
}
