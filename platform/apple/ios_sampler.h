// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-sampler (Claude, 2026-10-16; MEASUREMENT ONLY, off unless RECOMP_IOS_SAMPLER is set). Instruments cannot attach to the
// iPad here, so this is the CPU profiler: one thread that, every INTERVAL_US for DURATION_S starting START_S after launch,
// suspends each RUNNING thread of this process in turn, reads its pc/lr and frame-pointer chain (vm_read_overwrite: a bad
// frame pointer ends the walk instead of faulting), resumes it, and only then counts the stack (no allocation while a thread
// is suspended: it may hold the malloc lock). At the end it writes <home>/Library/r/private-trace/sampler-<pid>.txt:
//   SAMPLER slide=<main image slide> header=<addr> samples=<n> ticks=<n> interval_us=<n>
//   <count> <thread name or tid> <pc> <lr> <fp-return 1> ... (up to 24 frames; hex)
// RECOMP_IOS_SAMPLER = "START_S,DURATION_S,INTERVAL_US[,ALL[,FRAMECLASS]]" (e.g. "100,20,1000"; FRAMECLASS: see run()). ALL=1: waiting threads too, their name
// prefixed "W:" (running ones "R:"): a wall-clock profile that shows where a thread sleeps or waits.
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <map>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>
extern "C" std::atomic<std::uint64_t> recomp_ios_last_present_ns;
extern "C" std::atomic<unsigned> recomp_ios_app_inactive; // (ios_frame_presenter.mm, 2026-10-21 c485b) 1 while the app is not active
extern "C" unsigned recomp_ios_guest_read(std::uint32_t address,std::uint8_t* out,unsigned size) noexcept; // (guest_cooperative_executor.cpp)
extern "C" int recomp_ios_describe_handle(std::uint32_t handle,char* out,std::size_t size) noexcept; // (guest_import_registry.cpp)
extern "C" unsigned recomp_ios_wait_slots(std::uint32_t* tokens,std::uint32_t* objects,std::uint64_t* waitedNs,std::uint8_t (*head)[16],unsigned max) noexcept; // (guest_cooperative_executor.cpp) // (stall sampler: the renderer's present hook, steady-clock ns)
namespace recomp::ios_sampler {
constexpr int MaxFrames=24;
struct Key {
    std::string name; std::uint64_t pc[MaxFrames+2]; int n;
    bool operator<(const Key& o) const {
        if(name!=o.name)return name<o.name;
        if(n!=o.n)return n<o.n;
        return std::memcmp(pc,o.pc,sizeof(std::uint64_t)*n)<0;
    }
};
inline bool readWord(std::uint64_t addr,std::uint64_t& out) {
    vm_size_t got=0;
    return addr && !(addr&7) &&
        vm_read_overwrite(mach_task_self(),vm_address_t(addr),sizeof out,vm_address_t(&out),&got)==KERN_SUCCESS && got==sizeof out;
}
// One sample of this process's threads (running ones, or all with `all`): the sampler's tick, shared with the stall
// sampler (Claude 2026-10-21). False when the thread list could not be read.
inline bool snapshot(thread_act_t self,bool all,std::map<Key,unsigned>& counts,std::uint64_t& samples) {
    thread_act_array_t threads=nullptr;mach_msg_type_number_t count=0;
    if(task_threads(mach_task_self(),&threads,&count)!=KERN_SUCCESS)return false;
    for(mach_msg_type_number_t i=0;i<count;++i) {
        const thread_act_t t=threads[i];
        if(t==self){mach_port_deallocate(mach_task_self(),t);continue;}
        thread_basic_info_data_t info{};mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;
        const bool running=thread_info(t,THREAD_BASIC_INFO,thread_info_t(&info),&n)==KERN_SUCCESS &&
            info.run_state==TH_STATE_RUNNING && !(info.flags&TH_FLAGS_IDLE);
        const bool waiting=all && !running;
        if(running || waiting) {
            char name[64]={0};qos_class_t qos=QOS_CLASS_UNSPECIFIED;int rel=0;
            if(const pthread_t p=pthread_from_mach_thread_np(t)){pthread_getname_np(p,name,sizeof name);pthread_get_qos_class_np(p,&qos,&rel);}
            Key k{};k.n=0;
            if(thread_suspend(t)==KERN_SUCCESS) {
                arm_thread_state64_t st{};mach_msg_type_number_t sn=ARM_THREAD_STATE64_COUNT;
                if(thread_get_state(t,ARM_THREAD_STATE64,thread_state_t(&st),&sn)==KERN_SUCCESS) {
                    k.pc[k.n++]=arm_thread_state64_get_pc(st);k.pc[k.n++]=arm_thread_state64_get_lr(st);
                    std::uint64_t fp=arm_thread_state64_get_fp(st);
                    for(int f=0;f<MaxFrames && fp;++f) {
                        std::uint64_t next=0,ret=0;
                        if(!readWord(fp,next) || !readWord(fp+8,ret) || !ret)break;
                        k.pc[k.n++]=ret&0x0000000FFFFFFFFFull;
                        if(next<=fp)break;
                        fp=next;
                    }
                }
                thread_resume(t);
            }
            if(k.n) {
                if(!name[0]){std::uint64_t id=0;if(const pthread_t p=pthread_from_mach_thread_np(t))pthread_threadid_np(p,&id);
                    std::snprintf(name,sizeof name,"tid%llu",(unsigned long long)id);}
                for(char* c=name;*c;++c)if(*c==' ')*c='_';
                // the thread's QoS class as a suffix (@ui user-interactive, @in user-initiated, @df default, @ut utility,
                // @bg background, @un unspecified): iOS places threads on performance or efficiency cores by it
                const char* q=qos==QOS_CLASS_USER_INTERACTIVE?"@ui":qos==QOS_CLASS_USER_INITIATED?"@in":qos==QOS_CLASS_DEFAULT?"@df":
                              qos==QOS_CLASS_UTILITY?"@ut":qos==QOS_CLASS_BACKGROUND?"@bg":"@un";
                k.name=(all?std::string(running?"R:":"W:"):std::string())+name+q;++counts[k];++samples;
            }
        }
        mach_port_deallocate(mach_task_self(),t);
    }
    vm_deallocate(mach_task_self(),vm_address_t(threads),count*sizeof(thread_act_t));
    return true;
}
// Writes <home>/Library/r/private-trace/<file> in the sampler's format (`extra` follows the header fields).
inline void write(const std::string& file,const std::map<Key,unsigned>& counts,std::uint64_t samples,std::uint64_t ticks,unsigned intervalUs,const std::string& extra) {
    const char* home=std::getenv("HOME");if(!home)return;
    const std::string path=std::string(home)+"/Library/r/private-trace/"+file;
    if(FILE* f=std::fopen(path.c_str(),"w")) {
        std::fprintf(f,"SAMPLER slide=%llx header=%llx samples=%llu ticks=%llu interval_us=%u%s\n",
            (unsigned long long)_dyld_get_image_vmaddr_slide(0),(unsigned long long)(uintptr_t)_dyld_get_image_header(0),
            (unsigned long long)samples,(unsigned long long)ticks,intervalUs,extra.c_str());
        for(const auto& [k,c]:counts) {
            std::fprintf(f,"%u %s",c,k.name.c_str());
            for(int i=0;i<k.n;++i)std::fprintf(f," %llx",(unsigned long long)k.pc[i]);
            std::fputc('\n',f);
        }
        std::fclose(f);
        std::fprintf(stderr,"APPLE_IOS_SAMPLER written=%s samples=%llu ticks=%llu stacks=%zu\n",path.c_str(),
            (unsigned long long)samples,(unsigned long long)ticks,counts.size());
    }
}
inline void run(double start,double duration,unsigned intervalUs,bool all,bool frameClass=false) {
    pthread_setname_np("ios-sampler");
    std::this_thread::sleep_for(std::chrono::duration<double>(start));
    const thread_act_t self=mach_thread_self();
    std::map<Key,unsigned> counts;std::uint64_t samples=0,ticks=0;
    // ios-sampler-frameclass (Claude 2026-10-23; MEASUREMENT ONLY, 5th field = 1): each tick's samples wait in `pending` until the
    // next game present (recomp_ios_last_present_ns, the render hook, once per real frame); the frame's length then prefixes their
    // thread name with its class: "fA|" <= 34.5 ms, "fB|" <= 40 ms, "fC|" > 40 ms. The header adds frames/ms per class, so a
    // function's ms per frame in a class = samples * interval / frames: what grows in the over-budget frames.
    std::map<Key,unsigned> pending;std::uint64_t pendingSamples=0,lastPresent=0;
    std::uint64_t frames[3]={0,0,0},frameNs[3]={0,0,0};
    const auto end=std::chrono::steady_clock::now()+std::chrono::duration<double>(duration);
    while(std::chrono::steady_clock::now()<end) {
        ++ticks;
        if(!snapshot(self,all,frameClass?pending:counts,frameClass?pendingSamples:samples))break;
        if(frameClass) {
            const std::uint64_t p=recomp_ios_last_present_ns.load(std::memory_order_relaxed);
            if(p!=lastPresent) {
                if(lastPresent && !recomp_ios_app_inactive.load(std::memory_order_relaxed)) {
                    const std::uint64_t len=p-lastPresent;const int c=len<=34'500'000ull?0:len<=40'000'000ull?1:2;
                    ++frames[c];frameNs[c]+=len;
                    static const char* tag[3]={"fA|","fB|","fC|"};
                    for(auto& [k,n]:pending){Key q=k;q.name=std::string(tag[c])+k.name;counts[q]+=n;}
                    samples+=pendingSamples;
                }
                pending.clear();pendingSamples=0;lastPresent=p;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(intervalUs));
    }
    mach_port_deallocate(mach_task_self(),self);
    char extra[256]="";
    if(frameClass)std::snprintf(extra,sizeof extra," frameclass=1 framesA=%llu msA=%.1f framesB=%llu msB=%.1f framesC=%llu msC=%.1f",
        (unsigned long long)frames[0],frameNs[0]/1e6,(unsigned long long)frames[1],frameNs[1]/1e6,(unsigned long long)frames[2],frameNs[2]/1e6);
    write("sampler-"+std::to_string(getpid())+".txt",counts,samples,ticks,intervalUs,extra);
}
// ios-stall-sampler (Claude 2026-10-21; MEASUREMENT ONLY, off unless RECOMP_IOS_STALL_SAMPLER is set): c395's 313 ms freeze had
// every thread waiting. A watchdog thread checks the time since the last present (recomp_ios_last_present_ns, the renderer's
// present hook) every 20 ms; when a gap passes THRESH_MS it takes up to 5 all-thread snapshots (waiting threads too, 30 ms
// apart) while the gap lasts, then writes stall-<pid>-<n>.txt (header: gap_ms=) and logs APPLE_IOS_STALL. At most MAX
// stalls per run. RECOMP_IOS_STALL_SAMPLER = "THRESH_MS,MAX[,STEADY_S]" (e.g. "150,6,20"). c396 spent all six captures on
// the loading screen (gaps of ~340 ms every second before the first gameplay frame), so the watcher arms only once presents
// have run STEADY_S seconds (default 20) without a gap over THRESH_MS; the arm logs APPLE_IOS_STALL steady=.
inline std::uint64_t nowNs() {return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline void stallWatch(unsigned threshMs,unsigned maxStalls,unsigned steadyS) {
    pthread_setname_np("ios-stall-sampler");
    const thread_act_t self=mach_thread_self();unsigned stalls=0;
    std::uint64_t steadySince=0;bool armed=steadyS==0;
    while(stalls<maxStalls) {
        std::this_thread::sleep_for(std::chrono::milliseconds(threshMs<100?5:20)); // (a 55 ms threshold needs a finer poll)
        const std::uint64_t last=recomp_ios_last_present_ns.load(std::memory_order_relaxed);
        if(!last)continue;
        const std::uint64_t now=nowNs();std::uint64_t gap=now-last;
        if(recomp_ios_app_inactive.load(std::memory_order_relaxed)) { // (c485b) a lifecycle pause is not a stall: stand down, and after the
            if(armed && steadyS)std::fprintf(stderr,"APPLE_IOS_STALL inactive=1 armed=0\n"); // resume re-arm only after STEADY_S of presents
            armed=steadyS==0;steadySince=0;continue;
        }
        if(!armed) {
            if(gap>=std::uint64_t(std::max(threshMs,150u))*1'000'000ull || !steadySince){steadySince=now;continue;} // (loading: 150 ms gaps reset)
            if(now-steadySince<std::uint64_t(steadyS)*1'000'000'000ull)continue;
            armed=true;std::fprintf(stderr,"APPLE_IOS_STALL steady=%u armed=1\n",steadyS);
        }
        if(gap<std::uint64_t(threshMs)*1'000'000ull)continue;
        std::map<Key,unsigned> counts;std::uint64_t samples=0,ticks=0;
        const unsigned spacingMs=std::clamp(threshMs/5,5u,30u); // (c403: 50-66 ms gaps need a tighter spacing than 30 ms)
        for(int k=0;k<5;++k) {
            if(recomp_ios_last_present_ns.load(std::memory_order_relaxed)!=last)break; // the gap ended
            if(!snapshot(self,true,counts,samples))break;++ticks;
            std::this_thread::sleep_for(std::chrono::milliseconds(spacingMs));
        }
        if(recomp_ios_app_inactive.load(std::memory_order_relaxed)){armed=steadyS==0;steadySince=0;continue;} // (c485b) the pause began mid-capture
        gap=(recomp_ios_last_present_ns.load(std::memory_order_relaxed)!=last?recomp_ios_last_present_ns.load(std::memory_order_relaxed):nowNs())-last;
        ++stalls;
        write("stall-"+std::to_string(getpid())+"-"+std::to_string(stalls)+".txt",counts,samples,ticks,spacingMs*1000u," gap_ms="+std::to_string(gap/1'000'000ull));
        std::fprintf(stderr,"APPLE_IOS_STALL n=%u gap_ms=%llu snapshots=%llu\n",stalls,(unsigned long long)(gap/1'000'000ull),(unsigned long long)ticks);
        if(const char* mem=std::getenv("RECOMP_IOS_STALL_MEM")) { // ios-stall-mem: "HEXADDR:LEN,..." raw guest bytes at each capture
            std::string spec(mem);std::size_t at=0;
            while(at<spec.size()) {
                const auto comma=spec.find(',',at);const std::string item=spec.substr(at,comma==std::string::npos?std::string::npos:comma-at);
                at=comma==std::string::npos?spec.size():comma+1;
                unsigned a=0,len=0;if(std::sscanf(item.c_str(),"%x:%u",&a,&len)!=2 || !len || len>256)continue;
                std::uint8_t buf[256];const unsigned got=recomp_ios_guest_read(a,buf,len);
                char hex[513]={0};for(unsigned b=0;b<got;++b)std::snprintf(hex+2*b,3,"%02x",buf[b]);
                std::fprintf(stderr,"APPLE_IOS_STALL_MEM n=%u addr=%08x len=%u bytes=%s\n",stalls,a,got,hex);
            }
        }
        { // ios-wait-slots: what each waiting guest thread waits on (object; first 16 bytes when a guest address: type, signal state)
            static std::uint32_t tk[160],ob[160];static std::uint64_t ws[160];static std::uint8_t hd[160][16];
            const unsigned n=recomp_ios_wait_slots(tk,ob,ws,hd,160);
            for(unsigned i=0;i<n;++i) {
                char hex[33];for(int b=0;b<16;++b)std::snprintf(hex+2*b,3,"%02x",hd[i][b]);
                char state[96]="-";if(ob[i] && ob[i]<0x10000u)recomp_ios_describe_handle(ob[i],state,sizeof state); // NT handle: the registry's object
                std::fprintf(stderr,"APPLE_IOS_STALL_WAIT n=%u token=%08x object=%08x waited_ms=%llu head=%s state=%s\n",stalls,tk[i],ob[i],
                             (unsigned long long)(ws[i]/1'000'000ull),hex,state);
            }
        }
        while(recomp_ios_last_present_ns.load(std::memory_order_relaxed)==last)std::this_thread::sleep_for(std::chrono::milliseconds(20)); // one capture per gap
    }
    mach_port_deallocate(mach_task_self(),self);
}
inline void startFromEnvironment() {
    const char* spec=std::getenv("RECOMP_IOS_SAMPLER");if(!spec || !*spec)return;
    double start=100,duration=20;unsigned interval=1000,all=0,frameClass=0;
    if(std::sscanf(spec,"%lf,%lf,%u,%u,%u",&start,&duration,&interval,&all,&frameClass)<2 || duration<=0 || interval<100)return;
    std::fprintf(stderr,"APPLE_IOS_SAMPLER armed start=%.1f duration=%.1f interval_us=%u all=%u frameclass=%u\n",start,duration,interval,all,frameClass);
    std::thread([=]{run(start,duration,interval,all!=0,frameClass!=0);}).detach();
}
inline void startStallFromEnvironment() {
    const char* spec=std::getenv("RECOMP_IOS_STALL_SAMPLER");if(!spec || !*spec)return;
    unsigned thresh=150,maxStalls=6,steady=20;
    if(std::sscanf(spec,"%u,%u,%u",&thresh,&maxStalls,&steady)<1 || thresh<50)return;
    std::fprintf(stderr,"APPLE_IOS_STALL armed thresh_ms=%u max=%u steady_s=%u\n",thresh,maxStalls,steady);
    std::thread([=]{stallWatch(thresh,maxStalls,steady);}).detach();
}
}
#endif
