#pragma once

#include "guest_audio_backend.h"
#include "native_audio_queue.h"
#include <memory>
#include <mutex>

namespace recomp::audio {
class NativeAudioPlaybackBackend : public bootstrap::GuestAudioBackend, public StereoFrameWriter {
public:
    virtual bool requestStart() noexcept = 0;
    virtual bool started() const noexcept = 0;
    virtual std::optional<std::uint64_t> framesWritten() const noexcept = 0;
    virtual std::optional<std::uint64_t> framesRead() const noexcept = 0;
};

// One experimental native render client, never driven by an AAudio data callback. Every method
// may be called from the native callback worker (poll/beginCallback/finishCallback, and submit
// from its guest callback) and interleaved with import handlers on other guest threads
// (statistics, canActivate, publishWorker): all state, the queue and every backend call are
// serialized by L_audio. Lock order (multicore plan): R (registry) > ... > L_audio > leaves; the
// registry calls in holding R, the client never calls the registry or guest code, and backend
// calls made under L_audio never block (AAudio writes use timeout 0). No guest pointers are
// retained and no synthetic silence is inserted. This is a native buffering policy, not a
// reconstruction of console DMA timing or multi-client mixing.
class NativeAudioRenderClient final {
public:
    static constexpr std::uint32_t Handle=0x41550000; // Opaque reference-profile slot zero.
    enum class Demand { Callback, Wait, Failed };
    struct Statistics {
        std::uint32_t workerToken{0};
        std::uint64_t callbacksEntered{0},callbacksCompleted{0},acceptedPackets{0};
        std::uint64_t writtenFrames{0},serviceReadFrames{0},clippedSamples{0};
        std::size_t pendingPackets{0};
        bool startRequested{false},callbackActive{false},failed{false};
    };
    explicit NativeAudioRenderClient(std::shared_ptr<NativeAudioPlaybackBackend> backend)
        : backend_(std::move(backend)) {
        const auto c=backend_?backend_->configuration():std::nullopt;
        if (c && bootstrap::validNativeAudioConfiguration(*c)) configuration_=*c;
    }
    // Immutable after construction.
    const std::shared_ptr<NativeAudioPlaybackBackend>& backend() const noexcept { return backend_; }
    bool canActivate() const noexcept {
        std::lock_guard lock(audioLock());
        return !worker_ && !failed_ && sameConfiguration() && !backend_->started() &&
            backend_->framesWritten()==0 && backend_->framesRead()==0;
    }
    // Publication is non-fallible after canActivate() and a successful gated
    // enqueue. No task runs inline between those operations. Storage/backend
    // must outlive executor shutdown. A lost native device fails at next poll.
    void publishWorker(std::uint32_t token) noexcept {
        std::lock_guard lock(audioLock());
        if (!token || worker_ || failed_) {failed_=true;return;}
        worker_=token;
    }
    Demand poll(std::uint32_t token) noexcept {
        std::lock_guard lock(audioLock());
        return pollLocked(token);
    }
    bool beginCallback(std::uint32_t token) noexcept {
        std::lock_guard lock(audioLock());
        if (pollLocked(token)!=Demand::Callback || entered_==UINT64_MAX) return false;
        inCallback_=true;submitted_=false;++entered_;return true;
    }
    bool submit(std::uint32_t token,std::uint32_t handle,const std::uint8_t* snapshot,std::size_t size) noexcept {
        std::lock_guard lock(audioLock());
        if (failed_ || !worker_ || token!=worker_ || handle!=Handle || !inCallback_ || submitted_ ||
            !sameConfiguration() || !queue_.submit(snapshot,size)) return false;
        submitted_=true;return true;
    }
    bool finishCallback(std::uint32_t token) noexcept {
        std::lock_guard lock(audioLock());
        if (failed_ || token!=worker_ || !inCallback_ || !submitted_) {fail();return false;}
        inCallback_=false;submitted_=false;++completed_;return true;
    }
    Statistics statistics() const noexcept {
        std::lock_guard lock(audioLock());
        return {worker_,entered_,completed_,queue_.acceptedPackets(),queue_.writtenFrames(),readFrames_,
            queue_.clippedSamples(),queue_.pendingPackets(),startRequested_,inCallback_,failed_};
    }
private:
    // L_audio. One lock for every client (one live client per process); leaked so no call can
    // outlive it during static destruction.
    static std::mutex& audioLock() noexcept { static auto* lock=new std::mutex; return *lock; }
    Demand pollLocked(std::uint32_t token) noexcept {
        if (failed_ || !worker_ || token!=worker_ || inCallback_ || !sameConfiguration()) return fail();
        // Bounded work even for a writer accepting one frame per call.
        for(std::size_t i=0;i<NativeAudioQueue::CapacityPackets && queue_.pendingPackets();++i) {
            const auto result=queue_.drainOnce(*backend_);
            if (result==NativeAudioQueue::Drain::Failed) return fail();
            if (result==NativeAudioQueue::Drain::Backpressure) break;
        }
        if (!startRequested_ && queue_.writtenFrames()) {
            if (!backend_->requestStart()) return fail();
            startRequested_=true;
        }
        const auto written=backend_->framesWritten(),read=backend_->framesRead();
        if (!written || !read || *written!=queue_.writtenFrames() || *read<readFrames_ || *read>*written)
            return fail(); // Unsupported counter behavior is never fabricated consumption.
        readFrames_=*read;
        if (queue_.acceptedPackets()>UINT64_MAX/AudioFramesPerPacket) return fail();
        const auto accepted=queue_.acceptedPackets()*AudioFramesPerPacket;
        if (readFrames_>accepted) return fail();
        // Prime with real packets through the actual native capacity. A fixed
        // eight-packet credit would deadlock devices with bursts >2048 frames.
        const auto target=(std::uint64_t{configuration_.bufferCapacityFrames}+AudioFramesPerPacket-1)
            /AudioFramesPerPacket*AudioFramesPerPacket;
        return accepted-readFrames_<target && queue_.pendingPackets()<NativeAudioQueue::CapacityPackets
            ?Demand::Callback:Demand::Wait;
    }
    bool sameConfiguration() const noexcept {
        const auto c=backend_?backend_->configuration():std::nullopt;
        return c && bootstrap::validNativeAudioConfiguration(*c) &&
            c->sampleRate==configuration_.sampleRate && c->channels==configuration_.channels &&
            c->bufferCapacityFrames==configuration_.bufferCapacityFrames && c->framesPerBurst==configuration_.framesPerBurst;
    }
    Demand fail() noexcept {failed_=true;return Demand::Failed;}
    std::shared_ptr<NativeAudioPlaybackBackend> backend_; // Set once by the constructor.
    bootstrap::NativeAudioConfiguration configuration_{}; // Set once by the constructor.
    // Everything below is guarded by L_audio.
    NativeAudioQueue queue_;
    std::uint32_t worker_{0};std::uint64_t entered_{0},completed_{0},readFrames_{0};
    bool inCallback_{false},submitted_{false},startRequested_{false},failed_{false};
};
}
