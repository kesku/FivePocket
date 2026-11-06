#include "guest_image_loader.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace recomp::bootstrap {
namespace {

constexpr std::uint64_t ReadChunkSize = 1024U * 1024U;

std::string diagnosticWithCode(const char* message, std::uint64_t code) {
    return std::string(message) + " (system code " + std::to_string(code) + ')';
}

bool hasEmbeddedNull(const std::filesystem::path& path) {
    const auto& native = path.native();
    return native.find(std::filesystem::path::value_type{})
            != std::filesystem::path::string_type::npos;
}

void scrubPartialImage(std::uint8_t* destination, std::uint64_t bytesRead) noexcept {
    if (destination != nullptr && bytesRead != 0) {
        std::memset(destination, 0, static_cast<std::size_t>(bytesRead));
    }
}

#if defined(_WIN32)

class ScopedFile final {
public:
    explicit ScopedFile(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedFile() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

#else

class ScopedFile final {
public:
    explicit ScopedFile(int descriptor) noexcept : descriptor_(descriptor) {}
    ~ScopedFile() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
    int descriptor_{-1};
};

#endif

} // namespace

const char* guestImageLoadStateName(GuestImageLoadState state) noexcept {
    switch (state) {
    case GuestImageLoadState::Idle:
        return "idle";
    case GuestImageLoadState::Loading:
        return "loading";
    case GuestImageLoadState::Loaded:
        return "loaded";
    case GuestImageLoadState::InvalidRequest:
        return "invalid-request";
    case GuestImageLoadState::AddressSpaceUnavailable:
        return "address-space-unavailable";
    case GuestImageLoadState::AddressRangeInvalid:
        return "address-range-invalid";
    case GuestImageLoadState::InvalidPath:
        return "invalid-path";
    case GuestImageLoadState::SymlinkRejected:
        return "symlink-rejected";
    case GuestImageLoadState::NotRegularFile:
        return "not-regular-file";
    case GuestImageLoadState::OpenFailed:
        return "open-failed";
    case GuestImageLoadState::SizeMismatch:
        return "size-mismatch";
    case GuestImageLoadState::ShortRead:
        return "short-read";
    case GuestImageLoadState::ReadFailed:
        return "read-failed";
    }
    return "unknown";
}

bool GuestImageLoader::loadExplicitDecodedImage(
        GuestAddressSpace& addressSpace,
        const std::filesystem::path& explicitPath,
        std::uint32_t guestBase,
        std::uint64_t expectedSize) {
    result_ = {};
    result_.state = GuestImageLoadState::Loading;
    result_.guestBase = guestBase;
    result_.expectedSize = expectedSize;
    result_.diagnostic = "Validating explicit decoded image request";

    if (expectedSize == 0) {
        result_.state = GuestImageLoadState::InvalidRequest;
        result_.diagnostic = "Expected decoded image size must be nonzero";
        return false;
    }
    if (!addressSpace.isReserved() || addressSpace.base() == nullptr) {
        result_.state = GuestImageLoadState::AddressSpaceUnavailable;
        result_.diagnostic = "Guest address space must be reserved by the caller";
        return false;
    }

    const auto rangeStart = static_cast<std::uint64_t>(guestBase);
    if (expectedSize > GuestAddressSpace::Size
            || rangeStart > GuestAddressSpace::Size - expectedSize) {
        result_.state = GuestImageLoadState::AddressRangeInvalid;
        result_.diagnostic = "Decoded image does not fit in the 32-bit guest address space";
        return false;
    }
    if (explicitPath.empty() || !explicitPath.is_absolute() || hasEmbeddedNull(explicitPath)) {
        result_.state = GuestImageLoadState::InvalidPath;
        result_.diagnostic = "Caller must provide one absolute decoded image path";
        return false;
    }

    std::error_code pathError;
    const auto pathStatus = std::filesystem::symlink_status(explicitPath, pathError);
    if (pathError) {
        result_.state = GuestImageLoadState::OpenFailed;
        result_.diagnostic = diagnosticWithCode(
                "Could not inspect the explicit decoded image", pathError.value());
        return false;
    }
    if (std::filesystem::is_symlink(pathStatus)) {
        result_.state = GuestImageLoadState::SymlinkRejected;
        result_.diagnostic = "Symbolic links are not accepted as decoded images";
        return false;
    }
    if (!std::filesystem::is_regular_file(pathStatus)) {
        result_.state = GuestImageLoadState::NotRegularFile;
        result_.diagnostic = "Explicit decoded image is not a regular file";
        return false;
    }

    auto* destination = addressSpace.base() + rangeStart;

#if defined(_WIN32)
    ScopedFile file(CreateFileW(
            explicitPath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        result_.state = GuestImageLoadState::OpenFailed;
        result_.diagnostic = diagnosticWithCode(
                "Could not open the explicit decoded image", GetLastError());
        return false;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.get(), &information)) {
        result_.state = GuestImageLoadState::OpenFailed;
        result_.diagnostic = diagnosticWithCode(
                "Could not inspect the opened decoded image", GetLastError());
        return false;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result_.state = GuestImageLoadState::SymlinkRejected;
        result_.diagnostic = "Reparse points are not accepted as decoded images";
        return false;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            || GetFileType(file.get()) != FILE_TYPE_DISK) {
        result_.state = GuestImageLoadState::NotRegularFile;
        result_.diagnostic = "Opened decoded image is not a regular disk file";
        return false;
    }
    result_.observedFileSize =
            (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U)
            | information.nFileSizeLow;
    if (result_.observedFileSize != expectedSize) {
        result_.state = GuestImageLoadState::SizeMismatch;
        result_.diagnostic = "Decoded image size does not match the caller's exact expectation";
        return false;
    }

    while (result_.bytesRead < expectedSize) {
        const auto remaining = expectedSize - result_.bytesRead;
        const auto request = static_cast<DWORD>(std::min(remaining, ReadChunkSize));
        DWORD bytesRead = 0;
        if (!ReadFile(
                    file.get(),
                    destination + result_.bytesRead,
                    request,
                    &bytesRead,
                    nullptr)) {
            scrubPartialImage(destination, result_.bytesRead);
            result_.state = GuestImageLoadState::ReadFailed;
            result_.diagnostic = diagnosticWithCode("Decoded image read failed", GetLastError());
            return false;
        }
        if (bytesRead == 0) {
            scrubPartialImage(destination, result_.bytesRead);
            result_.state = GuestImageLoadState::ShortRead;
            result_.diagnostic = "Decoded image ended before the expected byte count";
            return false;
        }
        result_.bytesRead += bytesRead;
    }

    LARGE_INTEGER finalSize{};
    if (!GetFileSizeEx(file.get(), &finalSize)) {
        scrubPartialImage(destination, result_.bytesRead);
        result_.state = GuestImageLoadState::ReadFailed;
        result_.diagnostic = diagnosticWithCode(
                "Could not revalidate decoded image size", GetLastError());
        return false;
    }
    result_.observedFileSize = static_cast<std::uint64_t>(finalSize.QuadPart);

#else
    int openFlags = O_RDONLY;
#if defined(O_CLOEXEC)
    openFlags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    openFlags |= O_NOFOLLOW;
#endif
    ScopedFile file(open(explicitPath.c_str(), openFlags));
    if (file.get() < 0) {
        result_.state = errno == ELOOP
                ? GuestImageLoadState::SymlinkRejected
                : GuestImageLoadState::OpenFailed;
        result_.diagnostic = diagnosticWithCode(
                "Could not open the explicit decoded image", static_cast<std::uint64_t>(errno));
        return false;
    }

    struct stat information {};
    if (fstat(file.get(), &information) != 0) {
        result_.state = GuestImageLoadState::OpenFailed;
        result_.diagnostic = diagnosticWithCode(
                "Could not inspect the opened decoded image", static_cast<std::uint64_t>(errno));
        return false;
    }
    if (!S_ISREG(information.st_mode)) {
        result_.state = GuestImageLoadState::NotRegularFile;
        result_.diagnostic = "Opened decoded image is not a regular file";
        return false;
    }
    if (information.st_size < 0) {
        result_.state = GuestImageLoadState::SizeMismatch;
        result_.diagnostic = "Decoded image reported a negative size";
        return false;
    }
    result_.observedFileSize = static_cast<std::uint64_t>(information.st_size);
    if (result_.observedFileSize != expectedSize) {
        result_.state = GuestImageLoadState::SizeMismatch;
        result_.diagnostic = "Decoded image size does not match the caller's exact expectation";
        return false;
    }

    while (result_.bytesRead < expectedSize) {
        const auto remaining = expectedSize - result_.bytesRead;
        const auto request = static_cast<std::size_t>(std::min(remaining, ReadChunkSize));
        const auto readResult = read(
                file.get(), destination + result_.bytesRead, request);
        if (readResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            scrubPartialImage(destination, result_.bytesRead);
            result_.state = GuestImageLoadState::ReadFailed;
            result_.diagnostic = diagnosticWithCode(
                    "Decoded image read failed", static_cast<std::uint64_t>(errno));
            return false;
        }
        if (readResult == 0) {
            scrubPartialImage(destination, result_.bytesRead);
            result_.state = GuestImageLoadState::ShortRead;
            result_.diagnostic = "Decoded image ended before the expected byte count";
            return false;
        }
        result_.bytesRead += static_cast<std::uint64_t>(readResult);
    }

    struct stat finalInformation {};
    if (fstat(file.get(), &finalInformation) != 0 || finalInformation.st_size < 0) {
        const auto error = errno;
        scrubPartialImage(destination, result_.bytesRead);
        result_.state = GuestImageLoadState::ReadFailed;
        result_.diagnostic = diagnosticWithCode(
                "Could not revalidate decoded image size", static_cast<std::uint64_t>(error));
        return false;
    }
    result_.observedFileSize = static_cast<std::uint64_t>(finalInformation.st_size);
#endif

    if (result_.observedFileSize != expectedSize) {
        scrubPartialImage(destination, result_.bytesRead);
        result_.state = GuestImageLoadState::SizeMismatch;
        result_.diagnostic = "Decoded image size changed while it was being read";
        return false;
    }

    result_.bytesLoaded = result_.bytesRead;
    result_.state = GuestImageLoadState::Loaded;
    result_.diagnostic = "Explicit decoded image loaded; guest execution remains disabled";
    return true;
}

void GuestImageLoader::reset() {
    result_ = {};
}

const GuestImageLoadResult& GuestImageLoader::result() const noexcept {
    return result_;
}

} // namespace recomp::bootstrap
