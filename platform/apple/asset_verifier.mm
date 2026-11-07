// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/asset_verifier.h"
#include <CommonCrypto/CommonDigest.h>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <cstdlib>

#if defined(APPLE_ASSET_VERIFIER_TESTING)
// Test-only interceptors are absent from production and iOS objects. They let
// tiny local fixtures model an evicted inode without touching an iCloud file.
extern "C" void apple_asset_test_metadata(const char*, struct stat*);
extern "C" void apple_asset_test_before_open(const char*);
extern "C" void apple_asset_test_after_read(const char*, std::uint64_t);
#endif

namespace recomp::apple {
namespace {
constexpr std::size_t MaximumPathBytes = 1024, MaximumDepth = 32;
#if defined(APPLE_ASSET_VERIFIER_MUTANT_DIRECTORY_READ)
constexpr int DirectoryAccess = O_RDONLY | O_DIRECTORY;
#else
// Anchors are searched, never enumerated. Requiring directory-read access
// rejects otherwise traversable paths and is unnecessary for openat/fstatat.
constexpr int DirectoryAccess = O_SEARCH;
#endif
constexpr int DirectoryFlags = DirectoryAccess | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
struct Fd {
    int value{-1};
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
};
struct Directory {
    Fd fd;
    std::string name;
    struct stat metadata{};
    Directory(Fd opened, std::string component, const struct stat& st)
        : fd(std::move(opened)), name(std::move(component)), metadata(st) {}
};
using Chain = std::vector<Directory>;

bool parts(std::string_view path, bool root, std::vector<std::string>& result) {
    if (path.empty() || path.size() > (root ? 4096 : MaximumPathBytes)) return false;
    if (root) {
        if (path.front() != '/' || path.size() == 1) return false;
        path.remove_prefix(1);
    } else if (path.front() == '/') return false;
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if (part.empty() || part == "." || part == ".." || part.size() > 255) return false;
        for (unsigned char c : part) {
            if (root) {
                if (c < 32 || c > 126 || c == '\\') return false;
            } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
        }
        result.emplace_back(part);
        if (result.size() > MaximumDepth) return false;
        if (slash == path.npos) break;
        path.remove_prefix(slash + 1);
        if (path.empty()) return false;
    }
    return !result.empty();
}

bool sameInode(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode;
}
bool sameFile(const struct stat& a, const struct stat& b) {
    return sameInode(a, b) && a.st_size == b.st_size && a.st_flags == b.st_flags &&
        a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
        a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
}
bool sameDirectory(const struct stat& a, const struct stat& b) {
#if defined(APPLE_ASSET_VERIFIER_MUTANT_DIRECTORY_BYPASS)
    (void)a; (void)b;
    return true;
#elif defined(APPLE_ASSET_VERIFIER_MUTANT_DIRECTORY_FILE_METADATA)
    return sameFile(a, b);
#else
    // Directories anchor traversal; their entry counts and timestamps are not
    // asset bytes. The mini reports zero pathname timestamps for /private but
    // real timestamps through its fd. Preserve identity, mode and flags, then
    // revalidate the path chain. Regular files still require sameFile in full.
    return sameInode(a, b) && a.st_flags == b.st_flags;
#endif
}
// Failure-only metadata evidence. No new filesystem operations or acceptance rules.
unsigned statDelta(const struct stat& a,const struct stat& b) noexcept {
    return unsigned(a.st_dev!=b.st_dev)|(unsigned(a.st_ino!=b.st_ino)<<1)|
        (unsigned(a.st_mode!=b.st_mode)<<2)|(unsigned(a.st_size!=b.st_size)<<3)|
        (unsigned(a.st_flags!=b.st_flags)<<4)|
        (unsigned(a.st_mtimespec.tv_sec!=b.st_mtimespec.tv_sec || a.st_mtimespec.tv_nsec!=b.st_mtimespec.tv_nsec)<<5)|
        (unsigned(a.st_ctimespec.tv_sec!=b.st_ctimespec.tv_sec || a.st_ctimespec.tv_nsec!=b.st_ctimespec.tv_nsec)<<6);
}
void reportSourceChange(const char* phase,const char* scope,const std::string& component,
                        std::size_t ordinal,int parent,int fd,const ExpectedAsset* pin,
                        unsigned guard,const struct stat& before,const struct stat* opened,
                        const struct stat* after,const struct stat* namedAfter,
                        std::uint64_t bytes=0,bool hashAvailable=false,bool hashMatch=false) noexcept {
    const int savedErrno=errno;
    std::fprintf(stderr,"APPLE_ASSET_SOURCE_CHANGE phase=%s scope=%s component_len=%zu component=\"%.*s\" ordinal=%zu parent_fd=%d fd=%d "
        "asset_valid=%u asset_len=%zu asset=\"%.*s\" expected_bytes=%llu read_bytes=%llu guard_mask=%u "
        "opened_valid=%u after_valid=%u named_after_valid=%u opened_delta=%u after_delta=%u named_after_delta=%u "
        "hash_available=%u hash_match=%u saved_errno=%d\n",
        phase,scope,component.size(),int(component.size()),component.c_str(),ordinal,parent,fd,unsigned(pin!=nullptr),
        pin?pin->relativePath.size():0,int(pin?pin->relativePath.size():0),pin?pin->relativePath.c_str():"",
        static_cast<unsigned long long>(pin?pin->byteCount:0),static_cast<unsigned long long>(bytes),guard,
        unsigned(opened!=nullptr),unsigned(after!=nullptr),unsigned(namedAfter!=nullptr),
        opened?statDelta(before,*opened):0,after?statDelta(before,*after):0,namedAfter?statDelta(before,*namedAfter):0,
        unsigned(hashAvailable),unsigned(hashMatch),savedErrno);
    const auto row=[](const char* label,const struct stat* value) {
        if(!value)return;
        std::fprintf(stderr,"APPLE_ASSET_SOURCE_STAT snapshot=%s dev=%lld ino=%llu mode=%u flags=%u size=%lld mtime=%lld:%lld ctime=%lld:%lld\n",
            label,static_cast<long long>(value->st_dev),static_cast<unsigned long long>(value->st_ino),unsigned(value->st_mode),
            unsigned(value->st_flags),static_cast<long long>(value->st_size),
            static_cast<long long>(value->st_mtimespec.tv_sec),static_cast<long long>(value->st_mtimespec.tv_nsec),
            static_cast<long long>(value->st_ctimespec.tv_sec),static_cast<long long>(value->st_ctimespec.tv_nsec));
    };
    row("before",&before);row("opened",opened);row("after",after);row("named_after",namedAfter);
    errno=savedErrno;
}
AssetFailure metadataFailure(int error) {
    return error == ENOENT ? AssetFailure::Missing : error == ELOOP ? AssetFailure::Symlink : AssetFailure::MetadataFailed;
}
AssetFailure inspect(int parent, const std::string& name, bool directory, struct stat& st, int& error) {
    if (::fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) {
        error = errno;
        return metadataFailure(error);
    }
#if defined(APPLE_ASSET_VERIFIER_TESTING)
    apple_asset_test_metadata(name.c_str(), &st);
#endif
    if (S_ISLNK(st.st_mode)) return AssetFailure::Symlink;
    if (st.st_flags & SF_DATALESS) return AssetFailure::Dataless;
    if (directory && !S_ISDIR(st.st_mode)) return AssetFailure::NotDirectory;
    if (!directory && !S_ISREG(st.st_mode)) return AssetFailure::NotRegular;
    return AssetFailure::None;
}
AssetFailure openDirectory(int parent, const std::string& name, Chain& chain, int& error,
                           const char* scope,const ExpectedAsset* pin) {
    struct stat before{}, after{};
    auto failure = inspect(parent, name, true, before, error);
    if (failure != AssetFailure::None) return failure;
#if defined(APPLE_ASSET_VERIFIER_TESTING)
    apple_asset_test_before_open(name.c_str());
#endif
    Fd fd(::openat(parent, name.c_str(), DirectoryFlags));
    if (fd.value < 0) {
        error = errno;
        std::fprintf(stderr,"APPLE_ASSET_DIRECTORY_OPEN_FAILED component=\"%s\" errno=%d flags=%x\n",
                     name.c_str(),error,DirectoryFlags);
        return error == ELOOP ? AssetFailure::Symlink : AssetFailure::OpenFailed;
    }
    if (::fstat(fd.value, &after)) { error = errno; return AssetFailure::MetadataFailed; }
    if (!sameDirectory(before, after)) {
        reportSourceChange("directory-open",scope,name,chain.size(),parent,fd.value,pin,23,before,&after,nullptr,nullptr);
        return AssetFailure::SourceChanged;
    }
    chain.emplace_back(std::move(fd), name, before);
    return AssetFailure::None;
}
// A pathname replaced while its old directory is still held open must not be
// reported as the current app path. This checks anchors, without enumeration.
AssetFailure chainUnchanged(const Chain& chain, int firstParent, int& error,
                            const char* scope,const ExpectedAsset* pin) {
    int parent = firstParent;
    std::size_t ordinal=0;
    for (const auto& item : chain) {
        struct stat current{};
        const auto failure = inspect(parent, item.name, true, current, error);
        if (failure != AssetFailure::None) return failure;
        if (!sameInode(item.metadata, current)) {
            reportSourceChange("chain-revalidate",scope,item.name,ordinal,parent,item.fd.value,pin,7,item.metadata,nullptr,nullptr,&current);
            return AssetFailure::SourceChanged;
        }
        parent = item.fd.value;
        ++ordinal;
    }
    return AssetFailure::None;
}

struct CtimeRetry {bool eligible=false;struct stat checkpoint{};};

// verified-cache (Claude, 2026-10-16): every start used to SHA-256 the whole game data (~16 GB) before the guest started
// (60-75 s, and minutes when storage was slow). A file whose content was hashed and matched its pin is recorded with its
// identity (device, inode, size, modification and change times); a later start that finds the same pin and the same identity
// skips the re-hash (as git's index does). Any write, rename, replacement or attribute change moves ctime: a miss, a full hash.
// File: <root>/.asset-verified-v1 (text, one record per line), written atomically after a verification run.
struct VerifiedRecord {std::uint64_t size=0,dev=0,ino=0,mtimeNs=0,ctimeNs=0;std::array<std::uint8_t,32> sha{};};
struct VerifiedCache {std::string path;std::map<std::string,VerifiedRecord> records;bool dirty=false;unsigned hits=0,hashed=0;std::uint64_t hashedBytes=0;};
thread_local VerifiedCache* activeCache=nullptr;
std::uint64_t ns(const struct timespec& t){return std::uint64_t(t.tv_sec)*1000000000ull+std::uint64_t(t.tv_nsec);}
VerifiedRecord identity(const struct stat& st,const std::array<std::uint8_t,32>& sha) {
    VerifiedRecord r;r.size=std::uint64_t(st.st_size);r.dev=std::uint64_t(st.st_dev);r.ino=std::uint64_t(st.st_ino);
    r.mtimeNs=ns(st.st_mtimespec);r.ctimeNs=ns(st.st_ctimespec);r.sha=sha;return r;
}
bool sameIdentity(const VerifiedRecord& a,const VerifiedRecord& b) {
    return a.size==b.size && a.dev==b.dev && a.ino==b.ino && a.mtimeNs==b.mtimeNs && a.ctimeNs==b.ctimeNs && a.sha==b.sha;
}
void loadCache(VerifiedCache& c) {
    std::FILE* f=std::fopen(c.path.c_str(),"r");if(!f)return;
    char line[2048];
    while(std::fgets(line,sizeof line,f)) {
        char name[1100];char hex[65];unsigned long long v[5];
        if(std::sscanf(line,"%1099s %llu %llu %llu %llu %llu %64s",name,&v[0],&v[1],&v[2],&v[3],&v[4],hex)!=7 || std::strlen(hex)!=64)continue;
        VerifiedRecord r;r.size=v[0];r.dev=v[1];r.ino=v[2];r.mtimeNs=v[3];r.ctimeNs=v[4];bool ok=true;
        for(unsigned i=0;i<32;++i){unsigned b=0;if(std::sscanf(hex+2*i,"%2x",&b)!=1){ok=false;break;}r.sha[i]=std::uint8_t(b);}
        if(ok)c.records[name]=r;
    }
    std::fclose(f);
}
void saveCache(const VerifiedCache& c) {
    if(!c.dirty)return;
    const std::string tmp=c.path+".tmp";
    std::FILE* f=std::fopen(tmp.c_str(),"w");if(!f)return;
    bool ok=true;
    for(const auto& [name,r]:c.records) {
        if(name.find_first_of(" \t\r\n")!=std::string::npos)continue;
        char hex[65];for(unsigned i=0;i<32;++i)std::snprintf(hex+2*i,3,"%02x",r.sha[i]);
        ok=ok && std::fprintf(f,"%s %llu %llu %llu %llu %llu %s\n",name.c_str(),(unsigned long long)r.size,(unsigned long long)r.dev,
            (unsigned long long)r.ino,(unsigned long long)r.mtimeNs,(unsigned long long)r.ctimeNs,hex)>0;
    }
    ok=(std::fflush(f)==0) && ok;ok=(::fsync(fileno(f))==0) && ok;std::fclose(f);
    if(ok)std::rename(tmp.c_str(),c.path.c_str());else ::unlink(tmp.c_str());
}
AssetFailure verifyOne(int root, const std::vector<std::string>& components, const ExpectedAsset& pin,
                       int& error,const struct stat* retryBefore,CtimeRetry& retry) {
    Chain directories;
    int parent = root;
    for (std::size_t i = 0; i + 1 < components.size(); ++i) {
        const auto failure = openDirectory(parent, components[i], directories, error,"asset-chain",&pin);
        if (failure != AssetFailure::None) return failure;
        parent = directories.back().fd.value;
    }
    const auto& name = components.back();
    struct stat before{}, opened{}, after{}, namedAfter{};
    auto failure = inspect(parent, name, false, before, error);
    if (failure != AssetFailure::None) return failure;
    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) != pin.byteCount) return AssetFailure::SizeMismatch;
    if (retryBefore && !sameFile(*retryBefore,before)) {
        reportSourceChange("retry-before","asset",name,components.size()-1,parent,-1,&pin,127,*retryBefore,&before,nullptr,nullptr);
        return AssetFailure::SourceChanged;
    }
