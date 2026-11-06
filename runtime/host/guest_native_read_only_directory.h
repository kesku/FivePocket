#pragma once

#include "guest_file_metadata.h"
#include "guest_read_only_file.h"

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace recomp::bootstrap {
// A read-only view of an explicitly selected native directory. Missing files
// are determined by the live filesystem, not a synthetic empty catalog. Every
// guest component is resolved under a directory descriptor without symlinks.
class GuestNativeReadOnlyDirectory final {
    struct Descriptor {
        explicit Descriptor(int value) : value(value) {}
        ~Descriptor() { if (value >= 0) ::close(value); }
        Descriptor(const Descriptor&) = delete;
        Descriptor& operator=(const Descriptor&) = delete;
        int release() { const int result = value; value = -1; return result; }
        int value;
    };
    class File final : public GuestReadOnlyFile {
    public:
        File(int fd, std::uint64_t bytes) : descriptor_(fd), bytes_(bytes) {}
        std::uint64_t size() const noexcept override { return bytes_; }
        bool read(std::uint64_t offset, std::uint8_t* out, std::size_t bytes) const noexcept override {
            if (offset > bytes_ || bytes > bytes_ - offset || bytes > 1024 * 1024 || (bytes && !out)) return false;
            struct stat info{};
            if (::fstat(descriptor_.value, &info) || info.st_size < 0 || std::uint64_t(info.st_size) != bytes_) return false;
            std::size_t done = 0;
            while (done < bytes) {
                const auto count = ::pread(descriptor_.value, out + done, bytes - done, static_cast<off_t>(offset + done));
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) return false;
                done += count;
            }
            return true;
        }
    private:
        Descriptor descriptor_;
        std::uint64_t bytes_;
    };
public:
    GuestNativeReadOnlyDirectory(std::string_view mount, const char* nativePath)
        : root_(nativePath && nativePath[0] == '/'
                ? ::open(nativePath, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1) {
        const auto normalized = normalizeGuestFilePath(mount);
        if (normalized && normalized->back() == '\\' && normalized->find(':') + 2 == normalized->size())
            mount_ = *normalized;
    }
    bool ready() const noexcept { return root_.value >= 0 && !mount_.empty(); }
    bool owns(std::string_view path) const {
        const auto normalized = normalizeGuestFilePath(path);
        return ready() && normalized && normalized->compare(0, mount_.size(), mount_) == 0;
    }
    GuestFileQueryResult query(std::string_view path) const {
        bool missing = false;
        Descriptor file(resolve(path, missing));
        if (file.value < 0) return {missing ? GuestFileQueryState::NotFound : GuestFileQueryState::Unavailable, {}};
        struct stat info{};
        if (::fstat(file.value, &info) || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) || info.st_size < 0) return {};
        GuestFileMetadata metadata;
        // Creation time isn't portable; zero means unavailable. Expose the
        // actual access/write/change times and the read-only mount attributes.
        const auto fileTime = [](time_t seconds) -> std::uint64_t {
            constexpr std::int64_t Epoch = 11644473600LL;
            if (seconds < -Epoch || seconds > INT64_MAX / 10000000 - Epoch) return 0;
            return static_cast<std::uint64_t>(seconds + Epoch) * 10000000;
        };
        metadata.values[1] = fileTime(info.st_atime);
        metadata.values[2] = fileTime(info.st_mtime);
        metadata.values[3] = fileTime(info.st_ctime);
        metadata.attributes = S_ISDIR(info.st_mode) ? 0x11 : 1;
        if (S_ISREG(info.st_mode)) {
            metadata.values[5] = info.st_size;
            metadata.values[4] = info.st_blocks > 0 ? std::uint64_t(info.st_blocks) * 512 : 0;
            if (metadata.values[4] < metadata.values[5]) metadata.values[4] = metadata.values[5];
        }
        return {GuestFileQueryState::Found, metadata};
    }
    std::shared_ptr<const GuestReadOnlyFile> open(std::string_view path) const {
        bool missing = false;
        Descriptor file(resolve(path, missing));
        struct stat info{};
        if (file.value < 0 || ::fstat(file.value, &info) || !S_ISREG(info.st_mode) || info.st_size < 0
                || std::uint64_t(info.st_size) > (8ULL << 30)) return {};
        auto result = std::make_shared<File>(file.value, info.st_size);
        file.release();
        return result;
    }
private:
    int resolve(std::string_view path, bool& missing) const {
        const auto normalized = normalizeGuestFilePath(path);
        if (!ready() || !normalized || normalized->compare(0, mount_.size(), mount_) != 0) return -1;
        Descriptor current(::openat(root_.value, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (current.value < 0) return -1;
        std::size_t at = mount_.size();
        while (at < normalized->size()) {
            const auto end = normalized->find('\\', at);
            const auto part = normalized->substr(at, end == std::string::npos ? end : end - at);
            // A fresh open description avoids sharing readdir offsets with a
            // previous query. Ambiguous case aliases are rejected on Linux.
            const int scanFd = ::openat(current.value, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (scanFd < 0) return -1;
            DIR* scan = ::fdopendir(scanFd);
            if (!scan) { ::close(scanFd); return -1; }
            std::string match;
            bool ambiguous = false;
            errno = 0;
            while (const auto* entry = ::readdir(scan)) {
                std::string folded(entry->d_name);
                for (auto& c : folded) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (folded == part) {
                    if (!match.empty()) ambiguous = true;
                    match = entry->d_name;
                }
            }
            const int scanError = errno;
            ::closedir(scan);
            if (scanError || ambiguous) return -1;
            if (match.empty()) { missing = true; return -1; }
            const int next = ::openat(current.value, match.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK
                    | (end == std::string::npos ? 0 : O_DIRECTORY));
            if (next < 0) { missing = errno == ENOENT; return -1; }
            ::close(current.value);
            current.value = next;
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return current.release();
    }
    Descriptor root_;
    std::string mount_;
};
} // namespace recomp::bootstrap
