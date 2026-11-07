// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/launcher_settings_store.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recomp::apple::launcher {
namespace {
constexpr const char* Destination="apple-launcher-display-v1.txt";
constexpr const char* LockName=".apple-launcher-display-v1.lock";
#if defined(APPLE_LAUNCHER_STORE_MUTANT_FOLLOW)
constexpr int NoFollow=0;
#else
constexpr int NoFollow=O_NOFOLLOW;
#endif
struct Descriptor {
    int value;
    ~Descriptor(){if(value>=0)::close(value);}
    bool close() {const auto fd=value;value=-1;return fd<0 || ::close(fd)==0;}
};
bool noAliases(nlink_t links) noexcept {
#if defined(APPLE_LAUNCHER_STORE_MUTANT_LINKS)
    (void)links;return true;
#else
    return links<=1;
#endif
}
bool singleRegular(int fd,struct stat& st) noexcept {
    return ::fstat(fd,&st)==0 && S_ISREG(st.st_mode) && noAliases(st.st_nlink) && st.st_nlink>0;
}
bool snapshotRegular(int fd,struct stat& st) noexcept {
#if defined(APPLE_LAUNCHER_STORE_MUTANT_RETIRED)
    return singleRegular(fd,st);
#else
    // A pathname replacement unlinks an old open inode, not its complete data.
    // Zero links is legitimate for this already-open snapshot. Two links is an
    // alias and remains forbidden; locks and new temporaries still require one.
    return ::fstat(fd,&st)==0 && S_ISREG(st.st_mode) && noAliases(st.st_nlink);
#endif
}
struct Temporary {
    int directory;
    const char* name;
    bool owned=false;
    ~Temporary(){if(owned)::unlinkat(directory,name,0);}
};
std::atomic<std::uint64_t> serial{0};
}

LauncherSettingsStore::LauncherSettingsStore(int directory) noexcept {
    struct stat st{};
    if(::fstat(directory,&st)<0 || !S_ISDIR(st.st_mode))return;
    directory_=::fcntl(directory,F_DUPFD_CLOEXEC,0);
}
LauncherSettingsStore::~LauncherSettingsStore(){if(directory_>=0)::close(directory_);}

LoadedSettings LauncherSettingsStore::load(
#if defined(APPLE_LAUNCHER_STORE_TESTING)
                                         StoreReadHook hook,void* context
#endif
                                         ) const {
    if(directory_<0)return {LoadStatus::Unavailable,std::nullopt,EBADF};
    Descriptor file{::openat(directory_,Destination,O_RDONLY|O_CLOEXEC|NoFollow|O_NONBLOCK)};
    if(file.value<0) {
        const int error=errno;
        return {error==ENOENT?LoadStatus::Missing:error==ELOOP?LoadStatus::Invalid:LoadStatus::Unavailable,
                std::nullopt,error};
    }
    struct stat before{},after{};
    if(!snapshotRegular(file.value,before) || before.st_size<=0 || before.st_size>MaxStoredBytes)
        return {LoadStatus::Invalid,std::nullopt,EINVAL};
#if defined(APPLE_LAUNCHER_STORE_TESTING)
    if(hook)hook(file.value,context);
#endif
    char bytes[MaxStoredBytes+1];std::size_t used=0;
    while(used<sizeof bytes) {
        const auto count=::read(file.value,bytes+used,sizeof bytes-used);
        if(count<0){if(errno==EINTR)continue;return {LoadStatus::Unavailable,std::nullopt,errno};}
        if(!count)break;
        used+=std::size_t(count);
    }
    if(!snapshotRegular(file.value,after) || used!=std::size_t(before.st_size) || used>MaxStoredBytes ||
       before.st_size!=after.st_size || before.st_mtimespec.tv_sec!=after.st_mtimespec.tv_sec ||
       before.st_mtimespec.tv_nsec!=after.st_mtimespec.tv_nsec)
        return {LoadStatus::Invalid,std::nullopt,EINVAL};
    const auto value=parse(std::string_view(bytes,used));
    if(!value)return {LoadStatus::Invalid,std::nullopt,EINVAL};
    return {LoadStatus::Loaded,value,0};
}

