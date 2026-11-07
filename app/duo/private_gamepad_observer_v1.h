// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

#if defined(__cplusplus)
#define RECOMP_PAD_OBSERVER_NOEXCEPT noexcept
extern "C" {
#else
#define RECOMP_PAD_OBSERVER_NOEXCEPT
#endif
// Borrowed BE16 bytes: exactly the state encoded/copied by XamInputGetState.
// Valid only during this call. Never retain the pointer, block, throw, query a
// registry, write guest memory, or call UI. This is not gameplay-response proof.
typedef void (*RecompPrivateGamepadObserverV1)(void* context,uint32_t user,
    uint32_t guest_output,const uint8_t* be16,uint32_t bytes) RECOMP_PAD_OBSERVER_NOEXCEPT;
// 0 success, nonzero refusal without changing the installed observer/context.
// Before the run only; null clears after workers join (or before an unused run).
// Owner keeps context alive until clearing succeeds. One observer per library.
__attribute__((visibility("default")))
int recomp_private_trace_gamepad_observer_v1(RecompPrivateGamepadObserverV1 callback,
    void* context) RECOMP_PAD_OBSERVER_NOEXCEPT;
#if defined(__cplusplus)
}
#endif
#undef RECOMP_PAD_OBSERVER_NOEXCEPT
