// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/callback_measurement.h"

namespace recomp::apple::performance {
CallbackMeasurementWindow::Frame::Frame(CallbackMeasurementWindow* owner,MeasurementLifetime::Lease&& lease,
        std::uint32_t sequence,const ClockStamp& stamp) noexcept:owner_(owner),lease_(std::move(lease)) {
    value_.sourceSequence=sequence;value_.entryNs=stamp.ns;value_.callerKernelId=stamp.kernelId;
    value_.clockValid=stamp.timeInvalid==Invalid::None;
    value_.identitiesValid=!stamp.identityResult && stamp.kernelId;
}
void CallbackMeasurementWindow::Frame::mainEntered(const ClockStamp& stamp) noexcept {
    if(!lease_)return;
    value_.mainEntryNs=stamp.ns;value_.mainKernelId=stamp.kernelId;
#if defined(APPLE_CALLBACK_MEASUREMENT_MUTANT_MAIN_ID)
    value_.mainKernelId=value_.callerKernelId;
#endif
    value_.clockValid=value_.clockValid && stamp.timeInvalid==Invalid::None && phase_==0;
    value_.identitiesValid=value_.identitiesValid && !stamp.identityResult && stamp.kernelId;
    phase_=1;
}
void CallbackMeasurementWindow::Frame::presenterReturned(const ClockStamp& stamp) noexcept {
    if(!lease_)return;
    value_.presentEndNs=stamp.ns;
    value_.clockValid=value_.clockValid && stamp.timeInvalid==Invalid::None && phase_==1;
    value_.identitiesValid=value_.identitiesValid && !stamp.identityResult && stamp.kernelId==value_.mainKernelId;
    phase_=2;
}
bool CallbackMeasurementWindow::Frame::returned(const ClockStamp& stamp,bool accepted) noexcept {
    value_.returnNs=stamp.ns;value_.accepted=accepted;
    value_.clockValid=value_.clockValid && stamp.timeInvalid==Invalid::None && phase_==2;
    value_.identitiesValid=value_.identitiesValid && !stamp.identityResult && stamp.kernelId==value_.callerKernelId;
    bool stored=false;
    lease_.finish([&]() noexcept {stored=owner_->recording_.append(value_);});
    return stored;
}
bool CallbackMeasurementWindow::exportDrained(FILE* file,const char* runId,const char* scene,unsigned fps,bool fsr) noexcept {
    const auto state=lifetime_.status();
    if(!state.closed || !state.drained)return false;
    // Poison inside the existing bounded recorder; don't forge a missing
    // callback, renumber sequence IDs or claim a physical-frame stream.
    if(state.loss)recording_.invalidate();
    if(!recording_.close())return false;
    return recording_.write(file,runId,scene,fps,fsr);
}
}
