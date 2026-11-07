// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string_view>
namespace iosguestlimit {
enum class Policy : unsigned { Inherit=0, Thirty=30, Sixty=60 };
inline Policy parse(const char* value) {
    if(!value)return Policy::Inherit;
    const std::string_view text(value);
    if(text=="30")return Policy::Thirty;
    if(text=="60")return Policy::Sixty;
    throw std::invalid_argument("RECOMP_IOS_GUEST_FRAME_LIMIT requires exactly30 or60");
}
inline bool resolve60(Policy policy,bool file60) noexcept {
    return policy==Policy::Inherit?file60:policy==Policy::Sixty;
}
inline std::uint32_t readWord(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
}
struct Result {std::uint32_t before,wanted,after;bool changed,foreign;};
inline Result applyWord(std::uint8_t* p,bool on) noexcept {
    const auto before=readWord(p);const std::uint32_t wanted=on?1u:2u;
    if(before==wanted)return {before,wanted,before,false,false};
    if(before!=1 && before!=2)return {before,wanted,before,false,true};
    // Same byte stores and release publication as the original v280 applier.
    __atomic_store_n(p,std::uint8_t(0),__ATOMIC_RELAXED);__atomic_store_n(p+1,std::uint8_t(0),__ATOMIC_RELAXED);
    __atomic_store_n(p+2,std::uint8_t(0),__ATOMIC_RELAXED);__atomic_store_n(p+3,std::uint8_t(wanted),__ATOMIC_RELEASE);
    return {before,wanted,readWord(p),true,false};
}
}
