#pragma once
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace recomp::gpu {
inline bool readInput(const char* path, std::vector<uint8_t>& data, size_t limit) {
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    // Keep closure exception-safe if vector allocation throws in a caller's
    // bounded loader. Every success/error path owns exactly this descriptor.
    struct CloseFile {int fd;~CloseFile(){close(fd);}} owner{fd};
    struct stat before{};
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 1 ||
        static_cast<uint64_t>(before.st_size) > limit) return false;
    data.resize(static_cast<size_t>(before.st_size));
    size_t cursor = 0;
    while (cursor < data.size()) {
        const ssize_t count = read(fd, data.data() + cursor, data.size() - cursor);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        cursor += static_cast<size_t>(count);
    }
    struct stat after{};
    const bool valid = fstat(fd, &after) == 0 && before.st_size == after.st_size &&
        before.st_mtime == after.st_mtime;
    return valid;
}
}
