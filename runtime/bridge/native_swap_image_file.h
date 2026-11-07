#pragma once
#include "native_swap_image.h"
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recomp::trace {
// A private post-stop diagnostic, never a presentation receipt. The worker is
// staged in a fresh private per-run directory. Do not replace any existing file
// or follow a final-directory/file symlink. A failed partial file is retained
// for diagnosis and MUST NOT be reported as a successful capture.
inline bool saveNativeSwapImage(const std::filesystem::path& executable,
        const std::vector<std::uint8_t>& rgba) noexcept {
    if(rgba.size()!=gpu::NativeSwapImagePlan::PixelBytes)return false;
    int directory=-1,file=-1;
    try {
        if(!executable.is_absolute() || executable.filename().empty())return false;
        directory=open(executable.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(directory<0)return false;
        struct stat info{};
        if(fstat(directory,&info) || !S_ISDIR(info.st_mode) || info.st_uid!=geteuid() || (info.st_mode&0022)){
            close(directory);return false;
        }
        file=openat(directory,"native-swap.rgba",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        close(directory);directory=-1;
        if(file<0)return false;
        std::size_t done=0;
        while(done<rgba.size()){
            const auto count=write(file,rgba.data()+done,rgba.size()-done);
            if(count<0 && errno==EINTR)continue;
            if(count<=0){close(file);return false;}
            done+=static_cast<std::size_t>(count);
        }
        const auto result=close(file);file=-1;return result==0;
    } catch(...) {
        if(file>=0)close(file);
        if(directory>=0)close(directory);
        return false;
    }
}
}