SavedSettings LauncherSettingsStore::save(const std::optional<StoredDisplay>& expected,const DisplayChoices& choices
#if defined(APPLE_LAUNCHER_STORE_TESTING)
                                         ,StoreFault fault
#endif
                                         ) {
    if(directory_<0)return {SaveStatus::Failed,std::nullopt,EBADF};
    if(!valid(choices) || (expected && (!expected->generation || !valid(expected->choices))))
        return {SaveStatus::Invalid,std::nullopt,EINVAL};
    Descriptor lock{::openat(directory_,LockName,O_RDWR|O_CREAT|O_CLOEXEC|NoFollow|O_NONBLOCK,0600)};
    struct stat lockStat{};
    if(lock.value<0)return {SaveStatus::Failed,std::nullopt,errno};
    if(!singleRegular(lock.value,lockStat) || lockStat.st_size!=0)return {SaveStatus::Failed,std::nullopt,EINVAL};
    if(::flock(lock.value,LOCK_EX|LOCK_NB)<0)
        return {errno==EWOULDBLOCK?SaveStatus::Busy:SaveStatus::Failed,std::nullopt,errno};
    const auto previous=load();
    if(previous.status!=LoadStatus::Missing && previous.status!=LoadStatus::Loaded)
        return {SaveStatus::Failed,std::nullopt,previous.error};
#if !defined(APPLE_LAUNCHER_STORE_MUTANT_CONFLICT)
    if(previous.value!=expected)return {SaveStatus::Conflict,std::nullopt,0};
#endif
    const auto generation=previous.value?previous.value->generation:0;
    if(generation==std::numeric_limits<std::uint64_t>::max())return {SaveStatus::Invalid,std::nullopt,EOVERFLOW};
    const StoredDisplay next{generation+1,choices};
    const auto bytes=serialise(next);
    if(!bytes)return {SaveStatus::Invalid,std::nullopt,EINVAL};
    char name[100];
    const auto sequence=serial.fetch_add(1,std::memory_order_relaxed);
    const auto length=std::snprintf(name,sizeof name,".apple-launcher-display-%lld-%llu.tmp",
                                   static_cast<long long>(::getpid()),static_cast<unsigned long long>(sequence));
    if(length<=0 || std::size_t(length)>=sizeof name)return {SaveStatus::Failed,std::nullopt,EOVERFLOW};
#if defined(APPLE_LAUNCHER_STORE_MUTANT_DIRECT)
    const char* temporaryName=Destination;
    const int createFlags=O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|NoFollow;
#else
    const char* temporaryName=name;
    const int createFlags=O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|NoFollow;
#endif
    Temporary temporary{directory_,temporaryName};
    Descriptor file{::openat(directory_,temporaryName,createFlags,0600)};
    if(file.value<0)return {SaveStatus::Failed,std::nullopt,errno};
    temporary.owned=true;
    struct stat fileStat{};
    if(!singleRegular(file.value,fileStat))return {SaveStatus::Failed,std::nullopt,EINVAL};
    std::size_t used=0;
    while(used<bytes->size()) {
        auto remaining=bytes->size()-used;
#if defined(APPLE_LAUNCHER_STORE_TESTING)
        if(fault==StoreFault::PartialWrite) {
            if(used)return {SaveStatus::Failed,std::nullopt,EIO};
            remaining=1;
        }
#endif
        const auto count=::write(file.value,bytes->data()+used,remaining);
        if(count<0){if(errno==EINTR)continue;return {SaveStatus::Failed,std::nullopt,errno};}
        if(!count)return {SaveStatus::Failed,std::nullopt,EIO};
        used+=std::size_t(count);
    }
#if !defined(APPLE_LAUNCHER_STORE_MUTANT_NO_SYNC)
#if defined(APPLE_LAUNCHER_STORE_TESTING)
    if(fault==StoreFault::FileSync)return {SaveStatus::Failed,std::nullopt,EIO};
#endif
    if(::fsync(file.value)<0)return {SaveStatus::Failed,std::nullopt,errno};
#endif
    if(!file.close())return {SaveStatus::Failed,std::nullopt,errno};
#if defined(APPLE_LAUNCHER_STORE_TESTING)
    if(fault==StoreFault::BeforeRename)return {SaveStatus::Failed,std::nullopt,EIO};
#endif
    if(::renameat(directory_,temporaryName,directory_,Destination)<0)return {SaveStatus::Failed,std::nullopt,errno};
    temporary.owned=false;
#if defined(APPLE_LAUNCHER_STORE_TESTING)
    if(fault==StoreFault::DirectorySync) {
#if defined(APPLE_LAUNCHER_STORE_MUTANT_COMMIT_UNKNOWN)
        return {SaveStatus::Saved,next,0};
#else
        return {SaveStatus::CommittedUncertain,next,EIO};
#endif
    }
#endif
    if(::fsync(directory_)<0)return {SaveStatus::CommittedUncertain,next,errno};
    return {SaveStatus::Saved,next,0};
}
}
