// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
#include <cstring>
#include <stdexcept>
namespace iossamplerenum {
inline bool parse(const char* value) {
    if (!value || std::strcmp(value,"0")==0) return false;
    if (std::strcmp(value,"1")==0) return true;
    throw std::invalid_argument("RECOMP_IOS_SAMPLER_SET_BITS requires 0 or 1");
}
inline bool fromEnvironment() { return parse(std::getenv("RECOMP_IOS_SAMPLER_SET_BITS")); }
}
