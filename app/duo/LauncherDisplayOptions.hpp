// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace recomp::apple::launcher {
// Requested choices, not proof of effective game settings. Index order matches
// the launcher rows: cap, scaler, sharpness tenths, resolution, FPS counter,
// frame generation (Claude, 2026-10-18: Off / On; bit 15 of the word), draw distance (Claude, 2026-10-20: Original / 90 /
// 80 / 70 %; bits 16-17 of the word), traffic density (Claude, 2026-10-20: Original / 75 / 50 / 25 %; bits 18-19 of the word),
// shadow distance (Claude, 2026-10-20: Original / Near; bit 20 of the word), screen (Claude, 2026-10-21: Fill / Fit; bit 21).
struct DisplayChoices {
    std::array<std::uint8_t,10> values;
    bool operator==(const DisplayChoices&) const=default;
};
inline constexpr std::array<unsigned,10> ChoiceCounts{6,4,41,7,2,2,4,4,2,2};
inline constexpr std::array<unsigned,7> ResolutionPercent{50,75,88,100,115,125,150};
// (Claude, 2026-10-20) the game's level-of-detail scale in percent (world_load.h's LOD root); index 0 = the original
inline constexpr std::array<unsigned,4> DrawDistancePercent{100,90,80,70};
// (Claude, 2026-10-20) random and parked vehicle density in percent (world_load.h's density words); index 0 = the original
inline constexpr std::array<unsigned,4> TrafficPercent{100,75,50,25}; // (25 %: c284, the world-load density floor)
inline constexpr std::uint32_t OptionsValid=0x80000000u;
inline constexpr std::uint32_t OptionsFields=0x003fffffu;
inline bool valid(const DisplayChoices& choices) noexcept {
    for(unsigned i=0;i<ChoiceCounts.size();++i)if(choices.values[i]>=ChoiceCounts[i])return false;
    return true;
}
inline std::optional<std::uint32_t> encode(const DisplayChoices& choices) noexcept {
    if(!valid(choices))return std::nullopt;
    const auto& v=choices.values;
    return OptionsValid|unsigned(v[0])|(unsigned(v[1])<<3)|(unsigned(v[2])<<5)|(unsigned(v[3])<<11)|(unsigned(v[4])<<14)|(unsigned(v[5])<<15)|(unsigned(v[6])<<16)|(unsigned(v[7])<<18)|(unsigned(v[8])<<20)|(unsigned(v[9])<<21);
}
inline std::optional<DisplayChoices> decode(std::uint32_t word) noexcept {
    if(!(word&OptionsValid) || (word&~(OptionsValid|OptionsFields)))return std::nullopt;
    DisplayChoices choices{{std::uint8_t(word&7),std::uint8_t((word>>3)&3),std::uint8_t((word>>5)&63),
                           std::uint8_t((word>>11)&7),std::uint8_t((word>>14)&1),std::uint8_t((word>>15)&1),
                           std::uint8_t((word>>16)&3),std::uint8_t((word>>18)&3),std::uint8_t((word>>20)&1),std::uint8_t((word>>21)&1)}};
    return valid(choices)?std::optional(choices):std::nullopt;
}

struct StoredDisplay {
    std::uint64_t generation;
    DisplayChoices choices;
    bool operator==(const StoredDisplay&) const=default;
};
inline constexpr std::string_view StoredPrefix="APPLE_LAUNCHER_1 ";
inline constexpr unsigned MaxStoredBytes=96;
inline std::optional<std::string> serialise(const StoredDisplay& value) {
    if(!value.generation || !valid(value.choices))return std::nullopt;
    std::string text(StoredPrefix);text+=std::to_string(value.generation);
    // (a trailing choice is written only when it or a later one is not the original: with frame generation, draw distance,
    // traffic, shadow distance and screen at the original the file is the five-choice file older builds read)
    const auto& v=value.choices.values;
    const unsigned count=v[9]?10u:v[8]?9u:v[7]?8u:v[6]?7u:v[5]?6u:5u;
    for(unsigned i=0;i<count;++i) {
        text+=' ';text+=std::to_string(value.choices.values[i]);
    }
    text+='\n';return text;
}
inline std::optional<StoredDisplay> parse(std::string_view text) noexcept {
#if !defined(APPLE_LAUNCHER_OPTIONS_MUTANT_LONG)
    if(text.size()>MaxStoredBytes)return std::nullopt;
#endif
    if(!text.starts_with(StoredPrefix) || !text.ends_with('\n'))return std::nullopt;
    text.remove_prefix(StoredPrefix.size());text.remove_suffix(1);
    // generation and five to ten choices (a missing frame generation: Off; a missing draw distance, traffic, shadow
    // distance or screen: the original)
    std::array<std::uint64_t,11> numbers{};
    for(unsigned i=0;i<numbers.size();++i) {
        const auto stop=text.find(' ');
        const auto token=text.substr(0,stop);
        if(token.empty() || token.size()>20 || (token.size()>1 && token.front()=='0'))return std::nullopt;
        const auto parsed=std::from_chars(token.data(),token.data()+token.size(),numbers[i]);
        if(parsed.ec!=std::errc{} || parsed.ptr!=token.data()+token.size())return std::nullopt;
        if(i+1==numbers.size() || (i>=5 && stop==std::string_view::npos)) {if(stop!=std::string_view::npos)return std::nullopt;break;}
        if(stop==std::string_view::npos)return std::nullopt;
        text.remove_prefix(stop+1);
    }
    if(!numbers[0])return std::nullopt;
    DisplayChoices choices{};
    for(unsigned i=0;i<ChoiceCounts.size();++i) {
#if !defined(APPLE_LAUNCHER_OPTIONS_MUTANT_TRUNCATE)
        if(numbers[i+1]>=ChoiceCounts[i])return std::nullopt;
#endif
        choices.values[i]=std::uint8_t(numbers[i+1]);
    }
    if(!valid(choices))return std::nullopt;
    return StoredDisplay{numbers[0],choices};
}

