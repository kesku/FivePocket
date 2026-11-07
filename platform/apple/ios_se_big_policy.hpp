// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
#include <cstring>
#include <stdexcept>
namespace iossebig {
enum class Policy { File, Off, On };
inline Policy parse(const char* value) {
    if (!value || std::strcmp(value, "file") == 0) return Policy::File;
    if (std::strcmp(value, "0") == 0) return Policy::Off;
    if (std::strcmp(value, "1") == 0) return Policy::On;
    throw std::invalid_argument("RECOMP_IOS_SE_BIG_POLICY requires file, 0 or 1");
}
inline Policy fromEnvironment() { return parse(std::getenv("RECOMP_IOS_SE_BIG_POLICY")); }
constexpr unsigned apply(Policy policy, unsigned fileBits, unsigned bigBit) noexcept {
    return policy == Policy::File ? fileBits :
           policy == Policy::Off ? (fileBits & ~bigBit) : (fileBits | bigBit);
}
}
