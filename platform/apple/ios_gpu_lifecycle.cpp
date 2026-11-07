// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/ios_gpu_lifecycle.h"

#include <condition_variable>
#include <atomic>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace recomp::apple {

struct IosGpuLifecycle::Impl {
    struct Queue {VkDevice device; std::uint64_t generation; std::size_t admitted=0;};
    struct Device {std::size_t admitted=0; bool retiring=false;};
    mutable std::mutex state;
    std::mutex lifetime;
    std::condition_variable changed;
    Phase phase=Phase::Disabled;
    std::atomic<bool> activated{false};
    std::unordered_map<VkQueue,Queue> queues;
    std::unordered_map<VkDevice,Device> devices;
    // Only the lifetime-lock owner uses this scratch storage. Capacity may be
    // reused, but live handles never survive a completed drain operation.
    std::vector<VkQueue> drainQueues;
    std::size_t admitted=0;
    std::size_t waiting=0;
    std::uint64_t nextGeneration=1;
    VkResult lastDrainResult=VK_SUCCESS;
    PFN_vkQueueWaitIdle waitIdle;
    IsMainThread isMain;
    Impl(PFN_vkQueueWaitIdle driver,IsMainThread main):waitIdle(driver),isMain(main){}
};

IosGpuOperationLease::IosGpuOperationLease(IosGpuLifecycle* owner,VkQueue queue,
        std::uint64_t generation,bool accepted) noexcept:
    owner_(owner),queue_(queue),generation_(generation),accepted_(accepted){}
IosGpuOperationLease::~IosGpuOperationLease(){reset();}
IosGpuOperationLease::IosGpuOperationLease(IosGpuOperationLease&& other) noexcept {
    *this=std::move(other);
}
IosGpuOperationLease& IosGpuOperationLease::operator=(IosGpuOperationLease&& other) noexcept {
    if(this!=&other){reset();owner_=std::exchange(other.owner_,nullptr);
        queue_=std::exchange(other.queue_,VK_NULL_HANDLE);generation_=std::exchange(other.generation_,0);
        accepted_=std::exchange(other.accepted_,false);}
    return *this;
}
void IosGpuOperationLease::reset() noexcept {
    if(owner_)owner_->release(queue_,generation_);
    owner_=nullptr;queue_=VK_NULL_HANDLE;generation_=0;accepted_=false;
}

IosGpuLifecycle::IosGpuLifecycle(PFN_vkQueueWaitIdle driver,IsMainThread main):
    impl_(std::make_unique<Impl>(driver,main)){}
IosGpuLifecycle::~IosGpuLifecycle()=default;

