// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/ios_request_plan.h"
#include "private_trace_arguments.h"
#include <CommonCrypto/CommonDigest.h>

namespace recomp::apple {
bool planIosRequest(std::span<const std::uint8_t> raw,
                    const std::array<std::uint8_t,32>& expectedSha256,
                    std::string_view templateRoot,std::string_view templateId,
                    std::string_view newRoot,std::string_view newId,
                    IosRequestPlan& output) noexcept {
    using namespace private_trace;
    if (!raw.data() || raw.empty() || raw.size()>MaximumArgumentBytes || templateRoot.size()>2986 ||
        !canonicalPath(templateRoot) || !canonicalPath(newRoot) ||
        !requestId(templateId) || !requestId(newId) || templateId==newId) return false;
#if !defined(APPLE_REQUEST_MUTANT_ROOT_CAP)
    if (newRoot.size()>94) return false;
#endif
    std::array<std::uint8_t,32> actual{};
    if (!CC_SHA256(raw.data(),static_cast<CC_LONG>(raw.size()),actual.data())) return false;
#if !defined(APPLE_REQUEST_MUTANT_TEMPLATE_HASH)
    if (actual!=expectedSha256) return false;
#else
    (void)expectedSha256;
#endif
    try {
        const std::string oldTrace=std::string(templateRoot)+"/private-trace";
        const std::string newTrace=std::string(newRoot)+"/private-trace";
        const std::string oldPrefix=oldTrace+"/";
        const std::string oldRun="run-"+std::string(templateId)+"/";
        const std::string newRun="run-"+std::string(newId)+"/";
        std::vector<std::string> original;
        if (!parseArguments(raw.data(),raw.size(),oldTrace,templateId,original)) return false;
        IosRequestPlan candidate;
        candidate.arguments.reserve(original.size());
        for (auto value:original) {
            if (value.front()=='/') {
                if (!value.starts_with(oldPrefix)) return false;
                std::string suffix=value.substr(oldPrefix.size());
                if (suffix.starts_with(oldRun)) suffix=newRun+suffix.substr(oldRun.size());
                value=newTrace+"/"+suffix;
            }
            candidate.arguments.push_back(std::move(value));
        }
#if defined(APPLE_REQUEST_MUTANT_NONPATH)
        candidate.arguments[2]+="x";
#endif
        for (const auto& value:candidate.arguments) {
            if (candidate.serialized.size()+value.size()+1>MaximumArgumentBytes) return false;
            candidate.serialized.insert(candidate.serialized.end(),value.begin(),value.end());
            candidate.serialized.push_back(0);
        }
        std::vector<std::string> checked;
        if (!parseArguments(candidate.serialized.data(),candidate.serialized.size(),newTrace,newId,checked) ||
            checked!=candidate.arguments) return false;
        candidate.imageSourceRelative="private-trace/"+oldRun+"decoded.image.bin";
        candidate.imageDestinationRelative="private-trace/"+newRun+"decoded.image.bin";
        candidate.requestDestinationRelative="private-trace/"+newRun+"start-request.bin";
        output=std::move(candidate);return true;
    } catch (...) {return false;}
}
}
