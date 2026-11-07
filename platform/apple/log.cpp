// SPDX-License-Identifier: GPL-3.0-or-later
#include "android/log.h"
#include <cstdio>

extern "C" int __android_log_vprint(int priority, const char* tag,
                                    const char* format, va_list args) {
    if (!format) return -1;
    // stdio lock keeps a runtime message together across guest/renderer threads.
    flockfile(stderr);
    std::fprintf(stderr, "[%d/%s] ", priority, tag ? tag : "recomp");
#if defined(APPLE_LOG_MUTANT_DROP_ARGUMENTS)
    (void)args;
    const int count = std::fputs(format, stderr);
#else
    const int count = std::vfprintf(stderr, format, args);
#endif
    std::fputc('\n', stderr);
    funlockfile(stderr);
    return count;
}

extern "C" int __android_log_print(int priority, const char* tag,
                                   const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int count = __android_log_vprint(priority, tag, format, args);
    va_end(args);
    return count;
}
