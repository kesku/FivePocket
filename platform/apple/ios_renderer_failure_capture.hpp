// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
namespace recomp::gpu::rendererfailure {
using Capture=void(*)(const char*,int,std::uint64_t) noexcept;
inline std::atomic<Capture> capture{nullptr}; // optional process-lifetime diagnostic provider
// Failure-only. Caller has returned from Vulkan queue wrappers: no queue mutex
// or lifecycle admission lock may be held during the existing thread snapshot.
inline void notify(const char* operation,int result,std::uint64_t frames) noexcept {
    const int saved=errno;
#if defined(__APPLE__)
    if(const auto fn=capture.load(std::memory_order_acquire))fn(operation,result,frames);
#else
    (void)operation;(void)result;(void)frames;
#endif
    errno=saved;
}
}
