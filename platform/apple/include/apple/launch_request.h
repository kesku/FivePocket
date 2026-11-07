// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "private_trace_arguments.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <limits.h>

namespace recomp::apple {
// The supervisor authenticates every staged input. This is a second, local
// syntax/size boundary, not a replacement for that manifest or hash check.
inline bool loadLaunchRequest(const char* path, std::string_view files,
                              std::string_view id, std::vector<std::string>& args) {
    if (!path || files.size() > 94 || !private_trace::canonicalPath(files) ||
        !private_trace::requestId(id)) return false;
    const std::string trace = std::string(files) + "/private-trace";
    const std::string expected = trace + "/run-" + std::string(id) + "/start-request.bin";
    if (path != expected) return false;
    struct stat before{}, opened{}, after{};
    if (lstat(path, &before) || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
        before.st_size > private_trace::MaximumArgumentBytes || (before.st_flags & SF_DATALESS)) return false;
    char resolved[PATH_MAX];
    if (!realpath(path, resolved) || expected != resolved) return false;
    const auto same = [](const struct stat& a, const struct stat& b) {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
            a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
            a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
    };
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    struct Close { int fd; ~Close() { close(fd); } } closeFile{fd};
    if (fstat(fd, &opened) || !same(before, opened)) return false;
    std::array<std::uint8_t, private_trace::MaximumArgumentBytes> bytes{};
    std::size_t done = 0;
    while (done < static_cast<std::size_t>(before.st_size)) {
        const auto n = read(fd, bytes.data() + done, before.st_size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    std::uint8_t extra{};
    ssize_t trailing;
    do { trailing = read(fd, &extra, 1); } while (trailing < 0 && errno == EINTR);
    if (trailing != 0 || fstat(fd, &after) || !same(before, after)) return false;
    return private_trace::parseArguments(bytes.data(), done, trace, id, args);
}
}
