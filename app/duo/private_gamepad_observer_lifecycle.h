// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private runner only, not an app ABI. Begin before ANY worker can start;
// finish only after the complete run call has returned and joined its workers.
// A successful begin MUST be paired once, including exception paths. Never
// invoke finish from an observation timeout or a mere request to stop.
namespace recomp::input::delivery::embedding {
bool beginRun() noexcept;
void finishAfterJoin() noexcept;
// Bind-time selection and agent-only late-reader invariant checks. Read-only,
// lock-free; do not treat these as a device/process liveness measurement.
bool dedicatedObserverInstalled() noexcept;
bool dedicatedObserverRunning() noexcept;
}
