#pragma once

// Host-backed storage for Xbox 360 XCONTENT (saved-game) packages.
//
// Scope: one app-private directory tree on the host. This module owns the
// persistence and the NT/XAM result codes of content and file operations; it
// does NOT know about guest memory, user sign-in, root-name mounts, overlapped
// completion or kernel handles. The import registry maps those onto it.
//
// Layout below `root`:
//   c/<xuid:16 hex>/<device:8 hex>/<type:8 hex>/<hex(lowercase file name)>/
//       meta.bin   record + creator, checksummed; its presence IS existence
//       meta.tmp   staging copy of meta.bin (never read)
//       thumb.bin  optional thumbnail (thumb.tmp while staging)
//       data/      the content's file namespace (names stored lowercased)
//   c/.../<hex>.new/   replacement of the content <hex>, not committed yet; after
//                      the commit exchange (for an instant) the old content
//   c/.../<hex>.old/   old content set aside by a commit made with two renames
//                      (only where the host refuses an atomic exchange)
//   t/             trash: whole content trees are renamed here, then deleted
//
// Durability: content creation writes meta.tmp, fsyncs it, renames it to
// meta.bin and fsyncs the directory. A crash before the rename leaves a tree
// without meta.bin, which every query treats as absent and the next create
// wipes. Removing content renames the tree into t/ first.
//
// Replacing present content (CREATE_ALWAYS / TRUNCATE_EXISTING) never touches
// the old tree: the new content is built in <hex>.new, and this instance
// serves the key from there, so callers see exactly what they would see if
// the old content had been deleted. closeContent() (the registry calls it
// from XamContentClose) commits when the open that created the replacement
// is closed and no file of it is open (other opens of the same content do
// not hold it back): fsync the new tree, then atomically exchange <hex>.new
// and <hex> (the commit point; <hex> is the new content from that instant
// and <hex>.new the old one), then delete <hex>.new. Where the host refuses
// the exchange: rename <hex> to <hex>.old, rename <hex>.new to <hex>, delete
// <hex>.old. A commit that could not be made then is tried again at the next
// call of the store.
// The first use of a store repairs an interrupted replace and never promotes
// a staged tree:
//   <hex>.old  without a valid meta.bin: deleted; with nothing valid under
//              <hex>: renamed back to <hex>; next to valid content: kept
//              aside as <hex>.kept-old.<n> (never deleted).
//   <hex>.new  with a valid meta.bin: kept aside as <hex>.kept-new.<n>, always
//              (it may be a complete save that was never committed: the
//              store cannot tell complete from torn). Without a valid
//              meta.bin: deleted (never a content).
// A staged tree found at a replace is kept aside too (the replace is refused
// if that fails). Nothing reads or counts a kept tree; after a commit of the
// slot, which makes a newer complete save durable, all but the newest
// KeptPerSlot kept trees of the slot are deleted (pruneKept). A content close
// commits only the staged tree its own open created (same meta.bin file), and
// the two-rename commit keeps a leftover <hex>.old aside instead of deleting it
// (review repairs R1, R2). So a crash at any point leaves the old content or
// the complete new content, and no complete tree is deleted unless a newer
// complete save is durable under the name. A replacement that had a storage
// failure while it was written is never committed: it stays visible to this
// process only.
//
// The safe replace above is behind a BOOT switch (save-replace-merge), read
// once per store: <files>/private-trace/diag-save-replace, where <files> is
// $RECOMP_SAVES_DIR or the app's files directory (as in the registry's
// savesStateFor). Content (one word): "on" (safe replace), "off" (the v465
// store: the old content is deleted when the replace starts, no start-up
// repair), "sandbox" (safe replace only for a store whose root lies below
// <files>/saves-sandbox/, i.e. under diag-save-sandbox; "off" elsewhere),
// "sandbox-hold" (as "sandbox", and the first staged create of the process
// then logs SAVES_REPLACE_HOLD and pauses HoldSeconds: a force-stop timed off
// that line falls where the v465 store has no save; for a device test only).
// Absent: RECOMP_SAVE_REPLACE_DEFAULT (off unless built with =1). Anything
// else, or unreadable: "off". Logged once: SAVES_REPLACE_MODE.
// In "off" every call behaves as in v465; closeContent() still logs
// SAVES_CONTENT_CLOSE (commit=0), so the content close can be observed.
//
// Not thread-safe across instances: use exactly one SaveStore per root. The
// instance itself serialises its public methods with a mutex.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "recomp_app_dir.h" // data-paths: the app's files directory.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <cstdio>          // renamex_np (atomic exchange; host tests)
#elif defined(__linux__)
#include <sys/syscall.h>   // SYS_renameat2 (atomic exchange; no libc wrapper before Android API 30)
#endif
#if __has_include(<android/log.h>)   // (the host tests' stand-in captures the lines too)
#include <android/log.h>   // SAVES_CONTENT_CLOSE / SAVES_REPLACE_RECOVERED / SAVES_REPLACE_MODE (tag RecompSaves)
#define RECOMP_SAVE_STORE_LOG 1
#endif

namespace recomp::bootstrap::saves {

static_assert(sizeof(off_t) >= 8, "SaveStore requires 64-bit file offsets");

// XCONTENT_DATA exactly as the registry copies it (GuestContentCatalog::RecordBytes):
// +0 BE u32 device id, +4 BE u32 content type, +8 UTF-16BE display name[128],
// +264 char file name[42], +306 two pad bytes.
inline constexpr std::size_t RecordBytes = 0x134;
inline constexpr std::size_t DisplayNameOffset = 8;
inline constexpr std::size_t DisplayNameUnits = 128;
inline constexpr std::size_t FileNameOffset = 264;
inline constexpr std::size_t FileNameBytes = 42;
static_assert(FileNameOffset == DisplayNameOffset + DisplayNameUnits * 2, "XCONTENT_DATA layout");
static_assert(FileNameOffset + FileNameBytes + 2 == RecordBytes, "XCONTENT_DATA layout");
using Record = std::array<std::uint8_t, RecordBytes>;

inline constexpr std::uint64_t DefaultQuotaBytes = 512ULL * 1024 * 1024;
inline constexpr std::size_t MaximumThumbnailBytes = 64 * 1024;
inline constexpr std::size_t MaximumPathDepth = 8;
inline constexpr std::size_t MaximumNameBytes = 255;
inline constexpr std::size_t MaximumPathBytes = 1024;
inline constexpr std::size_t MaximumOpenHandles = 256;

// Win32 codes returned by the content-level (XAM) operations.
namespace win32 {
inline constexpr std::uint32_t Success = 0;
inline constexpr std::uint32_t FileNotFound = 2;      // remove / thumbnail of absent content
inline constexpr std::uint32_t PathNotFound = 3;      // OPEN_EXISTING / TRUNCATE_EXISTING of absent content
inline constexpr std::uint32_t AccessDenied = 5;      // host I/O failure, or content has open file handles
inline constexpr std::uint32_t NotEnoughMemory = 8;
inline constexpr std::uint32_t InvalidParameter = 87;
inline constexpr std::uint32_t DiskFull = 112;
inline constexpr std::uint32_t AlreadyExists = 183;   // CREATE_NEW of present content
} // namespace win32

// NTSTATUS codes returned by the file-level operations.
namespace nt {
inline constexpr std::uint32_t Success = 0;
inline constexpr std::uint32_t Unsuccessful = 0xC0000001;
inline constexpr std::uint32_t InvalidHandle = 0xC0000008;
inline constexpr std::uint32_t InvalidParameter = 0xC000000D;
inline constexpr std::uint32_t InvalidDeviceRequest = 0xC0000010;
inline constexpr std::uint32_t NoMemory = 0xC0000017;
inline constexpr std::uint32_t AccessDenied = 0xC0000022;
inline constexpr std::uint32_t ObjectNameInvalid = 0xC0000033;
inline constexpr std::uint32_t ObjectNameNotFound = 0xC0000034;
inline constexpr std::uint32_t ObjectNameCollision = 0xC0000035;
inline constexpr std::uint32_t ObjectPathNotFound = 0xC000003A;
inline constexpr std::uint32_t SharingViolation = 0xC0000043;
inline constexpr std::uint32_t DiskFull = 0xC000007F;
inline constexpr std::uint32_t FileIsADirectory = 0xC00000BA;
inline constexpr std::uint32_t DirectoryNotEmpty = 0xC0000101;
inline constexpr std::uint32_t NotADirectory = 0xC0000103;
inline constexpr std::uint32_t TooManyOpenedFiles = 0xC000011F;
inline constexpr std::uint32_t CannotDelete = 0xC0000121;
} // namespace nt

// XamContentCreateEx disposition: (flags & 0xF).
namespace content_disposition {
inline constexpr std::uint32_t CreateNew = 1;
inline constexpr std::uint32_t CreateAlways = 2;
inline constexpr std::uint32_t OpenExisting = 3;
inline constexpr std::uint32_t OpenAlways = 4;
inline constexpr std::uint32_t TruncateExisting = 5;
} // namespace content_disposition

// NtCreateFile CreateDisposition and IO_STATUS_BLOCK.Information.
namespace nt_disposition {
inline constexpr std::uint32_t Supersede = 0;
inline constexpr std::uint32_t Open = 1;
inline constexpr std::uint32_t Create = 2;
inline constexpr std::uint32_t OpenIf = 3;
inline constexpr std::uint32_t Overwrite = 4;
inline constexpr std::uint32_t OverwriteIf = 5;
} // namespace nt_disposition
namespace nt_information {
inline constexpr std::uint32_t Superseded = 0;
inline constexpr std::uint32_t Opened = 1;
inline constexpr std::uint32_t Created = 2;
inline constexpr std::uint32_t Overwritten = 3;
inline constexpr std::uint32_t Exists = 4;
inline constexpr std::uint32_t DoesNotExist = 5;
} // namespace nt_information

struct ContentKey {
    std::uint64_t xuid{0};
    std::uint32_t deviceId{0};
    std::uint32_t contentType{0};
    std::string fileName;      // as supplied; identity is case-insensitive
};

// closeContent() results (diagnostic; the guest never sees them).
namespace content_close {
inline constexpr std::uint32_t NothingPending = 0;  // the content was not a replacement
inline constexpr std::uint32_t Committed = 1;       // the replacement is now the content (atomic exchange); the old one is gone
inline constexpr std::uint32_t StillOpen = 2;       // not the open that created the replacement, or a file of it is still open
inline constexpr std::uint32_t HeldBack = 3;        // a storage failure hit the replacement: the old content is kept
inline constexpr std::uint32_t CommitFailed = 4;    // host I/O failure while committing: the old content is kept
inline constexpr std::uint32_t CommittedByRenames = 5;   // as Committed, but the host refused the exchange: two renames
} // namespace content_close

struct ContentOpen {
    std::uint32_t error{win32::InvalidParameter};
    bool created{false};       // XAM disposition out: created ? 1 (create) : 2 (open)
};

// NtCreateFile FILE_DIRECTORY_FILE / FILE_NON_DIRECTORY_FILE / neither.
enum class FileKind : std::uint8_t { Any, Directory, NonDirectory };

struct FileOpen {
    std::uint32_t status{nt::Unsuccessful};
    std::uint32_t information{nt_information::DoesNotExist};
    int fd{-1};                // host descriptor owned by the store; -1 on failure
    bool directory{false};     // fd is an O_DIRECTORY descriptor
};

struct IoResult {
    std::uint32_t status{nt::Success};
    std::size_t bytes{0};
};

struct FileInfo {
    std::uint64_t size{0};
    bool directory{false};
    // Windows FILETIME (100 ns since 1601-01-01 UTC). POSIX has no portable
    // birth time, so creationTime reports the last-write time.
    std::uint64_t creationTime{0};
    std::uint64_t lastAccessTime{0};
    std::uint64_t lastWriteTime{0};
    std::uint64_t changeTime{0};
};

struct DirectoryEntry {
    std::string name;          // lowercased as stored
    FileInfo info;
};

// Content file names: 1..42 printable ASCII bytes, no '\\', '/', ':' or "..".
inline bool isValidContentFileName(const std::string& name) noexcept {
    if (name.empty() || name.size() > FileNameBytes || name == "." || name.find("..") != std::string::npos) {
        return false;
    }
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E || c == '\\' || c == '/' || c == ':') { return false; }
    }
    return true;
}

