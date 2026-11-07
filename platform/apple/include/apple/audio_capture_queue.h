// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>

namespace recomp::apple {
// Optional diagnostics only. Exactly one AudioUnit producer and one file-writer
// consumer. No allocation, locks, file I/O, logging or wake syscall in push().
// Capture actual output (including playback underrun silence), not queued input.
// Overflow permanently invalidates this capture; it never backpressures audio.
template<std::uint32_t Capacity>
class AudioCaptureQueue final {
    static_assert(Capacity && (Capacity & (Capacity-1)) == 0);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
    std::array<float, std::size_t(Capacity)*2> samples_{};
    const std::uint64_t limit_;
    alignas(64) std::atomic<std::uint64_t> written_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    std::atomic<bool> overflow_{false}, finished_{false};
public:
    explicit AudioCaptureQueue(std::uint64_t limit) noexcept : limit_(limit) {
        if(!limit) finished_.store(true, std::memory_order_relaxed);
    }
    // Runtime samples and frame count were validated by the AudioUnit callback.
    void push(const float* output, std::uint32_t frames) noexcept {
        if(!output || !frames || finished_.load(std::memory_order_relaxed)) return;
        const auto write=written_.load(std::memory_order_relaxed);
        const auto read=read_.load(std::memory_order_acquire);
        const auto count=std::min<std::uint64_t>(frames, limit_-write);
        if(count>Capacity-(write-read)) {
#if !defined(APPLE_CAPTURE_MUTANT_HIDE_OVERFLOW)
            overflow_.store(true, std::memory_order_relaxed);
#endif
            finished_.store(true, std::memory_order_release);
            return;
        }
        const auto first=std::min<std::uint64_t>(count, Capacity-(write&(Capacity-1)));
        std::memcpy(samples_.data()+(write&(Capacity-1))*2, output, first*8);
        std::memcpy(samples_.data(), output+first*2, (count-first)*8);
#if defined(APPLE_CAPTURE_MUTANT_SILENCE)
        samples_.fill(0);
#endif
        written_.store(write+count, std::memory_order_release);
        if(write+count==limit_) finished_.store(true, std::memory_order_release);
    }
    // Only the consumer calls pop; no fabricated padding or dropped timestamps.
    std::uint32_t pop(float* output, std::uint32_t maximum) noexcept {
        if(!output || !maximum) return 0;
        const auto read=read_.load(std::memory_order_relaxed);
        const auto write=written_.load(std::memory_order_acquire);
        const auto count=std::min<std::uint64_t>(maximum, write-read);
        const auto first=std::min<std::uint64_t>(count, Capacity-(read&(Capacity-1)));
        std::memcpy(output, samples_.data()+(read&(Capacity-1))*2, first*8);
        std::memcpy(output+first*2, samples_.data(), (count-first)*8);
        read_.store(read+count, std::memory_order_release);
        return static_cast<std::uint32_t>(count);
    }
    // Called only after the AudioUnit has stopped invoking its producer.
    void finishAfterProducerStopped() noexcept {finished_.store(true,std::memory_order_release);}
    bool finished() const noexcept {return finished_.load(std::memory_order_acquire);}
    bool overflowed() const noexcept {return overflow_.load(std::memory_order_acquire);}
    std::uint64_t written() const noexcept {return written_.load(std::memory_order_acquire);}
    std::uint64_t read() const noexcept {return read_.load(std::memory_order_acquire);}
    bool complete() const noexcept {
#if defined(APPLE_CAPTURE_MUTANT_PARTIAL_COMPLETE)
        return finished() && !overflowed();
#else
        return finished() && !overflowed() && written()==limit_ && read()==limit_;
#endif
    }
};
}
