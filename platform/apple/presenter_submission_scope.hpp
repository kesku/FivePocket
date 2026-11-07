// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "vulkan_queue_lock.h"
namespace recomp::apple::sharedpresent {
// Presenter writes only presenter-owned LUT/output/readback resources. The
// borrowed generic source is read-only and leased through completed readback.
// Do not invalidate the guest-native write ticket with this private submission.
class PresenterSubmissionScope final {
    bool previous_;
public:
    PresenterSubmissionScope() noexcept : previous_(recomp::gpu::genericQueueSubmission) {
        recomp::gpu::genericQueueSubmission=true;
    }
    ~PresenterSubmissionScope() {recomp::gpu::genericQueueSubmission=previous_;}
    PresenterSubmissionScope(const PresenterSubmissionScope&)=delete;
    PresenterSubmissionScope& operator=(const PresenterSubmissionScope&)=delete;
};
inline VkResult submitPresenter(VkQueue queue,std::uint32_t count,const VkSubmitInfo* submits,VkFence fence) {
    PresenterSubmissionScope scope;
    return recomp::gpu::lockedQueueSubmit(queue,count,submits,fence);
}
}
