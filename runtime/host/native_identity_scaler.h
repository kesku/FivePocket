#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace recomp::gpu {
// Input-only candidate for an explicitly native720p identity scaler. This is
// NOT a command, a GPU configuration or evidence of Xbox scaler equivalence.
// Geometry/filter/impulse coefficients are checked; the26tail controls remain
// opaque and OWNED. A consumer must resolve their obligations separately.
// No guest pointers survive inspection, and no registry/GPU state is changed.
class NativeIdentityScalerCandidate final {
public:
    static constexpr std::uint32_t Width = 1280, Height = 720;
    static constexpr std::size_t AxisWords = 163, ControlWords = 26;
    static constexpr std::size_t RecordWords = AxisWords * 2 + ControlWords, RecordBytes = RecordWords * 4;
    // Actual caller packs X/width inHIGH16, Y/height inLOW16. This intentionally
    // does not follow the transposed comments in some emulator references.
    struct Inputs {
        std::array<std::uint32_t, 7> geometryAndFilters; // srcXY,srcWH,outXY,outWH,fbWH,vertical,horizontal
        std::array<std::uint32_t, 6> filterParameterBits;
    };
    static std::optional<NativeIdentityScalerCandidate> inspect(const Inputs& inputs,
            const std::uint8_t* record, std::size_t bytes) noexcept {
        constexpr std::uint32_t PackedSize = (Width << 16) | Height;
        constexpr std::array<std::uint32_t, 7> Expected{0, PackedSize, 0, PackedSize, PackedSize, 7, 7};
        if (inputs.geometryAndFilters != Expected || !record || bytes != RecordBytes ||
            reinterpret_cast<std::uintptr_t>(record) > std::numeric_limits<std::uintptr_t>::max() - RecordBytes)
            return std::nullopt;
        for (const auto bits : inputs.filterParameterBits) if (bits) return std::nullopt;
        NativeIdentityScalerCandidate candidate(inputs);
        for (std::size_t i = 0; i < RecordWords; ++i) {
            const auto* p = record + i * 4;
            candidate.record_[i] = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
                (std::uint32_t{p[2]} << 8) | p[3];
        }
        // These tap counts are the observed native timing profile, not a
        // universal Xbox filter7 definition. Validate all320coefficient slots,
        // including unused capacity. Q9 unity=512;16 phases, one unit perphase.
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const auto base = axis * AxisWords;
            const std::uint32_t taps = axis ? 10 : 5;
            if (candidate.record_[base] != taps || candidate.record_[base + 1] != 0x3D800000 ||
                candidate.record_[base + 162] != 0x80000) return std::nullopt;
            const auto begin = (taps - 1) * 16 / 2;
            for (std::size_t i = 0; i < 160; ++i)
                if (candidate.record_[base + 2 + i] != (i >= begin && i < begin + 16 ? 512U : 0U))
                    return std::nullopt;
        }
        return candidate;
    }
    const Inputs& inputs() const noexcept { return inputs_; }
    const std::array<std::uint32_t, RecordWords>& record() const noexcept { return record_; }
    std::array<std::uint32_t, ControlWords> unresolvedControls() const noexcept {
        std::array<std::uint32_t, ControlWords> result{};
        for (std::size_t i = 0; i < result.size(); ++i) result[i] = record_[AxisWords * 2 + i];
        return result;
    }
    // Only byte identity of a previously validated snapshot, not authentication,
    // memory ownership, command admission or a substitute for control semantics.
    bool matches(const std::uint8_t* record, std::size_t bytes) const noexcept {
        if (!record || bytes != RecordBytes ||
            reinterpret_cast<std::uintptr_t>(record) > std::numeric_limits<std::uintptr_t>::max() - RecordBytes) return false;
        for (std::size_t i = 0; i < RecordBytes; ++i)
            if (record[i] != static_cast<std::uint8_t>(record_[i / 4] >> (24 - (i % 4) * 8))) return false;
        return true;
    }
private:
    explicit NativeIdentityScalerCandidate(const Inputs& inputs) noexcept : inputs_(inputs) {}
    Inputs inputs_;
    std::array<std::uint32_t, RecordWords> record_{};
};

// Complete input policy for the native identity operation. This resolves the
// tail ONLY as a fixed native compatibility profile derived from the caller's
// geometry/phase arithmetic. It does not reconstruct the retail register-write
// sequence, change physical display timing or retire an ordered GPU command.
class NativeIdentityScalerPlan final {
public:
    static constexpr std::array<std::uint32_t, NativeIdentityScalerCandidate::ControlWords> expectedControls() noexcept {
        using C = NativeIdentityScalerCandidate;
        std::array<std::uint32_t, C::ControlWords> result{};
        // Zero origin/margins and unused fields. Offsets relative to1408byte
        // caller record, NOT GPU register addresses or assumed reset values.
        result[(1320 - 1304) / 4] = (C::Width + 31) & ~31U;
        result[(1340 - 1304) / 4] = (C::Width + 31) & ~31U;
        result[(1344 - 1304) / 4] = (C::Height + 1) & ~1U;
        result[(1356 - 1304) / 4] = (C::Width << 16) | C::Height;
        result[(1360 - 1304) / 4] = 1;
        result[(1364 - 1304) / 4] = (5 - 1) | ((10 - 1) << 8);
        // Axis increment is1.0Q19; progressive conversion shifts by5 toQ24.
        result[(1376 - 1304) / 4] = (1U << 19) << 5;
        result[(1388 - 1304) / 4] = 1U << 24;
        result[(1392 - 1304) / 4] = (1U << 19) << 5;
        // floor((Q19increment*2^-19+taps+1)*64+0.5)<<9.
        // Unity makes these exact integers; retain H6.0/V3.5Q16, not rounded
        // to an even-tap reference's V4.0 or mislabeled as hardware timestamps.
        result[(1380 - 1304) / 4] = ((1 + 10 + 1) * 64) << 9;
        result[(1396 - 1304) / 4] = ((1 + 5 + 1) * 64) << 9;
        result[(1404 - 1304) / 4] = (C::Width + 3) / 4 - 1;
        return result;
    }
    static std::optional<NativeIdentityScalerPlan> make(const NativeIdentityScalerCandidate& candidate) noexcept {
        if (candidate.unresolvedControls() != expectedControls()) return std::nullopt;
        return NativeIdentityScalerPlan(candidate);
    }
    const NativeIdentityScalerCandidate& candidate() const noexcept { return candidate_; }
    // The explicitly native operation maps logical pixel centers one-to-one.
    // Native surface letterboxing/format conversion is a subsequent operation.
    std::optional<std::uint32_t> rgbaSourceOffset(std::uint32_t x, std::uint32_t y) const noexcept {
        using C = NativeIdentityScalerCandidate;
        if (x >= C::Width || y >= C::Height) return std::nullopt;
        return (y * C::Width + x) * 4;
    }
private:
    explicit NativeIdentityScalerPlan(const NativeIdentityScalerCandidate& candidate) noexcept : candidate_(candidate) {}
    NativeIdentityScalerCandidate candidate_;
};
}
