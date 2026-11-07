// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <pthread.h>
#include <cerrno>
#include <pthread/qos.h>

namespace recomp::apple {
// Linux thread IDs and Darwin process IDs are not interchangeable. Never
// implement these legacy cross-thread operations using setpriority(pid).
// Errors propagate through the existing optional-affinity protocols.
#if defined(APPLE_THREAD_MUTANT_FALSE_CAPABILITY)
inline constexpr int UnsupportedThreadOperation = 0;
#else
inline constexpr int UnsupportedThreadOperation = ENOTSUP;
#endif
inline int unavailableThreadAffinity(int, unsigned) noexcept { return UnsupportedThreadOperation; }
inline int unavailableThreadPriority(int, int) noexcept { return UnsupportedThreadOperation; }
inline int unavailableThreadPriorityQuery(int, int*) noexcept { return UnsupportedThreadOperation; }
// Compiler warm-up is background host work. Apple chooses its cores; do not
// imitate the Thor's CPU masks or claim that QoS pins a core. Caller receives
// the real OS error, never an invented successful affinity result.
inline int backgroundCompilationThread() noexcept {
#if defined(APPLE_THREAD_MUTANT_SKIP_QOS)
    return 0;
#else
    return pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND,0);
#endif
}
}
