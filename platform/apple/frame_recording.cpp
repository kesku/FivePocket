// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/frame_recording.h"
#include <cstring>
#include <stdexcept>

namespace recomp::apple::performance {
namespace {
bool token(const char* value,std::size_t maximum) noexcept {
    if(!value || !*value)return false;
    std::size_t length=0;
    for(const auto* c=value;*c;++c) {
        if(++length>maximum || !((*c>='A' && *c<='Z') || (*c>='a' && *c<='z') || (*c>='0' && *c<='9') ||
            *c=='-' || *c=='_' || *c=='.'))return false;
    }
    return true;
}
bool sceneToken(const char* value) noexcept {
    return value && (!std::strcmp(value,"house") || !std::strcmp(value,"outdoor") ||
        !std::strcmp(value,"driving") || !std::strcmp(value,"prologue") || !std::strcmp(value,"other"));
}
bool valid(const CallbackMeasurement& value) noexcept {
    return value.clockValid && value.identitiesValid && value.entryNs &&
        value.entryNs<=value.mainEntryNs && value.mainEntryNs<=value.presentEndNs &&
        value.presentEndNs<=value.returnNs && value.callerKernelId && value.mainKernelId;
}
}
FrameRecording::FrameRecording(std::size_t capacity):capacity_(capacity) {
    if(!capacity || capacity>MaxCapacity)throw std::invalid_argument("bounded recording capacity required");
    records_=std::make_unique<CallbackMeasurement[]>(capacity);
}
FrameRecording::~FrameRecording()=default;
bool FrameRecording::append(const CallbackMeasurement& value) noexcept {
    if(state_.load(std::memory_order_acquire)!=RecordingState::Open) {loss_=true;++dropped_;return false;}
    if(gate_.test_and_set(std::memory_order_acquire)) {loss_=true;++dropped_;return false;}
    bool accepted=false;
    if(state_.load(std::memory_order_acquire)==RecordingState::Open) {
        const auto index=stored_.load(std::memory_order_relaxed);
        if(index<capacity_) {
            // Keep invalid/rejected observations; qualification must reject
            // them. Never silently filter or renumber a failed delivery.
            records_[index]=value;
            stored_.store(index+1,std::memory_order_release);
            if(!valid(value))invalid_=true;
            if(!value.accepted)loss_=true;
            if(index) {
                const auto& prior=records_[index-1];
#if !defined(APPLE_FRAME_RECORDING_MUTANT_HIDE_GAP)
                if(value.sourceSequence!=std::uint32_t(prior.sourceSequence+1))loss_=true;
#endif
                if(value.entryNs<=prior.entryNs || value.entryNs<prior.returnNs)invalid_=true;
            }
            accepted=true;
        } else {
            ++dropped_;
#if !defined(APPLE_FRAME_RECORDING_MUTANT_IGNORE_OVERFLOW)
            loss_=true;
#endif
        }
    } else {loss_=true;++dropped_;}
    gate_.clear(std::memory_order_release);
    return accepted;
}
bool FrameRecording::close() noexcept {
    const auto previous=state_.exchange(RecordingState::Closed,std::memory_order_acq_rel);
    if(previous!=RecordingState::Open)return false;
    if(gate_.test_and_set(std::memory_order_acquire)) {loss_=true;return false;}
    sealed_.store(true,std::memory_order_release);
    gate_.clear(std::memory_order_release);
    return true;
}
RecordingStatus FrameRecording::status() const noexcept {
    return {state_.load(),stored_.load(),capacity_,loss_.load(),invalid_.load(),
        writeFailed_.load(),sealed_.load()};
}
bool FrameRecording::write(FILE* file,const char* runId,const char* scene,unsigned fps,bool fsr) noexcept {
    if(writeStarted_ || !file || !token(runId,128) || !sceneToken(scene) || (fps!=30 && fps!=60) ||
        state_.load(std::memory_order_acquire)!=RecordingState::Closed || !sealed_.load(std::memory_order_acquire))return false;
    writeStarted_=true;
    const auto count=stored_.load(std::memory_order_acquire);
    if(!count)return false;
    const auto emit=[&](const char* format,auto... args) {
        if(std::fprintf(file,format,args...)<0) {writeFailed_=true;return false;}return true;
    };
    const char* interval="callback-entry";
#if defined(APPLE_FRAME_RECORDING_MUTANT_PHYSICAL_FRAME)
    interval="display-presented"; // deliberately lies about callback acceptance
#endif
    if(!emit("{\"event\":\"run\",\"schema\":\"apple-frame-measurement/1\",\"run_id\":\"%s\",\"purpose\":\"exploratory\",\"interval_kind\":\"%s\",\"clock\":\"device-monotonic-ns\",\"scene\":\"%s\",\"target_fps\":%u,\"fsr\":%s}\n",
        runId,interval,scene,fps,fsr?"true":"false"))return false;
    for(std::size_t i=0;i<count;++i) {
        const auto& r=records_[i];
        if(!emit("{\"event\":\"frame\",\"run_id\":\"%s\",\"index\":%zu,\"source_sequence\":%u,\"time_ns\":%llu,\"accepted\":%s}\n",
            runId,i,r.sourceSequence,static_cast<unsigned long long>(r.entryNs),r.accepted?"true":"false"))return false;
        const auto stage=[&](const char* name,std::uint64_t first,std::uint64_t last,std::uint64_t tid) {
            return emit("{\"event\":\"stage\",\"run_id\":\"%s\",\"frame_index\":%zu,\"name\":\"%s\",\"clock\":\"device-monotonic-ns\",\"begin_ns\":%llu,\"end_ns\":%llu,\"thread_id\":%llu}\n",
                runId,i,name,static_cast<unsigned long long>(first),static_cast<unsigned long long>(last),static_cast<unsigned long long>(tid));
        };
        if(!stage("callback-main-dispatch",r.entryNs,r.mainEntryNs,r.callerKernelId) ||
            !stage("main-presenter-call",r.mainEntryNs,r.presentEndNs,r.mainKernelId) ||
            !stage("callback-return",r.presentEndNs,r.returnNs,r.callerKernelId))return false;
    }
    // A later producer/closed-state append can set loss. Writing never clears
    // failure. Application ownership must guarantee quiescence before export.
    const bool complete=!loss_.load() && !invalid_.load();
    if(!emit("{\"event\":\"end\",\"run_id\":\"%s\",\"frame_count\":%zu,\"time_ns\":%llu,\"complete\":%s,\"dropped\":%llu}\n",
        runId,count,static_cast<unsigned long long>(records_[count-1].returnNs),complete?"true":"false",static_cast<unsigned long long>(dropped_.load())))return false;
    if(std::fflush(file) || std::ferror(file)) {writeFailed_=true;return false;}
    return true;
}
}
