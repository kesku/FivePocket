#pragma once
#include "native_identity_scaler.h"

namespace recomp::gpu {
// Original native-only command, not retail PM4. The caller copies these bytes
// from temporary stack storage into its command allocation. The issued owner
// therefore retains a value snapshot, NEVER a pointer into that temporary stack.
// A future ordered consumer must match the full issued record AND validate the
// current command allocation/stream. Matching is not authentication/completion.
class NativeScalerCommand final {
public:
    static constexpr std::uint32_t Words = 64, Bytes = Words * 4;
    struct Identity {
        std::uint64_t ticket;
        std::uint32_t previousReadWord, issuerThread;
    };
    static std::optional<NativeScalerCommand> make(Identity identity, const NativeIdentityScalerPlan& plan) noexcept {
        if (!identity.ticket || identity.previousReadWord >= 4 * 1024 * 1024 || !identity.issuerThread ||
            (identity.issuerThread & 3) || std::uint64_t{identity.issuerThread} + 4 > 0x100000000ULL) return std::nullopt;
        NativeScalerCommand result(identity, plan);
        // Invalid ordinary PM4 framing: apparent payload16384words exceeds this
        // bounded64word record. No ordinary decoder may retire it as a NOP.
        const std::array<std::uint32_t, 10> prefix{0xFFFF5343, 0x414C0001, Words, 1,
            static_cast<std::uint32_t>(identity.ticket >> 32), static_cast<std::uint32_t>(identity.ticket),
            identity.previousReadWord, identity.issuerThread, 1280, 720};
        unsigned index = 0;
        const auto put = [&](std::uint32_t word) {
            for (unsigned lane = 0; lane < 4; ++lane) result.bytes_[index * 4 + lane] = word >> (24 - lane * 8);
            ++index;
        };
        for (auto word : prefix) put(word);
        for (auto word : plan.candidate().inputs().geometryAndFilters) put(word);
        for (auto word : plan.candidate().inputs().filterParameterBits) put(word);
        for (auto word : plan.candidate().unresolvedControls()) put(word);
        // Coefficients are fixed by the accepted profile and remain owned in
        // plan_. Neither arbitrary coefficients nor a borrowed pointer escape.
        return result;
    }
    const Identity& identity() const noexcept { return identity_; }
    const NativeIdentityScalerPlan& plan() const noexcept { return plan_; }
    const std::array<std::uint8_t, Bytes>& bytes() const noexcept { return bytes_; }
    bool matches(const std::uint8_t* bytes, std::size_t size) const noexcept {
        return bytes && size == Bytes &&
            reinterpret_cast<std::uintptr_t>(bytes) <= std::numeric_limits<std::uintptr_t>::max() - Bytes &&
            std::memcmp(bytes_.data(), bytes, Bytes) == 0;
    }
    // Bounded search in ONE already-owned command allocation after the caller
    // has copied its stack output. No guessed destination address. A duplicate
    // issued record is ambiguous and rejects; this is still not execution.
    std::optional<std::uint32_t> uniqueOffset(const std::uint8_t* bytes,std::size_t size) const noexcept {
        if (!bytes || size<Bytes || size>8*1024*1024 || (size&3) ||
            reinterpret_cast<std::uintptr_t>(bytes)>std::numeric_limits<std::uintptr_t>::max()-size) return std::nullopt;
        std::optional<std::uint32_t> found;
        for (std::uint32_t offset=0;offset<=size-Bytes;offset+=4) {
            if (bytes[offset]!=bytes_[0] || !matches(bytes+offset,Bytes)) continue;
            if (found) return std::nullopt;
            found=offset;
        }
        return found;
    }
private:
    NativeScalerCommand(Identity identity, const NativeIdentityScalerPlan& plan) noexcept : identity_(identity), plan_(plan) {}
    Identity identity_;
    NativeIdentityScalerPlan plan_;
    std::array<std::uint8_t, Bytes> bytes_{};
};
}
