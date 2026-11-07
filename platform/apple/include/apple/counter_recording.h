// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "apple/performance_sampler.h"
#include <array>
#include <cstdio>

namespace recomp::apple::performance {
struct CounterRecordingStatus {
    std::size_t stored=0;
    std::uint64_t dropped=0;
    bool finished=false,writeFailed=false;
};

// One external serialized caller owns both Sampler and this synchronous writer.
// No worker, lock, file creation, close, retry, device operation or engine hook.
// The FILE* is owner-supplied and must remain alive through finish(). Bounds are
// transport bounds, not a claim that the sampler enumerated every thread.
class CounterRecording {
public:
    static constexpr std::size_t MaxSamples=512;
    explicit CounterRecording(FILE*,const char* runId,std::size_t maxSamples=MaxSamples,
                              std::size_t maxThreads=MaxThreads);
    CounterRecording(const CounterRecording&)=delete;
    CounterRecording& operator=(const CounterRecording&)=delete;
    bool append(const Snapshot&) noexcept;
    bool finish() noexcept;
    CounterRecordingStatus status() const noexcept {return status_;}
private:
    bool header() noexcept;
    FILE* file_;
    std::array<char,129> runId_{};
    const std::size_t maxSamples_,maxThreads_;
    CounterRecordingStatus status_;
    bool started_=false;
    std::uint64_t lastSequence_=0;
};
}
