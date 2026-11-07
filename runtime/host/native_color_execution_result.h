#pragma once
#include "native_color_edram_snapshot.h"
#include "native_color_pending_state.h"
#include "native_color_sync_progress.h"
#include "native_tiled_copy_publication.h"

namespace recomp::gpu {
// Owned CPU-visible output of the bounded executor. Public metadata is NOT an
// independent GPU fence certificate, owner lease or authorization to publish.
struct NativeColorChainExecutionResult {
    std::optional<NativeTiledCopyPublication> copy;
    std::optional<NativeColorEdramSnapshot> edram;
    // Prepared raw state, including post-output. Five completion-dependent
    // register values remain absent. Not live until a successful transaction.
    std::optional<NativeRegisterJournal> pendingRegisters;
    std::optional<NativeGraphicsState> pendingState;
    // Whole-capture only: modeled boundaries follow actual audited GPU stages.
    // Does not establish retail cache semantics or fill unknown register bits.
    std::optional<NativeColorSyncProgress> auditedTimeline;
    std::array<std::uint8_t,40> eventBytes{};
    unsigned nativeFences=0;
    // Separate from the four raster/copy/event stage fences. PostOutput adds
    // a fresh leading all-prior-work idle/setup fence, never an old receipt.
    unsigned leadingSetupFences=0;
};
static_assert(std::is_nothrow_move_assignable_v<NativeColorChainExecutionResult>);
}
