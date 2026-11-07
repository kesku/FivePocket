// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "apple/display_recording_export.h"
#include "apple/measurement_clock.h"
#include <dispatch/dispatch.h>
#include <atomic>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

namespace recomp::apple::performance {
// Created from the session's initial native clock stamp, not at late export.
// This diagnostic cutoff is121s, inside the existing140s host watchdog. An
// invalid clock refuses observation. It grants no app/session/input authority.
class DisplayFollowupControl {
    MeasurementClock clock_;
    std::atomic<bool> cancelled_{false};
    std::uint64_t cutoff_=0;
public:
    explicit DisplayFollowupControl(ClockStamp sessionStart) noexcept {
        constexpr std::uint64_t bound=121'000'000'000ULL;
        if(sessionStart.timeInvalid==Invalid::None && sessionStart.ns &&
           sessionStart.ns<=std::numeric_limits<std::uint64_t>::max()-bound)
            cutoff_=sessionStart.ns+bound;
    }
    void cancel() noexcept {cancelled_.store(true,std::memory_order_release);}
    bool cancelled() const noexcept {return cancelled_.load(std::memory_order_acquire);}
    std::uint64_t now() const noexcept {
        const auto stamp=clock_.read(false);
        return stamp.timeInvalid==Invalid::None?stamp.ns:0;
    }
    std::uint64_t cutoff() const noexcept {return cutoff_;}
};

namespace display_followup {
struct CloseFile {void operator()(FILE* f) const noexcept {if(f)std::fclose(f);}};
using File=std::unique_ptr<FILE,CloseFile>;

// Callback accounting only. In particular, zero timestamps do not become
// valid delivery merely because every registered callback has returned.
inline bool accounted(const DisplaySnapshot& s) noexcept {
    return s.closed && s.completed==s.acquired && s.samples.size()==s.acquired;
}
inline bool preserves(const DisplaySnapshot& first,const DisplaySnapshot& later) noexcept {
    if(!first.closed || !later.closed || first.acquired!=later.acquired ||
       first.layerEpoch!=later.layerEpoch || later.completed<first.completed ||
       later.rejectedRegistrations<first.rejectedRegistrations ||
       (first.overflow&&!later.overflow) || (first.invalidTime&&!later.invalidTime) ||
       (first.invalidIdentity&&!later.invalidIdentity) ||
       (first.duplicateCallback&&!later.duplicateCallback))return false;
    std::size_t j=0;
    for(const auto& original:first.samples) {
        while(j<later.samples.size() && later.samples[j].ordinal<original.ordinal)++j;
        if(j==later.samples.size())return false;
        const auto& current=later.samples[j];
        if(current.ordinal!=original.ordinal || current.drawableId!=original.drawableId ||
           std::bit_cast<std::uint64_t>(current.presentedHostSeconds)!=
               std::bit_cast<std::uint64_t>(original.presentedHostSeconds))return false;
    }
    return true;
}

// No pointer to the session, presenter, layer, or drawable survives into a tick.
// Each scheduled block owns this observer; Metal handlers independently own the
// recorder. No blocking wait, rendering, input, or recurring timer cycle.
class Observer:public std::enable_shared_from_this<Observer> {
    File output_;
    std::shared_ptr<DisplayRecording> recording_;
    std::shared_ptr<DisplayFollowupControl> control_;
    std::string runId_;
    DisplaySnapshot first_,last_;
    std::uint64_t lastObserved_=0;
    std::uint64_t started_=0,end_=0;
    bool startedOnce_=false,finished_=false;

    void finish(const char* reason,const DisplaySnapshot* latest,bool retained) noexcept {
        if(finished_)return;
        finished_=true;
        bool ok=output_!=nullptr;
        if(ok)ok=std::fprintf(output_.get(),
            "{\"schema\":\"apple-drawable-followup/1\",\"kind\":\"final\",\"run_id\":\"%s\","
            "\"reason\":\"%s\",\"original_rows_preserved\":%s,\"callback_accounting_complete\":%s,"
            "\"physical_delivery_complete\":false,\"snapshot_advisory\":true,\"new_frame_admission\":false,\"last_observed_native_ns\":%llu,\"native_cutoff_ns\":%llu}\n",
            runId_.c_str(),reason,retained?"true":"false",
            latest&&retained&&accounted(*latest)?"true":"false",
            (unsigned long long)lastObserved_,(unsigned long long)end_)>=0;
        if(ok && latest)ok=exportDisplaySnapshot(output_.get(),runId_.c_str(),*latest);
        if(output_){if(std::fflush(output_.get())!=0 || std::ferror(output_.get()))ok=false;
            if(std::fclose(output_.release())!=0)ok=false;}
        // Status is diagnostic output only; it never changes native END success.
        std::fprintf(stderr,"APPLE_DISPLAY_FOLLOWUP_END reason=%s output_ok=%u physical_unproven=1\n",reason,unsigned(ok));
    }
    void tick() noexcept {
        if(finished_)return;
        try {
            const auto now=control_->now();
            if(control_->cancelled()){finish("cancelled",nullptr,false);return;}
            // Never turn a callback after the cutoff into an in-budget completion.
            if(!now || !end_ || now>=end_){finish("deadline",&last_,true);return;}
            auto latest=recording_->snapshot();
            const auto observed=control_->now();
            if(!observed || observed>=end_){finish("deadline",&last_,true);return;}
            const bool retained=preserves(first_,latest)&&preserves(last_,latest);
            if(!retained){finish("changed-prior-evidence",&latest,false);return;}
            last_=std::move(latest);lastObserved_=observed;
            if(accounted(last_)){finish("callbacks-accounted",&last_,true);return;}
            auto self=shared_from_this();
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,50*NSEC_PER_MSEC),
                dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{self->tick();});
        } catch(...) {finish("exception",nullptr,false);}
    }
public:
    Observer(File file,std::shared_ptr<DisplayRecording> recording,
             std::shared_ptr<DisplayFollowupControl> control,const std::string& runId):
        output_(std::move(file)),recording_(std::move(recording)),control_(std::move(control)),runId_(runId) {}
    void cancel() noexcept {control_->cancel();}
    void begin(const DisplaySnapshot& original) noexcept {
        if(startedOnce_)return;
        startedOnce_=true;
        try {
            first_=original;last_=original;started_=control_->now();lastObserved_=started_;
            if(!started_ || !control_->cutoff() || control_->cancelled()) {
                finish("cancelled-or-invalid-clock",nullptr,false);return;
            }
            constexpr std::uint64_t grace=1'000'000'000ULL;
            end_=started_<=std::numeric_limits<std::uint64_t>::max()-grace?
                std::min(started_+grace,control_->cutoff()):0;
            if(std::fprintf(output_.get(),
                "{\"schema\":\"apple-drawable-followup/1\",\"kind\":\"original\",\"run_id\":\"%s\","
                "\"native_begin_ns\":%llu,\"native_cutoff_ns\":%llu,\"snapshot_advisory\":true}\n",
                runId_.c_str(),(unsigned long long)started_,(unsigned long long)end_)<0 ||
               !exportDisplaySnapshot(output_.get(),runId_.c_str(),first_)) {
                finish("original-copy-write-failed",nullptr,false);return;
            }
            tick();
        } catch(...) {finish("exception",nullptr,false);}
    }
};
} // namespace display_followup
} // namespace recomp::apple::performance
