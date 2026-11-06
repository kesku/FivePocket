#include "guest_thread_environment.h"

#include <array>
#include <cstring>
#include <limits>

namespace recomp::bootstrap {
namespace {

struct Range {
    std::uint32_t address;
    std::uint64_t size;
};

bool rangeFits(const Range& range) noexcept {
    return range.size <= GuestAddressSpace::Size
            && static_cast<std::uint64_t>(range.address)
                    <= GuestAddressSpace::Size - range.size;
}

bool rangesOverlap(const Range& left, const Range& right) noexcept {
    return left.size != 0 && right.size != 0
            && static_cast<std::uint64_t>(left.address)
                    < static_cast<std::uint64_t>(right.address) + right.size
            && static_cast<std::uint64_t>(right.address)
                    < static_cast<std::uint64_t>(left.address) + left.size;
}

bool contains(const Range& outer, const Range& inner) noexcept {
    return inner.address >= outer.address
            && static_cast<std::uint64_t>(inner.address) + inner.size
                    <= static_cast<std::uint64_t>(outer.address) + outer.size;
}

void writeU32(
        GuestAddressSpace& addressSpace,
        std::uint32_t address,
        std::uint32_t value) noexcept {
    auto* bytes = addressSpace.translate(address);
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
}

} // namespace

const char* guestThreadEnvironmentStateName(GuestThreadEnvironmentState state) noexcept {
    switch (state) {
    case GuestThreadEnvironmentState::Idle:
        return "idle";
    case GuestThreadEnvironmentState::Ready:
        return "ready";
    case GuestThreadEnvironmentState::InvalidRequest:
        return "invalid-request";
    case GuestThreadEnvironmentState::AddressSpaceUnavailable:
        return "address-space-unavailable";
    case GuestThreadEnvironmentState::AddressRangeInvalid:
        return "address-range-invalid";
    case GuestThreadEnvironmentState::AddressOverlap:
        return "address-overlap";
    }
    return "unknown";
}

bool GuestThreadEnvironment::initialize(
        GuestAddressSpace& addressSpace,
        const GuestThreadEnvironmentLayout& layout) {
    if (addressSpace_ != nullptr) {
        return false;
    }
    result_ = {};
    result_.layout = layout;
    if (!addressSpace.isReserved() || addressSpace.base() == nullptr) {
        result_.state = GuestThreadEnvironmentState::AddressSpaceUnavailable;
        result_.diagnostic = "Guest address space must be reserved by the caller";
        return false;
    }
    if (layout.pcrAddress == 0 || layout.threadAddress == 0
            || layout.tlsAddress == 0 || layout.stackBase == 0
            || layout.imageSize == 0 || layout.stackSize == 0 || layout.threadId == 0
            || ((layout.pcrAddress | layout.threadAddress | layout.tlsAddress
                    | layout.stackBase | layout.stackSize | layout.tlsDataSize) & 3U) != 0
            || layout.tlsDataSize > MaxStaticTlsSize
            || layout.tlsSlotCount > MaxTlsSlots
            || layout.tlsRawDataSize > layout.tlsDataSize
            || (layout.tlsRawDataSize != 0 && layout.tlsRawDataAddress == 0)) {
        result_.state = GuestThreadEnvironmentState::InvalidRequest;
        result_.diagnostic = "Thread addresses, identity, or bounded TLS metadata are invalid";
        return false;
    }

    const auto tlsSize = static_cast<std::uint64_t>(layout.tlsDataSize)
            + static_cast<std::uint64_t>(layout.tlsSlotCount) * 4U;
    if (tlsSize == 0) {
        result_.state = GuestThreadEnvironmentState::InvalidRequest;
        result_.diagnostic = "Thread TLS storage must contain static data or dynamic slots";
        return false;
    }
    const std::array<Range, 5> ranges{{
            {layout.pcrAddress, PcrSize},
            {layout.threadAddress, ThreadSize},
            {layout.tlsAddress, tlsSize},
            {layout.imageBase, layout.imageSize},
            {layout.stackBase, layout.stackSize},
    }};
    const Range rawTemplate{layout.tlsRawDataAddress, layout.tlsRawDataSize};
    for (const auto& range : ranges) {
        if (!rangeFits(range)) {
            result_.state = GuestThreadEnvironmentState::AddressRangeInvalid;
            result_.diagnostic = "Thread environment crosses the 32-bit guest boundary";
            return false;
        }
    }
    // The exclusive stack upper bound is itself written as a guest pointer.
    if (static_cast<std::uint64_t>(layout.stackBase) + layout.stackSize
                    > std::numeric_limits<std::uint32_t>::max()
            || (layout.tlsRawDataSize != 0
                    && (!rangeFits(rawTemplate) || !contains(ranges[3], rawTemplate)))) {
        result_.state = GuestThreadEnvironmentState::AddressRangeInvalid;
        result_.diagnostic = "Stack pointer or TLS template is outside its guest range";
        return false;
    }
    for (std::size_t left = 0; left < ranges.size(); ++left) {
        for (std::size_t right = left + 1; right < ranges.size(); ++right) {
            if (rangesOverlap(ranges[left], ranges[right])) {
                result_.state = GuestThreadEnvironmentState::AddressOverlap;
                result_.diagnostic = "Thread scratch, title image, or stack ranges overlap";
                return false;
            }
        }
    }

    // ABI reference: X_KPCR / X_KTHREAD and XThread::Create in public Xenia:
    // https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xthread.h
    // https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xthread.cc
    // Static TLS is followed immediately by the word-sized dynamic slots.
    std::memset(addressSpace.translate(layout.pcrAddress), 0, PcrSize);
    std::memset(addressSpace.translate(layout.threadAddress), 0, ThreadSize);
    std::memset(addressSpace.translate(layout.tlsAddress), 0, static_cast<std::size_t>(tlsSize));
    if (layout.tlsRawDataSize != 0) {
        std::memcpy(
                addressSpace.translate(layout.tlsAddress),
                addressSpace.translate(layout.tlsRawDataAddress),
                layout.tlsRawDataSize);
    }

    const auto stackHigh = layout.stackBase + layout.stackSize;
    writeU32(addressSpace, layout.pcrAddress, layout.tlsAddress);
    writeU32(addressSpace, layout.pcrAddress + 0x30U, layout.pcrAddress);
    writeU32(addressSpace, layout.pcrAddress + 0x70U, stackHigh);
    writeU32(addressSpace, layout.pcrAddress + 0x74U, layout.stackBase);
    writeU32(addressSpace, layout.pcrAddress + 0x100U, layout.threadAddress);
    writeU32(addressSpace, layout.threadAddress + 0x5CU, stackHigh);
    writeU32(addressSpace, layout.threadAddress + 0x60U, layout.stackBase);
    writeU32(addressSpace, layout.threadAddress + 0x68U, layout.tlsAddress);
    writeU32(addressSpace, layout.threadAddress + 0x14CU, layout.threadId);

    addressSpace_ = &addressSpace;
    tlsStorageSize_ = static_cast<std::uint32_t>(tlsSize);
    result_.state = GuestThreadEnvironmentState::Ready;
    result_.diagnostic = "Bounded PCR, minimal thread record, and guest TLS initialized";
    return true;
}

void GuestThreadEnvironment::reset() {
    if (addressSpace_ != nullptr && addressSpace_->isReserved()) {
        const auto& layout = result_.layout;
        std::memset(addressSpace_->translate(layout.pcrAddress), 0, PcrSize);
        std::memset(addressSpace_->translate(layout.threadAddress), 0, ThreadSize);
        std::memset(addressSpace_->translate(layout.tlsAddress), 0, tlsStorageSize_);
    }
    addressSpace_ = nullptr;
    tlsStorageSize_ = 0;
    result_ = {};
}

const GuestThreadEnvironmentResult& GuestThreadEnvironment::result() const noexcept {
    return result_;
}

bool GuestThreadEnvironment::ready() const noexcept {
    return addressSpace_ != nullptr && addressSpace_->isReserved()
            && result_.succeeded();
}

std::uint32_t GuestThreadEnvironment::dynamicTlsAddress() const noexcept {
    return ready() && result_.layout.tlsSlotCount != 0
            ? result_.layout.tlsAddress + result_.layout.tlsDataSize : 0;
}

std::uint32_t GuestThreadEnvironment::tlsStorageSize() const noexcept {
    return ready() ? tlsStorageSize_ : 0;
}

} // namespace recomp::bootstrap