inline bool isValidContentKey(const ContentKey& key) noexcept {
    return key.contentType != 0 && isValidContentFileName(key.fileName);
}

namespace detail {

inline std::uint32_t loadBe32(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}
inline std::uint64_t loadBe64(const std::uint8_t* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) { value = (value << 8) | p[i]; }
    return value;
}
inline void storeBe32(std::uint8_t* p, std::uint32_t value) noexcept {
    for (int i = 3; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(value & 0xFF); value >>= 8; }
}
inline void storeBe64(std::uint8_t* p, std::uint64_t value) noexcept {
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(value & 0xFF); value >>= 8; }
}

inline std::string toLowerAscii(std::string text) {
    for (auto& c : text) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + ('a' - 'A')); }
    }
    return text;
}

inline std::string hexNumber(std::uint64_t value, int digits) {
    static constexpr char Digits[] = "0123456789abcdef";
    std::string out(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i) { out[static_cast<std::size_t>(i)] = Digits[value & 0xF]; value >>= 4; }
    return out;
}
inline std::string hexEncode(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) { out += hexNumber(static_cast<unsigned char>(c), 2); }
    return out;
}
inline int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    return -1;   // uppercase is not a canonical encoding
}
inline std::optional<std::uint64_t> parseHexNumber(const std::string& text, std::size_t digits) {
    if (text.size() != digits) { return std::nullopt; }
    std::uint64_t value = 0;
    for (const char c : text) {
        const int d = hexDigit(c);
        if (d < 0) { return std::nullopt; }
        value = (value << 4) | static_cast<std::uint64_t>(d);
    }
    return value;
}
inline std::optional<std::string> hexDecode(const std::string& text) {
    if (text.empty() || text.size() % 2 != 0) { return std::nullopt; }
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = hexDigit(text[i]), lo = hexDigit(text[i + 1]);
        if (hi < 0 || lo < 0) { return std::nullopt; }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

inline std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (std::size_t i = 0; i < size; ++i) { hash = (hash ^ data[i]) * 0x100000001B3ULL; }
    return hash;
}

inline std::uint64_t saturatingMultiply(std::uint64_t a, std::uint64_t b) noexcept {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) { return std::numeric_limits<std::uint64_t>::max(); }
    return a * b;
}
inline std::uint64_t saturatingSubtract(std::uint64_t a, std::uint64_t b) noexcept { return a > b ? a - b : 0; }

inline struct timespec accessTimeOf(const struct stat& st) noexcept {
#if defined(__APPLE__)
    return st.st_atimespec;
#else
    return st.st_atim;
#endif
}
inline struct timespec writeTimeOf(const struct stat& st) noexcept {
#if defined(__APPLE__)
    return st.st_mtimespec;
#else
    return st.st_mtim;
#endif
}
inline struct timespec changeTimeOf(const struct stat& st) noexcept {
#if defined(__APPLE__)
    return st.st_ctimespec;
#else
    return st.st_ctim;
#endif
}
inline std::uint64_t fileTimeFrom(const struct timespec& ts) noexcept {
    constexpr std::int64_t UnixEpochSeconds = 11644473600LL;  // 1601 -> 1970
    if (ts.tv_sec < -UnixEpochSeconds) { return 0; }
    const auto seconds = static_cast<std::uint64_t>(static_cast<std::int64_t>(ts.tv_sec) + UnixEpochSeconds);
    const auto hundreds = static_cast<std::uint64_t>(ts.tv_nsec < 0 ? 0 : ts.tv_nsec / 100);
    if (seconds > (std::numeric_limits<std::uint64_t>::max() - hundreds) / 10000000ULL) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return seconds * 10000000ULL + hundreds;
}
inline FileInfo infoFromStat(const struct stat& st) noexcept {
    FileInfo info;
    info.directory = S_ISDIR(st.st_mode);
    info.size = info.directory || st.st_size < 0 ? 0 : static_cast<std::uint64_t>(st.st_size);
    info.lastWriteTime = fileTimeFrom(writeTimeOf(st));
    info.creationTime = info.lastWriteTime;
    info.lastAccessTime = fileTimeFrom(accessTimeOf(st));
    info.changeTime = fileTimeFrom(changeTimeOf(st));
    return info;
}

inline bool isRealDirectory(const std::string& path) noexcept {
    struct stat st{};
    return ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
// mkdir 0700; an existing real directory (not a symlink) is also success.
inline bool makeDirectory(const std::string& path) noexcept {
    if (::mkdir(path.c_str(), 0700) == 0) { return true; }
    const int error = errno;
    if (error == EEXIST && isRealDirectory(path)) { return true; }
    errno = error == EEXIST ? ENOTDIR : error;
    return false;
}
inline bool makeDirectories(const std::string& path) {
    if (path.empty()) { errno = EINVAL; return false; }
    for (std::size_t at = path.find('/', 1); at != std::string::npos; at = path.find('/', at + 1)) {
        const auto prefix = path.substr(0, at);
        struct stat st{};
        if (::stat(prefix.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) { continue; }
        if (!makeDirectory(prefix)) { return false; }
    }
    // The caller-supplied root may itself be a symlinked directory; every
    // directory the store creates below it is checked with lstat instead.
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) { return true; }
    return makeDirectory(path);
}
inline void syncDirectory(const std::string& path) noexcept {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { (void)::fsync(fd); (void)::close(fd); }
}
inline std::string parentOf(const std::string& path) {
    const auto at = path.find_last_of('/');
    return at == std::string::npos || at == 0 ? std::string("/") : path.substr(0, at);
}
// Sorted child names, without "." and "..". Empty if unreadable.
inline std::vector<std::string> directoryNames(const std::string& path) {
    std::vector<std::string> names;
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) { return names; }
    try {
        while (const dirent* entry = ::readdir(dir)) {
            const std::string name(entry->d_name);
            if (name != "." && name != "..") { names.push_back(name); }
        }
    } catch (...) { ::closedir(dir); throw; }
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}
inline constexpr int MaximumTreeDepth = 64;
// Deletes a tree without following symlinks. Missing path is success.
inline bool removeTree(const std::string& path, int depth = 0) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) { return errno == ENOENT; }
    if (!S_ISDIR(st.st_mode)) { return ::unlink(path.c_str()) == 0 || errno == ENOENT; }
    if (depth > MaximumTreeDepth) { return false; }
    bool ok = true;
    for (const auto& name : directoryNames(path)) { ok = removeTree(path + "/" + name, depth + 1) && ok; }
    return (::rmdir(path.c_str()) == 0 || errno == ENOENT) && ok;
}
// Sum of logical sizes of regular files below path (symlinks are not followed).
inline std::uint64_t treeBytes(const std::string& path, int depth = 0) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) { return 0; }
    if (S_ISREG(st.st_mode)) { return st.st_size < 0 ? 0 : static_cast<std::uint64_t>(st.st_size); }
    if (!S_ISDIR(st.st_mode) || depth > MaximumTreeDepth) { return 0; }
    std::uint64_t total = 0;
    for (const auto& name : directoryNames(path)) {
        const auto bytes = treeBytes(path + "/" + name, depth + 1);
        total = bytes > std::numeric_limits<std::uint64_t>::max() - total ? std::numeric_limits<std::uint64_t>::max() : total + bytes;
    }
    return total;
}

inline constexpr std::size_t MaximumIoChunk = std::size_t{1} << 30;

inline int writeAllAt(int fd, const std::uint8_t* data, std::size_t size, std::uint64_t offset, std::size_t& done) noexcept {
    done = 0;
    while (done < size) {
        const auto chunk = std::min(size - done, MaximumIoChunk);
        const auto written = ::pwrite(fd, data + done, chunk, static_cast<off_t>(offset + done));
        if (written < 0) {
            if (errno == EINTR) { continue; }
            return errno;
        }
        if (written == 0) { return ENOSPC; }
        done += static_cast<std::size_t>(written);
    }
    return 0;
}
inline int readAllAt(int fd, std::uint8_t* data, std::size_t size, std::uint64_t offset, std::size_t& done) noexcept {
    done = 0;
    while (done < size) {
        const auto chunk = std::min(size - done, MaximumIoChunk);
        const auto got = ::pread(fd, data + done, chunk, static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) { continue; }
            return errno;
        }
        if (got == 0) { break; }   // end of file
        done += static_cast<std::size_t>(got);
    }
    return 0;
}

// Write tmp, fsync, rename over final, fsync the directory. Returns errno or 0.
inline int writeFileAtomic(const std::string& directory, const char* tempName, const char* finalName,
                           const std::uint8_t* data, std::size_t size) {
    const auto temp = directory + "/" + tempName;
    const auto target = directory + "/" + finalName;
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { return errno; }
    std::size_t done = 0;
    int error = writeAllAt(fd, data, size, 0, done);
    if (error == 0 && ::fsync(fd) != 0) { error = errno; }
    if (::close(fd) != 0 && error == 0 && errno != EINTR) { error = errno; }
    if (error == 0 && ::rename(temp.c_str(), target.c_str()) != 0) { error = errno; }
    if (error != 0) { (void)::unlink(temp.c_str()); return error; }
    syncDirectory(directory);
    return 0;
}

