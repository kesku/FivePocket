// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recomp::apple {
// Read one small, stable command from an already selected private directory.
// Callers supply fixed leaf names. Keep a spare byte for the allocation-free
// EOF check; command contents never select a path.
template<std::size_t N>
bool readBoundedCommandFile(int directory,const char* name,
                           std::array<char,N>& bytes,std::size_t& size) noexcept {
    static_assert(N>1);
    size=0;
    const int fd=openat(directory,name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return false;
    struct Close {int fd;~Close(){::close(fd);}} closer{fd};
    struct stat before{},after{};
    if(fstat(fd,&before)!=0 || !S_ISREG(before.st_mode) || before.st_nlink!=1 ||
       before.st_size<=0 || static_cast<std::uintmax_t>(before.st_size)>N-1)return false;
    const auto expected=static_cast<std::size_t>(before.st_size);
    std::size_t done=0;
    while(done<expected) {
        const auto count=pread(fd,bytes.data()+done,expected-done,static_cast<off_t>(done));
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;
        done+=static_cast<std::size_t>(count);
    }
    ssize_t extra;
    do {extra=pread(fd,bytes.data()+expected,1,static_cast<off_t>(expected));}
    while(extra<0 && errno==EINTR);
    if(extra!=0 || fstat(fd,&after)!=0 || !S_ISREG(after.st_mode) || after.st_nlink!=1 ||
       before.st_dev!=after.st_dev || before.st_ino!=after.st_ino || before.st_size!=after.st_size ||
       before.st_mtimespec.tv_sec!=after.st_mtimespec.tv_sec ||
       before.st_mtimespec.tv_nsec!=after.st_mtimespec.tv_nsec ||
       before.st_ctimespec.tv_sec!=after.st_ctimespec.tv_sec ||
       before.st_ctimespec.tv_nsec!=after.st_ctimespec.tv_nsec)return false;
    size=done;return true;
}
}