#if defined(APPLE_ASSET_VERIFIER_TESTING)
    apple_asset_test_before_open(name.c_str());
#endif
    Fd fd(::openat(parent, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (fd.value < 0) { error = errno; return error == ELOOP ? AssetFailure::Symlink : AssetFailure::OpenFailed; }
    if (::fstat(fd.value, &opened)) { error = errno; return AssetFailure::MetadataFailed; }
    if (!sameFile(before, opened)) {
        reportSourceChange("file-open","asset",name,components.size()-1,parent,fd.value,&pin,127,before,&opened,nullptr,nullptr);
        return AssetFailure::SourceChanged;
    }
    if (activeCache && !retryBefore) { // verified-cache: the same pin and the same file identity as a verified earlier hash
        const auto found=activeCache->records.find(pin.relativePath);
        if (found!=activeCache->records.end() && sameIdentity(found->second,identity(opened,pin.sha256)) && pin.byteCount==std::uint64_t(opened.st_size)) {
            failure = chainUnchanged(directories, root, error,"asset-chain",&pin);
            if (failure != AssetFailure::None) return failure;
            ++activeCache->hits;
            return AssetFailure::None;
        }
    }
    CC_SHA256_CTX context{};
    if (CC_SHA256_Init(&context) != 1) return AssetFailure::HashFailed;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t bytes = 0;
    while (true) {
        const auto count = ::read(fd.value, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            error = errno;
            return AssetFailure::ReadFailed;
        }
        if (!count) break;
        bytes += static_cast<std::uint64_t>(count);
        if (bytes > pin.byteCount) return AssetFailure::SizeMismatch;
        if (CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(count)) != 1) return AssetFailure::HashFailed;
#if defined(APPLE_ASSET_VERIFIER_TESTING)
        apple_asset_test_after_read(pin.relativePath.c_str(), bytes);
#endif
    }
    std::array<std::uint8_t, 32> actual{};
    if (CC_SHA256_Final(actual.data(), &context) != 1) return AssetFailure::HashFailed;
    if (::fstat(fd.value, &after)) { error = errno; return AssetFailure::MetadataFailed; }
    failure = inspect(parent, name, false, namedAfter, error);
    if (failure != AssetFailure::None) return failure;
    if (!sameFile(before, after) || !sameFile(before, namedAfter)) {
        reportSourceChange("file-after-hash","asset",name,components.size()-1,parent,fd.value,&pin,127,before,&opened,&after,&namedAfter,bytes,true,actual==pin.sha256);
        // Never accept the changed snapshot. Only a stable ctime transition may
        // request one fresh authentication from the already trusted root.
        if (!retryBefore && bytes==pin.byteCount && actual==pin.sha256 && sameFile(before,opened) &&
            statDelta(before,after)==64 && statDelta(before,namedAfter)==64 && sameFile(after,namedAfter)) {
            failure=chainUnchanged(directories,root,error,"asset-chain",&pin);
            if (failure!=AssetFailure::None)return failure;
            retry.eligible=true;retry.checkpoint=after;
        }
        return AssetFailure::SourceChanged;
    }
    failure = chainUnchanged(directories, root, error,"asset-chain",&pin);
    if (failure != AssetFailure::None) return failure;
    if (bytes != pin.byteCount) return AssetFailure::SizeMismatch;
#if !defined(APPLE_ASSET_VERIFIER_MUTANT_IGNORE_HASH)
    if (actual != pin.sha256) return AssetFailure::ContentMismatch;
#endif
    if (activeCache) { // verified-cache: this identity's content matched its pin
        activeCache->records[pin.relativePath]=identity(after,pin.sha256);activeCache->dirty=true;
        ++activeCache->hashed;activeCache->hashedBytes+=bytes;
    }
    return AssetFailure::None;
}
} // namespace

