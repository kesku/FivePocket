// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Diagnostic-only, explicit run configuration. Never resumes/restarts a game.
// Captures user stacks one thread at a time before presenter failure unwinds.
// Temporary thread suspension is recorded; kernel stacks/true limit stay unknown.
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/mach_time.h>
#include <mach/arm/thread_status.h>
#include <os/proc.h>
#include <execinfo.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <ptrauth.h>
namespace iosfailure {
constexpr unsigned MaxThreads=128,MaxFrames=64;
struct Thread {
    std::uint64_t id=0,begin=0,end=0,pc=0,sp=0,fp=0;
    int identify=-1,basic=-1,state=-1,suspend=-1,resume=-1,nameResult=-1;
    unsigned stateCount=0,basicCount=0,frames=0; int walkEnd=0;bool self=false,truncated=false;
    thread_basic_info_data_t info{};char name[64]{};
    std::array<std::uintptr_t,MaxFrames> stack{};
};
struct Snapshot {
    std::uint64_t begin=0,end=0,footprint=0,resident=0,headroom=0;
    int enumeration=-1,memory=-1,thermal=-1,portCleanupFailures=0,storageCleanup=-1;
    unsigned returned=0,count=0,memoryCount=0;bool truncated=false,resumeFailed=false;
    std::array<Thread,MaxThreads> threads{};
};
inline std::atomic<bool> enabled{false},captured{false};
inline int directory=-1;
inline char runId[33]{};
inline std::uint64_t now() noexcept {return mach_continuous_time();}
inline bool validSession(const char* s) noexcept {
    if(!s || std::strlen(s)!=32)return false;
    for(unsigned i=0;i<32;++i)if(!((s[i]>='a'&&s[i]<='f')||(s[i]>='0'&&s[i]<='9')))return false;
    return true;
}
inline bool configure(const char* path,const char* session) noexcept {
    if(enabled.load() || !validSession(session))return false;
    const int fd=::open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return false;
    char name[64]{};std::snprintf(name,sizeof(name),"failure-%s",session);
    if(mkdirat(fd,name,0700)!=0){close(fd);return false;}
    const int owned=openat(fd,name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(fd);
    if(owned<0)return false;
    std::memcpy(runId,session,32);directory=owned;captured.store(false);enabled.store(true,std::memory_order_release);return true;
}
struct MachApi {
    static int deallocate(mach_port_t p) noexcept {return mach_port_deallocate(mach_task_self(),p);}
    static int storage(thread_act_array_t p,mach_msg_type_number_t n) noexcept {return vm_deallocate(mach_task_self(),reinterpret_cast<vm_address_t>(p),n*sizeof(thread_t));}
    static int suspend(thread_t t) noexcept {return thread_suspend(t);}
    static int resume(thread_t t) noexcept {return thread_resume(t);}
    static int state(thread_t t,arm_thread_state64_t& r,mach_msg_type_number_t& n) noexcept {return thread_get_state(t,ARM_THREAD_STATE64,reinterpret_cast<thread_state_t>(&r),&n);}
    static int read(std::uintptr_t fp,std::uintptr_t (&pair)[2],vm_size_t& n) noexcept {return vm_read_overwrite(mach_task_self(),fp,sizeof(pair),reinterpret_cast<vm_address_t>(pair),&n);}
};
template<class Api=MachApi> inline void collect(Snapshot& out) noexcept {
    out.begin=now();
    out.thermal=int(NSProcessInfo.processInfo.thermalState);
    task_vm_info_data_t vm{};mach_msg_type_number_t n=TASK_VM_INFO_COUNT;
    out.memory=task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&vm),&n);out.memoryCount=n;
    if(out.memory==KERN_SUCCESS && n*sizeof(natural_t)>=offsetof(task_vm_info_data_t,phys_footprint)+sizeof(vm.phys_footprint)) {
        out.footprint=vm.phys_footprint;out.resident=vm.resident_size;
    }
#if TARGET_OS_IOS
    out.headroom=os_proc_available_memory();
#endif
    thread_act_array_t ports=nullptr;mach_msg_type_number_t count=0;
    out.enumeration=task_threads(mach_task_self(),&ports,&count);out.returned=count;
    const thread_t self=mach_thread_self();
    if(out.enumeration==KERN_SUCCESS && ports) {
        out.count=std::min<unsigned>(count,MaxThreads);out.truncated=count>MaxThreads;
        // No file I/O, Foundation calls, or heap allocation while a target is suspended.
        for(unsigned i=0;i<out.count;++i) {
            auto& t=out.threads[i];t.begin=now();t.self=ports[i]==self;
            thread_identifier_info_data_t ident{};n=THREAD_IDENTIFIER_INFO_COUNT;
            t.identify=thread_info(ports[i],THREAD_IDENTIFIER_INFO,reinterpret_cast<thread_info_t>(&ident),&n);t.id=ident.thread_id;
            n=THREAD_BASIC_INFO_COUNT;t.basic=thread_info(ports[i],THREAD_BASIC_INFO,reinterpret_cast<thread_info_t>(&t.info),&n);t.basicCount=n;
            thread_extended_info_data_t extended{};n=THREAD_EXTENDED_INFO_COUNT;
            t.nameResult=thread_info(ports[i],THREAD_EXTENDED_INFO,reinterpret_cast<thread_info_t>(&extended),&n);
            if(t.nameResult==KERN_SUCCESS)std::memcpy(t.name,extended.pth_name,std::min(sizeof(t.name)-1,sizeof(extended.pth_name)));
            if(t.self) {
                void* addresses[MaxFrames]{};
                const int frames=::backtrace(addresses,MaxFrames);t.frames=frames>0?unsigned(frames):0;
                for(unsigned j=0;j<t.frames;++j)t.stack[j]=reinterpret_cast<std::uintptr_t>(addresses[j]);
                t.state=-1;t.truncated=t.frames==MaxFrames;t.walkEnd=1;
            } else {
                t.suspend=Api::suspend(ports[i]);
                if(t.suspend==KERN_SUCCESS) {
                    arm_thread_state64_t regs{};n=ARM_THREAD_STATE64_COUNT;
                    t.state=Api::state(ports[i],regs,n);t.stateCount=n;
                    if(t.state==KERN_SUCCESS && n>=ARM_THREAD_STATE64_COUNT) {
                        t.pc=arm_thread_state64_get_pc(regs);t.sp=arm_thread_state64_get_sp(regs);t.fp=arm_thread_state64_get_fp(regs);
                        t.stack[t.frames++]=t.pc;
                        auto fp=t.fp;
                        while(t.frames<MaxFrames && fp>=t.sp && fp-t.sp<16*1024*1024 && !(fp&15)) {
                            std::uintptr_t pair[2]{};vm_size_t copied=0;
                            if(Api::read(fp,pair,copied)!=KERN_SUCCESS || copied!=sizeof(pair)){t.walkEnd=2;break;}
                            if(pair[1])t.stack[t.frames++]=reinterpret_cast<std::uintptr_t>(ptrauth_strip(reinterpret_cast<void*>(pair[1]),ptrauth_key_return_address));
                            if(pair[0]<=fp || pair[0]-fp>1024*1024){t.walkEnd=3;break;}
                            fp=pair[0];
                        }
                        t.truncated=t.frames==MaxFrames;if(t.truncated)t.walkEnd=4;
                    }
                    // Always balance a successful suspension, including state/read failures.
                    t.resume=Api::resume(ports[i]);
                    if(t.resume!=KERN_SUCCESS && t.resume!=KERN_TERMINATED){out.resumeFailed=true;out.count=i+1;out.truncated=true;t.end=now();break;}
                }
            }
            t.end=now();
        }
        for(unsigned i=0;i<count;++i)if(Api::deallocate(ports[i])!=KERN_SUCCESS)++out.portCleanupFailures;
        out.storageCleanup=Api::storage(ports,count);
    }
    if(Api::deallocate(self)!=KERN_SUCCESS)++out.portCleanupFailures;
    out.end=now();
}
inline void frameFields(char* out,std::size_t size,std::uint64_t frames,bool rendererFrames) noexcept {
    if(rendererFrames)std::snprintf(out,size,"\"accepted_frames\":null,\"renderer_frames\":%llu",static_cast<unsigned long long>(frames));
    else std::snprintf(out,size,"\"accepted_frames\":%llu",static_cast<unsigned long long>(frames));
}
template<class Api=MachApi> inline void capture(const char* operation,int code,std::uint64_t frames,bool rendererFrames=false) noexcept {
    if(!enabled.load(std::memory_order_acquire) || captured.exchange(true))return;
    // Preallocate before any suspension. A single snapshot is under100KiB.
    Snapshot snapshot{};collect<Api>(snapshot);
    if(snapshot.resumeFailed) {
        // Do not enter stdio/heap locks that a suspended thread might own.
        const int raw=openat(directory,"resume-failure.raw",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(raw>=0){const char* p=reinterpret_cast<const char*>(&snapshot);std::size_t remaining=sizeof(snapshot);
            while(remaining){const auto n=::write(raw,p,remaining);if(n<0 && errno==EINTR)continue;if(n<=0)break;p+=n;remaining-=std::size_t(n);}::close(raw);}
        constexpr char message[]="APPLE_IOS_FAILURE_CAPTURE_FATAL resume_failed=1 process_exit=74\n";
        (void)::write(STDERR_FILENO,message,sizeof(message)-1);std::_Exit(74);
    }
    const int fd=openat(directory,"presenter-failure.json",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0){std::fputs("APPLE_IOS_FAILURE_CAPTURE write_open_failed=1\n",stderr);return;}
    FILE* f=fdopen(fd,"w");if(!f){close(fd);std::fputs("APPLE_IOS_FAILURE_CAPTURE fdopen_failed=1\n",stderr);return;}
    mach_timebase_info_data_t clock{};const int clockResult=mach_timebase_info(&clock);
    char frameFieldsText[96]{};frameFields(frameFieldsText,sizeof(frameFieldsText),frames,rendererFrames);
    std::fprintf(f,"{\"schema\":\"ios-presenter-failure/1\",\"run\":\"%s\",\"operation\":\"%s\",\"vk\":%d,%s,\"pid\":%d,\"begin_ticks\":%llu,\"end_ticks\":%llu,\"clock_result\":%d,\"numer\":%u,\"denom\":%u,\"thermal\":%d,\"memory_result\":%d,\"memory_count\":%u,\"footprint\":%llu,\"resident\":%llu,\"dynamic_headroom\":%llu,\"exact_memory_limit\":null,\"kernel_stacks\":null,\"enumeration_result\":%d,\"returned\":%u,\"truncated\":%s,\"threads\":[",
        runId,operation,code,frameFieldsText,getpid(),snapshot.begin,snapshot.end,clockResult,clock.numer,clock.denom,snapshot.thermal,snapshot.memory,snapshot.memoryCount,snapshot.footprint,snapshot.resident,snapshot.headroom,snapshot.enumeration,snapshot.returned,snapshot.truncated?"true":"false");
    for(unsigned i=0;i<snapshot.count;++i) {
        const auto& t=snapshot.threads[i];
        // Encode the bounded native name as hex, avoiding unescaped JSON strings.
        std::fprintf(f,"%s{\"id\":%llu,\"self\":%s,\"begin_ticks\":%llu,\"end_ticks\":%llu,\"identify_result\":%d,\"basic_result\":%d,\"basic_count\":%u,\"state_result\":%d,\"state_count\":%u,\"walk_end\":%d,\"suspend_result\":%d,\"resume_result\":%d,\"run_state\":%d,\"suspend_count\":%d,\"name_hex\":\"",i?",":"",t.id,t.self?"true":"false",t.begin,t.end,t.identify,t.basic,t.basicCount,t.state,t.stateCount,t.walkEnd,t.suspend,t.resume,t.info.run_state,t.info.suspend_count);
        for(unsigned j=0;j<sizeof(t.name) && t.name[j];++j)std::fprintf(f,"%02x",unsigned(static_cast<unsigned char>(t.name[j])));
        std::fprintf(f,"\",\"pc\":%llu,\"sp\":%llu,\"fp\":%llu,\"truncated\":%s,\"stack\":[",t.pc,t.sp,t.fp,t.truncated?"true":"false");
        for(unsigned j=0;j<t.frames;++j)std::fprintf(f,"%s%llu",j?",":"",static_cast<unsigned long long>(t.stack[j]));
        std::fputs("]}",f);
    }
    std::fprintf(f,"],\"port_cleanup_failures\":%d,\"storage_cleanup\":%d,\"stacks_temporarily_suspended\":true,\"stack_walk_complete_unproven\":true}\n",snapshot.portCleanupFailures,snapshot.storageCleanup);
    const bool ok=!ferror(f) && fflush(f)==0;const int closed=fclose(f);
    std::fprintf(stderr,"APPLE_IOS_FAILURE_CAPTURE file=presenter-failure.json written=%u threads=%u thermal=%d footprint=%llu exact_limit_unknown=1\n",unsigned(ok && !closed),snapshot.count,snapshot.thermal,snapshot.footprint);
}
}