inline std::uint32_t win32FromErrno(int error) noexcept {
    switch (error) {
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
        return win32::DiskFull;
    case ENOMEM: return win32::NotEnoughMemory;
    default: return win32::AccessDenied;
    }
}
inline std::uint32_t ntFromErrno(int error) noexcept {
    switch (error) {
    case ENOENT: return nt::ObjectNameNotFound;
    case ENOTDIR: return nt::ObjectPathNotFound;
    case EEXIST: return nt::ObjectNameCollision;
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
    case EFBIG:
        return nt::DiskFull;
    case EISDIR: return nt::FileIsADirectory;
    case ENOTEMPTY: return nt::DirectoryNotEmpty;
    case EACCES: case EPERM: case EROFS: case ELOOP: return nt::AccessDenied;
    case ENAMETOOLONG: return nt::ObjectNameInvalid;
    case EMFILE: case ENFILE: return nt::TooManyOpenedFiles;
    case ENOMEM: return nt::NoMemory;
    case EBADF: return nt::InvalidHandle;
    default: return nt::Unsuccessful;
    }
}

// Content-relative guest path -> lowercased components. Accepts '\\' and '/'
// separators and one trailing separator; "" is the content root. Mirrors the
// registry's normalizeGuestFilePath component rules.
inline std::optional<std::vector<std::string>> parseContentPath(const std::string& path) {
    if (path.size() > MaximumPathBytes) { return std::nullopt; }
    std::vector<std::string> parts;
    std::string current;
    const auto accept = [&]() {
        if (current.empty() || current == "." || current == ".." || current.back() == '.' || current.back() == ' '
                || current.size() > MaximumNameBytes || parts.size() == MaximumPathDepth) {
            return false;
        }
        parts.push_back(current);
        current.clear();
        return true;
    };
    std::size_t end = path.size();
    if (end > 0 && (path[end - 1] == '\\' || path[end - 1] == '/')) {
        if (--end == 0) { return std::nullopt; }   // a lone separator is an absolute path
    }
    for (std::size_t i = 0; i < end; ++i) {
        char c = path[i];
        const auto u = static_cast<unsigned char>(c);
        if (c == '\\' || c == '/') {
            if (!accept()) { return std::nullopt; }
            continue;
        }
        if (u < 32 || u >= 127 || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == ':') {
            return std::nullopt;
        }
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }   // case-insensitive namespace
        current.push_back(c);
    }
    if (end > 0 && !accept()) { return std::nullopt; }
    return parts;
}

// meta.bin: magic, BE version, BE creator xuid, normalized record, BE FNV-1a-64.
inline constexpr std::array<std::uint8_t, 8> MetaMagic{'G', 'X', 'S', 'A', 'V', 'E', '0', '1'};
inline constexpr std::uint32_t MetaVersion = 1;
inline constexpr std::size_t MetaVersionOffset = 8;
inline constexpr std::size_t MetaCreatorOffset = 12;
inline constexpr std::size_t MetaRecordOffset = 20;
inline constexpr std::size_t MetaChecksumOffset = MetaRecordOffset + RecordBytes;
inline constexpr std::size_t MetaBytes = MetaChecksumOffset + 8;
inline constexpr const char* MetaFile = "meta.bin";
inline constexpr const char* MetaTemp = "meta.tmp";
inline constexpr const char* ThumbFile = "thumb.bin";
inline constexpr const char* ThumbTemp = "thumb.tmp";
inline constexpr const char* DataDirectory = "data";
inline constexpr const char* StagingSuffix = ".new";
inline constexpr const char* RetiredSuffix = ".old";
// Trees start-up found and would not delete (they may hold the only complete
// save): <hex>.kept-old.<n>, <hex>.kept-new.<n>. Nothing reads or deletes them.
inline constexpr const char* KeptRetiredSuffix = ".kept-old";
inline constexpr const char* KeptStagingSuffix = ".kept-new";
// save-replace-merge (after the review): kept trees of one slot that survive a commit of that slot (pruneKept).
inline constexpr std::size_t KeptPerSlot = 2;

// Atomically exchanges two existing paths (both names stay present throughout).
// False with errno if the host cannot or will not do it.
inline bool exchangePaths(const std::string& a, const std::string& b) noexcept {
#if defined(__APPLE__)
    return ::renamex_np(a.c_str(), b.c_str(), RENAME_SWAP) == 0;
#elif defined(__linux__) && defined(SYS_renameat2)
    return ::syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), 2U /* RENAME_EXCHANGE */) == 0;
#else
    (void)a; (void)b; errno = ENOTSUP; return false;
#endif
}

inline bool endsWith(const std::string& text, const char* suffix) noexcept {
    const auto n = std::strlen(suffix);
    return text.size() > n && text.compare(text.size() - n, n, suffix) == 0;
}
// fsyncs every regular file and directory below path (symlinks are not
// followed). False if something could not be opened or synced.
inline bool syncTree(const std::string& path, int depth = 0) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0 || depth > MaximumTreeDepth) { return false; }
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) { return false; }
    bool ok = true;
    if (S_ISDIR(st.st_mode)) {
        for (const auto& name : directoryNames(path)) { ok = syncTree(path + "/" + name, depth + 1) && ok; }
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (S_ISDIR(st.st_mode) ? O_DIRECTORY : 0));
    if (fd < 0) { return false; }
    ok = ::fsync(fd) == 0 && ok;
    (void)::close(fd);
    return ok;
}

} // namespace detail

inline bool sameContent(const ContentKey& a, const ContentKey& b) {
    return a.xuid == b.xuid && a.deviceId == b.deviceId && a.contentType == b.contentType
            && detail::toLowerAscii(a.fileName) == detail::toLowerAscii(b.fileName);
}

// Parses the identity of a guest XCONTENT_DATA (RecordBytes). Bytes after the
// file name's NUL are ignored. Device id is not validated here.
inline std::optional<ContentKey> keyFromRecord(std::uint64_t xuid, const std::uint8_t* record) noexcept {
    if (record == nullptr) { return std::nullopt; }
    try {
        ContentKey key{xuid, detail::loadBe32(record), detail::loadBe32(record + 4), {}};
        for (std::size_t i = 0; i < FileNameBytes && record[FileNameOffset + i] != 0; ++i) {
            key.fileName.push_back(static_cast<char>(record[FileNameOffset + i]));
        }
        if (!isValidContentKey(key)) { return std::nullopt; }
        return key;
    } catch (...) { return std::nullopt; }
}

// The record persisted and enumerated: identity words and the display name up
// to its first NUL unit, the file name up to its first NUL, zero elsewhere.
inline Record normalizeRecord(const std::uint8_t* record) noexcept {
    Record out{};
    std::memcpy(out.data(), record, FileNameOffset);
    bool ended = false;
    for (std::size_t i = 0; i < DisplayNameUnits; ++i) {
        const auto at = DisplayNameOffset + i * 2;
        if (ended) { out[at] = 0; out[at + 1] = 0; continue; }
        ended = out[at] == 0 && out[at + 1] == 0;
    }
    for (std::size_t i = 0; i < FileNameBytes && record[FileNameOffset + i] != 0; ++i) {
        out[FileNameOffset + i] = record[FileNameOffset + i];
    }
    return out;
}

// ---- the boot switch of the safe replace (save-replace-merge) -------------
// FromSwitch: read the switch at the store's first use. Legacy: the v465
// store. Safe: stage, commit at the creating open's close, repair at start-up.
enum class ReplaceMode : std::uint8_t { FromSwitch, Legacy, Safe };
namespace replace_switch {
inline constexpr const char* Relative = "/private-trace/diag-save-replace";
inline constexpr const char* SandboxParent = "/saves-sandbox/";   // save_sandbox.h: <files>/saves-sandbox/<name>/saves
inline constexpr std::size_t MaximumBytes = 64;
inline constexpr unsigned HoldSeconds = 10;   // "sandbox-hold": the pause after the first staged create
#if defined(RECOMP_SAVE_REPLACE_DEFAULT) && RECOMP_SAVE_REPLACE_DEFAULT
inline constexpr bool DefaultSafe = true;
#else
inline constexpr bool DefaultSafe = false;   // first device stage: the v465 behaviour unless the switch says otherwise
#endif
struct Decision {
    ReplaceMode mode{ReplaceMode::Legacy};
    bool hold{false};   // "sandbox-hold" in a sandbox: pause once after the first staged create
    std::string line;   // SAVES_REPLACE_MODE ...
};
// Pure: the mode for a store at storeRoot, given what the switch file held.
inline Decision decide(const std::string& filesRoot, const std::string& storeRoot, bool present, bool readable,
                       const std::string& content) {
    std::vector<std::string> words;
    for (std::size_t at = content.find_first_not_of(" \t\r\n"); at != std::string::npos; at = content.find_first_not_of(" \t\r\n", at)) {
        const auto end = std::min(content.find_first_of(" \t\r\n", at), content.size());
        words.push_back(content.substr(at, end - at));
        at = end;
    }
    const std::string word = words.size() == 1 ? words[0] : std::string();
    const bool extra = words.size() > 1;
    const std::string sandboxPrefix = filesRoot + SandboxParent;
    const bool inSandbox = !filesRoot.empty() && storeRoot.size() > sandboxPrefix.size()
            && storeRoot.compare(0, sandboxPrefix.size(), sandboxPrefix) == 0;
    Decision out;
    std::string source;
    if (!present) { out.mode = DefaultSafe ? ReplaceMode::Safe : ReplaceMode::Legacy; source = "default"; }
    else if (!readable || content.size() > MaximumBytes || extra) { out.mode = ReplaceMode::Legacy; source = "invalid-switch"; }
    else if (word == "on") { out.mode = ReplaceMode::Safe; source = "switch-on"; }
    else if (word == "off") { out.mode = ReplaceMode::Legacy; source = "switch-off"; }
    else if (word == "sandbox") { out.mode = inSandbox ? ReplaceMode::Safe : ReplaceMode::Legacy; source = inSandbox ? "switch-sandbox" : "switch-sandbox-not-in-sandbox"; }
    else if (word == "sandbox-hold") {
        out.mode = inSandbox ? ReplaceMode::Safe : ReplaceMode::Legacy;
        out.hold = inSandbox;
        source = inSandbox ? "switch-sandbox-hold" : "switch-sandbox-hold-not-in-sandbox";
    }
    else { out.mode = ReplaceMode::Legacy; source = "invalid-switch"; }
    out.line = std::string("SAVES_REPLACE_MODE safe=") + (out.mode == ReplaceMode::Safe ? "1" : "0") + " source=" + source
            + " default=" + (DefaultSafe ? "on" : "off") + " sandbox_root=" + (inSandbox ? "1" : "0") + " hold=" + (out.hold ? "1" : "0")
            + " root=" + storeRoot;
    return out;
}
// Reads <filesRoot>/private-trace/diag-save-replace (read-only) and decides.
[[gnu::noinline, gnu::cold]] inline Decision readSwitch(const std::string& filesRoot, const std::string& storeRoot) noexcept {
    try {
        std::string content;
        bool present = true, readable = false;
        const int fd = ::open((filesRoot + Relative).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char buffer[MaximumBytes + 1];
            std::size_t filled = 0;
            readable = true;
            while (filled < sizeof buffer) {
                const auto got = ::read(fd, buffer + filled, sizeof buffer - filled);
                if (got < 0) { if (errno == EINTR) { continue; } readable = false; break; }
                if (got == 0) { break; }
                filled += static_cast<std::size_t>(got);
            }
            (void)::close(fd);
            content.assign(buffer, filled);
        } else if (errno == ENOENT || errno == ENOTDIR) {
            present = false;
        }
        return decide(filesRoot, storeRoot, present, readable, content);
    } catch (...) { return Decision{}; }
}
// The files root the registry's savesStateFor uses (guest_import_registry.cpp: RECOMP_SAVES_DIR or the app's files).
inline std::string filesRoot() {
    const char* base = std::getenv("RECOMP_SAVES_DIR");
    return base ? std::string(base) : std::string(RECOMP_APP_FILES);
}
} // namespace replace_switch

