// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace recomp::apple {

class IosGpuLifecycle;

// Acquire before any queue/renderer lock. The lease must cover the entire raw
// Vulkan call, including its queue-lock wait. Move-only; release is automatic.
class IosGpuOperationLease {
    friend class IosGpuLifecycle;
    IosGpuLifecycle* owner_=nullptr;
    VkQueue queue_=VK_NULL_HANDLE;
    std::uint64_t generation_=0;
    bool accepted_=false;
    IosGpuOperationLease(IosGpuLifecycle*,VkQueue,std::uint64_t,bool) noexcept;
public:
    IosGpuOperationLease() noexcept=default;
    ~IosGpuOperationLease();
    IosGpuOperationLease(const IosGpuOperationLease&)=delete;
    IosGpuOperationLease& operator=(const IosGpuOperationLease&)=delete;
    IosGpuOperationLease(IosGpuOperationLease&&) noexcept;
    IosGpuOperationLease& operator=(IosGpuOperationLease&&) noexcept;
    explicit operator bool() const noexcept {return accepted_;}
    void reset() noexcept;
};

class IosGpuLifecycle {
    friend class IosGpuOperationLease;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void release(VkQueue,std::uint64_t) noexcept;
public:
    enum class Phase {Disabled,Running,Closing,Draining,Drained,Failed,Stopped};
    struct Snapshot {
        Phase phase=Phase::Disabled;
        std::size_t registeredQueues=0;
        std::size_t admittedOperations=0;
        std::size_t waitingOperations=0;
        VkResult lastDrainResult=VK_SUCCESS;
    };
    using IsMainThread=bool(*)();
    // Injected callbacks permit host testing of the actual coordination. The
    // process singleton below binds the raw driver and pthread_main_np.
    IosGpuLifecycle(PFN_vkQueueWaitIdle,IsMainThread);
    ~IosGpuLifecycle();
    IosGpuLifecycle(const IosGpuLifecycle&)=delete;
    IosGpuLifecycle& operator=(const IosGpuLifecycle&)=delete;

    // Opt in before starting guest threads; no mid-run activation is supported.
    bool activate();
    // Register immediately after queue creation, before its first submission.
    // State-only, so lazy presenter creation on main remains supported. A new
    // queue registered while closed has never-admitted work in this epoch.
    bool registerQueue(VkDevice,VkQueue);
    // Disabled mode is transparent. Enabled unknown/retiring queues reject.
    // Closed workers park; main callers take only the short state mutex and
    // never wait for foreground/drain on the CV. Ordinary state-lock contention
    // is not a failed submission. Never hold a queue or renderer lock here.
    IosGpuOperationLease acquire(VkQueue);
    // Frame producer only, off-main, with no queue/renderer lock held. This
    // parks without owning a GPU operation lease; the caller must recheck UI
    // lifecycle before presenting after its main-queue dispatch. Disabled mode
    // succeeds; failed/stopped or main-thread use while enabled rejects.
    bool waitUntilRunning();

    // Main-thread operations take only the short state lock; they never take a
    // lifetime/queue/renderer lock or call Vulkan. Resume requires proven drain.
    // requestPause returns true ONLY for a new Running -> Closing transition.
    // Repeated requests are harmless false; inspect snapshot to distinguish a
    // pending/completed pause from failed/stopped state. Schedule drain once.
    bool requestPause();
    bool resume();
    void stop();
    // Off-main only. Lock order: lifetime -> state. CV waits release state;
    // state is never held across driver calls. Drain holds lifetime throughout
    // raw queue waits, excluding owner teardown and stale VkQueue handles.
    bool drain();
    // Explicit resource teardown only; may block. NEVER call from pause/resume.
    // Call BEFORE the owner's raw vkDeviceWaitIdle and before destroying any
    // device/queue-owned resources. This rejects new leases and waits for CPU
    // operations; the owner's raw device-idle must still drain GPU work.
    // Removed queues cannot be passed to gated wrappers afterwards.
    bool retireDevice(VkDevice);
    Snapshot snapshot() const;
};

// Intentionally retained for process lifetime; runtime threads can outlive the
// application session. Local test instances must outlive all their leases.
IosGpuLifecycle& iosGpuLifecycle();

} // namespace recomp::apple