// One atomic publication makes every field belong to the same saved snapshot.
// Drafts must never be published. freeze() is a one-way guest-ownership handoff;
// after it, no menu writer can change the game configuration in this session.
// An absent saved setting freezes to zero: existing renderer behaviour remains.
class DisplayOptionsChannel {
public:
    bool publishSaved(std::optional<DisplayChoices> choices) noexcept {
        const auto encoded=choices?encode(*choices):std::optional<std::uint32_t>(0);
        if(!encoded)return false;
        auto before=word_.load(std::memory_order_acquire);
        for(unsigned attempts=0;attempts<8;++attempts) {
#if !defined(APPLE_LAUNCHER_OPTIONS_MUTANT_AFTER_FREEZE)
            if(before&Frozen)return false;
#endif
            if(word_.compare_exchange_weak(before,*encoded,std::memory_order_acq_rel,std::memory_order_acquire))return true;
        }
        return false; // bounded failure, never spin on another writer
    }
    void freeze() noexcept {word_.fetch_or(Frozen,std::memory_order_acq_rel);}
    std::uint32_t read() const noexcept {
        const auto word=word_.load(std::memory_order_acquire);
#if !defined(APPLE_LAUNCHER_OPTIONS_MUTANT_DRAFT)
        if(!(word&Frozen))return 0;
#endif
        return std::uint32_t(word);
    }
private:
    static constexpr std::uint64_t Frozen=1ull<<63;
    std::atomic<std::uint64_t> word_{0};
};

// Helpers for the private renderer proposal. Absent/invalid words retain the
// exact legacy state. These do not implement Apple scaler/sharpness or physical
// display pacing. In particular, "Off" clears optional pacing, not the game's
// standing60 limiter; its menu choice must not claim an unlimited frame rate.
inline bool fpsCounter(std::uint32_t word,bool legacy) noexcept {
    const auto choices=decode(word);
#if defined(APPLE_LAUNCHER_OPTIONS_MUTANT_FPS)
    (void)choices;return legacy;
#else
    return choices?choices->values[4]!=0:legacy;
#endif
}
inline unsigned paceSwitches(std::uint32_t word,unsigned legacy,unsigned capMask,
                             unsigned cap40,unsigned cap30,unsigned cap24,unsigned automatic) noexcept {
    const auto choices=decode(word);if(!choices)return legacy;
    constexpr unsigned Off=0,Sixty=1,Forty=2,Thirty=3,TwentyFour=4,Auto=5;
    unsigned selected=0;
    switch(choices->values[0]) {
    case Off:case Sixty:break;
    case Forty:selected=cap40;break;
    case Thirty:selected=cap30;break;
    case TwentyFour:selected=cap24;break;
    case Auto:selected=automatic;break;
    }
#if defined(APPLE_LAUNCHER_OPTIONS_MUTANT_MODIFIERS)
    (void)capMask;
    return selected;
#else
    return (legacy&~capMask)|selected;
#endif
}
// Frame generation (Claude, 2026-10-18): On = the frame-generation switches (diag-fg-pair-between, diag-fg-between), the
// 30 fps cap with desired present times and stage A of a scaled render resolution. Absent/invalid word or Off: false.
inline bool frameGeneration(std::uint32_t word) noexcept {
    const auto choices=decode(word);
    return choices && choices->values[5]!=0;
}
// Draw distance (Claude, 2026-10-20; the owner: "Original by default, down to 70 %"): the LOD scale in percent; absent/invalid
// word or Original: nullopt (world_load.h then keeps the game's own scale).
inline std::optional<unsigned> drawDistancePercent(std::uint32_t word) noexcept {
    const auto choices=decode(word);
    if(!choices || !choices->values[6])return std::nullopt;
    return DrawDistancePercent[choices->values[6]];
}
// Traffic density (Claude, 2026-10-20): random and parked vehicle density in percent; absent/invalid word or Original:
// nullopt (world_load.h then keeps the game's own densities).
inline std::optional<unsigned> trafficPercent(std::uint32_t word) noexcept {
    const auto choices=decode(word);
    if(!choices || !choices->values[7] || choices->values[7]>=TrafficPercent.size())return std::nullopt;
    return TrafficPercent[choices->values[7]];
}
// Shadow distance (Claude, 2026-10-20): Near = the far shadow cascade is not drawn (ios-shadow-skip-band3; c342-c346: about -0.9 ms
// per frame while driving). Absent/invalid word or Original: false.
inline bool shadowNear(std::uint32_t word) noexcept {
    const auto choices=decode(word);
    return choices && choices->values[8]!=0;
}
// Screen (Claude, 2026-10-21): Fit = the letterbox (the whole 16:9 picture, bars where the screen's shape differs); Fill (the
// default, owner 2026-10-20 "without black bars") covers the screen. c357/c358: Fit is ~1.1 ms per real frame faster driving.
inline bool screenFit(std::uint32_t word) noexcept {
    const auto choices=decode(word);
    return choices && choices->values[9]!=0;
}
inline std::optional<unsigned> resolutionPercent(std::uint32_t word) noexcept {
#if defined(APPLE_LAUNCHER_OPTIONS_MUTANT_RESOLUTION)
    (void)word;return std::nullopt;
#else
    const auto choices=decode(word);
    return choices?std::optional(ResolutionPercent[choices->values[3]]):std::nullopt;
#endif
}
}