class SaveStore final {
public:
    // mode: FromSwitch (the registry) reads diag-save-replace at the first use; tests pass Legacy or Safe.
    explicit SaveStore(std::string root, std::uint64_t quotaBytes = DefaultQuotaBytes,
                       ReplaceMode mode = ReplaceMode::FromSwitch) noexcept
        : root_(std::move(root)), quota_(quotaBytes), mode_(mode) {
        while (root_.size() > 1 && root_.back() == '/') { root_.pop_back(); }
    }
    ~SaveStore() {
        for (const auto& entry : handles_) { (void)::close(entry.first); }
    }
    SaveStore(const SaveStore&) = delete;
    SaveStore& operator=(const SaveStore&) = delete;

    // Host directory of a content (diagnostics/tests). Empty for invalid keys.
    [[nodiscard]] std::string contentPath(const ContentKey& key) const noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return isValidContentKey(key) && !root_.empty() ? contentDirectory(key) : std::string();
        }
        catch (...) { return std::string(); }
    }
    [[nodiscard]] std::uint64_t quotaBytes() const noexcept { return quota_; }
    [[nodiscard]] std::uint64_t usedBytes() noexcept {
        try { std::lock_guard<std::mutex> lock(mutex_); recover(); ensureUsage(); return used_; }
        catch (...) { return 0; }
    }

    // ---- content lifetime (Win32 results) ----------------------------------

    // record is required unless disposition is OPEN_EXISTING; when given it
    // must describe the same content as key. mount names this open (the
    // registry passes the root name it mounts the content under); the same
    // name is given to closeContent() when this open is closed.
    [[nodiscard]] ContentOpen openContent(const ContentKey& key, std::uint32_t disposition,
                                          const std::uint8_t* record, const std::string& mount) noexcept {
        namespace cd = content_disposition;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!isValidContentKey(key) || root_.empty() || disposition < cd::CreateNew || disposition > cd::TruncateExisting) {
                return {win32::InvalidParameter, false};
            }
            if (record == nullptr && disposition != cd::OpenExisting) { return {win32::InvalidParameter, false}; }
            if (record != nullptr) {
                const auto described = keyFromRecord(key.xuid, record);
                if (!described || !sameContent(*described, key)) { return {win32::InvalidParameter, false}; }
            }
            recover();
            const auto live = liveDirectory(key);
            auto dir = contentDirectory(key);
            const bool present = readMeta(dir, key).has_value();
            // The open that creates a staged content owns its commit: closeContent commits when that open is
            // closed, whatever other opens of the same content exist (made before or after, closed or never).
            const auto opened = [&](bool created) {
                if (const auto found = replaces_.find(live); created && found != replaces_.end()) {
                    found->second.failed = false;   // a fresh staged content: nothing failed in it yet
                    found->second.owner = mount;
                    found->second.closed = false;
                    found->second.key = key;
                    // save-replace-review: remember which tree this open created; another process (a restarted game
                    // while this one still runs) can delete it and stage its own under the same name.
                    if (::lstat((found->second.staging + "/" + detail::MetaFile).c_str(), &found->second.made) != 0) { found->second.failed = true; }
                    if (hold_) { holdOnce(key); }   // "sandbox-hold" (device test): the old content is untouched here
                }
                return ContentOpen{win32::Success, created};
            };
            switch (disposition) {
            case cd::CreateNew:
                if (present) { return {win32::AlreadyExists, false}; }
                break;
            case cd::CreateAlways:
                if (present) {
                    if (const auto error = safe() ? supersede(live, dir) : discard(dir)) { return {error, false}; }
                }
                break;
            case cd::OpenExisting:
                return present ? opened(false) : ContentOpen{win32::PathNotFound, false};
            case cd::OpenAlways:
                if (present) { return opened(false); }
                break;
            case cd::TruncateExisting:
                if (!present) { return {win32::PathNotFound, false}; }
                if (const auto error = safe() ? supersede(live, dir) : discard(dir)) { return {error, false}; }
                break;
            default:
                return {win32::InvalidParameter, false};
            }
            const auto error = createContent(key, dir, record);
            return error == win32::Success ? opened(true) : ContentOpen{error, false};
        } catch (...) { return {win32::NotEnoughMemory, false}; }
    }

    // The caller closed the successful openContent() of key that was named
    // mount (XamContentClose). Commits a replacement if that open created it
    // and no file of it is open; see content_close. Never changes what later
    // calls return: only what survives a crash.
    // noinline + noexcept: the call adds no branch to the (profiled, hot) registry dispatch function.
    [[nodiscard]] __attribute__((noinline)) std::uint32_t closeContent(const ContentKey& key, const std::string& mount) noexcept {
        std::uint32_t result = content_close::CommitFailed;
        [[maybe_unused]] const auto started = std::chrono::steady_clock::now();   // us=: the close's cost under the registry lock
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            result = closeContentLocked(key, mount);
        } catch (...) {}
#if defined(RECOMP_SAVE_STORE_LOG)
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
        __android_log_print(result == content_close::CommitFailed ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "RecompSaves",
                            "SAVES_CONTENT_CLOSE name=%s commit=%u us=%lld (0 no replace, 1 committed by exchange, 2 not the open that created the replacement or a file still open, 3 held back, 4 commit failed, 5 committed by two renames)",
                            key.fileName.c_str(), result, static_cast<long long>(micros));
