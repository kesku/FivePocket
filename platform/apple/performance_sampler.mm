// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/performance_sampler.h"
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/proc.h>
#include <pthread.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace recomp::apple::performance {
namespace {
std::atomic<std::uint64_t> nextGeneration{1};
struct ThermalNotifications {std::atomic<std::uint64_t> count{0};};
struct ThreadList {
    thread_act_array_t ports=nullptr;
    mach_msg_type_number_t count=0;
    CleanupSample dispose() noexcept {
        const auto ownedPorts=ports;const auto ownedCount=count;
        // Consume ownership before attempting disposal. A failed release is
        // reported, never retried by the destructor.
        ports=nullptr;count=0;
        return cleanupThreadList(ownedPorts,ownedCount,[](thread_t port) noexcept {
            return mach_port_deallocate(mach_task_self(),port);
        },[](thread_act_array_t storage,std::uint32_t size) noexcept {
            return vm_deallocate(mach_task_self(),reinterpret_cast<vm_address_t>(storage),
                                 vm_size_t(size)*sizeof(thread_t));
        });
    }
    ~ThreadList() {(void)dispose();} // fallback for an exception/early return
};
constexpr std::uint32_t fieldCount(std::size_t offset,std::size_t size) noexcept {
    return std::uint32_t((offset+size+sizeof(natural_t)-1)/sizeof(natural_t));
}
}
struct Sampler::Impl {
    mach_timebase_info_data_t timebase{};
    kern_return_t timebaseResult=KERN_FAILURE;
    std::uint64_t epoch=0,generation=0,sequence=0;
    bool cleanupFailureSeen=false;
    std::shared_ptr<ThermalNotifications> thermal=std::make_shared<ThermalNotifications>();
    id thermalObserver=nil;
    Impl() {
        timebaseResult=mach_timebase_info(&timebase);epoch=mach_continuous_time();
        generation=nextGeneration.fetch_add(1,std::memory_order_relaxed);
        // Apple's notification contract requires property access FIRST.
        (void)NSProcessInfo.processInfo.thermalState;
        const auto owned=thermal;
        thermalObserver=[NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoThermalStateDidChangeNotification
            object:nil queue:nil usingBlock:^(NSNotification*) {
                auto count=owned->count.load(std::memory_order_relaxed);
                while (count!=UINT64_MAX && !owned->count.compare_exchange_weak(count,count+1,std::memory_order_relaxed)) {}
            }];
    }
    ~Impl() {
        if (thermalObserver) [NSNotificationCenter.defaultCenter removeObserver:thermalObserver];
        // A delivery already in progress owns its state; no raw owner capture.
    }
    Invalid now(std::uint64_t& ns) const noexcept {
        if (timebaseResult!=KERN_SUCCESS) {ns=0;return Invalid::ApiError;}
        return checkedNanos(mach_continuous_time(),timebase.numer,timebase.denom,ns);
    }
};
Sampler::Sampler():impl_(std::make_unique<Impl>()) {}
Sampler::~Sampler()=default;

Snapshot Sampler::sample() noexcept {
    @autoreleasepool {
        Snapshot out;auto& state=*impl_;
        out.pid=getpid();out.samplerEpochTicks=state.epoch;out.samplerGeneration=state.generation;
        if (state.sequence==UINT64_MAX || !state.epoch || !state.generation) {out.clockInvalid=Invalid::Overflow;return out;}
        out.sequence=++state.sequence;out.clockInvalid=state.now(out.startNs);
        out.currentIdentityResult=pthread_threadid_np(nullptr,&out.currentKernelId);
        if (out.currentIdentityResult || !out.currentKernelId) out.currentKernelId=0;
        out.thermal=thermalState(int(NSProcessInfo.processInfo.thermalState));
        out.thermalNotifications=state.thermal->count.load(std::memory_order_relaxed);

        task_vm_info_data_t info{};mach_msg_type_number_t vmCount=TASK_VM_INFO_COUNT;
        const auto vmResult=task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&info),&vmCount);
        RawMemory memory;memory.reply={vmResult,vmCount,TASK_VM_INFO_COUNT};
        memory.residentRequiredCount=fieldCount(offsetof(task_vm_info_data_t,resident_size),sizeof(info.resident_size));
        memory.footprintRequiredCount=fieldCount(offsetof(task_vm_info_data_t,phys_footprint),sizeof(info.phys_footprint));
        memory.residentBytes=info.resident_size;memory.footprintBytes=info.phys_footprint;
