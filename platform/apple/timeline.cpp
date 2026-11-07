// SPDX-License-Identifier: GPL-3.0-or-later
// Apple-only owner of the existing timeline implementation. The executor TU
// force-includes the original diag_timeline.h declarations, and remains wholly
// unmodified (avoids shifting its line-keyed diagnostic statics and PGO names).
#if !defined(__APPLE__)
#error "Apple timeline owner must not be added to the Android build"
#endif
#include "recomp_app_dir.h"
#if defined(APPLE_TIMELINE_TEST_DIR)
#define RECOMP_DIAG_SWITCH_DIR 1
#define RECOMP_DIAG_SWITCH(name) APPLE_TIMELINE_TEST_DIR "/" name
#endif
#ifndef RECOMP_APPLE_TIMELINE_IMPLEMENTATION
#error "Bind the exact private timeline implementation via the build receipt"
#endif
#include RECOMP_APPLE_TIMELINE_IMPLEMENTATION
