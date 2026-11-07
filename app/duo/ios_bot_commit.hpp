// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace iosbot {
inline std::atomic<bool> configured{false}, revoked{false};

template<class State> bool physicalActivity(const State& s) noexcept {
    return s.buttons || s.leftTrigger >= 128 || s.rightTrigger >= 128 ||
        std::abs(int(s.leftX)) >= 8192 || std::abs(int(s.leftY)) >= 8192 ||
        std::abs(int(s.rightX)) >= 8192 || std::abs(int(s.rightY)) >= 8192;
}
template<class State> bool stopForPhysical(const State& s) noexcept {
    if(!configured.load(std::memory_order_acquire))return false;
    if(!revoked.load(std::memory_order_acquire) && !physicalActivity(s))return false;
    if(!revoked.exchange(true,std::memory_order_acq_rel))
        std::fputs("APPLE_IOS_BOT_OWNER_TAKEOVER source=physical input_stopped=1\n",stderr);
    return true;
}
inline std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> out;
    for(std::size_t at=0;at<text.size();) {
        if(text[at]==' ' || text[at]=='\t' || text[at]=='\r' || text[at]=='\n'){++at;continue;}
        const auto begin=at;
        while(at<text.size() && text[at]!=' ' && text[at]!='\t' && text[at]!='\r' && text[at]!='\n')++at;
        out.push_back(text.substr(begin,at-begin));
    }
    return out;
}
template<class T> bool integer(std::string_view text,T& value) {
    const auto r=std::from_chars(text.data(),text.data()+text.size(),value);
    return r.ec==std::errc{} && r.ptr==text.data()+text.size();
}
inline bool hex(std::string_view value,std::size_t size) {
    if(value.size()!=size)return false;
    for(char c:value)if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return false;
    return true;
}
inline std::string sha256(std::string_view data) {
    unsigned char raw[CC_SHA256_DIGEST_LENGTH];
    if(!CC_SHA256(data.data(),static_cast<CC_LONG>(data.size()),raw))throw std::runtime_error("SHA256 failed");
    constexpr char digits[]="0123456789abcdef";std::string out;
    for(auto b:raw){out+=digits[b>>4];out+=digits[b&15];}return out;
}
inline bool validSteps(std::string_view raw) {
    if(raw.empty() || raw.size()>32768 || raw.back()!='\n')return false;
    unsigned rows=0;std::uint64_t total=0;
    for(std::size_t at=0;at<raw.size();) {
        const auto end=raw.find('\n',at);if(end==std::string_view::npos)return false;
        const auto fields=words(raw.substr(at,end-at));at=end+1;
        if(fields.size()!=8 || ++rows>256)return false;
        std::array<long long,8> v{};
        for(unsigned i=0;i<8;++i)if(!integer(fields[i],v[i]))return false;
        if(v[0]<0 || v[0]>65535 || v[1]<0 || v[1]>255 || v[2]<0 || v[2]>255 || v[7]<1 || v[7]>60000)return false;
        for(unsigned i=3;i<7;++i)if(v[i]<-32768 || v[i]>32767)return false;
        total+=std::uint64_t(v[7]);if(total>600000)return false;
    }
    return rows>0;
}
inline std::optional<std::string> readFile(int dir,const std::string& name,std::size_t limit) {
    const int fd=::openat(dir,name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return {};
    struct Close {int fd;~Close(){::close(fd);}} close{fd};
    struct stat before{},after{};
    if(::fstat(fd,&before) || !S_ISREG(before.st_mode) || before.st_nlink!=1 || before.st_size<0 || std::uint64_t(before.st_size)>limit)return {};
    std::string result;std::array<char,4096> buffer{};
    for(;;){const auto n=::read(fd,buffer.data(),buffer.size());if(n<0){if(errno==EINTR)continue;return {};}if(!n)break;
        result.append(buffer.data(),std::size_t(n));if(result.size()>limit)return {};}
    if(::fstat(fd,&after) || before.st_size!=after.st_size || result.size()!=std::size_t(before.st_size) ||
       before.st_mtimespec.tv_sec!=after.st_mtimespec.tv_sec || before.st_mtimespec.tv_nsec!=after.st_mtimespec.tv_nsec)return {};
    return result;
}
inline bool writeAtomic(int dir,const std::string& target,const std::string& temp,std::string_view raw) {
    const int fd=::openat(dir,temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return false;
    std::size_t at=0;bool ok=true;
    while(at<raw.size()){const auto n=::write(fd,raw.data()+at,raw.size()-at);if(n<0 && errno==EINTR)continue;if(n<=0){ok=false;break;}at+=std::size_t(n);}
    if(::close(fd))ok=false;
    if(!ok)return false; // retain failed temporary evidence
    return ::renameat(dir,temp.c_str(),dir,target.c_str())==0;
}
inline bool sandboxSwitchMatches(const std::string& directory,const std::string& session) {
    if(!hex(session,32))return false;
    const int dir=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(dir<0)return false;
    const auto value=readFile(dir,"diag-save-sandbox",256);::close(dir);
    // (Claude, 2026-10-16) also a seeded sandbox of the same name (free-roam tests start from a copied save; both isolate the saves)
    return value && (*value=="copy ios-bot-"+session+"\n" || *value=="reuse ios-bot-"+session+"\n");
}

// Runs on the app's main actor. The existing runtime retains its 16ms reader,
// device-clock sequence execution, and physical-pad OR/max behavior.
class CommitPublisher {
public:
    CommitPublisher(const std::string& directory,std::string session,bool sandboxReady,std::uint64_t owner)
      :session_(std::move(session)),owner_(owner) {
        if(!sandboxReady || !hex(session_,32) || configured.load())throw std::runtime_error("sandboxed exclusive iOS bot session required");
        dir_=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(dir_<0)throw std::runtime_error("private-trace directory unavailable");
        revoked.store(false);configured.store(true,std::memory_order_release);
        std::fprintf(stderr,"APPLE_IOS_BOT_READY session=%s sandbox_verified=1 input_synthetic=1\n",session_.c_str());
    }
    ~CommitPublisher(){stop();if(dir_>=0)::close(dir_);}
    CommitPublisher(const CommitPublisher&)=delete;
    CommitPublisher& operator=(const CommitPublisher&)=delete;
    void stop() noexcept {revoked.store(true,std::memory_order_release);}
    bool poll(std::uint64_t now,std::uint64_t owner,bool active) {
        if(owner!=owner_ || !active || revoked.load(std::memory_order_acquire)) {
            if(!closed_)std::fprintf(stderr,"APPLE_IOS_BOT_OWNER_TAKEOVER session=%s input_stopped=1\n",session_.c_str());
            closed_=true;stop();return false;
        }
        if(now<lastTime_){closed_=true;stop();return false;}
        lastTime_=now;if(now<next_)return false;next_=now+16;
        const auto marker=readFile(dir_,"diag-stick.commit",256);if(!marker)return false;
        const auto f=words(*marker);std::uint64_t sequence=0,size=0;
        if(f.size()!=5 || f[0]!="IOSBOT1" || f[1]!=session_ || !integer(f[2],sequence) || !sequence || sequence<=sequence_ ||
           !hex(f[3],64) || !integer(f[4],size) || !size || size>32768)return false;
        const std::string file="diag-stick."+session_+"."+std::to_string(sequence)+".tmp";
        const auto data=readFile(dir_,file,32768);
        if(!data || data->size()!=size || sha256(*data)!=f[3] || !validSteps(*data))return false;
        if(!writeAtomic(dir_,"diag-stick","diag-stick.publish-"+std::to_string(sequence),*data)) {
            closed_=true;stop();throw std::runtime_error("atomic iOS input publication failed");
        }
        sequence_=sequence;
        std::fprintf(stderr,"APPLE_IOS_BOT_PUBLISHED session=%s sequence=%llu sha256=%s bytes=%llu consumption_acknowledged=0\n",
            session_.c_str(),(unsigned long long)sequence,std::string(f[3]).c_str(),(unsigned long long)size);
        return true;
    }
private:
    int dir_=-1;std::string session_;std::uint64_t owner_=0,sequence_=0,lastTime_=0,next_=0;bool closed_=false;
};
}
