#pragma once

#include "guest_address_space.h"

#include <cstdint>
#include <string>

namespace recomp::bootstrap {

enum class GuestXexHeaderLookupState {
    Found,
    NotFound,
    GuestMemoryFault,
    InvalidHeader,
};

enum class GuestXexHeaderValueKind {
    None,
    Immediate,
    ValuePointer,
    DataPointer,
};

struct GuestXexHeaderLookupResult {
    GuestXexHeaderLookupState state{GuestXexHeaderLookupState::InvalidHeader};
    GuestXexHeaderValueKind kind{GuestXexHeaderValueKind::None};
    std::uint32_t value{0};
    std::string diagnostic;

    [[nodiscard]] bool found() const noexcept {
        return state == GuestXexHeaderLookupState::Found;
    }
};

// Bounded reader for a raw XEX2 header already copied into guest memory. It
// implements the three optional-header value forms used by
// RtlImageXexHeaderField without retaining a host path or payload copy.
class GuestXexHeader final {
public:
    static constexpr std::uint32_t FixedHeaderSize = 24;
    static constexpr std::uint32_t MaximumHeaderSize = 16U * 1024U * 1024U;
    static constexpr std::uint32_t MaximumOptionalHeaders = 4096;

    [[nodiscard]] static GuestXexHeaderLookupResult lookup(
            const GuestAddressSpace& addressSpace,
            std::uint32_t xexHeaderAddress,
            std::uint32_t key) noexcept;
};

[[nodiscard]] const char* guestXexHeaderLookupStateName(
        GuestXexHeaderLookupState state) noexcept;
[[nodiscard]] const char* guestXexHeaderValueKindName(
        GuestXexHeaderValueKind kind) noexcept;

} // namespace recomp::bootstrap
