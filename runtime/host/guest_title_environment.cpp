#include "guest_title_environment.h"

#include <cstring>

namespace recomp::bootstrap {
namespace {

bool rangeFits(std::uint32_t address, std::uint64_t size) noexcept {
    return size <= GuestAddressSpace::Size
            && static_cast<std::uint64_t>(address) <= GuestAddressSpace::Size - size;
}

bool rangesOverlap(
        std::uint32_t leftAddress,
        std::uint64_t leftSize,
        std::uint32_t rightAddress,
        std::uint64_t rightSize) noexcept {
    const auto leftEnd = static_cast<std::uint64_t>(leftAddress) + leftSize;
    const auto rightEnd = static_cast<std::uint64_t>(rightAddress) + rightSize;
    return static_cast<std::uint64_t>(leftAddress) < rightEnd
            && static_cast<std::uint64_t>(rightAddress) < leftEnd;
}

std::uint32_t readU32(
        const GuestAddressSpace& addressSpace,
        std::uint32_t address) noexcept {
    const auto* bytes = addressSpace.translate(address);
    return (static_cast<std::uint32_t>(bytes[0]) << 24U)
            | (static_cast<std::uint32_t>(bytes[1]) << 16U)
            | (static_cast<std::uint32_t>(bytes[2]) << 8U)
            | static_cast<std::uint32_t>(bytes[3]);
}

void writeU16(
        GuestAddressSpace& addressSpace,
        std::uint32_t address,
        std::uint16_t value) noexcept {
    auto* bytes = addressSpace.translate(address);
    bytes[0] = static_cast<std::uint8_t>(value >> 8U);
    bytes[1] = static_cast<std::uint8_t>(value);
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

const char* guestTitleEnvironmentStateName(GuestTitleEnvironmentState state) noexcept {
    switch (state) {
    case GuestTitleEnvironmentState::Idle:
        return "idle";
    case GuestTitleEnvironmentState::Ready:
        return "ready";
    case GuestTitleEnvironmentState::InvalidRequest:
        return "invalid-request";
    case GuestTitleEnvironmentState::AddressSpaceUnavailable:
        return "address-space-unavailable";
    case GuestTitleEnvironmentState::AddressRangeInvalid:
        return "address-range-invalid";
    case GuestTitleEnvironmentState::AddressOverlap:
        return "address-overlap";
    }
    return "unknown";
}

bool GuestTitleEnvironment::initialize(
        GuestAddressSpace& addressSpace,
        const GuestTitleEnvironmentLayout& layout) {
    reset();
    result_.layout = layout;
    result_.diagnostic = "Validating guest title environment layout";

    if (!addressSpace.isReserved() || addressSpace.base() == nullptr) {
        result_.state = GuestTitleEnvironmentState::AddressSpaceUnavailable;
        result_.diagnostic = "Guest address space must be reserved by the caller";
        return false;
    }
    if (layout.executableModuleHandleSlot == 0
            || layout.exportedVariableAddress == 0
            || layout.moduleAddress == 0
            || layout.xexHeaderAddress == 0
            || layout.imageSize == 0
            || (layout.executableModuleHandleSlot & 3U) != 0
            || (layout.exportedVariableAddress & 3U) != 0
            || (layout.moduleAddress & 3U) != 0
            || (layout.xexHeaderAddress & 3U) != 0) {
        result_.state = GuestTitleEnvironmentState::InvalidRequest;
        result_.diagnostic = "Title environment addresses must be nonzero and word-aligned";
        return false;
    }
    if (!rangeFits(layout.imageBase, layout.imageSize)
            || !rangeFits(layout.executableModuleHandleSlot, 4)
            || !rangeFits(layout.exportedVariableAddress, 4)
            || !rangeFits(layout.moduleAddress, ModuleStructureSize)
            || !rangeFits(layout.xexHeaderAddress, 4)
            || layout.entryPoint < layout.imageBase
            || static_cast<std::uint64_t>(layout.entryPoint)
                    >= static_cast<std::uint64_t>(layout.imageBase) + layout.imageSize) {
        result_.state = GuestTitleEnvironmentState::AddressRangeInvalid;
        result_.diagnostic = "Title environment layout crosses a guest range boundary";
        return false;
    }

    const auto imageSize = static_cast<std::uint64_t>(layout.imageSize);
    const bool scratchOverlapsImage =
            rangesOverlap(layout.exportedVariableAddress, 4, layout.imageBase, imageSize)
            || rangesOverlap(
                    layout.moduleAddress,
                    ModuleStructureSize,
                    layout.imageBase,
                    imageSize)
            || rangesOverlap(layout.xexHeaderAddress, 4, layout.imageBase, imageSize);
    const bool scratchOverlapsScratch =
            rangesOverlap(
                    layout.exportedVariableAddress,
                    4,
                    layout.moduleAddress,
                    ModuleStructureSize)
            || rangesOverlap(layout.exportedVariableAddress, 4, layout.xexHeaderAddress, 4)
            || rangesOverlap(
                    layout.moduleAddress,
                    ModuleStructureSize,
                    layout.xexHeaderAddress,
                    4);
    if (scratchOverlapsImage || scratchOverlapsScratch) {
        result_.state = GuestTitleEnvironmentState::AddressOverlap;
        result_.diagnostic = "Synthetic title structures overlap the image or one another";
        return false;
    }
    if (!rangesOverlap(
                layout.executableModuleHandleSlot,
                4,
                layout.imageBase,
                imageSize)) {
        result_.state = GuestTitleEnvironmentState::InvalidRequest;
        result_.diagnostic = "Executable-module data-import slot is outside the title image";
        return false;
    }

    if ((layout.threadObjectTypeSlot == 0) != (layout.threadObjectTypeAddress == 0)) {
        result_.state = GuestTitleEnvironmentState::InvalidRequest;
        result_.diagnostic = "Thread object type slot and identity must be supplied together";
        return false;
    }
    if (layout.threadObjectTypeSlot != 0) {
        const auto slot = layout.threadObjectTypeSlot;
        const auto identity = layout.threadObjectTypeAddress;
        if (((slot | identity) & 3U) != 0 || !rangeFits(identity, ThreadObjectTypeIdentitySize)
                || !rangeFits(slot, 4) || slot < layout.imageBase
                || static_cast<std::uint64_t>(slot) + 4 > layout.imageBase + imageSize
                || slot == layout.executableModuleHandleSlot) {
            result_.state = GuestTitleEnvironmentState::InvalidRequest;
            result_.diagnostic = "Thread object type slot or identity range is invalid";
            return false;
        }
        if (rangesOverlap(identity, 4, layout.imageBase, imageSize)
                || rangesOverlap(identity, 4, layout.exportedVariableAddress, 4)
                || rangesOverlap(identity, 4, layout.moduleAddress, ModuleStructureSize)
                || rangesOverlap(identity, 4, layout.xexHeaderAddress, 4)) {
            result_.state = GuestTitleEnvironmentState::AddressOverlap;
            result_.diagnostic = "Thread object type identity overlaps other title storage";
            return false;
        }
        // The parser validates the library and unpaired data record. Raw XEX
        // descriptors are big endian, while XenonRecomp's decoded image may
        // have swapped them in place. Accept only ordinal 0x1B/data in either
        // parser-supported representation; preserve the exact original bytes.
        const auto encoded = readU32(addressSpace, slot);
        const auto swapped = (encoded >> 24U) | ((encoded >> 8U) & 0x0000FF00U)
                | ((encoded << 8U) & 0x00FF0000U) | (encoded << 24U);
        if ((encoded & 0xFF00FFFFU) != 0x1BU && (swapped & 0xFF00FFFFU) != 0x1BU) {
            result_.state = GuestTitleEnvironmentState::InvalidRequest;
            result_.diagnostic = "Thread object type import word does not match ordinal 0x1B";
            return false;
        }
    }

    originalImportSlotValue_ = readU32(addressSpace, layout.executableModuleHandleSlot);
    std::memset(addressSpace.translate(layout.exportedVariableAddress), 0, 4);
    std::memset(
            addressSpace.translate(layout.moduleAddress),
            0,
            ModuleStructureSize);

    // The imported slot resolves to the exported kernel variable; that variable
    // contains the HMODULE pointer. HMODULE is an X_LDR_DATA_TABLE_ENTRY-like
    // structure whose XEX-header field is at offset 0x58.
    writeU32(
            addressSpace,
            layout.executableModuleHandleSlot,
            layout.exportedVariableAddress);
    writeU32(addressSpace, layout.exportedVariableAddress, layout.moduleAddress);
    writeU32(
            addressSpace,
            layout.moduleAddress + ModuleImageBaseOffset,
            layout.imageBase);
    writeU32(
            addressSpace,
            layout.moduleAddress + ModuleFullImageSizeOffset,
            layout.imageSize);
    writeU32(
            addressSpace,
            layout.moduleAddress + ModuleEntryPointOffset,
            layout.entryPoint);
    writeU16(
            addressSpace,
            layout.moduleAddress + ModuleLoadCountOffset,
            1);
    writeU32(
            addressSpace,
            layout.moduleAddress + ModuleXexHeaderOffset,
            layout.xexHeaderAddress);

    if (layout.threadObjectTypeSlot != 0) {
        originalThreadTypeSlotValue_ = readU32(addressSpace, layout.threadObjectTypeSlot);
        writeU32(addressSpace, layout.threadObjectTypeAddress, 0);
        writeU32(addressSpace, layout.threadObjectTypeSlot, layout.threadObjectTypeAddress);
    }

    addressSpace_ = &addressSpace;
    result_.state = GuestTitleEnvironmentState::Ready;
    result_.diagnostic =
            "Synthetic title module and optional opaque thread-type identity initialized";
    return true;
}

void GuestTitleEnvironment::reset() {
    if (addressSpace_ != nullptr && addressSpace_->isReserved()) {
        const auto layout = result_.layout;
        writeU32(
                *addressSpace_,
                layout.executableModuleHandleSlot,
                originalImportSlotValue_);
        std::memset(addressSpace_->translate(layout.exportedVariableAddress), 0, 4);
        std::memset(
                addressSpace_->translate(layout.moduleAddress),
                0,
                ModuleStructureSize);
        if (layout.threadObjectTypeSlot != 0) {
            writeU32(*addressSpace_, layout.threadObjectTypeSlot, originalThreadTypeSlotValue_);
            writeU32(*addressSpace_, layout.threadObjectTypeAddress, 0);
        }
    }
    addressSpace_ = nullptr;
    originalImportSlotValue_ = 0;
    originalThreadTypeSlotValue_ = 0;
    result_ = {};
}

const GuestTitleEnvironmentResult& GuestTitleEnvironment::result() const noexcept {
    return result_;
}

} // namespace recomp::bootstrap
