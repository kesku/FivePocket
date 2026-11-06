#pragma once
#include "native_deferred_color_pending_state.h"
#include "native_deferred_color_sync_progress.h"
#include "native_color_edram_snapshot.h"
#include "native_tiled_copy_publication.h"

namespace recomp::gpu {
// Separate future live-prefix transport. No conversion from stopped diagnostic
// results, old standalone event receipts or ticket2 completion. Public fields
// are sanity-checkable bookkeeping, NOT independent GPU/ownership certificates.
// Only a fresh live executor under the retained whole-submission gate may
// produce this for publication. The surface/IRQ tail still has to complete.
struct NativeDeferredColorExecutionResult {
    std::optional<NativeDeferredSwapSubmission> submission;
    std::optional<NativeTiledCopyPublication> copy;
    std::optional<NativeColorEdramSnapshot> edram;
    std::optional<NativeRegisterJournal> pendingRegisters;
    std::optional<NativeGraphicsState> pendingState;
    std::optional<NativeDeferredColorSyncProgress> auditedPrefixTimeline;
    unsigned nativeFences=0,leadingSetupFences=0;
    bool committed=false;
};
static_assert(std::is_nothrow_move_assignable_v<NativeDeferredColorExecutionResult>);
}
