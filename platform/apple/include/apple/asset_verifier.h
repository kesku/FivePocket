// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace recomp::apple {

struct ExpectedAsset {
    std::string relativePath;
    std::uint64_t byteCount{};
    std::array<std::uint8_t, 32> sha256{};
};

enum class AssetFailure {
    NotRun,
    None,
    InvalidRoot,
    InvalidRecord,
    DuplicatePath,
    LimitsExceeded,
    Missing,
    Symlink,
    Dataless,
    NotDirectory,
    NotRegular,
    MetadataFailed,
    OpenFailed,
    SizeMismatch,
    ContentMismatch,
    SourceChanged,
    ReadFailed,
    HashFailed,
};

struct AssetVerification {
    AssetFailure failure{AssetFailure::NotRun};
    // A failure before selecting a record (root/list validation) may use NoRecord.
    static constexpr std::size_t NoRecord = static_cast<std::size_t>(-1);
    std::size_t failedIndex{NoRecord};
    std::string failedPath; // At most 1024 bytes, truncated for an invalid oversized name.
    int systemError{};
    std::size_t verifiedCount{};
    std::uint64_t verifiedBytes{};
    bool allMatched() const noexcept { return failure == AssetFailure::None; }
};

inline constexpr std::size_t MaximumAssetRecords = 128;
inline constexpr std::uint64_t MaximumAssetBytes = 8ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t MaximumAssetTotalBytes = 32ULL * 1024 * 1024 * 1024;

// CALLER AUTHENTICATION PRECONDITION: expected must come from pins the caller
// independently authenticated (for example a verified, signed build resource),
// NOT a manifest merely read from writable disk alongside the assets. This API
// does not authenticate a manifest, the caller, signing, or the game library.
//
// canonicalRoot is an absolute already-canonical app-owned directory, with no
// symlink components. Only explicit relative records are visited; no directory
// contents are enumerated, no saves are inferred, and no files are changed.
// All symlinks (even internal ones) and dataless files/directories are refused.
// List validation precedes I/O. Empty lists, excessive counts/sizes, duplicate
// names, dot/traversal paths, and non-ASCII asset names are rejected.
//
// Streaming, synchronous, bounded-memory I/O: invoke off the UI thread. A match
// means observed bytes matched at verification time, NOT an enduring file lease
// or permission to launch. Subsequent use must preserve/revalidate ownership.
// It is not a gameplay, decoder, storage-capacity, or complete-install verdict.
AssetVerification verifyAuthenticatedAssets(std::string_view canonicalRoot,
                                             std::span<const ExpectedAsset> expected);
const char* assetFailureName(AssetFailure failure) noexcept;

} // namespace recomp::apple
