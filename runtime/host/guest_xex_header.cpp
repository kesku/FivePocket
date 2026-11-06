#include "guest_xex_header.h"

#include <cstddef>
#include <cstdint>

namespace recomp::bootstrap {
namespace {

constexpr std::uint32_t OptionalHeaderOffset = 24;
constexpr std::uint32_t OptionalHeaderSize = 8;

bool contains(
        const GuestAddressSpace& addressSpace,
        std::uint32_t address,
        std::uint64_t size) noexcept {
    return addressSpace.isReserved()
            && size <= GuestAddressSpace::Size
            && static_cast<std::uint64_t>(address) <= GuestAddressSpace::Size - size;
}

bool readU32(
        const GuestAddressSpace& addressSpace,
        std::uint32_t address,
        std::uint32_t& value) noexcept {
    if (!contains(addressSpace, address, 4)) {
        return false;
    }
    const auto* bytes = addressSpace.translate(address);
    value = (static_cast<std::uint32_t>(bytes[0]) << 24U)
            | (static_cast<std::uint32_t>(bytes[1]) << 16U)
            | (static_cast<std::uint32_t>(bytes[2]) << 8U)
            | static_cast<std::uint32_t>(bytes[3]);
    return true;
}

GuestXexHeaderLookupResult fail(
        GuestXexHeaderLookupState state,
        const char* diagnostic) {
    return {state, GuestXexHeaderValueKind::None, 0, diagnostic};
}

} // namespace

const char* guestXexHeaderLookupStateName(GuestXexHeaderLookupState state) noexcept {
    switch (state) {
    case GuestXexHeaderLookupState::Found:
        return "found";
    case GuestXexHeaderLookupState::NotFound:
        return "not-found";
    case GuestXexHeaderLookupState::GuestMemoryFault:
        return "guest-memory-fault";
    case GuestXexHeaderLookupState::InvalidHeader:
        return "invalid-header";
    }
    return "unknown";
}

const char* guestXexHeaderValueKindName(GuestXexHeaderValueKind kind) noexcept {
    switch (kind) {
    case GuestXexHeaderValueKind::None:
        return "none";
    case GuestXexHeaderValueKind::Immediate:
        return "immediate";
    case GuestXexHeaderValueKind::ValuePointer:
        return "value-pointer";
    case GuestXexHeaderValueKind::DataPointer:
        return "data-pointer";
    }
    return "unknown";
}

GuestXexHeaderLookupResult GuestXexHeader::lookup(
        const GuestAddressSpace& addressSpace,
        std::uint32_t xexHeaderAddress,
        std::uint32_t key) noexcept {
    if (xexHeaderAddress == 0
            || !contains(addressSpace, xexHeaderAddress, FixedHeaderSize)) {
        return fail(
                GuestXexHeaderLookupState::GuestMemoryFault,
                "XEX2 fixed header is outside guest memory");
    }

    const auto* magic = addressSpace.translate(xexHeaderAddress);
    if (magic[0] != 'X' || magic[1] != 'E' || magic[2] != 'X' || magic[3] != '2') {
        return fail(
                GuestXexHeaderLookupState::InvalidHeader,
                "Guest header does not have XEX2 magic");
    }

    std::uint32_t headerSize = 0;
    std::uint32_t headerCount = 0;
    if (!readU32(addressSpace, xexHeaderAddress + 8, headerSize)
            || !readU32(addressSpace, xexHeaderAddress + 20, headerCount)) {
        return fail(
                GuestXexHeaderLookupState::GuestMemoryFault,
                "XEX2 size/count fields are outside guest memory");
    }
    if (headerSize < FixedHeaderSize || headerSize > MaximumHeaderSize) {
        return fail(
                GuestXexHeaderLookupState::InvalidHeader,
                "XEX2 header size is outside the configured bounds");
    }
    if (headerCount > MaximumOptionalHeaders) {
        return fail(
                GuestXexHeaderLookupState::InvalidHeader,
                "XEX2 optional-header count exceeds the configured bound");
    }
    const auto tableSize = static_cast<std::uint64_t>(headerCount) * OptionalHeaderSize;
    if (tableSize > headerSize - FixedHeaderSize
            || !contains(addressSpace, xexHeaderAddress, headerSize)) {
        return fail(
                GuestXexHeaderLookupState::InvalidHeader,
                "XEX2 optional-header table is outside the declared header");
    }

    bool found = false;
    std::uint32_t foundEntryAddress = 0;
    std::uint32_t foundValue = 0;
    for (std::uint32_t index = 0; index < headerCount; ++index) {
        const auto entryOffset = static_cast<std::uint64_t>(OptionalHeaderOffset)
                + static_cast<std::uint64_t>(index) * OptionalHeaderSize;
        const auto entryAddress64 = static_cast<std::uint64_t>(xexHeaderAddress)
                + entryOffset;
        if (entryAddress64 > UINT32_MAX) {
            return fail(
                    GuestXexHeaderLookupState::GuestMemoryFault,
                    "XEX2 optional-header address crosses the guest boundary");
        }
        const auto entryAddress = static_cast<std::uint32_t>(entryAddress64);
        std::uint32_t entryKey = 0;
        std::uint32_t entryValue = 0;
        if (!readU32(addressSpace, entryAddress, entryKey)
                || !readU32(addressSpace, entryAddress + 4, entryValue)) {
            return fail(
                    GuestXexHeaderLookupState::GuestMemoryFault,
                    "XEX2 optional-header entry is outside guest memory");
        }
        if (entryKey != key) {
            continue;
        }
        if (found) {
            return fail(
                    GuestXexHeaderLookupState::InvalidHeader,
                    "XEX2 optional-header key is duplicated");
        }
        found = true;
        foundEntryAddress = entryAddress;
        foundValue = entryValue;
    }

    if (!found) {
        return fail(
                GuestXexHeaderLookupState::NotFound,
                "XEX2 optional-header key is absent");
    }

    switch (key & 0xFFU) {
    case 0:
        return {
                GuestXexHeaderLookupState::Found,
                GuestXexHeaderValueKind::Immediate,
                foundValue,
                "Resolved immediate XEX2 optional-header value",
        };
    case 1:
        return {
                GuestXexHeaderLookupState::Found,
                GuestXexHeaderValueKind::ValuePointer,
                foundEntryAddress + 4,
                "Resolved pointer to XEX2 optional-header value",
        };
    default:
        if (foundValue >= headerSize) {
            return fail(
                    GuestXexHeaderLookupState::InvalidHeader,
                    "XEX2 optional-header data offset exceeds the declared header");
        }
        return {
                GuestXexHeaderLookupState::Found,
                GuestXexHeaderValueKind::DataPointer,
                xexHeaderAddress + foundValue,
                "Resolved pointer to XEX2 optional-header data",
        };
    }
}

} // namespace recomp::bootstrap
