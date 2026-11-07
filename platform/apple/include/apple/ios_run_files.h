// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "apple/asset_verifier.h"
#include "apple/ios_request_plan.h"

namespace recomp::apple {
enum class IosRunFilesFailure { NotRun, None, InvalidPlan, SourceRejected,
    DirectoryRejected, DirectoryExists, CopyFailed, RequestFailed, Changed, VerificationFailed };
struct IosRunFilesResult {
    IosRunFilesFailure failure{IosRunFilesFailure::NotRun};
    int systemError{};
    // If true, preserve this directory on failure; never auto-delete or reuse.
    bool directoryCreated{};
    std::string directoryRelative;
    IosRequestPlan plan; // populated only on full success
    bool prepared() const noexcept {return failure==IosRunFilesFailure::None;}
};

// Off-main-thread, first preparation only. Pins/template must be authenticated
// by the signed build. Creates exclusively one new run directory, copies the
// pinned decoded image, writes the exact rebased request, then re-verifies both.
// Refuses collisions, links, dataless components and mutation. Retains partial
// output on error, without deleting/overwriting anything or touching saves.
// This verifies only image/request preparation, NOT all assets, the decoder,
// runtime execution or a lifetime lease. Final launch admission must revalidate
// the full asset set and keep this app's write/launch ownership serialized.
IosRunFilesResult prepareIosRunFiles(std::string_view root,
    std::span<const std::uint8_t> requestTemplate,
    const std::array<std::uint8_t,32>& requestSha256,
    std::string_view templateRoot,std::string_view templateId,
    std::string_view newId,const ExpectedAsset& image);
}
