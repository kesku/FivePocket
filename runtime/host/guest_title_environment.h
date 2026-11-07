#pragma once

#include "guest_address_space.h"

#include <cstdint>
#include <string>

namespace recomp::bootstrap {

enum class GuestTitleEnvironmentState {
    Idle,
    Ready,
    InvalidRequest,
    AddressSpaceUnavailable,
    AddressRangeInvalid,
    AddressOverlap,
};

struct GuestTitleEnvironmentLayout {
    std::uint32_t executableModuleHandleSlot{0};
    std::uint32_t exportedVariableAddress{0};
    std::uint32_t moduleAddress{0};
    std::uint32_t xexHeaderAddress{0};
    std::uint32_t imageBase{0};
    std::uint32_t imageSize{0};
    std::uint32_t entryPoint{0};
    // Optional paired binding, parser-confirmed xboxkrnl ordinal 0x1B.
    std::uint32_t threadObjectTypeSlot{0};
    std::uint32_t threadObjectTypeAddress{0};
};

struct GuestTitleEnvironmentResult {
    GuestTitleEnvironmentState state{GuestTitleEnvironmentState::Idle};
    GuestTitleEnvironmentLayout layout{};
    std::string diagnostic{"No guest title environment requested"};

    [[nodiscard]] bool succeeded() const noexcept {
        return state == GuestTitleEnvironmentState::Ready;
    }
};

// Dormant caller-driven initialization for the minimum Xbox title-module data
// seam. The caller supplies a parser-validated data-import slot and distinct
// scratch addresses. The address space must outlive this object or reset must
// be called first. This class does not load XEX headers or dispatch imports.
class GuestTitleEnvironment final {
public:
    static constexpr std::uint32_t ModuleStructureSize = 0x64U;
    static constexpr std::uint32_t ModuleImageBaseOffset = 0x1CU;
    static constexpr std::uint32_t ModuleFullImageSizeOffset = 0x38U;
    static constexpr std::uint32_t ModuleEntryPointOffset = 0x3CU;
    static constexpr std::uint32_t ModuleLoadCountOffset = 0x40U;
    static constexpr std::uint32_t ModuleXexHeaderOffset = 0x58U;
    // Opaque address identity for type comparisons, not a complete OBJECT_TYPE.
    static constexpr std::uint32_t ThreadObjectTypeIdentitySize = 4U;

    [[nodiscard]] bool initialize(
            GuestAddressSpace& addressSpace,
            const GuestTitleEnvironmentLayout& layout);
    void reset();

    [[nodiscard]] const GuestTitleEnvironmentResult& result() const noexcept;

private:
    GuestAddressSpace* addressSpace_{nullptr};
    std::uint32_t originalImportSlotValue_{0};
    std::uint32_t originalThreadTypeSlotValue_{0};
    GuestTitleEnvironmentResult result_{};
};

[[nodiscard]] const char* guestTitleEnvironmentStateName(
        GuestTitleEnvironmentState state) noexcept;

} // namespace recomp::bootstrap
