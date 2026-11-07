// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace recomp::apple::performance {
// A callback receipt is NOT a displayed frame. This recorder changes no
// scheduling, fences, pixel verification or guest delivery. Production hookup
// and the timing perturbation of its clock reads remain unqualified.
struct CallbackMeasurement {
    std::uint32_t sourceSequence=0;
    std::uint64_t entryNs=0,mainEntryNs=0,presentEndNs=0,returnNs=0;
    std::uint64_t callerKernelId=0,mainKernelId=0;
    bool accepted=false,clockValid=false,identitiesValid=false;
};
enum class RecordingState : unsigned { Open, Closed };
struct RecordingStatus {
    RecordingState state=RecordingState::Open;
    std::size_t stored=0,capacity=0;
    bool loss=false,invalid=false,writeFailed=false,sealed=false;
};

// Owner creates before guest start; append owns a nonblocking try-gate only
// during a bounded memory copy. No allocation, I/O, wait or retry in append.
// close() also tries once: contention permanently poisons the result, never
// waits for an in-progress producer or reads its storage. Lifetime must outlive
// every callback. A single separate consumer can write ONLY after close.
class FrameRecording {
public:
    static constexpr std::size_t MaxCapacity=32768;
    explicit FrameRecording(std::size_t capacity=MaxCapacity);
    ~FrameRecording();
    FrameRecording(const FrameRecording&)=delete;
    FrameRecording& operator=(const FrameRecording&)=delete;
    bool append(const CallbackMeasurement&) noexcept;
    // Diagnostic ownership loss is distinct from an invented frame/drop count.
    void invalidate() noexcept {invalid_.store(true,std::memory_order_release);}
    bool close() noexcept;
    RecordingStatus status() const noexcept;
    // No file creation/overwrite here. Caller owns a fresh authorized stream.
    // Schema has an exploratory callback-entry label, never display-presented
    // or a timing-qualified purpose. CPU/core/GPU data are not invented.
    bool write(FILE*,const char* runId,const char* scene,unsigned fps,bool fsr) noexcept;
private:
    std::unique_ptr<CallbackMeasurement[]> records_;
    const std::size_t capacity_;
    std::atomic_flag gate_=ATOMIC_FLAG_INIT;
    std::atomic<RecordingState> state_{RecordingState::Open};
    std::atomic<std::size_t> stored_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> loss_{false},invalid_{false},writeFailed_{false},sealed_{false};
    bool writeStarted_=false; // serialized consumer, never changed by producer
};
}
