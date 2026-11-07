// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdexcept>
#include <string_view>

namespace iospassdiag {
enum class Policy { FileDriven, Disabled, AllowFiles };
inline Policy parse(const char* value) {
    if (!value) return Policy::FileDriven;
    const std::string_view text(value);
    if (text == "0") return Policy::Disabled;
    if (text == "1") return Policy::AllowFiles;
    throw std::invalid_argument("RECOMP_IOS_PASS_PROFILING must be 0 or 1 when set");
}
struct State { bool profile; bool dump; };
// The caller supplies existing file-driven state. 1 never enables an absent flag.
inline State apply(Policy policy, bool profile, bool dump) noexcept {
    if (policy == Policy::Disabled) return {false, false};
    return {profile, dump};
}
inline const char* name(Policy policy) noexcept {
    return policy == Policy::Disabled ? "0" : policy == Policy::AllowFiles ? "1" : "unset";
}
}
