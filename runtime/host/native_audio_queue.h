#pragma once

#include "guest_audio_frame.h"

namespace recomp::audio {
class StereoFrameWriter {
public:
    virtual ~StereoFrameWriter() = default;
    // Non-blocking, no guest reentry. Returns exactly the accepted frame count,
    // zero for backpressure, negative for failure. Never reports playback.
    virtual std::int32_t writeStereo(const float* interleaved,std::uint32_t frames) noexcept = 0;
};

// Bounded snapshots. Not internally synchronized: the owning
// NativeAudioRenderClient serializes every call (producer submit and consumer
// drainOnce alike) under L_audio, so a standalone queue must stay confined to
// one thread. Accepted packets survive partial writes and backpressure.
// A sink error permanently stops this queue: retrying an ambiguous write could
// duplicate sound. Destruction discards pending data; it does not mark it played.
class NativeAudioQueue final {
public:
    static constexpr std::size_t CapacityPackets=8;
    enum class Drain { Empty, Progress, Backpressure, Failed };
    bool submit(const std::uint8_t* snapshot,std::size_t bytes) noexcept {
        if (failed_ || count_==CapacityPackets || acceptedPackets_==UINT64_MAX ||
            clippedSamples_>UINT64_MAX-AudioFramesPerPacket*2) return false;
        PlanarAudioPacket planar;StereoAudioPacket stereo;std::uint32_t clipped=0;
        if (!decodePlanarAudioPacket(snapshot,bytes,planar) ||
            !mixNormalizedStereoNoLfe(planar,stereo,clipped)) return false;
        packets_[(head_+count_)%CapacityPackets]=stereo;
        ++count_;++acceptedPackets_;clippedSamples_+=clipped;return true;
    }
    Drain drainOnce(StereoFrameWriter& sink) noexcept {
        if (failed_) return Drain::Failed;
        if (!count_) return Drain::Empty;
        const auto remaining=static_cast<std::uint32_t>(AudioFramesPerPacket)-offset_;
        if (writtenFrames_>UINT64_MAX-remaining) {failed_=true;return Drain::Failed;}
        const auto written=sink.writeStereo(packets_[head_].data()+offset_*2,remaining);
        if (written<0 || static_cast<std::uint32_t>(written)>remaining) {failed_=true;return Drain::Failed;}
        if (!written) return Drain::Backpressure;
        offset_+=static_cast<std::uint32_t>(written);writtenFrames_+=written;
        if (offset_==AudioFramesPerPacket) {offset_=0;head_=(head_+1)%CapacityPackets;--count_;}
        return Drain::Progress;
    }
    std::size_t pendingPackets() const noexcept { return count_; }
    std::uint64_t acceptedPackets() const noexcept { return acceptedPackets_; }
    std::uint64_t writtenFrames() const noexcept { return writtenFrames_; }
    std::uint64_t clippedSamples() const noexcept { return clippedSamples_; }
    bool failed() const noexcept { return failed_; }
private:
    std::array<StereoAudioPacket,CapacityPackets> packets_{};
    std::size_t head_{0},count_{0};std::uint32_t offset_{0};
    std::uint64_t acceptedPackets_{0},writtenFrames_{0},clippedSamples_{0};
    bool failed_{false};
};
}
