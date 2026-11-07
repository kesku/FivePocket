// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_gamepad_state.h"
#include <array>
#include <cstdint>

// Proposal, not linked into the installed app. One active private-runner
// episode per library. Install before workers; remove after workers join.
// Observer must not block, throw, reenter a registry, or write guest memory.
namespace recomp::input::delivery {
using Observer=void(*)(std::uint32_t,std::uint32_t,const GamepadState&) noexcept;
bool install(Observer observer) noexcept;
bool remove(Observer expected) noexcept;
// Single-call replacement for the existing guarded memcpy. The
// caller's exact BE16 packet is copied unchanged; no dispatch branch is added.
void copyAndObserve(void* destination,const std::array<std::uint8_t,16>& packet,
    std::uint32_t user,std::uint32_t output,const GamepadState& state) noexcept;
}
