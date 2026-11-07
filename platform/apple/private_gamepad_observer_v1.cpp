// SPDX-License-Identifier: GPL-3.0-or-later
#include "private_gamepad_observer_v1.h"
#include "private_gamepad_observer_lifecycle.h"
#include "native_gamepad_delivery.h"
#include <atomic>

namespace {
std::atomic_flag admission=ATOMIC_FLAG_INIT;
std::atomic<bool> running{false};
std::atomic<RecompPrivateGamepadObserverV1> callback{nullptr};
std::atomic<void*> context{nullptr};
static_assert(std::atomic<RecompPrivateGamepadObserverV1>::is_always_lock_free &&
    std::atomic<void*>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);
void bridge(std::uint32_t user,std::uint32_t output,const recomp::input::GamepadState& state) noexcept {
    // Admission/clear is allowed only with NO guest workers. Thread creation
    // and join bind the pointer pair's lifetime; neither half changes in-run.
    const auto target=callback.load(std::memory_order_relaxed);
    if(!target)return;
    const auto packet=recomp::input::encodeGamepadState(state);
    target(context.load(std::memory_order_relaxed),user,output,packet.data(),packet.size());
}
struct Unlock {~Unlock(){admission.clear(std::memory_order_release);}};
}
extern "C" __attribute__((visibility("default")))
int recomp_private_trace_gamepad_observer_v1(RecompPrivateGamepadObserverV1 next,void* nextContext) noexcept {
    if(admission.test_and_set(std::memory_order_acquire))return 1;
    Unlock unlock;
    if(running.load(std::memory_order_acquire))return 1;
    if(next) {
        if(callback.load(std::memory_order_relaxed))return 1;
        if(!recomp::input::delivery::install(bridge))return 1;
        context.store(nextContext,std::memory_order_relaxed);
        callback.store(next,std::memory_order_relaxed);
    } else {
        if(!callback.load(std::memory_order_relaxed))return 0;
        if(!recomp::input::delivery::remove(bridge))return 1;
        callback.store(nullptr,std::memory_order_relaxed);
        context.store(nullptr,std::memory_order_relaxed);
    }
    return 0;
}
namespace recomp::input::delivery::embedding {
bool dedicatedObserverInstalled() noexcept {
    return callback.load(std::memory_order_relaxed)!=nullptr;
}
bool dedicatedObserverRunning() noexcept {
    return running.load(std::memory_order_acquire) && dedicatedObserverInstalled();
}
bool beginRun() noexcept {
    if(admission.test_and_set(std::memory_order_acquire))return false;
    Unlock unlock;
    if(running.load(std::memory_order_acquire))return false;
    running.store(true,std::memory_order_release);return true;
}
void finishAfterJoin() noexcept {
    // No guest callback survives the caller's join. A concurrent refused
    // registration may briefly hold admission; finish need not spin on it.
    running.store(false,std::memory_order_release);
}
}
