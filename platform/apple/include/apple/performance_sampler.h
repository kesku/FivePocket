// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace recomp::apple::performance {
// Counters only. Neither QoS nor a Mach state snapshot establishes core
// residency or time spent runnable. No exact/future memory limit is derived.
inline constexpr std::size_t MaxThreads = 256;
#if defined(APPLE_PERF_MUTANT_QOS_CORE)
inline constexpr bool CoreResidencyAvailable = true; // deliberately wrong
#else
inline constexpr bool CoreResidencyAvailable = false;
#endif
inline constexpr bool RunnableWaitAvailable = false;

enum class Invalid { None, ApiError, ShortReply, Value, Overflow, Identity,
                     Incomplete, CounterReset, NotCpu, ClockOrder };
enum class CounterSource { MachThreadCpu, WallClock };
enum class ThermalState : int { Unavailable=-1, Nominal=0, Fair=1, Serious=2, Critical=3 };
enum class CleanupStatus { NoOwnership, NoApiErrors, ApiErrors };

struct CleanupSample {
    CleanupStatus status=CleanupStatus::NoOwnership;
    std::uint32_t attemptedPortReleases=0,failedPortReleases=0;
    std::int32_t firstPortError=0,storageReleaseResult=0;
    bool storageReleaseAttempted=false;
};

struct ApiReply {
    std::int32_t result = -1;
    std::uint32_t returnedCount = 0, requiredCount = 0;
};
inline Invalid replyValidity(ApiReply reply) noexcept {
    if (reply.result != 0) return Invalid::ApiError;
#if !defined(APPLE_PERF_MUTANT_IGNORE_COUNT)
    if (!reply.requiredCount || reply.returnedCount < reply.requiredCount) return Invalid::ShortReply;
#endif
    return Invalid::None;
}
inline Invalid checkedNanos(std::uint64_t ticks, std::uint32_t numer,
                            std::uint32_t denom, std::uint64_t& nanos) noexcept {
    nanos=0;
    if (!numer || !denom) return Invalid::Value;
#if defined(APPLE_PERF_MUTANT_TIME)
    const auto converted=(ticks*std::uint64_t(numer))/denom; // unchecked overflow
#else
    const __uint128_t converted=__uint128_t(ticks)*numer/denom;
#endif
    if (converted > std::numeric_limits<std::uint64_t>::max()) return Invalid::Overflow;
    nanos=std::uint64_t(converted);
    return Invalid::None;
}
inline Invalid cpuNanos(std::int64_t seconds, std::int64_t micros,
                        std::uint64_t& nanos) noexcept {
    nanos=0;
    if (seconds<0 || micros<0 || micros>=1'000'000) return Invalid::Value;
    const __uint128_t value=__uint128_t(seconds)*1'000'000'000+std::uint64_t(micros)*1'000;
    if (value>std::numeric_limits<std::uint64_t>::max()) return Invalid::Overflow;
    nanos=std::uint64_t(value);return Invalid::None;
}
inline ThermalState thermalState(int value) noexcept {
    return value>=0 && value<=3 ? ThermalState(value) : ThermalState::Unavailable;
}

struct RawMemory {
    ApiReply reply;
    std::uint32_t residentRequiredCount=0, footprintRequiredCount=0;
    std::uint64_t residentBytes=0, footprintBytes=0, availableBytes=0;
    bool availableSupported=false;
};
struct MemorySample {
    ApiReply reply;
    std::uint32_t residentRequiredCount=0,footprintRequiredCount=0;
    std::uint64_t residentBytes=0, footprintBytes=0, availableBytes=0;
    bool residentValid=false, footprintValid=false, availableValid=false;
};
inline MemorySample validateMemory(const RawMemory& raw) noexcept {
    MemorySample out;out.reply=raw.reply;
    out.residentRequiredCount=raw.residentRequiredCount;out.footprintRequiredCount=raw.footprintRequiredCount;
#if defined(APPLE_PERF_MUTANT_DROP_MEMORY_COUNTS)
    out.residentRequiredCount=0;out.footprintRequiredCount=0;
#endif
    out.residentValid=replyValidity({raw.reply.result,raw.reply.returnedCount,raw.residentRequiredCount})==Invalid::None;
    out.footprintValid=replyValidity({raw.reply.result,raw.reply.returnedCount,raw.footprintRequiredCount})==Invalid::None;
#if defined(APPLE_PERF_MUTANT_BAD_FIELDS)
    if (out.residentValid) out.residentBytes=raw.footprintBytes; // deliberately swapped
    if (out.footprintValid) out.footprintBytes=raw.residentBytes;
#else
    if (out.residentValid) out.residentBytes=raw.residentBytes;
    if (out.footprintValid) out.footprintBytes=raw.footprintBytes;
#endif
    // os_proc_available_memory has no error return: zero is a valid snapshot.
    out.availableValid=raw.availableSupported;
    if (out.availableValid) out.availableBytes=raw.availableBytes;
    return out;
}

