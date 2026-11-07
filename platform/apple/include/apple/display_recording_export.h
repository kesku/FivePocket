// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "apple/display_observation.h"
#include <cstdio>

namespace recomp::apple::performance {
// Write an ADVISORY snapshot, after recording callbacks/away from rendering.
// Does not close a stream, snapshot a recorder, sleep, or certify physical
// delivery. Callers own file setup, detach/drain and whole-run completeness.
// Other measurement timestamps use another clock: no automatic clock join.
bool exportDisplaySnapshot(FILE* stream,const char* runId,const DisplaySnapshot&) noexcept;
}
