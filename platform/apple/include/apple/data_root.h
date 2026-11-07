// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "private_trace_arguments.h"
#include <string>
#include <string_view>

namespace recomp::apple {
// v507 recomp_app_dir.h caps the files directory at94 bytes. A canonical
// /private/var/mobile/.../<UUID>/Library/r fits exactly; Application Support
// includes a forbidden space and is too long. Never truncate or use a symlink
// alias to get past that bound. Pass a physically resolved Library directory.
inline bool dataRootPath(std::string_view canonicalLibrary, std::string& result) {
    result.clear();
    if (!private_trace::canonicalPath(canonicalLibrary)) return false;
    std::string candidate(canonicalLibrary);
    candidate += "/r";
#if defined(APPLE_DATA_ROOT_MUTANT_TRUNCATE)
    candidate.resize(std::min(candidate.size(),std::size_t{94}));
#endif
    if (candidate.size()>94 || !private_trace::canonicalPath(candidate)) return false;
    result=std::move(candidate);
    return true;
}
// iOS main-thread setup. Creates only Library/r, excludes it from iCloud backup,
// refuses links/non-directories/unsupported canonical length. Does not alter
// assets, saves, entitlement limits or an existing directory's contents.
bool prepareIosDataRoot(std::string& result, std::string& error);
}
