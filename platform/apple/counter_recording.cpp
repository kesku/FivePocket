// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/counter_recording.h"
#include <cstring>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace recomp::apple::performance {
namespace {
bool token(const char* value) noexcept {
    if(!value || !*value)return false;
    std::size_t length=0;
    for(const auto* c=value;*c;++c)
        if(++length>128 || !((*c>='A' && *c<='Z') || (*c>='a' && *c<='z') ||
            (*c>='0' && *c<='9') || *c=='-' || *c=='_' || *c=='.'))return false;
    return true;
}
unsigned long long u64(std::uint64_t value) noexcept {return static_cast<unsigned long long>(value);}
std::uint64_t kernelId(std::uint64_t value) noexcept {
#if defined(APPLE_COUNTER_RECORDING_MUTANT_TRUNCATE_ID)
    return std::uint32_t(value); // deliberately loses the kernel identity
#else
    return value;
#endif
}
const char* boolean(bool value) noexcept {return value?"true":"false";}
}
CounterRecording::CounterRecording(FILE* file,const char* runId,std::size_t maxSamples,std::size_t maxThreads):
    file_(file),maxSamples_(maxSamples),maxThreads_(maxThreads) {
    if(!file || !token(runId) || !maxSamples || maxSamples>MaxSamples || !maxThreads || maxThreads>MaxThreads)
        throw std::invalid_argument("owner stream, token and bounded counter capacities required");
    std::memcpy(runId_.data(),runId,std::strlen(runId)+1);
}
bool CounterRecording::header() noexcept {
    if(started_)return !status_.writeFailed;
    started_=true;
    const char* memoryKind="os-proc-available-memory-headroom";
#if defined(APPLE_COUNTER_RECORDING_MUTANT_PROMOTE_HEADROOM)
    memoryKind="exact-jetsam-limit"; // deliberately invents a process limit
#endif
    if(std::fprintf(file_,"{\"event\":\"run\",\"schema\":\"apple-counter-measurement/2\",\"run_id\":\"%s\",\"purpose\":\"exploratory\",\"clock\":\"mach-continuous-ns\",\"cpu_clock\":\"mach-thread-cpu-ns\",\"max_samples\":%zu,\"max_threads\":%zu,\"available_memory_kind\":\"%s\",\"core_residency_available\":false,\"runnable_wait_available\":false,\"exact_jetsam_limit_available\":false,\"cpu_utilization_available\":false}\n",
        runId_.data(),maxSamples_,maxThreads_,memoryKind)<0)status_.writeFailed=true;
    return !status_.writeFailed;
}
bool CounterRecording::append(const Snapshot& value) noexcept {
    if(status_.finished || status_.writeFailed)return false;
    if(status_.stored>=maxSamples_ || value.threadCount>maxThreads_ || value.threadCount>MaxThreads) {
        if(status_.dropped!=UINT64_MAX)++status_.dropped;
        return false;
    }
    if(!header())return false;
    const auto emit=[&](const char* format,auto... args) {
        int result;
        if constexpr(sizeof...(args)==0)result=std::fputs(format,file_);
        else result=std::fprintf(file_,format,args...);
        if(result<0) {status_.writeFailed=true;return false;}return true;
    };
    auto sequence=value.sequence;
#if defined(APPLE_COUNTER_RECORDING_MUTANT_HIDE_GAP)
    sequence=status_.stored+1; // deliberately hides upstream loss/reset
#endif
    auto cleanup=value.cleanup;
    auto cleanupFailureSeen=value.cleanupFailureSeen;
#if defined(APPLE_COUNTER_RECORDING_MUTANT_HIDE_CLEANUP)
    cleanup.status=CleanupStatus::NoApiErrors;cleanup.failedPortReleases=0;
    cleanup.firstPortError=0;cleanup.storageReleaseResult=0;cleanupFailureSeen=false;
#endif
    if(!emit("{\"event\":\"sample\",\"run_id\":\"%s\",\"index\":%zu,\"clock\":\"mach-continuous-ns\",\"cpu_clock\":\"mach-thread-cpu-ns\",\"pid\":%lld,\"sampler_epoch_ticks\":%llu,\"sampler_generation\":%llu,\"sequence\":%llu,\"start_ns\":%llu,\"end_ns\":%llu,\"current_kernel_id\":%llu,\"clock_invalid\":%d,\"current_identity_result\":%d,\"threads_result\":%d,\"returned_threads\":%u,\"thread_count\":%zu,\"truncated\":%s,\"cleanup_failure_seen\":%s,\"cleanup\":{\"status\":%d,\"attempted_port_releases\":%u,\"failed_port_releases\":%u,\"first_port_error\":%d,\"storage_release_result\":%d,\"storage_release_attempted\":%s},",
        runId_.data(),status_.stored,static_cast<long long>(value.pid),u64(value.samplerEpochTicks),u64(value.samplerGeneration),
        u64(sequence),u64(value.startNs),u64(value.endNs),u64(kernelId(value.currentKernelId)),int(value.clockInvalid),
        value.currentIdentityResult,value.threadsResult,value.returnedThreads,value.threadCount,boolean(value.truncated),
        boolean(cleanupFailureSeen),int(cleanup.status),cleanup.attemptedPortReleases,cleanup.failedPortReleases,
        cleanup.firstPortError,cleanup.storageReleaseResult,boolean(cleanup.storageReleaseAttempted)))return false;
    const auto& m=value.memory;
    if(!emit("\"memory\":{\"reply\":{\"result\":%d,\"returned_count\":%u,\"required_count\":%u},\"resident_required_count\":%u,\"footprint_required_count\":%u,\"resident_bytes\":%llu,\"footprint_bytes\":%llu,\"available_bytes\":%llu,\"resident_valid\":%s,\"footprint_valid\":%s,\"available_valid\":%s},\"thermal\":%d,\"thermal_notifications\":%llu,\"threads\":[",
        m.reply.result,m.reply.returnedCount,m.reply.requiredCount,m.residentRequiredCount,m.footprintRequiredCount,u64(m.residentBytes),u64(m.footprintBytes),u64(m.availableBytes),
        boolean(m.residentValid),boolean(m.footprintValid),boolean(m.availableValid),int(value.thermal),u64(value.thermalNotifications)))return false;
    for(std::size_t i=0;i<value.threadCount;++i) {
        const auto& thread=value.threads[i];
        if(!emit("%s{\"kernel_id\":%llu,\"read_begin_ns\":%llu,\"read_end_ns\":%llu,\"user_cpu_ns\":%llu,\"system_cpu_ns\":%llu,\"invalid\":%d,\"name_valid\":%s,\"name_bytes_hex\":\"",
            i?",":"",u64(kernelId(thread.kernelId)),u64(thread.readBeginNs),u64(thread.readEndNs),u64(thread.userCpuNs),u64(thread.systemCpuNs),int(thread.invalid),boolean(thread.nameValid)))return false;
        // Fixed bytes preserve even invalid/nonterminated names without JSON
        // injection, locale conversion or reading beyond the bounded array.
        constexpr char digits[]="0123456789abcdef";
        std::array<char,2*std::tuple_size_v<decltype(thread.name)>> nameHex;
        for(std::size_t n=0;n<thread.name.size();++n) {
            const auto byte=static_cast<unsigned char>(thread.name[n]);
            nameHex[2*n]=digits[byte>>4];nameHex[2*n+1]=digits[byte&15];
        }
        if(std::fwrite(nameHex.data(),1,nameHex.size(),file_)!=nameHex.size()) {
            status_.writeFailed=true;return false;
        }
        if(!emit("\""))return false;
#if !defined(APPLE_COUNTER_RECORDING_MUTANT_DROP_MACH_STATE)
        if(!emit(",\"mach_state\":{\"required_count\":%u,\"run_state\":%d,\"flags\":%d,\"suspend_count\":%d,\"fields_present\":%s}",
            thread.stateRequiredCount,thread.runState,thread.flags,thread.suspendCount,boolean(thread.stateFieldsPresent)))return false;
#endif
        for(const auto item:{std::pair{"identity_reply",thread.identityReply},std::pair{"cpu_reply",thread.cpuReply},std::pair{"name_reply",thread.nameReply}})
            if(!emit(",\"%s\":{\"result\":%d,\"returned_count\":%u,\"required_count\":%u}",item.first,item.second.result,item.second.returnedCount,item.second.requiredCount))return false;
        if(!emit("}"))return false;
    }
    if(!emit("]}\n"))return false;
    ++status_.stored;lastSequence_=sequence;
    return true;
}
bool CounterRecording::finish() noexcept {
    if(status_.finished || status_.writeFailed)return false;
    status_.finished=true;
    if(!header())return false;
    if(std::fprintf(file_,"{\"event\":\"end\",\"run_id\":\"%s\",\"sample_count\":%zu,\"last_sequence\":%llu,\"complete\":%s,\"dropped\":%llu,\"write_failed\":false}\n",
        runId_.data(),status_.stored,u64(lastSequence_),boolean(!status_.dropped),u64(status_.dropped))<0 ||
        std::fflush(file_) || std::ferror(file_))status_.writeFailed=true;
    return !status_.writeFailed;
}
}