#endif
        return result;
    }

    [[nodiscard]] bool exists(const ContentKey& key) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            recover();
            return isValidContentKey(key) && !root_.empty() && readMeta(contentDirectory(key), key).has_value();
        } catch (...) { return false; }
    }

    // Stored records ordered by (device id, lowercased file name). deviceId 0
    // is XCONTENTDEVICE_ANY; contentType 0 matches nothing.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> enumerate(std::uint64_t xuid, std::uint32_t deviceId,
                                                                  std::uint32_t contentType) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<std::vector<std::uint8_t>> rows;
            if (contentType == 0 || root_.empty()) { return rows; }
            recover();
            forEachContent([&](const ContentKey& found, const std::string&, const Meta& meta) {
                if (found.xuid != xuid) { return; }
                if (deviceId != 0 && found.deviceId != deviceId) { return; }
                if (found.contentType != contentType) { return; }
                rows.emplace_back(meta.record.begin(), meta.record.end());
            });
            return rows;   // forEachContent visits (xuid, device, type, name) in sorted order
        } catch (...) { return {}; }
    }

    [[nodiscard]] std::uint32_t remove(const ContentKey& key) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!isValidContentKey(key) || root_.empty()) { return win32::InvalidParameter; }
            recover();
            const auto live = liveDirectory(key);
            const auto dir = contentDirectory(key);
            if (!readMeta(dir, key)) { return win32::FileNotFound; }
            if (safe() && dir == live && !hasHandlesIn(dir) && ensureRoot()) {
                // No replace is pending: a tree an earlier commit could not delete must not bring the deleted save
                // back. It goes first: a process that dies between the two steps leaves the save, not an older one.
                for (const char* suffix : {detail::RetiredSuffix, detail::StagingSuffix}) {
                    struct stat st{};
                    if (::lstat((live + suffix).c_str(), &st) == 0) { (void)moveToTrash(live + suffix); }
                }
            }
            return discard(dir);
        } catch (...) { return win32::NotEnoughMemory; }
    }

    [[nodiscard]] std::optional<std::uint64_t> creator(const ContentKey& key) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!isValidContentKey(key) || root_.empty()) { return std::nullopt; }
            recover();
            const auto meta = readMeta(contentDirectory(key), key);
            if (!meta) { return std::nullopt; }
            return meta->creator;
        } catch (...) { return std::nullopt; }
    }

    [[nodiscard]] std::uint32_t setThumbnail(const ContentKey& key, const std::uint8_t* data, std::size_t size) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!isValidContentKey(key) || root_.empty() || size > MaximumThumbnailBytes || (data == nullptr && size != 0)) {
                return win32::InvalidParameter;
            }
            recover();
            const auto dir = contentDirectory(key);
            if (!readMeta(dir, key)) { return win32::FileNotFound; }
            ensureUsage();
            const auto before = detail::treeBytes(dir + "/" + detail::ThumbFile);
            if (size > before && size - before > remaining()) { return win32::DiskFull; }
            const std::uint8_t empty = 0;
            if (const int error = detail::writeFileAtomic(dir, detail::ThumbTemp, detail::ThumbFile,
                                                          size == 0 ? &empty : data, size)) {
                return detail::win32FromErrno(error);
            }
            used_ = detail::saturatingSubtract(used_, before) + size;
            return win32::Success;
        } catch (...) { return win32::NotEnoughMemory; }
    }

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> thumbnail(const ContentKey& key) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!isValidContentKey(key) || root_.empty()) { return std::nullopt; }
            recover();
            const auto dir = contentDirectory(key);
            if (!readMeta(dir, key)) { return std::nullopt; }
            const auto path = dir + "/" + detail::ThumbFile;
            const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) { return std::nullopt; }
            struct stat st{};
            std::optional<std::vector<std::uint8_t>> out;
            if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0
                    && static_cast<std::uint64_t>(st.st_size) <= MaximumThumbnailBytes) {
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(st.st_size));
                std::size_t done = 0;
                if (detail::readAllAt(fd, bytes.data(), bytes.size(), 0, done) == 0 && done == bytes.size()) {
                    out = std::move(bytes);
                }
            }
            (void)::close(fd);
            return out;
        } catch (...) { return std::nullopt; }
    }

    // Device capacity as the guest should see it: host free space and the
    // store quota, whichever is smaller. 0 if the root cannot be created.
    [[nodiscard]] std::uint64_t freeBytes() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            struct statvfs vs{};
            if (!ensureRoot() || ::statvfs(root_.c_str(), &vs) != 0) { return 0; }
            recover();
            ensureUsage();
            const std::uint64_t unit = vs.f_frsize != 0 ? vs.f_frsize : vs.f_bsize;
            return std::min(detail::saturatingMultiply(vs.f_bavail, unit), remaining());
        } catch (...) { return 0; }
    }
    [[nodiscard]] std::uint64_t totalBytes() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            struct statvfs vs{};
            if (!ensureRoot() || ::statvfs(root_.c_str(), &vs) != 0) { return 0; }
            const std::uint64_t unit = vs.f_frsize != 0 ? vs.f_frsize : vs.f_bsize;
            return std::min(detail::saturatingMultiply(vs.f_blocks, unit), quota_);
        } catch (...) { return 0; }
    }

    // ---- files inside a content (NTSTATUS results) --------------------------

    // directory=true is FILE_DIRECTORY_FILE; false is "no directory option".
    [[nodiscard]] FileOpen openFile(const ContentKey& key, const std::string& path, std::uint32_t ntDisposition,
                                    bool write, bool directory) noexcept {
        return openFile(key, path, ntDisposition, write, directory ? FileKind::Directory : FileKind::Any);
    }
    [[nodiscard]] FileOpen openFile(const ContentKey& key, const std::string& path, std::uint32_t ntDisposition,
                                    bool write, FileKind kind) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            recover();
            const auto result = openFileLocked(key, path, ntDisposition, write, kind);
            if (write && isStorageFailure(result.status) && isValidContentKey(key)) { noteFailure(contentDirectory(key)); }
            return result;
        } catch (...) { return {nt::NoMemory, nt_information::DoesNotExist, -1, false}; }
    }

    // Bytes read; 0 bytes with Success at or past end of file (the registry
    // maps that to STATUS_END_OF_FILE when the request was non-empty).
    [[nodiscard]] IoResult read(int fd, std::uint64_t offset, void* dst, std::size_t size) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto* handle = find(fd);
            if (handle == nullptr) { return {nt::InvalidHandle, 0}; }
            if (handle->directory) { return {nt::InvalidDeviceRequest, 0}; }
            if (size == 0) { return {nt::Success, 0}; }
            if (dst == nullptr || offset > MaximumOffset) { return {nt::InvalidParameter, 0}; }
            const auto limit = std::min<std::uint64_t>(size, MaximumOffset - offset);
            std::size_t done = 0;
            const int error = detail::readAllAt(fd, static_cast<std::uint8_t*>(dst), static_cast<std::size_t>(limit), offset, done);
            return {error == 0 ? nt::Success : detail::ntFromErrno(error), done};
        } catch (...) { return {nt::NoMemory, 0}; }
    }

    [[nodiscard]] IoResult write(int fd, std::uint64_t offset, const void* src, std::size_t size) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto* handle = find(fd);
            if (handle == nullptr) { return {nt::InvalidHandle, 0}; }
            if (handle->directory) { return {nt::InvalidDeviceRequest, 0}; }
            if (!handle->write) { return {nt::AccessDenied, 0}; }
            if (size == 0) { return {nt::Success, 0}; }
            if (src == nullptr || offset > MaximumOffset || size > MaximumOffset - offset) { return {nt::InvalidParameter, 0}; }
            ensureUsage();
            struct stat before{};
            if (::fstat(fd, &before) != 0) { return {detail::ntFromErrno(errno), 0}; }
            const auto oldSize = static_cast<std::uint64_t>(before.st_size);
            const auto end = offset + size;
            const auto growth = end > oldSize ? end - oldSize : 0;
            if (growth > remaining()) { noteFailure(handle->contentDir); return {nt::DiskFull, 0}; }
            std::size_t done = 0;
            const int error = detail::writeAllAt(fd, static_cast<const std::uint8_t*>(src), size, offset, done);
            if (error != 0) { noteFailure(handle->contentDir); }
            struct stat after{};
            if (::fstat(fd, &after) == 0 && static_cast<std::uint64_t>(after.st_size) > oldSize) {
                used_ += static_cast<std::uint64_t>(after.st_size) - oldSize;
            }
            return {error == 0 ? nt::Success : detail::ntFromErrno(error), done};
        } catch (...) { return {nt::NoMemory, 0}; }
    }

    [[nodiscard]] std::optional<std::uint64_t> size(int fd) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto* handle = find(fd);
            struct stat st{};
            if (handle == nullptr || handle->directory || ::fstat(fd, &st) != 0 || st.st_size < 0) { return std::nullopt; }
            return static_cast<std::uint64_t>(st.st_size);
        } catch (...) { return std::nullopt; }
    }

    // FileEndOfFileInformation: zero-fills on growth.
    [[nodiscard]] std::uint32_t setSize(int fd, std::uint64_t newSize) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto* handle = find(fd);
            if (handle == nullptr) { return nt::InvalidHandle; }
            if (handle->directory) { return nt::InvalidDeviceRequest; }
            if (!handle->write) { return nt::AccessDenied; }
            if (newSize > MaximumOffset) { return nt::InvalidParameter; }
            ensureUsage();
            struct stat st{};
            if (::fstat(fd, &st) != 0) { return detail::ntFromErrno(errno); }
            const auto oldSize = static_cast<std::uint64_t>(st.st_size);
            if (newSize > oldSize && newSize - oldSize > remaining()) { noteFailure(handle->contentDir); return nt::DiskFull; }
            while (::ftruncate(fd, static_cast<off_t>(newSize)) != 0) {
                if (errno != EINTR) { const int error = errno; noteFailure(handle->contentDir); return detail::ntFromErrno(error); }
            }
            used_ = newSize > oldSize ? used_ + (newSize - oldSize) : detail::saturatingSubtract(used_, oldSize - newSize);
            return nt::Success;
        } catch (...) { return nt::NoMemory; }
    }

    [[nodiscard]] std::uint32_t flush(int fd) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto* handle = find(fd);
            if (handle == nullptr) { return nt::InvalidHandle; }
            if (::fsync(fd) == 0) { return nt::Success; }
            const int error = errno;
            noteFailure(handle->contentDir);
            return detail::ntFromErrno(error);
        } catch (...) { return nt::NoMemory; }
    }

    std::uint32_t close(int fd) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = handles_.find(fd);
            if (found == handles_.end()) { return nt::InvalidHandle; }
            const auto closing = found->second;
            handles_.erase(found);
            if (::close(fd) != 0 && errno != EINTR) {
                const int error = errno;
                if (closing.write) { noteFailure(closing.contentDir); }
                return detail::ntFromErrno(error);
            }
            return nt::Success;
        } catch (...) { return nt::NoMemory; }
    }

    [[nodiscard]] std::optional<FileInfo> stat(const ContentKey& key, const std::string& path) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            recover();
            const auto resolved = resolve(key, path);
            if (resolved.status != nt::Success || !resolved.exists) { return std::nullopt; }
            return detail::infoFromStat(resolved.st);
        } catch (...) { return std::nullopt; }
    }
    [[nodiscard]] std::optional<FileInfo> statHandle(int fd) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            struct stat st{};
            if (find(fd) == nullptr || ::fstat(fd, &st) != 0) { return std::nullopt; }
            return detail::infoFromStat(st);
        } catch (...) { return std::nullopt; }
    }

    // Children of a directory, sorted by (lowercased) name.
    [[nodiscard]] std::optional<std::vector<DirectoryEntry>> list(const ContentKey& key, const std::string& path) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            recover();
            const auto resolved = resolve(key, path);
            if (resolved.status != nt::Success || !resolved.exists || !S_ISDIR(resolved.st.st_mode)) { return std::nullopt; }
            std::vector<DirectoryEntry> entries;
            for (const auto& name : detail::directoryNames(resolved.host)) {
                struct stat st{};
                if (::lstat((resolved.host + "/" + name).c_str(), &st) != 0) { continue; }
                if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) { continue; }
                entries.push_back({name, detail::infoFromStat(st)});
            }
            return entries;
        } catch (...) { return std::nullopt; }
    }

    // Deletes a closed file or an empty directory (the registry performs
    // delete-on-close by closing its handle first).
    [[nodiscard]] std::uint32_t removeFile(const ContentKey& key, const std::string& path) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            recover();
            const auto resolved = resolve(key, path);
            if (resolved.status != nt::Success) { return resolved.status; }
            if (resolved.relative.empty()) { return nt::CannotDelete; }
            if (!resolved.exists) { return nt::ObjectNameNotFound; }
            for (const auto& entry : handles_) {
                const auto& open = entry.second;
                if (open.contentDir == resolved.contentDir
                        && (open.relative == resolved.relative || open.relative.rfind(resolved.relative + "\\", 0) == 0)) {
                    return nt::SharingViolation;
                }
            }
            ensureUsage();
            if (S_ISDIR(resolved.st.st_mode)) {
                if (::rmdir(resolved.host.c_str()) != 0) {
                    return errno == ENOTEMPTY || errno == EEXIST ? nt::DirectoryNotEmpty : detail::ntFromErrno(errno);
                }
            } else {
                if (::unlink(resolved.host.c_str()) != 0) { return detail::ntFromErrno(errno); }
                used_ = detail::saturatingSubtract(used_, static_cast<std::uint64_t>(resolved.st.st_size));
            }
            detail::syncDirectory(resolved.parent);
            return nt::Success;
        } catch (...) { return nt::NoMemory; }
    }

