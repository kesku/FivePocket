// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// hud2 (2026-10-22, attempt v575): an optional second window for the HUD copy on a secondary display (AYN Thor's lower
// screen). Separate from private_trace_api.h so that the V1-V6 entries and every library built before it are unchanged.
//
// A library that exports recomp_set_hud_window_v1 accepts the app's second window. The app looks the export up after it
// opened the library and, only when found, calls it ONCE before it calls the run entry (V6). The library then shows the
// minimap and the wanted-star row of the picture it presents on that window while a window exists (generation != 0), and
// nothing otherwise. Same contract as V6's window: acquireWindow returns a new reference to the current window (or null) and
// its generation; releaseWindow drops a reference acquireWindow returned; windowGeneration changes whenever the window is
// attached, resized, replaced or detached (a generation whose acquireWindow gives null: no window). All three are thread-safe, may be called on any thread, and must stay
// valid until the process ends. Returns 0 when accepted, 2 when the structure is invalid.
#include <cstdint>
struct ANativeWindow;
struct RecompHudWindowV1 {
    std::uint32_t structureBytes,version,reserved0,reserved1;
    void* windowUser;
    ANativeWindow* (*acquireWindow)(void*,std::uint64_t*) noexcept;
    void (*releaseWindow)(void*,ANativeWindow*) noexcept;
    std::uint64_t (*windowGeneration)(void*) noexcept;
};
extern "C" __attribute__((visibility("default")))
int recomp_set_hud_window_v1(const RecompHudWindowV1* window) noexcept;