struct RawThread {
    ApiReply identityReply, cpuReply;
    std::uint64_t kernelId=0, readBeginNs=0, readEndNs=0;
    std::int64_t userSeconds=0,userMicros=0,systemSeconds=0,systemMicros=0;
    CounterSource source=CounterSource::MachThreadCpu;
    // Raw THREAD_BASIC_INFO fields; minimum is derived from the real SDK by
    // the producer. Presence is separate from CPU/identity/clock validity.
    std::uint32_t stateRequiredCount=0;
    std::int32_t runState=0,flags=0,suspendCount=0;
};
struct ThreadSample {
    ApiReply identityReply, cpuReply, nameReply;
    std::uint64_t kernelId=0,readBeginNs=0,readEndNs=0,userCpuNs=0,systemCpuNs=0;
    Invalid invalid=Invalid::Value;
    std::array<char,64> name{};
    bool nameValid=false;
    std::uint32_t stateRequiredCount=0;
    std::int32_t runState=0,flags=0,suspendCount=0;
    bool stateFieldsPresent=false;
};
inline ThreadSample validateThread(const RawThread& raw) noexcept {
    ThreadSample out;out.identityReply=raw.identityReply;out.cpuReply=raw.cpuReply;
    out.kernelId=raw.kernelId;out.readBeginNs=raw.readBeginNs;out.readEndNs=raw.readEndNs;
    out.stateRequiredCount=raw.stateRequiredCount;
#if !defined(APPLE_PERF_MUTANT_DROP_STATE)
    out.runState=raw.runState;out.flags=raw.flags;out.suspendCount=raw.suspendCount;
#endif
#if defined(APPLE_PERF_MUTANT_STATE_UNKNOWN)
    if(out.runState>5)out.runState=1; // deliberately invents a known state
#endif
    out.stateFieldsPresent=
#if !defined(APPLE_PERF_MUTANT_STATE_ERROR)
        raw.cpuReply.result==0 &&
#endif
#if !defined(APPLE_PERF_MUTANT_STATE_COUNT)
        raw.stateRequiredCount && raw.cpuReply.returnedCount>=raw.stateRequiredCount &&
#endif
        true;
    out.invalid=replyValidity(raw.identityReply);
    if (out.invalid==Invalid::None) out.invalid=replyValidity(raw.cpuReply);
    if (out.invalid==Invalid::None && !raw.kernelId) out.invalid=Invalid::Identity;
    if (out.invalid==Invalid::None && (!raw.readBeginNs || raw.readEndNs<raw.readBeginNs)) out.invalid=Invalid::ClockOrder;
#if !defined(APPLE_PERF_MUTANT_WALL_CPU)
    if (out.invalid==Invalid::None && raw.source!=CounterSource::MachThreadCpu) out.invalid=Invalid::NotCpu;
#endif
    if (out.invalid==Invalid::None) out.invalid=cpuNanos(raw.userSeconds,raw.userMicros,out.userCpuNs);
    if (out.invalid==Invalid::None) out.invalid=cpuNanos(raw.systemSeconds,raw.systemMicros,out.systemCpuNs);
    return out;
}

