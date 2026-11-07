// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace recomp::apple {
// One serialized guest-side producer, one AudioUnit render consumer. The producer
// may also discard stalled output. A nonblocking claim keeps discarded slots from
// being reused while the callback still reads them. No allocation, guest callback,
// waiting or I/O on either data path. Destroy only after stopping the AudioUnit.
template <std::uint32_t Capacity> class StereoRing final {
    static_assert(Capacity > 0);
    std::array<float, std::size_t(Capacity) * 2> samples_{};
    alignas(64) std::atomic<std::uint64_t> written_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    alignas(64) std::atomic_flag consumerClaim_ = ATOMIC_FLAG_INIT;
    class ConsumerClaim final {
        std::atomic_flag& flag_;
        const bool acquired_;
    public:
        explicit ConsumerClaim(std::atomic_flag& flag) noexcept
            : flag_(flag), acquired_(!flag.test_and_set(std::memory_order_acquire)) {}
        ConsumerClaim(const ConsumerClaim&) = delete;
        ConsumerClaim& operator=(const ConsumerClaim&) = delete;
        ~ConsumerClaim() { if (acquired_) flag_.clear(std::memory_order_release); }
        explicit operator bool() const noexcept { return acquired_; }
    };
public:
    std::uint32_t write(const float* input, std::uint32_t count) noexcept {
        if (!input) return 0;
        const auto w = written_.load(std::memory_order_relaxed);
        const auto r = read_.load(std::memory_order_acquire);
        const auto accepted = std::min<std::uint64_t>(count, Capacity - (w-r));
        for (std::uint32_t i=0;i<accepted;++i) {
            const auto index = ((w+i)%Capacity)*2;
            samples_[index]=input[i*2]; samples_[index+1]=input[i*2+1];
        }
        written_.store(w+accepted, std::memory_order_release);
        return static_cast<std::uint32_t>(accepted);
    }
    std::uint32_t render(float* output, std::uint32_t count) noexcept {
        if (!output) return 0;
        const ConsumerClaim claim(consumerClaim_);
        if (!claim) {
            std::fill(output,output+std::size_t(count)*2,0.0f);
            return 0;
        }
        const auto r=read_.load(std::memory_order_relaxed);
        const auto w=written_.load(std::memory_order_acquire);
        const auto consumed=std::min<std::uint64_t>(count,w-r);
        for (std::uint32_t i=0;i<consumed;++i) {
            const auto index=((r+i)%Capacity)*2;
#if defined(APPLE_AUDIO_MUTANT_SWAP_CHANNELS)
            output[i*2]=samples_[index+1]; output[i*2+1]=samples_[index];
#else
            output[i*2]=samples_[index]; output[i*2+1]=samples_[index+1];
#endif
        }
        // Hardware underflow is silence, never invented guest frames/counters.
        std::fill(output+consumed*2,output+std::size_t(count)*2,0.0f);
#if defined(APPLE_AUDIO_MUTANT_COUNT_SILENCE)
        read_.store(r+count,std::memory_order_release);
#else
        read_.store(r+consumed,std::memory_order_release);
#endif
        return static_cast<std::uint32_t>(consumed);
    }
    // Keep the title's clock moving when output stalls (for example a Bluetooth
    // route switch). A callback may still be in flight, so defer discard while it
    // holds sample slots. Neither path waits for the other.
    std::uint64_t discard(std::uint64_t count) noexcept {
        const ConsumerClaim claim(consumerClaim_);
        if (!claim) return 0;
        const auto r=read_.load(std::memory_order_relaxed);
        const auto w=written_.load(std::memory_order_acquire);
        const auto n=std::min<std::uint64_t>(count,w-r);
        read_.store(r+n,std::memory_order_release);
        return n;
    }
    std::uint64_t written() const noexcept { return written_.load(std::memory_order_acquire); }
    std::uint64_t read() const noexcept { return read_.load(std::memory_order_acquire); }
};

// (Claude, 2026-10-20) stall drain decision, polled by the title's audio thread (single caller). While the output unit should
// be running but its read position has not moved for thresholdNs, the frames the device would have played since the stall
// began (rate x time) are due for discard, so the title's audio clock keeps real time (a Bluetooth route switch stopped the
// unit's pulls for 4.3 s in c285 and froze the game with it). The caller discards, then records the read position it left.
struct StallDrain {
    std::uint64_t lastRead=~std::uint64_t{0};
    std::int64_t sinceNs=0;
    std::uint64_t credited=0;   // frames already made due during this stall
    std::uint64_t dropped=0;    // frames actually discarded during this stall (for the log)
    std::int64_t stalledNs=0;   // the stall's length so far
};
inline std::uint64_t stallDrainDue(StallDrain& s,std::uint64_t readNow,std::int64_t nowNs,bool shouldRun,
                                   std::int64_t thresholdNs,std::uint32_t rate) noexcept {
    if(!shouldRun || readNow!=s.lastRead){s.lastRead=readNow;s.sinceNs=nowNs;s.credited=0;s.dropped=0;s.stalledNs=0;return 0;}
    s.stalledNs=nowNs-s.sinceNs;
    if(s.stalledNs<thresholdNs)return 0;
    const auto due=static_cast<std::uint64_t>((static_cast<long double>(s.stalledNs)*rate)/1e9L);
    if(due<=s.credited)return 0;
    const auto n=due-s.credited;s.credited=due;return n;
}
inline void stallDrainNote(StallDrain& s,std::uint64_t readBefore,std::uint64_t discarded) noexcept {
    s.lastRead=readBefore+discarded;s.dropped+=discarded;
}
}
