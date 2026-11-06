#pragma once

#include "guest_address_space.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace recomp::bootstrap {

enum class GuestImageLoadState {
    Idle,
    Loading,
    Loaded,
    InvalidRequest,
    AddressSpaceUnavailable,
    AddressRangeInvalid,
    InvalidPath,
    SymlinkRejected,
    NotRegularFile,
    OpenFailed,
    SizeMismatch,
    ShortRead,
    ReadFailed,
};

struct GuestImageLoadResult {
    GuestImageLoadState state{GuestImageLoadState::Idle};
    std::uint32_t guestBase{0};
    std::uint64_t expectedSize{0};
    std::uint64_t observedFileSize{0};
    std::uint64_t bytesRead{0};
    std::uint64_t bytesLoaded{0};
    std::string diagnostic{"No decoded image load requested"};

    [[nodiscard]] bool succeeded() const noexcept {
        return state == GuestImageLoadState::Loaded;
    }
};

// This is a dormant, caller-driven primitive. It never searches for files,
// chooses a title, invokes guest code, or runs during asset-free startup.
class GuestImageLoader final {
public:
    [[nodiscard]] bool loadExplicitDecodedImage(
            GuestAddressSpace& addressSpace,
            const std::filesystem::path& explicitPath,
            std::uint32_t guestBase,
            std::uint64_t expectedSize);

    void reset();

    [[nodiscard]] const GuestImageLoadResult& result() const noexcept;

private:
    GuestImageLoadResult result_{};
};

[[nodiscard]] const char* guestImageLoadStateName(GuestImageLoadState state) noexcept;

} // namespace recomp::bootstrap