struct Snapshot {
    std::uint64_t samplerEpochTicks=0,samplerGeneration=0,sequence=0,startNs=0,endNs=0,currentKernelId=0;
    std::int64_t pid=0;
    Invalid clockInvalid=Invalid::Value;
    std::int32_t currentIdentityResult=-1,threadsResult=-1;
    std::uint32_t returnedThreads=0;
    std::size_t threadCount=0;
    bool truncated=false;
    // NoApiErrors means each release call returned zero, not that later
    // observations may assume a prior cleanup succeeded without checking it.
    CleanupSample cleanup;
    bool cleanupFailureSeen=false; // latched within this sampler; no retries
    MemorySample memory;
    ThermalState thermal=ThermalState::Unavailable;
    std::uint64_t thermalNotifications=0;
    std::array<ThreadSample,MaxThreads> threads{};
};
inline bool completeThreads(const Snapshot& value) noexcept {
    if (value.clockInvalid!=Invalid::None || value.threadsResult!=0 || value.truncated ||
        !value.threadCount || value.threadCount>MaxThreads || value.threadCount!=value.returnedThreads ||
        value.cleanupFailureSeen || value.cleanup.status!=CleanupStatus::NoApiErrors || !value.cleanup.storageReleaseAttempted ||
        value.cleanup.storageReleaseResult!=0 || value.cleanup.failedPortReleases ||
        value.cleanup.attemptedPortReleases!=value.returnedThreads) return false;
    for (std::size_t i=0;i<value.threadCount;++i) {
        if (value.threads[i].invalid!=Invalid::None) return false;
        for (std::size_t j=0;j<i;++j)
            if (value.threads[i].kernelId==value.threads[j].kernelId) return false;
    }
    return true;
}
struct CpuDelta {
    Invalid invalid=Invalid::Identity;
    std::uint64_t kernelId=0,userCpuNs=0,systemCpuNs=0,wallUpperNs=0;
};
inline CpuDelta threadDelta(const Snapshot& before,const Snapshot& after,std::uint64_t kernelId) noexcept {
    CpuDelta out;out.kernelId=kernelId;
    if (before.pid<=0 || before.pid!=after.pid || !before.samplerEpochTicks ||
        before.samplerEpochTicks!=after.samplerEpochTicks || !before.samplerGeneration ||
        before.samplerGeneration!=after.samplerGeneration || !before.sequence || after.sequence<=before.sequence) return out;
    if (!completeThreads(before) || !completeThreads(after)) {out.invalid=Invalid::Incomplete;return out;}
    const ThreadSample *a=nullptr,*b=nullptr;
    for (std::size_t i=0;i<before.threadCount;++i) if (before.threads[i].kernelId==kernelId) a=&before.threads[i];
    for (std::size_t i=0;i<after.threadCount;++i) if (after.threads[i].kernelId==kernelId) b=&after.threads[i];
#if defined(APPLE_PERF_MUTANT_IDENTITY)
    if (!b) b=&after.threads[0]; // wrongly joins a replacement thread
#endif
    if (!a || !b || !kernelId) return out;
    if (b->readBeginNs<a->readEndNs) {out.invalid=Invalid::ClockOrder;return out;}
    if (b->userCpuNs<a->userCpuNs || b->systemCpuNs<a->systemCpuNs) {out.invalid=Invalid::CounterReset;return out;}
    out.userCpuNs=b->userCpuNs-a->userCpuNs;out.systemCpuNs=b->systemCpuNs-a->systemCpuNs;
    out.wallUpperNs=b->readEndNs-a->readBeginNs;
    // Two microsecond-rounded Mach components can introduce up to 2us of
    // quantization error. This is not a general performance tolerance.
    const __uint128_t cpu=__uint128_t(out.userCpuNs)+out.systemCpuNs;
    if (cpu>__uint128_t(out.wallUpperNs)+2'000) {out.invalid=Invalid::NotCpu;return out;}
    out.invalid=Invalid::None;return out;
}

// Cleanup intentionally visits the WHOLE returned list, not its sampled prefix.
// Also used after an API failure if it returned storage/rights to dispose of.
template<class Port,class ReleasePort,class ReleaseStorage>
inline CleanupSample cleanupThreadList(Port* ports,std::uint32_t count,ReleasePort releasePort,
                                      ReleaseStorage releaseStorage) noexcept {
    CleanupSample out;
    if (!ports) return out;
    auto releaseCount=count;
#if defined(APPLE_PERF_MUTANT_TRUNCATED_CLEANUP)
    if (releaseCount>MaxThreads) releaseCount=MaxThreads; // deliberately leaks
#endif
    for (std::uint32_t i=0;i<releaseCount;++i) if (ports[i]) {
        ++out.attemptedPortReleases;
        const std::int32_t result=releasePort(ports[i]);
#if !defined(APPLE_PERF_MUTANT_IGNORE_CLEANUP_FAILURES)
        if (result) {
            if (!out.failedPortReleases) out.firstPortError=result;
            ++out.failedPortReleases;
        }
#else
        (void)result; // deliberately drops every release failure
#endif
    }
    out.storageReleaseAttempted=true;
    const std::int32_t storageResult=releaseStorage(ports,count);
#if !defined(APPLE_PERF_MUTANT_IGNORE_CLEANUP_FAILURES)
    out.storageReleaseResult=storageResult;
#else
    (void)storageResult; // deliberately invents a clean result
#endif
    out.status=out.failedPortReleases || out.storageReleaseResult ? CleanupStatus::ApiErrors : CleanupStatus::NoApiErrors;
    return out;
}

// A serialized, low-frequency caller owns this object. It starts no worker,
// emits no logging, does not alter QoS/affinity, and does not wait on work.
class Sampler {
public:
    Sampler();
    ~Sampler();
    Sampler(const Sampler&)=delete;
    Sampler& operator=(const Sampler&)=delete;
    Snapshot sample() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
