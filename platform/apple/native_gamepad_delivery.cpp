// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_gamepad_delivery.h"
#include <atomic>
#include <cstring>

namespace recomp::input::delivery {
namespace {
std::atomic<Observer> observer{nullptr};
static_assert(std::atomic<Observer>::is_always_lock_free,
    "native delivery observer pointer must not introduce a hidden lock");
}
bool install(Observer next) noexcept {
    if(!next)return false;
    Observer empty=nullptr;
    return observer.compare_exchange_strong(empty,next,std::memory_order_release,std::memory_order_relaxed);
}
bool remove(Observer expected) noexcept {
    if(!expected)return false;
    return observer.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel,std::memory_order_relaxed);
}
__attribute__((noinline))
void copyAndObserve(void* destination,const std::array<std::uint8_t,16>& packet,
    std::uint32_t user,std::uint32_t output,const GamepadState& state) noexcept {
    std::memcpy(destination,packet.data(),packet.size());
    const auto current=observer.load(std::memory_order_relaxed);
    if(current)current(user,output,state);
}
}
