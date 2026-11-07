// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cerrno>
#include <cstdint>
#include <limits>
#include <sys/statvfs.h>

namespace recomp::apple {
struct StorageSpace {
    bool valid{};
    std::uint64_t availableBytes{};
    int error{};
};
inline StorageSpace storageSpaceFromBlocks(std::uint64_t unit, std::uint64_t available,
                                          std::uint64_t total) noexcept {
    if (!unit || available > total
#if !defined(APPLE_STORAGE_MUTANT_WRAP)
        || available > std::numeric_limits<std::uint64_t>::max()/unit
#endif
       )
        return {false,0,EOVERFLOW};
#if defined(APPLE_STORAGE_MUTANT_BYTE_UNITS)
    return {true,available,0};
#else
    return {true,available*unit,0};
#endif
}
// Snapshot only, not reserved space or a guarantee a subsequent copy will fit.
// Caller supplies its already prepared private data root. No enumeration/I/O
// of user files, deletion, purge request, or optimistic purgeable-space claim.
inline StorageSpace storageSpace(const char* root) noexcept {
    if (!root || !*root) return {false,0,EINVAL};
    struct statvfs st{};
    if (::statvfs(root,&st)) {
#if defined(APPLE_STORAGE_MUTANT_ERROR_SUCCESS)
        return {true,0,0};
#else
        return {false,0,errno};
#endif
    }
    return storageSpaceFromBlocks(st.f_frsize,st.f_bavail,st.f_blocks);
}
}
