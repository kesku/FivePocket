// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/ios_run_files.h"
#include "apple/launch_request.h"
#include <CommonCrypto/CommonDigest.h>
#include <cerrno>
#include <utility>

#if defined(APPLE_RUN_FILES_TESTING)
extern "C" void apple_run_files_test_event(const char*,int);
#define RUN_EVENT(name,fd) apple_run_files_test_event(name,fd)
#else
#define RUN_EVENT(name,fd) ((void)0)
#endif

namespace recomp::apple {
namespace {
struct Fd {
    int value{-1};
    explicit Fd(int fd=-1):value(fd){}
    ~Fd(){if(value>=0)::close(value);}
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
    Fd(Fd&& other) noexcept:value(std::exchange(other.value,-1)){}
};
bool sameDirectory(const struct stat& a,const struct stat& b) {
    return a.st_dev==b.st_dev && a.st_ino==b.st_ino && a.st_mode==b.st_mode && a.st_flags==b.st_flags;
}
bool sameFile(const struct stat& a,const struct stat& b) {
    return sameDirectory(a,b) && a.st_size==b.st_size && a.st_nlink==b.st_nlink &&
        a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec &&
        a.st_ctimespec.tv_sec==b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec==b.st_ctimespec.tv_nsec;
}
bool resident(const struct stat& value,bool directory) {
    return !(value.st_flags&SF_DATALESS) && (directory?S_ISDIR(value.st_mode):S_ISREG(value.st_mode));
}
struct Anchor {Fd fd;int parent;std::string name;struct stat metadata;};
struct Chain {
    Fd slash{::open("/",O_SEARCH|O_CLOEXEC|O_NOFOLLOW)};
    std::vector<Anchor> items;
    int append(int parent,std::string_view name) {
        std::string text(name);struct stat before{},after{};
        if (::fstatat(parent,text.c_str(),&before,AT_SYMLINK_NOFOLLOW) || !resident(before,true)) return -1;
        Fd fd(::openat(parent,text.c_str(),O_SEARCH|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
        if(fd.value<0 || ::fstat(fd.value,&after) || !sameDirectory(before,after))return -1;
        const int result=fd.value;
        items.push_back({std::move(fd),parent,std::move(text),before});return result;
    }
    int path(int parent,std::string_view value) {
        while(!value.empty()) {
            const auto next=value.find('/');
            parent=append(parent,value.substr(0,next));
            if(parent<0 || next==value.npos)return parent;
            value.remove_prefix(next+1);
        }
        return parent;
    }
    bool unchanged() const {
        for(const auto& item:items) {
            struct stat value{};
            if(::fstatat(item.parent,item.name.c_str(),&value,AT_SYMLINK_NOFOLLOW) ||
               !resident(value,true) || !sameDirectory(value,item.metadata))return false;
        }
        return true;
    }
};
bool writeAll(int fd,std::span<const std::uint8_t> bytes) {
    while(!bytes.empty()) {
        const auto count=::write(fd,bytes.data(),bytes.size());
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;
        bytes=bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}
bool finishFile(int fd) {return ::fchmod(fd,0400)==0 && ::fsync(fd)==0;}
bool prepareNativeDirectories(Chain& chain,int trace,std::string_view root,const IosRequestPlan& plan) {
#if defined(APPLE_RUN_FILES_MUTANT_SKIP_MOUNTS)
    (void)chain;(void)trace;
#endif
    // These are the three real mount roots in the authenticated request and
    // canonical ship config. A file-only transfer does not carry empty dirs.
    // Never replace, empty or seed an existing mount (especially not saves).
    constexpr std::pair<std::string_view,std::string_view> mounts[]{
        {"--native-cache-directory","native-cache"},
        {"--native-update-directory","native-update"},
        {"--native-game-cache-directory","native-game-cache"}};
    for(const auto& [flag,name]:mounts) {
        bool seen=false;
        for(std::size_t i=0;i<plan.arguments.size();++i) {
            if(plan.arguments[i]!=flag)continue;
            const std::string expected=std::string(root)+"/private-trace/inputs/"+std::string(name);
            if(seen || i+1==plan.arguments.size() || plan.arguments[i+1]!=expected)return false;
            seen=true;
#if !defined(APPLE_RUN_FILES_MUTANT_SKIP_MOUNTS)
            const int inputs=chain.append(trace,"inputs");
            if(inputs<0 || !chain.unchanged())return false;
            const std::string leaf(name);
            if(::mkdirat(inputs,leaf.c_str(),0700) && errno!=EEXIST)return false;
            // append rejects files, symlinks, dataless paths and substitutions.
            if(chain.append(inputs,name)<0 || !chain.unchanged())return false;
#endif
        }
    }
    return true;
}
bool copyImage(int sourceDirectory,int targetDirectory,const ExpectedAsset& pin) {
    struct stat before{},opened{},after{},named{};
    constexpr auto name="decoded.image.bin";
    if(::fstatat(sourceDirectory,name,&before,AT_SYMLINK_NOFOLLOW) || !resident(before,false) ||
       before.st_size<0 || static_cast<std::uint64_t>(before.st_size)!=pin.byteCount)return false;
    Fd source(::openat(sourceDirectory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));
    if(source.value<0 || ::fstat(source.value,&opened) || !sameFile(before,opened))return false;
    Fd target(::openat(targetDirectory,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    if(target.value<0)return false;
    CC_SHA256_CTX hash{};if(CC_SHA256_Init(&hash)!=1)return false;
    std::array<std::uint8_t,64*1024> bytes{};
    std::uint64_t total=0;
    while(true) {
        const auto count=::read(source.value,bytes.data(),bytes.size());
        if(count<0 && errno==EINTR)continue;
        if(count<0)return false;
        if(!count)break;
        total+=static_cast<std::uint64_t>(count);
        if(total>pin.byteCount || CC_SHA256_Update(&hash,bytes.data(),static_cast<CC_LONG>(count))!=1 ||
           !writeAll(target.value,{bytes.data(),static_cast<std::size_t>(count)}))return false;
    }
    std::array<std::uint8_t,32> digest{};
    if(CC_SHA256_Final(digest.data(),&hash)!=1 || total!=pin.byteCount || digest!=pin.sha256 ||
       ::fstat(source.value,&after) || ::fstatat(sourceDirectory,name,&named,AT_SYMLINK_NOFOLLOW) ||
       !sameFile(before,after) || !sameFile(before,named))return false;
    return finishFile(target.value);
}
}

IosRunFilesResult prepareIosRunFiles(std::string_view root,std::span<const std::uint8_t> requestTemplate,
    const std::array<std::uint8_t,32>& requestSha256,std::string_view templateRoot,
    std::string_view templateId,std::string_view newId,const ExpectedAsset& image) {
    IosRunFilesResult result;
    auto fail=[&](IosRunFilesFailure failure) {result.failure=failure;result.systemError=errno;return std::move(result);};
    IosRequestPlan plan;
    errno=0;
    if(!planIosRequest(requestTemplate,requestSha256,templateRoot,templateId,root,newId,plan) ||
       image.relativePath!=plan.imageSourceRelative || image.byteCount==0 || image.byteCount>MaximumAssetBytes)
        return fail(IosRunFilesFailure::InvalidPlan);
    if(!verifyAuthenticatedAssets(root,{&image,1}).allMatched())return fail(IosRunFilesFailure::SourceRejected);
    Chain chain;
    if(chain.slash.value<0)return fail(IosRunFilesFailure::DirectoryRejected);
    const int rootFd=chain.path(chain.slash.value,root.substr(1));
    if(rootFd<0)return fail(IosRunFilesFailure::DirectoryRejected);
    const int trace=chain.append(rootFd,"private-trace");
    if(trace<0)return fail(IosRunFilesFailure::DirectoryRejected);
    const int source=chain.append(trace,"run-"+std::string(templateId));
    if(source<0 || !chain.unchanged())return fail(IosRunFilesFailure::DirectoryRejected);
    if(!prepareNativeDirectories(chain,trace,root,plan))return fail(IosRunFilesFailure::DirectoryRejected);
    const std::string name="run-"+std::string(newId);
    result.directoryRelative="private-trace/"+name;
    if(::mkdirat(trace,name.c_str(),0700)) {
#if defined(APPLE_RUN_FILES_MUTANT_REUSE)
        if(errno!=EEXIST)return fail(IosRunFilesFailure::DirectoryRejected);
#else
        return fail(errno==EEXIST?IosRunFilesFailure::DirectoryExists:IosRunFilesFailure::DirectoryRejected);
#endif
    }
    result.directoryCreated=true;
    const int destination=chain.append(trace,name);
    if(destination<0 || !chain.unchanged())return fail(IosRunFilesFailure::DirectoryRejected);
    RUN_EVENT("created",destination);
    if(!copyImage(source,destination,image))return fail(IosRunFilesFailure::CopyFailed);
    Fd request(::openat(destination,"start-request.bin",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    if(request.value<0 || !writeAll(request.value,plan.serialized) || !finishFile(request.value))
        return fail(IosRunFilesFailure::RequestFailed);
    RUN_EVENT("written",destination);
    if(!chain.unchanged())return fail(IosRunFilesFailure::Changed);
    std::array<std::uint8_t,32> requestHash{};
    if(!CC_SHA256(plan.serialized.data(),static_cast<CC_LONG>(plan.serialized.size()),requestHash.data()))
        return fail(IosRunFilesFailure::VerificationFailed);
    const ExpectedAsset destinationPins[]{image,{plan.imageDestinationRelative,image.byteCount,image.sha256},
        {plan.requestDestinationRelative,plan.serialized.size(),requestHash}};
#if !defined(APPLE_RUN_FILES_MUTANT_FINAL_HASH)
    if(!verifyAuthenticatedAssets(root,destinationPins).allMatched())return fail(IosRunFilesFailure::VerificationFailed);
#else
    (void)destinationPins;
#endif
    std::vector<std::string> parsed;
    const std::string requestPath=std::string(root)+"/"+plan.requestDestinationRelative;
    if(!loadLaunchRequest(requestPath.c_str(),root,newId,parsed) || parsed!=plan.arguments || !chain.unchanged())
        return fail(IosRunFilesFailure::VerificationFailed);
    result.failure=IosRunFilesFailure::None;result.systemError=0;result.plan=std::move(plan);return result;
}
}
