// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Original Apple adapter for the two logging functions used by the runtime.
// These are diagnostics only; never pretend missing game operations succeeded.
#include <stdarg.h>

enum android_LogPriority {
    ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT = 1, ANDROID_LOG_VERBOSE = 2,
    ANDROID_LOG_DEBUG = 3, ANDROID_LOG_INFO = 4, ANDROID_LOG_WARN = 5,
    ANDROID_LOG_ERROR = 6, ANDROID_LOG_FATAL = 7, ANDROID_LOG_SILENT = 8
};
#ifdef __cplusplus
extern "C" {
#endif
int __android_log_vprint(int priority, const char* tag, const char* format, va_list args)
    __attribute__((format(printf, 3, 0)));
int __android_log_print(int priority, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
#ifdef __cplusplus
}
#endif
