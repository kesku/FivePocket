#pragma once
#include "native_color_edram_snapshot.h"
#include "native_tiled_copy_publication.h"
#include "native_deferred_color_sync_progress.h"
#include "native_deferred_color_pending_state.h"
#include <type_traits>

namespace recomp::gpu {
// Private off-screen output ONLY. Deliberately no standalone color final-event
// bytes, old synchronization timeline or live receipt. Pending state is only
// a preallocated candidate; this diagnostic never installs it in the engine.
// This result cannot complete the native surface/IRQ tail or retire the IB.
struct NativeDeferredColorDiagnosticResult {
    std::optional<NativeTiledCopyPublication> copy;
    std::optional<NativeColorEdramSnapshot> edram;
    std::optional<NativeDeferredColorSyncProgress> auditedPrefixTimeline;
    std::optional<NativeRegisterJournal> pendingRegisters;
    std::optional<NativeGraphicsState> pendingState;
    unsigned nativeFences=0,leadingSetupFences=0;
};
static_assert(std::is_nothrow_move_assignable_v<NativeDeferredColorDiagnosticResult>);
}
