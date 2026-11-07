// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LauncherDisplayOptions.hpp"

namespace recomp::apple::launcher {
// Main-thread host operations. The integrating host must supply an actually
// saved/loaded snapshot, never a draft, before the authenticated guest handoff.
// No I/O, assumed defaults, menu wiring or consumer capability is provided here.
bool publishSavedLauncherDisplayOptions(const StoredDisplay&) noexcept;
bool freezeLauncherDisplayOptions() noexcept;
// Diagnostics/file-based A/B must freeze exactly0 even if a pre-guest caller
// queued a saved value. Refuses non-main ownership or an already-frozen channel.
bool freezeLegacyLauncherDisplayOptions() noexcept;
}
// Explicit mandatory link dependency of the proposed Apple renderer bridge.
// Initially/no saved value:0, preserving existing renderer behaviour. Calling
// threads read one atomic word, with no UIKit, file access or thread hop.
extern "C" std::uint32_t recomp_apple_launcher_display_options() noexcept;
