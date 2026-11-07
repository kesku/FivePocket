// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "apple/ios_run_files.h"

namespace recomp::apple {
enum class IosStageFailure { NotRun, None, InvalidCatalog, AssetsBefore, RunFiles, AssetsAfter };
struct IosStagePreparation {
    IosStageFailure failure{IosStageFailure::NotRun};
    AssetVerification assets;
    IosRunFilesResult files;
    bool prepared() const noexcept {return failure==IosStageFailure::None;}
};
// Synchronous off-main connection: authenticate the entire signed catalog,
// prepare exclusive request files, then reauthenticate all inputs plus the new
// image/request. On failure no launch arguments are returned, but partial paths
// and error evidence remain. Caller owns a serialized staging/launch operation;
// hashes are measurements, not immutable lifetime leases. This API neither
// authenticates caller-provided pins nor proves a linked decoder or gameplay.
IosStagePreparation prepareIosStage(std::string_view root,
    std::span<const ExpectedAsset> authenticatedAssets,
    std::span<const std::uint8_t> requestTemplate,
    const std::array<std::uint8_t,32>& requestSha256,
    std::string_view templateRoot,std::string_view templateId,std::string_view newId);
}
