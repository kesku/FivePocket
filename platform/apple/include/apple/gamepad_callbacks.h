// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "private_trace_api.h"

namespace recomp::apple {
// Bind only the input portion; display ownership and ABI fields belong to the
// launcher. Input must outlive every guest callback and be destroyed on main.
template<class Input>
inline void bindGamepadCallbacks(RecompPrivateTraceCallbacksV5& callbacks, Input& input) noexcept {
    callbacks.inputUser = &input;
    callbacks.gamepadState = [](void* user, std::uint32_t index,
                               recomp::input::GamepadState* state) noexcept -> int {
        if (!state) return -1;
        *state = {};
        if (!user) return 0;
        return static_cast<Input*>(user)->snapshot(index, state);
    };
    callbacks.gamepadCapabilities = [](void* user, std::uint32_t index,
                                      recomp::input::GamepadState* state) noexcept -> int {
        if (!state) return -1;
        *state = {};
        if (!user) return 0;
        return static_cast<Input*>(user)->capabilities(index, state);
    };
#if defined(APPLE_GAMEPAD_MUTANT_CALLBACKS)
    callbacks.gamepadCapabilities = callbacks.gamepadState;
#endif
}
}