bool IosGpuLifecycle::activate(){
    std::lock_guard lock(impl_->state);
    if(impl_->phase!=Phase::Disabled || !impl_->waitIdle || !impl_->isMain)return false;
    impl_->phase=Phase::Running;impl_->activated.store(true,std::memory_order_release);return true;
}
bool IosGpuLifecycle::registerQueue(VkDevice device,VkQueue queue){
    if(!device || !queue)return false;
    std::lock_guard lock(impl_->state);
    if(impl_->phase==Phase::Failed || impl_->phase==Phase::Stopped)return false;
    auto found=impl_->queues.find(queue);
    if(found!=impl_->queues.end())return found->second.device==device && !impl_->devices.at(device).retiring;
    auto [owner,inserted]=impl_->devices.try_emplace(device);
    (void)inserted;
    if(owner->second.retiring || !impl_->nextGeneration)return false;
    impl_->queues.emplace(queue,Impl::Queue{device,impl_->nextGeneration++});
    impl_->changed.notify_all();return true;
}
IosGpuOperationLease IosGpuLifecycle::acquire(VkQueue queue){
    if(!impl_->activated.load(std::memory_order_acquire))return {nullptr,queue,0,true};
    const bool main=impl_->isMain && impl_->isMain();
    // The state mutex never covers Vulkan, I/O or a lifetime/renderer lock.
    // Main may briefly contend here, but never waits for the closed gate.
    std::unique_lock lock(impl_->state);
    if(impl_->phase==Phase::Disabled)return {nullptr,queue,0,true};
    auto found=impl_->queues.find(queue);
    if(found==impl_->queues.end())return {};
    const auto generation=found->second.generation;
    while(true){
        found=impl_->queues.find(queue);
        if(found==impl_->queues.end() || found->second.generation!=generation ||
           impl_->devices.at(found->second.device).retiring ||
           impl_->phase==Phase::Failed || impl_->phase==Phase::Stopped)return {};
        if(impl_->phase==Phase::Running){
            ++found->second.admitted;++impl_->devices.at(found->second.device).admitted;++impl_->admitted;
            return {this,queue,generation,true};
        }
        if(main)return {};
        ++impl_->waiting;impl_->changed.wait(lock);--impl_->waiting;
    }
}
bool IosGpuLifecycle::waitUntilRunning(){
    if(!impl_->activated.load(std::memory_order_acquire))return true;
    std::unique_lock lock(impl_->state);
    if(impl_->phase==Phase::Disabled)return true;
    if(!impl_->isMain || impl_->isMain())return false;
    impl_->changed.wait(lock,[&]{return impl_->phase==Phase::Running ||
        impl_->phase==Phase::Failed || impl_->phase==Phase::Stopped;});
    return impl_->phase==Phase::Running;
}
void IosGpuLifecycle::release(VkQueue queue,std::uint64_t generation) noexcept {
    std::lock_guard lock(impl_->state);
    const auto found=impl_->queues.find(queue);
    // Queue retirement cannot erase an admitted lease. Never decrement a new
    // queue that happens to reuse a retired opaque handle.
    if(found==impl_->queues.end() || found->second.generation!=generation || !found->second.admitted)
        std::terminate();
    --found->second.admitted;--impl_->devices.at(found->second.device).admitted;--impl_->admitted;
    impl_->changed.notify_all();
}
bool IosGpuLifecycle::requestPause(){
    std::lock_guard lock(impl_->state);
    switch(impl_->phase){
    case Phase::Running:impl_->phase=Phase::Closing;return true;
    case Phase::Closing:case Phase::Draining:case Phase::Drained:return false;
    default:return false;
    }
}
bool IosGpuLifecycle::resume(){
    std::lock_guard lock(impl_->state);
    if(impl_->phase!=Phase::Drained)return false;
    impl_->phase=Phase::Running;impl_->changed.notify_all();return true;
}
void IosGpuLifecycle::stop(){
    std::lock_guard lock(impl_->state);
    impl_->phase=Phase::Stopped;
    // Stop is terminal even if startup never reached activate(). Publish the
    // closed state to callers that would otherwise take the disabled bypass.
    impl_->activated.store(true,std::memory_order_release);
    impl_->changed.notify_all();
}
bool IosGpuLifecycle::drain(){
    if(!impl_->isMain || impl_->isMain())return false;
    std::unique_lock lifetime(impl_->lifetime);
    auto& queues=impl_->drainQueues;
    queues.clear();
    struct ClearSnapshot {
        std::vector<VkQueue>& queues;
        ~ClearSnapshot(){queues.clear();}
    } clearSnapshot{queues};
    {
        std::unique_lock lock(impl_->state);
        if(impl_->phase==Phase::Drained)return true;
        if(impl_->phase!=Phase::Closing)return false;
        impl_->phase=Phase::Draining;
        impl_->changed.wait(lock,[&]{return !impl_->admitted || impl_->phase==Phase::Stopped;});
        if(impl_->phase!=Phase::Draining)return false;
        try {
            queues.reserve(impl_->queues.size());
            for(const auto& [queue,owner]:impl_->queues){(void)owner;queues.push_back(queue);}
        } catch(const std::bad_alloc&) {
            // This is called from a dispatched lifecycle block. Complete the
            // normal failure handshake rather than escaping with a closed
            // gate whose parked workers were never notified.
            impl_->lastDrainResult=VK_ERROR_OUT_OF_HOST_MEMORY;
            impl_->phase=Phase::Failed;impl_->changed.notify_all();return false;
        }
    }
    for(auto queue:queues){
        // Lifetime lock excludes retireDevice until this raw handle is idle.
        // No state/queue/renderer mutex is held across the driver call.
        const VkResult result=impl_->waitIdle(queue);
        std::lock_guard lock(impl_->state);
        impl_->lastDrainResult=result;
        if(impl_->phase!=Phase::Draining)return false;
        if(result!=VK_SUCCESS){impl_->phase=Phase::Failed;impl_->changed.notify_all();return false;}
    }
    std::lock_guard lock(impl_->state);
    if(impl_->phase!=Phase::Draining)return false;
    impl_->phase=Phase::Drained;impl_->changed.notify_all();return true;
}
bool IosGpuLifecycle::retireDevice(VkDevice device){
    if(!device)return false;
    std::unique_lock lifetime(impl_->lifetime);
    std::unique_lock lock(impl_->state);
    auto owner=impl_->devices.find(device);
    if(owner==impl_->devices.end())return true;
    owner->second.retiring=true;impl_->changed.notify_all();
    impl_->changed.wait(lock,[&]{return !impl_->devices.at(device).admitted;});
    for(auto it=impl_->queues.begin();it!=impl_->queues.end();){
        if(it->second.device==device)it=impl_->queues.erase(it);else ++it;
    }
    impl_->devices.erase(device);impl_->changed.notify_all();return true;
}
IosGpuLifecycle::Snapshot IosGpuLifecycle::snapshot() const {
    std::lock_guard lock(impl_->state);
    return {impl_->phase,impl_->queues.size(),impl_->admitted,impl_->waiting,impl_->lastDrainResult};
}

namespace {
bool actualMainThread(){
#if defined(__APPLE__)
    return pthread_main_np()!=0;
#else
    return false;
#endif
}
}
IosGpuLifecycle& iosGpuLifecycle(){
    static auto* gate=new IosGpuLifecycle(&vkQueueWaitIdle,actualMainThread);return *gate;
}

} // namespace recomp::apple
