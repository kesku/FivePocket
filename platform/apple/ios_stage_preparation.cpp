// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/ios_stage_preparation.h"
#include <CommonCrypto/CommonDigest.h>

namespace recomp::apple {
IosStagePreparation prepareIosStage(std::string_view root,std::span<const ExpectedAsset> authenticatedAssets,
    std::span<const std::uint8_t> requestTemplate,const std::array<std::uint8_t,32>& requestSha256,
    std::string_view templateRoot,std::string_view templateId,std::string_view newId) {
    IosStagePreparation result;
    IosRequestPlan planned;
    if(authenticatedAssets.empty() || authenticatedAssets.size()>MaximumAssetRecords-2 ||
       !planIosRequest(requestTemplate,requestSha256,templateRoot,templateId,root,newId,planned)) {
        result.failure=IosStageFailure::InvalidCatalog;return result;
    }
    const ExpectedAsset* image=nullptr;
    for(const auto& asset:authenticatedAssets) {
        if(asset.relativePath==planned.imageSourceRelative) {
            if(image) {result.failure=IosStageFailure::InvalidCatalog;return result;}
            image=&asset;
        }
        if(asset.relativePath==planned.imageDestinationRelative || asset.relativePath==planned.requestDestinationRelative) {
            result.failure=IosStageFailure::InvalidCatalog;return result;
        }
    }
    if(!image) {result.failure=IosStageFailure::InvalidCatalog;return result;}
    result.assets=verifyAuthenticatedAssets(root,authenticatedAssets);
    if(!result.assets.allMatched()) {result.failure=IosStageFailure::AssetsBefore;return result;}
    result.files=prepareIosRunFiles(root,requestTemplate,requestSha256,templateRoot,templateId,newId,*image);
    if(!result.files.prepared()) {result.failure=IosStageFailure::RunFiles;return result;}
    std::vector<ExpectedAsset> records;
    records.reserve(authenticatedAssets.size()+2);
    records.insert(records.end(),authenticatedAssets.begin(),authenticatedAssets.end());
    records.push_back({result.files.plan.imageDestinationRelative,image->byteCount,image->sha256});
    std::array<std::uint8_t,32> digest{};
    if(!CC_SHA256(result.files.plan.serialized.data(),static_cast<CC_LONG>(result.files.plan.serialized.size()),digest.data())) {
        result.files.plan={};result.failure=IosStageFailure::AssetsAfter;return result;
    }
    records.push_back({result.files.plan.requestDestinationRelative,result.files.plan.serialized.size(),digest});
#if !defined(APPLE_STAGE_MUTANT_SKIP_RECHECK)
    result.assets=verifyAuthenticatedAssets(root,records);
#endif
    if(!result.assets.allMatched()) {
        result.files.plan={};result.failure=IosStageFailure::AssetsAfter;return result;
    }
    result.failure=IosStageFailure::None;return result;
}
}
