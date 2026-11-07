#pragma once

#include <cstdint>
#include <array>
#include <limits>
#include <optional>

namespace recomp::bootstrap {
// 1601-01-01 -> 1970-01-01, in 100-nanosecond ticks. Signed LARGE_INTEGER
// range is intentional; this is system time, not the CPU performance counter.
inline constexpr std::int64_t FileTimeUnixEpoch = 116444736000000000LL;
// Proleptic Gregorian calendar, UTC-like civil fields; no host timezone/DST.
// Fields are signed SHORT year/month/day/hour/minute/second/millisecond/weekday.
// Weekday is explicitly ignored, matching RtlTimeFieldsToTime's contract.
inline std::optional<std::uint64_t> fileTimeFromFields(const std::array<std::int32_t, 8>& fields) noexcept {
    const auto year = fields[0], month = fields[1], day = fields[2];
    if (year < 1601 || year > 32767 || month < 1 || month > 12 || day < 1
            || fields[3] < 0 || fields[3] > 23 || fields[4] < 0 || fields[4] > 59
            || fields[5] < 0 || fields[5] > 59 || fields[6] < 0 || fields[6] > 999) { return std::nullopt; }
    constexpr std::array<std::int32_t, 12> lengths{31,28,31,30,31,30,31,31,30,31,30,31};
    constexpr std::array<std::int32_t, 12> preceding{0,31,59,90,120,151,181,212,243,273,304,334};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > lengths[month - 1] + (month == 2 && leap ? 1 : 0)) { return std::nullopt; }
    const auto previous = static_cast<std::uint64_t>(year - 1);
    const auto days = 365 * previous + previous / 4 - previous / 100 + previous / 400 - 584388
            + preceding[month - 1] + (month > 2 && leap ? 1 : 0) + day - 1;
    const auto ticks = (((days * 24 + fields[3]) * 60 + fields[4]) * 60 + fields[5]) * 10000000 + fields[6] * 10000ULL;
    if (ticks > static_cast<std::uint64_t>(INT64_MAX)) { return std::nullopt; }
    return ticks;
}
inline std::optional<std::uint64_t> fileTimeFromUnixTicks(std::int64_t ticks) noexcept {
    if (ticks < -FileTimeUnixEpoch || ticks > INT64_MAX - FileTimeUnixEpoch) { return std::nullopt; }
    return static_cast<std::uint64_t>(ticks + FileTimeUnixEpoch);
}
inline std::optional<std::uint64_t> advanceFileTime(std::uint64_t base, std::int64_t elapsedTicks) noexcept {
    if (elapsedTicks < 0 || base > static_cast<std::uint64_t>(INT64_MAX)
            || static_cast<std::uint64_t>(elapsedTicks) > static_cast<std::uint64_t>(INT64_MAX) - base) {
        return std::nullopt;
    }
    return base + static_cast<std::uint64_t>(elapsedTicks);
}
} // namespace recomp::bootstrap