private:
    static constexpr std::uint64_t MaximumOffset = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    struct Meta {
        Record record{};
        std::uint64_t creator{0};
    };
    struct Handle {
        std::string contentDir;
        std::string relative;      // lowercased, '\\'-joined; "" is the content root
        bool write{false};
        bool directory{false};
    };
    struct Resolved {
        std::uint32_t status{nt::Unsuccessful};
        std::string contentDir, parent, host, relative;
        bool exists{false};
        struct stat st{};
    };

    [[nodiscard]] std::string contentsRoot() const { return root_ + "/c"; }
    [[nodiscard]] std::string trashRoot() const { return root_ + "/t"; }
    // A replacement in progress of the content at the map key (its final
    // directory). While the entry exists the key is served from `staging`
    // and the old tree at the final directory is not read or changed.
    struct Replace {
        std::string staging;       // always <hex>.new
        bool failed{false};        // a storage failure hit the staged content: never commit it
        std::string owner{};       // the mount name of the open that created the staged content
        bool closed{false};        // that open was closed: commit as soon as nothing holds it back
        ContentKey key{};          // the content (set when the staged content is created)
        struct stat made{};        // save-replace-review: the staged meta.bin as created (dev, inode, times)
    };

    // Where this instance reads and writes the content of key.
    [[nodiscard]] std::string contentDirectory(const ContentKey& key) const {
        auto live = liveDirectory(key);
        const auto found = replaces_.find(live);
        return found == replaces_.end() ? live : found->second.staging;
    }
    // The content's final directory (its name is the canonical encoding).
    [[nodiscard]] std::string liveDirectory(const ContentKey& key) const {
        return contentsRoot() + "/" + detail::hexNumber(key.xuid, 16) + "/" + detail::hexNumber(key.deviceId, 8) + "/"
                + detail::hexNumber(key.contentType, 8) + "/" + detail::hexEncode(detail::toLowerAscii(key.fileName));
    }
    [[nodiscard]] std::uint64_t remaining() const noexcept { return detail::saturatingSubtract(quota_, used_); }

    [[nodiscard]] const Handle* find(int fd) const {
        const auto found = handles_.find(fd);
        return found == handles_.end() ? nullptr : &found->second;
    }
    [[nodiscard]] bool hasHandlesIn(const std::string& contentDir) const {
        return std::any_of(handles_.begin(), handles_.end(), [&](const auto& entry) { return entry.second.contentDir == contentDir; });
    }

    bool ensureRoot() {
        if (rootReady_) { return true; }
        if (root_.empty() || !detail::makeDirectories(root_) || !detail::makeDirectory(contentsRoot())
                || !detail::makeDirectory(trashRoot())) {
            return false;
        }
        for (const auto& name : detail::directoryNames(trashRoot())) { (void)detail::removeTree(trashRoot() + "/" + name); }
        rootReady_ = true;
        return true;
    }

    void ensureUsage() {
        if (usageKnown_) { return; }
        std::uint64_t total = 0;
        forEachContent([&](const ContentKey&, const std::string& dir, const Meta&) { total += contentUsage(dir); });
        used_ = total;
        usageKnown_ = true;
    }
    [[nodiscard]] static std::uint64_t contentUsage(const std::string& dir) {
        return detail::treeBytes(dir + "/" + detail::DataDirectory) + detail::treeBytes(dir + "/" + detail::ThumbFile);
    }

    [[nodiscard]] static std::optional<Meta> decodeMeta(const std::uint8_t* bytes, const ContentKey& key) {
        if (!std::equal(detail::MetaMagic.begin(), detail::MetaMagic.end(), bytes)) { return std::nullopt; }
        if (detail::loadBe32(bytes + detail::MetaVersionOffset) != detail::MetaVersion) { return std::nullopt; }
        if (detail::loadBe64(bytes + detail::MetaChecksumOffset) != detail::fnv1a64(bytes, detail::MetaChecksumOffset)) { return std::nullopt; }
        const auto described = keyFromRecord(key.xuid, bytes + detail::MetaRecordOffset);
        if (!described || !sameContent(*described, key)) { return std::nullopt; }
        Meta meta;
        std::copy_n(bytes + detail::MetaRecordOffset, RecordBytes, meta.record.begin());
        meta.creator = detail::loadBe64(bytes + detail::MetaCreatorOffset);
        return meta;
    }
    [[nodiscard]] static std::optional<Meta> readMeta(const std::string& dir, const ContentKey& key) {
        const auto path = dir + "/" + detail::MetaFile;
        const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) { return std::nullopt; }
        std::array<std::uint8_t, detail::MetaBytes> bytes{};
        struct stat st{};
        std::size_t done = 0;
        const bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && static_cast<std::uint64_t>(st.st_size) == detail::MetaBytes
                && detail::readAllAt(fd, bytes.data(), bytes.size(), 0, done) == 0 && done == bytes.size();
        (void)::close(fd);
        if (!ok) { return std::nullopt; }
        return decodeMeta(bytes.data(), key);
    }

    // Visits every content with a valid meta.bin in (xuid, device, type,
    // name) order. Directory names must be the canonical encoding.
    template <typename Visitor>
    void forEachContent(Visitor&& visit) const {
        const auto contents = contentsRoot();
        for (const auto& xuidName : detail::directoryNames(contents)) {
            const auto xuid = detail::parseHexNumber(xuidName, 16);
            if (!xuid) { continue; }
            const auto xuidDir = contents + "/" + xuidName;
            for (const auto& deviceName : detail::directoryNames(xuidDir)) {
                const auto device = detail::parseHexNumber(deviceName, 8);
                if (!device) { continue; }
                const auto deviceDir = xuidDir + "/" + deviceName;
                for (const auto& typeName : detail::directoryNames(deviceDir)) {
                    const auto type = detail::parseHexNumber(typeName, 8);
                    if (!type || *type == 0) { continue; }
                    const auto typeDir = deviceDir + "/" + typeName;
                    auto names = detail::directoryNames(typeDir);
                    // After a commit that failed half way the final name can be absent while the key is still
                    // served from its staged tree: keep it in the list.
                    for (const auto& pending : replaces_) {
                        if (detail::parentOf(pending.first) != typeDir) { continue; }
                        const auto base = pending.first.substr(typeDir.size() + 1);
                        if (std::binary_search(names.begin(), names.end(), base)) { continue; }
                        names.push_back(base);
                        std::sort(names.begin(), names.end());
                    }
                    for (const auto& encoded : names) {
                        const auto name = detail::hexDecode(encoded);
                        if (!name || !isValidContentFileName(*name) || detail::toLowerAscii(*name) != *name) { continue; }
                        const ContentKey key{*xuid, static_cast<std::uint32_t>(*device), static_cast<std::uint32_t>(*type), *name};
                        const auto replaced = replaces_.find(typeDir + "/" + encoded);
                        const auto dir = replaced == replaces_.end() ? typeDir + "/" + encoded : replaced->second.staging;
                        if (const auto meta = readMeta(dir, key)) { visit(key, dir, *meta); }
                    }
                }
            }
        }
    }

    // Moves a tree into trash atomically, then deletes it. Returns errno.
    int moveToTrash(const std::string& path) {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto target = trashRoot() + "/" + std::to_string(::getpid()) + "-" + std::to_string(++trashCounter_);
            if (::rename(path.c_str(), target.c_str()) == 0) {
                detail::syncDirectory(detail::parentOf(path));
                (void)detail::removeTree(target);
                return 0;
            }
            if (errno != EEXIST && errno != ENOTEMPTY) { return errno; }
        }
        return EEXIST;
    }

    // Removes present content (its meta, thumbnail and every file).
    std::uint32_t discard(const std::string& dir) {
        if (hasHandlesIn(dir)) { return win32::AccessDenied; }
        if (!ensureRoot()) { return detail::win32FromErrno(errno); }
        ensureUsage();
        const auto bytes = contentUsage(dir);
        if (const int error = moveToTrash(dir)) { return error == ENOENT ? win32::FileNotFound : detail::win32FromErrno(error); }
        used_ = detail::saturatingSubtract(used_, bytes);
        return win32::Success;
    }

    // Present content at dir is to be replaced by a new, empty one. v459
    // deleted the tree here. Now a first replace leaves the old tree in place
    // and redirects the key (and dir) to the staging directory; replacing a
    // replacement that is still uncommitted deletes only that replacement.
    // Results and usage accounting are those of discard().
    std::uint32_t supersede(const std::string& live, std::string& dir) {
        if (dir != live) { return discard(dir); }
        if (hasHandlesIn(dir)) { return win32::AccessDenied; }
        if (!ensureRoot()) { return detail::win32FromErrno(errno); }
        ensureUsage();
        // The staging name must be free before the key is redirected to it: a tree found there was not made by
        // this replace (a leftover that could not be deleted) and must never be served or committed.
        const auto staging = live + detail::StagingSuffix;
        struct stat st{};
        if (::lstat(staging.c_str(), &st) == 0) {
            // save-replace-merge (after the review): it may be a complete save (start-up could not set it aside, or
            // another process staged it): keep it aside; if that fails the replace is refused (as v465 when the old
            // content cannot be discarded) and nothing is deleted.
            if (detail::isRealDirectory(staging)) {
                keepAside(staging, live + detail::KeptStagingSuffix, staging.substr(detail::parentOf(staging).size() + 1),
                          "kept: a staged tree found at a replace (not made by this replace; restore or delete by hand)");
                if (::lstat(staging.c_str(), &st) == 0) { return win32::AccessDenied; }
            } else if (const int error = moveToTrash(staging); error != 0 && error != ENOENT) { return detail::win32FromErrno(error); }   // a file: never a content
        }
        const auto bytes = contentUsage(dir);
        dir = staging;
        replaces_.emplace(live, Replace{dir, true});   // not committable until createContent succeeded (openContent clears it)
        used_ = detail::saturatingSubtract(used_, bytes);
        return win32::Success;
    }

    std::uint32_t closeContentLocked(const ContentKey& key, const std::string& mount) {
        if (!isValidContentKey(key) || root_.empty()) { return content_close::NothingPending; }
        recover();
        const auto found = replaces_.find(liveDirectory(key));
        if (found == replaces_.end()) { return content_close::NothingPending; }
        auto& replace = found->second;
        if (!replace.closed) {
            // Only the close of the open that created the staged content says the new content is complete.
            // Another open of the same content (the one the game loaded from, closed now or never) does not.
            if (replace.owner != mount) { return content_close::StillOpen; }
            replace.closed = true;
        }
        return settle(found);
    }

    // Commits a staged content whose creating open is closed, if no file of
    // it is open and nothing failed in it. Erases the entry when committed.
    std::uint32_t settle(std::map<std::string, Replace>::iterator found) {
        const auto& replace = found->second;
        if (hasHandlesIn(replace.staging)) { return content_close::StillOpen; }
        if (replace.failed || !readMeta(replace.staging, replace.key)) { return content_close::HeldBack; }
        if (!sameTree(replace.staging, replace.made)) { found->second.failed = true; return content_close::HeldBack; }   // save-replace-review: not the tree this store created
        const auto how = commit(found->first, replace.staging);
        if (how != content_close::CommitFailed) { replaces_.erase(found); }
        return how;
    }

    // A closed, complete replacement that is still staged (its commit failed,
    // or a file of it was open at the close) is committed at the next call of
    // the store: otherwise it would wait for a content close that may never
    // come, and the next replace of the content would delete it.
    void settlePending() {
        for (auto next = replaces_.begin(); next != replaces_.end();) {
            const auto found = next++;
            if (!found->second.closed || found->second.failed) { continue; }
            [[maybe_unused]] const auto name = found->second.key.fileName;
            [[maybe_unused]] const auto how = settle(found);
#if defined(RECOMP_SAVE_STORE_LOG)
            if (how == content_close::Committed || how == content_close::CommittedByRenames) {
                __android_log_print(ANDROID_LOG_WARN, "RecompSaves", "SAVES_CONTENT_COMMIT_RETRY name=%s commit=%u (a replacement that was closed earlier is committed now)",
                                    name.c_str(), how);
            }
#endif
        }
    }

    // Makes the staged content the content. Any step may be the last one a
    // killed process executes; recover() puts the tree right from there. If a
    // step fails the staged tree keeps its .new name (never a name that
    // recover() would take for content) and the key stays served from it.
    std::uint32_t commit(const std::string& live, const std::string& staging) {
        const auto typeDir = detail::parentOf(live);
        if (!detail::syncTree(staging)) { return content_close::CommitFailed; }
        if (detail::exchangePaths(staging, live)) {   // the commit point: no instant without content under the final name
            detail::syncDirectory(typeDir);
            (void)moveToTrash(staging);               // the old content now
            pruneKept(live);
            return content_close::Committed;
        }
        // The host refused the exchange: set the old tree aside under a name recover() knows, move the new one in.
        const auto retired = live + detail::RetiredSuffix;
        struct stat st{};
        const bool oldInPlace = ::lstat(live.c_str(), &st) == 0;   // false only if an earlier attempt could not put it back
        if (oldInPlace) {
            if (::lstat(retired.c_str(), &st) == 0) {
                // save-replace-review: a leftover set-aside tree may be the only complete save (start-up could not set
                // it aside): keep it, never delete it before the new content is under the final name.
                keepAside(retired, live + detail::KeptRetiredSuffix, retired.substr(typeDir.size() + 1));
                if (::lstat(retired.c_str(), &st) == 0) { return content_close::CommitFailed; }
            }
            if (::rename(live.c_str(), retired.c_str()) != 0) { return content_close::CommitFailed; }
        }
        if (::rename(staging.c_str(), live.c_str()) != 0) {
            if (oldInPlace) { (void)::rename(retired.c_str(), live.c_str()); }
            return content_close::CommitFailed;
        }
        detail::syncDirectory(typeDir);
        (void)moveToTrash(retired);
        pruneKept(live);
        return content_close::CommittedByRenames;
    }

    // Before the first read of the tree: put right what a replace that an
    // earlier process did not complete left behind. Never promotes a staged
    // tree, and never deletes a tree with a valid meta.bin unless valid
    // content stands under the final name and no set-aside old content stood
    // next to it: such a tree may be the only complete save (a library without
    // this code may have written, or deleted, the content since). It is kept
    // aside under a name nothing acts on. Does not write anything when there
    // is nothing to recover. Runs once, or again at the next call if putting
    // an old content back failed.
    void recover() {
        if (!safe()) { return; }   // the v465 store: no staged content, no start-up repair
        if (!replaces_.empty()) { settlePending(); }
        if (recovered_) { return; }
        recovered_ = true;
        bool again = false;
        const auto contents = contentsRoot();
        for (const auto& xuidName : detail::directoryNames(contents)) {
            const auto xuid = detail::parseHexNumber(xuidName, 16);
            for (const auto& deviceName : detail::directoryNames(contents + "/" + xuidName)) {
                const auto device = detail::parseHexNumber(deviceName, 8);
                for (const auto& typeName : detail::directoryNames(contents + "/" + xuidName + "/" + deviceName)) {
                    const auto type = detail::parseHexNumber(typeName, 8);
                    const auto typeDir = contents + "/" + xuidName + "/" + deviceName + "/" + typeName;
                    const auto names = detail::directoryNames(typeDir);
                    std::vector<std::string> untouched;   // final names whose trees stay as they are for a later attempt
                    std::vector<std::string> doubted;     // final names whose content had a set-aside old content next to it
                    // Old content set aside by a two-rename commit. With nothing valid under the final name it
                    // is the content and goes back. Next to valid content it is redundant if the commit had
                    // completed, but the last complete save if a library without this code wrote that content
                    // and died in it: start-up cannot tell, so it is kept aside.
                    for (const auto& name : names) {
                        if (!detail::endsWith(name, detail::RetiredSuffix)) { continue; }
                        const auto encoded = name.substr(0, name.size() - std::strlen(detail::RetiredSuffix));
                        const auto decoded = detail::hexDecode(encoded);
                        if (!xuid || !device || !type || !decoded) { continue; }   // not a place the store keeps content in
                        const ContentKey key{*xuid, static_cast<std::uint32_t>(*device), static_cast<std::uint32_t>(*type), *decoded};
                        const auto retired = typeDir + "/" + name;
                        const auto live = typeDir + "/" + encoded;
                        if (replaces_.count(live) != 0) { continue; }   // (a repeated run) this process is replacing it
                        if (!ensureRoot()) { return; }
                        if (!detail::isRealDirectory(retired) || !readMeta(retired, key)) {
                            noteRecovered("removed: a set-aside tree without a valid meta.bin", name, moveToTrash(retired) == 0);
                            continue;
                        }
                        if (readMeta(live, key)) {
                            doubted.push_back(encoded);
                            keepAside(retired, live + detail::KeptRetiredSuffix, name);
                            continue;
                        }
                        if (const int error = moveToTrash(live); error != 0 && error != ENOENT) {   // a tree without valid meta
                            again = true; untouched.push_back(encoded);
                            continue;
                        }
                        const bool restored = ::rename(retired.c_str(), live.c_str()) == 0;
                        if (restored) { detail::syncDirectory(typeDir); usageKnown_ = false; }
                        else { again = true; untouched.push_back(encoded); }
                        noteRecovered("restored: the old content is back under its name", name, restored);
                    }
                    // A staged tree next to valid content that was not written after it is an uncommitted
                    // replacement: deleted. A staged tree with a valid meta.bin is kept aside where no valid
                    // content is under the final name, where an old content stood set aside, and where the
                    // content under the final name was written after it: then that content is the new one of a
                    // commit exchange (the staged tree is the old content, a spare copy), or a library without
                    // this code wrote it since and may have died in the write (the staged tree may be the last
                    // complete save). Start-up cannot tell the two apart.
                    for (const auto& name : names) {
                        if (!detail::endsWith(name, detail::StagingSuffix)) { continue; }
                        const auto encoded = name.substr(0, name.size() - std::strlen(detail::StagingSuffix));
                        const auto staged = typeDir + "/" + name;
                        const auto live = typeDir + "/" + encoded;
                        const auto decoded = detail::hexDecode(encoded);
                        if (!xuid || !device || !type || !decoded || !detail::isRealDirectory(staged)) { continue; }   // not made by the store
                        if (replaces_.count(live) != 0) { continue; }   // (a repeated run) this process's own staged content
                        if (std::find(untouched.begin(), untouched.end(), encoded) != untouched.end()) { continue; }
                        if (!ensureRoot()) { return; }
                        {
                            // save-replace-merge (after the review, the lead's decision): a staged tree with a valid
                            // meta.bin is never deleted at start-up. The store cannot tell a complete tree from a torn
                            // one (meta.bin holds no size or checksum of the data), and a complete never-committed save
                            // may be the only complete one (next to a torn visible save). It is kept aside; the bound is
                            // pruneKept() after the next commit of the slot. The order (writtenAfter, a tie counting as
                            // "after": review repair R3) now only chooses the log text.
                            const ContentKey key{*xuid, static_cast<std::uint32_t>(*device), static_cast<std::uint32_t>(*type), *decoded};
                            const bool doubt = std::find(doubted.begin(), doubted.end(), encoded) != doubted.end();
                            if (readMeta(staged, key)) {
                                keepAside(staged, live + detail::KeptStagingSuffix, name,
                                          doubt || !readMeta(live, key) || writtenAfter(live, staged)
                                                  ? "kept: a staged tree next to doubted or missing content, or not newer than the content (the old content of a committed exchange, or content another library wrote since; restore or delete by hand)"
                                                  : "kept: a staged tree written after the content (an uncommitted replacement; restore or delete by hand)");
                                continue;
                            }
                        }
                        noteRecovered("removed: a staged tree without a valid meta.bin (never a content)", name, moveToTrash(staged) == 0);
                    }
                }
            }
        }
        recovered_ = !again;
    }

    // save-replace-merge (after the review): the bound on kept trees. Called after a commit of the content at live:
    // a complete save (closed by its creating open, no failed write, fsynced) is now durable under the final name
    // (the directory was synced), and it was created in this process after every kept tree of the slot existed
    // (kept trees are made by start-up or at a replace, from trees created earlier). So every kept tree of the slot
    // is older than a durable complete save; the newest KeptPerSlot of them (by meta.bin time, then name) stay for a
    // restore by hand, the others are deleted. Start-up never deletes a kept tree.
    void pruneKept(const std::string& live) {
        const auto typeDir = detail::parentOf(live);
        const auto base = live.substr(typeDir.size() + 1);
        struct Kept { std::int64_t sec; long nsec; std::string name; };
        std::vector<Kept> kept;
        for (const auto& name : detail::directoryNames(typeDir)) {
            if (name.rfind(base + detail::KeptRetiredSuffix + ".", 0) != 0 && name.rfind(base + detail::KeptStagingSuffix + ".", 0) != 0) { continue; }
            struct stat st{};
            const bool timed = ::lstat((typeDir + "/" + name + "/" + detail::MetaFile).c_str(), &st) == 0;
            const auto when = detail::writeTimeOf(st);
            kept.push_back({timed ? static_cast<std::int64_t>(when.tv_sec) : 0, timed ? static_cast<long>(when.tv_nsec) : 0L, name});
        }
        if (kept.size() <= detail::KeptPerSlot) { return; }
        std::sort(kept.begin(), kept.end(), [](const Kept& a, const Kept& b) {
            return a.sec != b.sec ? a.sec > b.sec : a.nsec != b.nsec ? a.nsec > b.nsec : a.name > b.name;
        });
        for (std::size_t i = detail::KeptPerSlot; i < kept.size(); ++i) {
            const bool ok = moveToTrash(typeDir + "/" + kept[i].name) == 0;
#if defined(RECOMP_SAVE_STORE_LOG)
            __android_log_print(ANDROID_LOG_WARN, "RecompSaves", "SAVES_REPLACE_PRUNED dir=%s ok=%d (a kept tree older than the save just committed; %zu newer kept trees stay)",
                                kept[i].name.c_str(), int(ok), static_cast<std::size_t>(detail::KeptPerSlot));
#else
            (void)ok;
#endif
        }
    }

    // save-replace-review: true if dir's meta.bin is still the file this store created (same device, inode, mtime, ctime).
    [[nodiscard]] static bool sameTree(const std::string& dir, const struct stat& made) {
        struct stat now{};
        if (::lstat((dir + "/" + detail::MetaFile).c_str(), &now) != 0) { return false; }
        const auto m = detail::writeTimeOf(now), mm = detail::writeTimeOf(made), c = detail::changeTimeOf(now), cm = detail::changeTimeOf(made);
        return now.st_dev == made.st_dev && now.st_ino == made.st_ino && m.tv_sec == mm.tv_sec && m.tv_nsec == mm.tv_nsec
                && c.tv_sec == cm.tv_sec && c.tv_nsec == cm.tv_nsec;
    }

    // True if the content at dir was created later than the content at other
    // (last-write times of their meta.bin, which only a create writes), or at
    // the same time (save-replace-review: a tie keeps the staged tree). False when unknown.
    [[nodiscard]] static bool writtenAfter(const std::string& dir, const std::string& other) {
        struct stat a{}, b{};
        if (::lstat((dir + "/" + detail::MetaFile).c_str(), &a) != 0 || ::lstat((other + "/" + detail::MetaFile).c_str(), &b) != 0) { return false; }
        const auto ta = detail::writeTimeOf(a), tb = detail::writeTimeOf(b);
        return ta.tv_sec > tb.tv_sec || (ta.tv_sec == tb.tv_sec && ta.tv_nsec >= tb.tv_nsec);   // save-replace-review: a tie keeps the tree
    }

    // Renames a tree start-up will not delete to the first free <base>.<n>.
    // If that fails the tree stays where it is.
    void keepAside(const std::string& path, const std::string& base, const std::string& name,
                   const char* what = "kept: a tree with a valid meta.bin that may be the only complete save, set aside as <name>.kept-old.<n> or .kept-new.<n> (restore or delete by hand)") {
        bool ok = false;
        for (int n = 0; n < 1000 && !ok; ++n) {
            const auto target = base + "." + std::to_string(n);
            struct stat st{};
            if (::lstat(target.c_str(), &st) == 0) { continue; }
            ok = ::rename(path.c_str(), target.c_str()) == 0;
            break;
        }
        if (ok) { detail::syncDirectory(detail::parentOf(path)); }
        noteRecovered(what, name, ok);
    }

    // True if replaces are safe in this store. The switch is read at the first call (under mutex_).
    bool safe() {
        if (mode_ == ReplaceMode::FromSwitch) {
            const auto decision = replace_switch::readSwitch(replace_switch::filesRoot(), root_);
            mode_ = decision.mode;
            hold_ = decision.hold;
#if defined(RECOMP_SAVE_STORE_LOG)
            __android_log_print(ANDROID_LOG_INFO, "RecompSaves", "%s", decision.line.c_str());
#endif
        }
        return mode_ == ReplaceMode::Safe;
    }

    // "sandbox-hold": once per store, right after a staged content was created (the window in which the v465 store
    // has deleted the old content and holds none). Pauses with the store's and the caller's locks held.
    void holdOnce(const ContentKey& key) {
        hold_ = false;
#if defined(RECOMP_SAVE_STORE_LOG)
        __android_log_print(ANDROID_LOG_WARN, "RecompSaves", "SAVES_REPLACE_HOLD name=%s seconds=%u (device test: force-stop now; the next boot must load the old save)",
                            key.fileName.c_str(), replace_switch::HoldSeconds);
#else
        (void)key;
#endif
        std::this_thread::sleep_for(std::chrono::seconds(replace_switch::HoldSeconds));
    }

    static void noteRecovered(const char* what, const std::string& name, bool ok) noexcept {
#if defined(RECOMP_SAVE_STORE_LOG)
        __android_log_print(ANDROID_LOG_WARN, "RecompSaves", "SAVES_REPLACE_RECOVERED %s (dir=%s ok=%d)", what, name.c_str(), int(ok));
#else
        (void)what; (void)name; (void)ok;
#endif
    }

    [[nodiscard]] static bool isStorageFailure(std::uint32_t status) noexcept {
        return status == nt::DiskFull || status == nt::Unsuccessful || status == nt::NoMemory
                || status == nt::AccessDenied || status == nt::TooManyOpenedFiles;
    }
    // A write to the content at contentDir failed for a storage reason.
    void noteFailure(const std::string& contentDir) noexcept {
        for (auto& entry : replaces_) {
            if (entry.second.staging == contentDir) { entry.second.failed = true; }
        }
    }

    std::uint32_t createContent(const ContentKey& key, const std::string& dir, const std::uint8_t* record) {
        if (!ensureRoot()) { return detail::win32FromErrno(errno); }
        ensureUsage();
        const auto typeDir = detail::parentOf(dir);
        const auto deviceDir = detail::parentOf(typeDir);
        if (!detail::makeDirectory(detail::parentOf(deviceDir)) || !detail::makeDirectory(deviceDir)
                || !detail::makeDirectory(typeDir)) {
            return detail::win32FromErrno(errno);
        }
        // A tree without a valid meta.bin is a torn create; it was never
        // counted in usage and none of its files may leak into the new content.
        struct stat st{};
        if (::lstat(dir.c_str(), &st) == 0) {
            if (const int error = moveToTrash(dir)) { return detail::win32FromErrno(error); }
        }
        const auto data = dir + "/" + detail::DataDirectory;
        if (::mkdir(dir.c_str(), 0700) != 0) { return detail::win32FromErrno(errno); }
        if (::mkdir(data.c_str(), 0700) != 0) {
            const int error = errno;
            (void)detail::removeTree(dir);
            return detail::win32FromErrno(error);
        }
        detail::syncDirectory(dir);
        detail::syncDirectory(typeDir);
        std::array<std::uint8_t, detail::MetaBytes> meta{};
        std::copy(detail::MetaMagic.begin(), detail::MetaMagic.end(), meta.begin());
        detail::storeBe32(meta.data() + detail::MetaVersionOffset, detail::MetaVersion);
        detail::storeBe64(meta.data() + detail::MetaCreatorOffset, key.xuid);
        const auto normalized = normalizeRecord(record);
        std::copy(normalized.begin(), normalized.end(), meta.begin() + detail::MetaRecordOffset);
        detail::storeBe64(meta.data() + detail::MetaChecksumOffset, detail::fnv1a64(meta.data(), detail::MetaChecksumOffset));
        if (const int error = detail::writeFileAtomic(dir, detail::MetaTemp, detail::MetaFile, meta.data(), meta.size())) {
            (void)detail::removeTree(dir);
            return detail::win32FromErrno(error);
        }
        return win32::Success;
    }

    // Validates the key/path, requires present content and existing parent
    // directories, and lstat()s the target without following symlinks.
    [[nodiscard]] Resolved resolve(const ContentKey& key, const std::string& path) const {
        Resolved out;
        if (!isValidContentKey(key) || root_.empty()) { out.status = nt::InvalidParameter; return out; }
        const auto parts = detail::parseContentPath(path);
        if (!parts) { out.status = nt::ObjectNameInvalid; return out; }
        out.contentDir = contentDirectory(key);
        if (!readMeta(out.contentDir, key)) { out.status = nt::ObjectPathNotFound; return out; }
        out.host = out.contentDir + "/" + detail::DataDirectory;
        struct stat st{};
        if (::lstat(out.host.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) { out.status = nt::ObjectPathNotFound; return out; }
        for (std::size_t i = 0; i + 1 < parts->size(); ++i) {
            out.host += "/" + (*parts)[i];
            if (::lstat(out.host.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) { out.status = nt::ObjectPathNotFound; return out; }
        }
        out.parent = out.host;
        for (std::size_t i = 0; i < parts->size(); ++i) { out.relative += (i == 0 ? "" : "\\") + (*parts)[i]; }
        if (parts->empty()) {
            out.parent = detail::parentOf(out.host);
            out.exists = true;
            out.st = st;
        } else {
            out.host += "/" + parts->back();
            if (::lstat(out.host.c_str(), &out.st) == 0) {
                out.exists = true;
            } else if (errno != ENOENT) {
                out.status = detail::ntFromErrno(errno);
                return out;
            }
        }
        if (out.exists && !S_ISREG(out.st.st_mode) && !S_ISDIR(out.st.st_mode)) { out.status = nt::AccessDenied; return out; }
        out.status = nt::Success;
        return out;
    }

    FileOpen track(int fd, const Resolved& resolved, bool write, bool directory, std::uint32_t information) {
        try {
            handles_.emplace(fd, Handle{resolved.contentDir, resolved.relative, write, directory});
        } catch (...) {
            (void)::close(fd);
            return {nt::NoMemory, information, -1, false};
        }
        return {nt::Success, information, fd, directory};
    }

    FileOpen openFileLocked(const ContentKey& key, const std::string& path, std::uint32_t disposition, bool write, FileKind kind) {
        namespace nd = nt_disposition;
        namespace ni = nt_information;
        const auto fail = [](std::uint32_t status, std::uint32_t information) { return FileOpen{status, information, -1, false}; };
        if (disposition > nd::OverwriteIf) { return fail(nt::InvalidParameter, ni::DoesNotExist); }
        if (kind == FileKind::Directory && disposition != nd::Open && disposition != nd::Create && disposition != nd::OpenIf) {
            return fail(nt::InvalidParameter, ni::DoesNotExist);
        }
        const auto resolved = resolve(key, path);
        if (resolved.status != nt::Success) { return fail(resolved.status, ni::DoesNotExist); }
        if (handles_.size() >= MaximumOpenHandles) { return fail(nt::TooManyOpenedFiles, resolved.exists ? ni::Exists : ni::DoesNotExist); }
        const int access = write ? O_RDWR : O_RDONLY;

        if (resolved.exists && S_ISDIR(resolved.st.st_mode)) {
            if (kind == FileKind::NonDirectory) { return fail(nt::FileIsADirectory, ni::Exists); }
            if (disposition == nd::Create) { return fail(nt::ObjectNameCollision, ni::Exists); }
            if (disposition != nd::Open && disposition != nd::OpenIf) { return fail(nt::FileIsADirectory, ni::Exists); }
            const int fd = ::open(resolved.host.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) { return fail(detail::ntFromErrno(errno), ni::Exists); }
            return track(fd, resolved, false, true, ni::Opened);
        }
        if (resolved.exists) {
            if (kind == FileKind::Directory) { return fail(nt::NotADirectory, ni::Exists); }
            if (disposition == nd::Create) { return fail(nt::ObjectNameCollision, ni::Exists); }
            std::uint32_t information = ni::Opened;
            if (disposition == nd::Supersede || disposition == nd::Overwrite || disposition == nd::OverwriteIf) {
                ensureUsage();
                const int truncating = ::open(resolved.host.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
                if (truncating < 0) { return fail(detail::ntFromErrno(errno), ni::Exists); }
                struct stat st{};
                int error = ::fstat(truncating, &st) == 0 ? 0 : errno;
                if (error == 0 && ::ftruncate(truncating, 0) != 0) { error = errno; }
                (void)::close(truncating);
                if (error != 0) { return fail(detail::ntFromErrno(error), ni::Exists); }
                used_ = detail::saturatingSubtract(used_, static_cast<std::uint64_t>(st.st_size));
                information = disposition == nd::Supersede ? ni::Superseded : ni::Overwritten;
            }
            const int fd = ::open(resolved.host.c_str(), access | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) { return fail(detail::ntFromErrno(errno), ni::Exists); }
            return track(fd, resolved, write, false, information);
        }
        if (disposition == nd::Open || disposition == nd::Overwrite) { return fail(nt::ObjectNameNotFound, ni::DoesNotExist); }
        if (kind == FileKind::Directory) {
            if (::mkdir(resolved.host.c_str(), 0700) != 0) { return fail(detail::ntFromErrno(errno), ni::DoesNotExist); }
            detail::syncDirectory(resolved.parent);
            const int fd = ::open(resolved.host.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) { return fail(detail::ntFromErrno(errno), ni::DoesNotExist); }
            return track(fd, resolved, false, true, ni::Created);
        }
        const int fd = ::open(resolved.host.c_str(), access | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) { return fail(detail::ntFromErrno(errno), ni::DoesNotExist); }
        detail::syncDirectory(resolved.parent);
        return track(fd, resolved, write, false, ni::Created);
    }

    std::string root_;
    std::uint64_t quota_{DefaultQuotaBytes};
    std::uint64_t used_{0};
    bool usageKnown_{false};
    bool rootReady_{false};
    std::uint64_t trashCounter_{0};
    std::map<int, Handle> handles_;
    std::map<std::string, Replace> replaces_;
    bool recovered_{false};
    ReplaceMode mode_{ReplaceMode::FromSwitch};
    bool hold_{false};
    mutable std::mutex mutex_;
};

} // namespace recomp::bootstrap::saves