#if TARGET_OS_IOS
        memory.availableSupported=true;memory.availableBytes=os_proc_available_memory();
#endif
        out.memory=validateMemory(memory);

        ThreadList list;
        out.threadsResult=task_threads(mach_task_self(),&list.ports,&list.count);
        out.returnedThreads=list.count;
        out.truncated=list.count>MaxThreads;
        // Even a failed task_threads call is followed by ThreadList cleanup.
        if (out.threadsResult==KERN_SUCCESS && list.ports) {
            out.threadCount=std::min<std::size_t>(list.count,MaxThreads);
            for (std::size_t i=0;i<out.threadCount;++i) {
                RawThread raw;const auto begin=state.now(raw.readBeginNs);
                thread_identifier_info_data_t identity{};mach_msg_type_number_t idCount=THREAD_IDENTIFIER_INFO_COUNT;
                const auto idResult=thread_info(list.ports[i],THREAD_IDENTIFIER_INFO,reinterpret_cast<thread_info_t>(&identity),&idCount);
                raw.identityReply={idResult,idCount,THREAD_IDENTIFIER_INFO_COUNT};raw.kernelId=identity.thread_id;
                thread_basic_info_data_t cpu{};mach_msg_type_number_t cpuCount=THREAD_BASIC_INFO_COUNT;
                const auto cpuResult=thread_info(list.ports[i],THREAD_BASIC_INFO,reinterpret_cast<thread_info_t>(&cpu),&cpuCount);
                raw.cpuReply={cpuResult,cpuCount,THREAD_BASIC_INFO_COUNT};
                raw.stateRequiredCount=fieldCount(offsetof(thread_basic_info_data_t,suspend_count),sizeof(cpu.suspend_count));
#if !defined(APPLE_PERF_MUTANT_SDK_STATE_DROP)
                raw.runState=cpu.run_state;raw.flags=cpu.flags;raw.suspendCount=cpu.suspend_count;
#endif
                raw.userSeconds=cpu.user_time.seconds;raw.userMicros=cpu.user_time.microseconds;
                raw.systemSeconds=cpu.system_time.seconds;raw.systemMicros=cpu.system_time.microseconds;
                const auto end=state.now(raw.readEndNs);
                out.threads[i]=validateThread(raw);
                if (begin!=Invalid::None || end!=Invalid::None) out.threads[i].invalid=begin!=Invalid::None?begin:end;
                thread_extended_info_data_t extended{};mach_msg_type_number_t nameCount=THREAD_EXTENDED_INFO_COUNT;
                const auto nameResult=thread_info(list.ports[i],THREAD_EXTENDED_INFO,reinterpret_cast<thread_info_t>(&extended),&nameCount);
                auto& thread=out.threads[i];thread.nameReply={nameResult,nameCount,THREAD_EXTENDED_INFO_COUNT};
                thread.nameValid=replyValidity(thread.nameReply)==Invalid::None &&
                    std::memchr(extended.pth_name,0,sizeof(extended.pth_name));
                if (thread.nameValid) std::memcpy(thread.name.data(),extended.pth_name,thread.name.size());
            }
        }
        // Explicit disposal binds every release attempt/error to THIS sample;
        // ThreadList still owns the exceptional/early-return fallback above.
        out.cleanup=list.dispose();
        state.cleanupFailureSeen=state.cleanupFailureSeen || out.cleanup.status==CleanupStatus::ApiErrors;
        out.cleanupFailureSeen=state.cleanupFailureSeen;
        const auto end=state.now(out.endNs);
        if (out.clockInvalid==Invalid::None) out.clockInvalid=end;
        if (out.clockInvalid==Invalid::None && out.endNs<out.startNs) out.clockInvalid=Invalid::ClockOrder;
        return out;
    }
}
}
