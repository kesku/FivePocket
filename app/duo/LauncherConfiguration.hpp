// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LauncherSettingsOwner.hpp"
#include "render_scale.h"
#include "pace_caps.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recomp::apple::launcher {
enum class ConfigurationStatus { Captured, Unavailable, Unsupported };
struct OriginalConfiguration {
    ConfigurationStatus status=ConfigurationStatus::Unavailable;
    SettingsPolicy policy{};
    unsigned paceFlags=0;
    recomp::gpu::rs::Setting scale;
};
namespace configuration {
struct Fd {int value=-1;~Fd(){if(value>=0)::close(value);}};
inline bool directory(int fd) noexcept {struct stat st{};return ::fstat(fd,&st)==0 && S_ISDIR(st.st_mode);}
inline bool flag(int directory,const char* name,bool& present) noexcept {
    struct stat st{};
    if(::fstatat(directory,name,&st,AT_SYMLINK_NOFOLLOW)<0) {
        if(errno!=ENOENT)return false;present=false;return true;
    }
    if(!S_ISREG(st.st_mode) || st.st_nlink!=1)return false;
    present=true;return true;
}
inline bool resolution(int root,recomp::gpu::rs::Setting& setting) noexcept {
    Fd folder{::openat(root,"settings",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
    if(folder.value<0)return errno==ENOENT;
    Fd file{::openat(folder.value,"render-scale.txt",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
    if(file.value<0)return errno==ENOENT;
    struct stat before{},after{};
    if(::fstat(file.value,&before)<0 || !S_ISREG(before.st_mode) || before.st_nlink!=1)return false;
    char bytes[recomp::gpu::rs::MaxFileBytes+1];std::size_t count=0;
    while(count<sizeof bytes) {
        const auto read=::read(file.value,bytes+count,sizeof bytes-count);
        if(read<0){if(errno==EINTR)continue;return false;}
        if(!read)break;count+=std::size_t(read);
    }
    if(::fstat(file.value,&after)<0 || before.st_size!=after.st_size ||
       before.st_mtimespec.tv_sec!=after.st_mtimespec.tv_sec || before.st_mtimespec.tv_nsec!=after.st_mtimespec.tv_nsec ||
       after.st_nlink!=1)return false;
    setting=recomp::gpu::rs::parse(bytes,count);return true;
}
}
// Capture actual legacy files through the already-owned root FD. This is not
// asset/request authentication: the ORIGINAL host preparation gates still run.
// Private headers are compiler-discovered inputs, never copied into public git.
inline OriginalConfiguration captureLauncherConfiguration(int root) noexcept {
    OriginalConfiguration captured;
    if(!configuration::directory(root))return captured;
    configuration::Fd trace{::openat(root,"private-trace",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
    if(trace.value<0)return captured;
    bool standing=false,fps=false;
    if(!configuration::flag(trace.value,"diag-v280-limit60",standing) ||
       !configuration::flag(trace.value,"diag-fps-overlay",fps))return captured;
    if(!standing){captured.status=ConfigurationStatus::Unsupported;return captured;}
    const char* names[]{"diag-pace-40","diag-pace-30","diag-pace-24","diag-pace-auto",
                        "diag-pace-dpt","diag-pace-lowlat","diag-pace-nolend"};
    constexpr unsigned bits[]{recomp::pace::SwCap40,recomp::pace::SwCap30,recomp::pace::SwCap24,recomp::pace::SwAuto,
                              recomp::pace::SwDpt,recomp::pace::SwLowLat,recomp::pace::SwNoLend};
    for(unsigned i=0;i<7;++i) {bool present=false;if(!configuration::flag(trace.value,names[i],present))return captured;
        if(present)captured.paceFlags|=bits[i];}
    if(!configuration::resolution(root,captured.scale))return captured;
    // (Claude, 2026-10-18) the original cap on Apple is 30 fps: without a saved choice the renderer (paceWindow) runs 30 with
    // desired present times whatever the pace files say, and the guest limit is 30 (trace_runner). The Android priority
    // (automatic, then24, then30, then40 from the files) does not apply here; the files are still captured above.
    const unsigned cap=3;
    const int effectivePercent=captured.scale.valid?captured.scale.percent:100;
    unsigned resolution=0;while(resolution<ResolutionPercent.size() && ResolutionPercent[resolution]!=unsigned(effectivePercent))++resolution;
    if(resolution==ResolutionPercent.size()){captured.status=ConfigurationStatus::Unsupported;return captured;}
    // Scaler/sharpness indices are explicitly NOT APPLICABLE on Apple, not
    // invented effective Android defaults. The coordinator renders unavailable.
    // Frame generation (Claude, 2026-10-18): the original is Off (no file selects it); Off and On are both offered.
    // Draw distance (Claude, 2026-10-20): the original is Original (100 %); Original, 90, 80 and 70 % are offered.
    // Traffic density (Claude, 2026-10-20): the original is Original (100 %); Original, 75, 50 and 25 % are offered.
    // Shadow distance (Claude, 2026-10-20): the original is Original (every cascade); Original and Near are offered.
    // Screen (Claude, 2026-10-21): the original is Fill (owner 2026-10-20); Fill and Fit are offered.
    captured.policy={{{std::uint8_t(cap),3,0,std::uint8_t(resolution),std::uint8_t(fps),0,0,0,0,0}},{{63,8,1,127,3,3,15,15,3,3}}};
#if defined(APPLE_LAUNCHER_CONFIGURATION_MUTANT_CAP)
    captured.policy.original.values[0]=1;
#endif
    captured.status=ConfigurationStatus::Captured;return captured;
}
// Creates only the app-owned settings DIRECTORY if absent. Never replaces it,
// follows a redirected path or edits legacy setting/flag/asset/save contents.
inline int openLauncherSettingsDirectory(int root) noexcept {
    if(!configuration::directory(root))return -1;
    int fd=::openat(root,"settings",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0 || errno!=ENOENT)return fd;
    if(::mkdirat(root,"settings",0700)<0 && errno!=EEXIST)return -1;
    return ::openat(root,"settings",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
}
}
