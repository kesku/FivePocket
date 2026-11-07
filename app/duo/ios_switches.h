// SPDX-License-Identifier: GPL-3.0-or-later
// ios-switches (Claude, 2026-10-16; iOS test runs only). The renderer's runtime switches are files in private-trace/, which
// the iOS shell cannot write. On Apple, this translation unit's access() goes through recompIosAccess(), which answers for a
// switch named in the launch environment and otherwise asks the file system as before:
//   RECOMP_IOS_SWITCHES_ON  = name,name,...   present for the whole run
//   RECOMP_IOS_SWITCHES_OFF = name,name,...   absent for the whole run
//   RECOMP_IOS_SWITCHES_AB  = name,name,...;P present during odd P-second periods of the run (in-run A/B: off, on, off, ...)
//   RECOMP_IOS_SWITCHES_AB_INV = name,name,...[;P] present during EVEN periods (on, off, on, ...; P from _AB when absent): the
//                            same factor in the other phase position, against the phase-position bias of drive runs
//   RECOMP_IOS_SWITCHES_LATE = name,name,...;S   present from S seconds after launch, and never on the first query of each
//                            name (absent before: a switch with a leftover rule, e.g. world_load.h's diag-wl-*, sees it absent
//                            first and counts it for the rest of the run, however late the first query comes)
// A name is the switch's file name (e.g. diag-v287-gpu-passes). Without these variables every call is a plain access().
#ifndef RECOMP_IOS_SWITCHES_FUNCS
#define RECOMP_IOS_SWITCHES_FUNCS
#if defined(__APPLE__)
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
namespace recomp::ios_switches {
struct Lists {std::vector<std::string> on,off,ab,abInv,late;double period=0,lateAfter=0;std::chrono::steady_clock::time_point t0=std::chrono::steady_clock::now();};
inline std::vector<std::string> split(const char* s) {
    std::vector<std::string> v;if(!s)return v;std::string cur;
    for(const char* p=s;;++p){if(!*p || *p==','){if(!cur.empty())v.push_back(cur);cur.clear();if(!*p)break;}else cur+=*p;}
    return v;
}
inline const Lists& lists() {
    static const Lists l=[]{Lists x;x.on=split(std::getenv("RECOMP_IOS_SWITCHES_ON"));x.off=split(std::getenv("RECOMP_IOS_SWITCHES_OFF"));
        if(const char* ab=std::getenv("RECOMP_IOS_SWITCHES_AB")) {std::string s(ab);const auto k=s.find(';');
            x.ab=split(s.substr(0,k).c_str());if(k!=std::string::npos)x.period=std::atof(s.c_str()+k+1);}
        if(const char* ai=std::getenv("RECOMP_IOS_SWITCHES_AB_INV")) {std::string s(ai);const auto k=s.find(';');
            x.abInv=split(s.substr(0,k).c_str());if(k!=std::string::npos && x.period<=0)x.period=std::atof(s.c_str()+k+1);}
        if(x.period<=0){x.ab.clear();x.abInv.clear();}
        if(const char* la=std::getenv("RECOMP_IOS_SWITCHES_LATE")) {std::string s(la);const auto k=s.find(';');
            x.late=split(s.substr(0,k).c_str());if(k!=std::string::npos)x.lateAfter=std::atof(s.c_str()+k+1);if(x.lateAfter<=0)x.late.clear();}
        return x;}();
    return l;
}
// (2026-10-21) per late name: has it been asked once (see recompIosAccess)
constexpr std::size_t MaxLate=16;
inline std::atomic<bool>* lateAsked(){static std::atomic<bool> a[MaxLate]{};return a;}
// A synthetic "no such file" (errno ENOENT, as a real lookup of a missing file: world_load.h counts a switch as seen absent only then)
inline int absent(){errno=ENOENT;return -1;}
inline bool listed(const std::vector<std::string>& v,const char* name) {for(const auto& s:v)if(s==name)return true;return false;}
// The A/B phase now (0 = off period, 1 = on period); also logged by the renderer's timing line on iOS.
inline int abPhase() {
    const auto& l=lists();if(l.ab.empty() && l.abInv.empty())return -1;
    const double t=std::chrono::duration<double>(std::chrono::steady_clock::now()-l.t0).count();
    return int(t/l.period)&1;
}
// ios-access-cache (Claude 2026-10-23, gt1; v3 after gt3 and gt5): the renderer worker polls ~200 switch files with access() every
// 30/60 presents; on the iPad one access() costs ~40 us (sandbox path checks): ~8 ms per poll on the presenting worker, and 65 % of
// the worker's access() time fell in the 6.5 % of frames over 40 ms (gt1 frame-classed sampler). With the switch ios-access-cache
// (read from the launch lists only: no syscall), an F_OK query for a SWITCH name ("diag-..." / "ios-...", lower case, digits and
// '-' only; never "diag-stick", the bot's input file) directly in a ".../private-trace" directory is answered from the last real
// answer for that name while it is younger than 3 s (success or ENOENT only). The switch files are not written on iOS during a run
// (the shell cannot; the harness sets switches through the launch lists), and the polls run every 1-2 s, so a switch file created
// mid-run would be seen at most 3 s late. History: v1 (gt3/gt4) listed the directory: the device's private-trace holds > 4096
// entries, the listing was refused and re-read on every call (6 ms per call, frames 55-83 ms). v2 (gt5/gt6) kept answers while the
// directory's mtime was unchanged: the bot's CommitPublisher writes into private-trace continuously, so it never engaged.
// APPLE_IOS_ACCESS (every 5 s, MEASUREMENT): calls, answered from the cache, real lookups stored, ms inside access, phase.
constexpr std::size_t AccessCacheMax=1024; // names
struct AccessCache {std::mutex m;std::unordered_map<std::string,std::pair<int,std::chrono::steady_clock::time_point>> names;};
inline AccessCache& accessCache(){static AccessCache c;return c;}
struct AccessStats {std::atomic<std::uint64_t> calls{0},cached{0},ns{0},refresh{0};std::atomic<std::int64_t> lastLogNs{0};};
inline AccessStats& accessStats(){static AccessStats s;return s;}
inline bool switchName(const char* n) {
    if(std::strncmp(n,"diag-",5)!=0 && std::strncmp(n,"ios-",4)!=0)return false;
    if(std::strcmp(n,"diag-stick")==0)return false;
    for(const char* c=n;*c;++c)if(!((*c>='a' && *c<='z') || (*c>='0' && *c<='9') || *c=='-'))return false;
    return true;
}
inline bool listSwitch(const char* name) { // a switch answered from the launch lists alone (no file system)
    const auto& l=lists();
    if(listed(l.on,name))return true;
    if(listed(l.off,name))return false;
    if(listed(l.ab,name))return abPhase()==1;
    if(listed(l.abInv,name))return abPhase()==0;
    return false;
}
// 1 = answered (*result set; errno ENOENT when absent), 0 = not this cache's query (the caller asks the file system)
inline int cachedAccess(const char* path,int mode,int* result) {
    static constexpr char Tail[]="/private-trace";constexpr std::size_t tl=sizeof Tail-1;
    if(mode!=F_OK || !path)return 0;
    const char* slash=std::strrchr(path,'/');
    if(!slash || !slash[1] || !switchName(slash+1))return 0;
    const std::size_t dl=std::size_t(slash-path);
    if(dl<tl || std::memcmp(slash-tl,Tail,tl)!=0)return 0;
    const auto now=std::chrono::steady_clock::now();
    const std::string key(path); // (the full path: one private-trace directory in practice, any number allowed)
    auto& c=accessCache();
    {
        std::lock_guard<std::mutex> g(c.m);
        if(const auto it=c.names.find(key);it!=c.names.end() && now-it->second.second<std::chrono::seconds(3)) {
            if(it->second.first)errno=ENOENT;*result=it->second.first;return 1;
        }
    }
    const int r=::access(path,mode)==0?0:-1;const int e=errno; // (outside the lock: ~40 us on the iPad)
    if(r==0 || e==ENOENT) {
        std::lock_guard<std::mutex> g(c.m);
        if(c.names.size()<AccessCacheMax || c.names.count(key)){c.names[key]={r,now};accessStats().refresh.fetch_add(1,std::memory_order_relaxed);}
    }
    errno=e;*result=r;return 1;
}
inline int cachedOrRealAccess(const char* path,int mode) {
    auto& s=accessStats();
    const auto t=std::chrono::steady_clock::now();
    int r=0;const bool hit=listSwitch("ios-access-cache") && cachedAccess(path,mode,&r);
    if(!hit)r=::access(path,mode);
    const int saved=errno;
    const auto e=std::chrono::steady_clock::now();
    s.calls.fetch_add(1,std::memory_order_relaxed);if(hit)s.cached.fetch_add(1,std::memory_order_relaxed);
    s.ns.fetch_add(std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(e-t).count()),std::memory_order_relaxed);
    const std::int64_t en=std::chrono::duration_cast<std::chrono::nanoseconds>(e.time_since_epoch()).count();
    std::int64_t last=s.lastLogNs.load(std::memory_order_relaxed);
    if(!last)s.lastLogNs.compare_exchange_strong(last,en,std::memory_order_relaxed);
    else if(en-last>=5'000'000'000ll && s.lastLogNs.compare_exchange_strong(last,en,std::memory_order_relaxed)) {
        std::fprintf(stderr,"APPLE_IOS_ACCESS phase=%d calls=%llu cached=%llu refresh=%llu ms=%.3f\n",abPhase(),
            (unsigned long long)s.calls.exchange(0),(unsigned long long)s.cached.exchange(0),(unsigned long long)s.refresh.exchange(0),s.ns.exchange(0)/1e6);
    }
    errno=saved;
    return r;
}
inline int recompIosAccess(const char* path,int mode) {
    const auto& l=lists();
    if(path && (!l.on.empty() || !l.off.empty() || !l.ab.empty() || !l.abInv.empty() || !l.late.empty())) {
        const char* slash=std::strrchr(path,'/');const char* name=slash?slash+1:path;
        if(listed(l.on,name))return 0;
        if(listed(l.off,name))return absent();
        if(listed(l.ab,name))return abPhase()==1?0:absent();
        if(listed(l.abInv,name))return abPhase()==0?0:absent();
        // (2026-10-21) a late switch answers its FIRST query for each name with "absent" whatever the time: c450's cold launch
        // (the first after an install) first polled world_load.h after the 60 s mark, so diag-wl-lod-unlock was never seen
        // absent, counted as a leftover for the whole run, and the 70 % draw distance never applied (lod=100, leftover=128).
        for(std::size_t k=0;k<l.late.size() && k<MaxLate;++k)if(l.late[k]==name) {
            if(!lateAsked()[k].exchange(true,std::memory_order_relaxed))return absent();
            return std::chrono::duration<double>(std::chrono::steady_clock::now()-l.t0).count()>=l.lateAfter?0:absent();
        }
    }
    return cachedOrRealAccess(path,mode);
}
}
// (a global-namespace name, so a "::access(...)" call site expands to a valid qualified call)
inline int recomp_ios_switches_access(const char* path,int mode){return ::recomp::ios_switches::recompIosAccess(path,mode);}
#else
#include <unistd.h>
inline int recomp_ios_switches_access(const char* path,int mode){return ::access(path,mode);}
#endif
#endif // RECOMP_IOS_SWITCHES_FUNCS
// The macro: every access() after this line in the including file goes through the overrides. A header that is also included
// by files with other "access" names (direct_present.h: trace_runner.cpp) defines RECOMP_IOS_SWITCHES_NO_MACRO and calls
// recomp_ios_switches_access() by name instead.
#if defined(__APPLE__) && !defined(RECOMP_IOS_SWITCHES_NO_MACRO) && !defined(access)
#define access(path,mode) recomp_ios_switches_access(path,mode)
#endif