AssetVerification verifyAuthenticatedAssetsUncached(std::string_view canonicalRoot, std::span<const ExpectedAsset> expected);
AssetVerification verifyAuthenticatedAssets(std::string_view canonicalRoot, std::span<const ExpectedAsset> expected) {
    VerifiedCache cache;cache.path=std::string(canonicalRoot)+"/.asset-verified-v1";loadCache(cache);
    const bool off=std::getenv("RECOMP_IOS_ASSET_FULL_HASH")!=nullptr; // (a full hash on request)
    if(!off)activeCache=&cache;
    struct Reset {~Reset(){activeCache=nullptr;}} reset;
    auto result=verifyAuthenticatedAssetsUncached(canonicalRoot,expected);
    activeCache=nullptr;
    if(!off)saveCache(cache);
    std::fprintf(stderr,"APPLE_ASSET_CACHE records=%zu hits=%u hashed=%u hashed_mb=%.0f failure=%s full_hash=%u\n",expected.size(),cache.hits,cache.hashed,
        double(cache.hashedBytes)/1048576.0,assetFailureName(result.failure),unsigned(off));
    return result;
}
AssetVerification verifyAuthenticatedAssetsUncached(std::string_view canonicalRoot, std::span<const ExpectedAsset> expected) {
    AssetVerification result;
    std::vector<std::string> rootParts;
    if (!parts(canonicalRoot, true, rootParts)) { result.failure = AssetFailure::InvalidRoot; return result; }
    if (expected.empty() || expected.size() > MaximumAssetRecords) { result.failure = AssetFailure::LimitsExceeded; return result; }
    std::set<std::string> names;
    std::vector<std::vector<std::string>> paths;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& item = expected[i];
        result.failedIndex = i;
        result.failedPath = item.relativePath.substr(0, MaximumPathBytes);
        paths.emplace_back();
        if (!parts(item.relativePath, false, paths.back())) { result.failure = AssetFailure::InvalidRecord; return result; }
        if (!names.insert(item.relativePath).second) { result.failure = AssetFailure::DuplicatePath; return result; }
        if (item.byteCount > MaximumAssetBytes || total > MaximumAssetTotalBytes - item.byteCount) {
            result.failure = AssetFailure::LimitsExceeded; return result;
        }
        total += item.byteCount;
    }
    result.failedIndex = AssetVerification::NoRecord;
    result.failedPath.clear();
    Fd filesystemRoot(::open("/", DirectoryFlags));
    if (filesystemRoot.value < 0) { result.failure = AssetFailure::OpenFailed; result.systemError = errno; return result; }
    Chain rootChain;
    int root = filesystemRoot.value;
    for (const auto& component : rootParts) {
        result.failure = openDirectory(root, component, rootChain, result.systemError,"root-chain",nullptr);
        if (result.failure != AssetFailure::None) return result;
        root = rootChain.back().fd.value;
    }
    bool ctimeRetrySpent=false; // One additional whole-file read per verification invocation.
    for (std::size_t i = 0; i < expected.size(); ++i) {
        result.failedIndex = i;
        result.failedPath = expected[i].relativePath;
        CtimeRetry retry{};
        bool didRetry=false;
        result.failure = verifyOne(root, paths[i], expected[i], result.systemError,nullptr,retry);
        if (result.failure==AssetFailure::SourceChanged && retry.eligible && !ctimeRetrySpent) {
            // The original absolute-root descriptors remain owned and checked.
            result.failure=chainUnchanged(rootChain,filesystemRoot.value,result.systemError,"root-chain",&expected[i]);
            if (result.failure!=AssetFailure::None)return result;
            ctimeRetrySpent=true;didRetry=true;
            const int savedErrno=errno;
            std::fprintf(stderr,"APPLE_ASSET_CTIME_RETRY index=%zu attempt=2 total_retry_limit=1 first_failure_preserved=1\n",i);
            errno=savedErrno;
            CtimeRetry ignored{};
            result.failure=verifyOne(root,paths[i],expected[i],result.systemError,&retry.checkpoint,ignored);
        }
        if (result.failure == AssetFailure::None)
            result.failure = chainUnchanged(rootChain, filesystemRoot.value, result.systemError,"root-chain",&expected[i]);
        if (didRetry) {
            const int completedErrno=errno;
            std::fprintf(stderr,"APPLE_ASSET_CTIME_RETRY_RESULT index=%zu failure=%s errno=%d\n",i,assetFailureName(result.failure),result.systemError);
            errno=completedErrno;
        }
        if (result.failure != AssetFailure::None) return result;
        ++result.verifiedCount;
        result.verifiedBytes += expected[i].byteCount;
    }
    result.failedIndex = AssetVerification::NoRecord;
    result.failedPath.clear();
    result.failure = AssetFailure::None; // Success is published only after every requested record.
    return result;
}

const char* assetFailureName(AssetFailure value) noexcept {
    switch (value) {
#define CASE(name) case AssetFailure::name: return #name
        CASE(NotRun); CASE(None); CASE(InvalidRoot); CASE(InvalidRecord); CASE(DuplicatePath); CASE(LimitsExceeded);
        CASE(Missing); CASE(Symlink); CASE(Dataless); CASE(NotDirectory); CASE(NotRegular);
        CASE(MetadataFailed); CASE(OpenFailed); CASE(SizeMismatch); CASE(ContentMismatch);
        CASE(SourceChanged); CASE(ReadFailed); CASE(HashFailed);
#undef CASE
    }
    return "Unknown";
}
} // namespace recomp::apple
