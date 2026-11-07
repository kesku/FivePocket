#include "native_xma_frame_decoder.h"
#include <cmath>
#include <cstring>
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/samplefmt.h"
}

namespace recomp::audio {
NativeXmaFrameDecoder::NativeXmaFrameDecoder() {
    const auto* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
    if (codec) context_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
}
NativeXmaFrameDecoder::~NativeXmaFrameDecoder() {
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&context_);
}
bool NativeXmaFrameDecoder::available() const noexcept { return context_ && frame_ && packet_; }
bool NativeXmaFrameDecoder::configure(std::uint32_t rate, std::uint32_t channels) {
    if (!available() || channels < 1 || channels > 2
            || (rate != 24000 && rate != 32000 && rate != 44100 && rate != 48000)) return false;
    if (rate_ == rate && channels_ == channels) return true;
    avcodec_close(context_);
    rate_ = channels_ = 0;
    context_->sample_rate = rate;
    context_->channels = channels;
    if (avcodec_open2(context_, avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES), nullptr) < 0) return false;
    rate_ = rate; channels_ = channels;
    return true;
}
void NativeXmaFrameDecoder::reset() noexcept {
    if (context_ && rate_) avcodec_flush_buffers(context_);
    if (frame_) av_frame_unref(frame_);
    if (packet_) av_packet_unref(packet_);
}
NativeXmaFrameDecoder::Result NativeXmaFrameDecoder::decode(const std::uint8_t* data,
        std::size_t bytes, std::uint32_t firstBit, std::uint32_t bitCount, Frame& output) {
    if (!available() || !rate_) return Result::CodecFailure;
    if (!data || firstBit > 7 || bitCount < 15 || bitCount >= 0x7FFF) return Result::InvalidInput;
    const auto payloadBytes = (firstBit + bitCount + 7) / 8;
    if (bytes < payloadBytes) return Result::InvalidInput;
    std::uint32_t declaredBits = 0;
    for (unsigned bit = firstBit; bit < firstBit + 15; ++bit)
        declaredBits = (declaredBits << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1U);
    if (declaredBits != bitCount) return Result::InvalidInput;
    av_frame_unref(frame_);
    av_packet_unref(packet_);
    if (av_new_packet(packet_, payloadBytes + 1) < 0) return Result::CodecFailure;
    const auto endPadding = payloadBytes * 8 - firstBit - bitCount;
    packet_->data[0] = (firstBit << 5) | (endPadding << 2);
    std::memcpy(packet_->data + 1, data, payloadBytes);
    const auto sent = avcodec_send_packet(context_, packet_);
    if (sent < 0) return sent == AVERROR_INVALIDDATA ? Result::InvalidInput : Result::CodecFailure;
    const auto received = avcodec_receive_frame(context_, frame_);
    if (received < 0) return received == AVERROR_INVALIDDATA ? Result::InvalidInput : Result::CodecFailure;
    if (frame_->format != AV_SAMPLE_FMT_FLTP || frame_->channels != int(channels_)
            || frame_->nb_samples != 512) return Result::CodecFailure;
    Frame staged;
    staged.channels = channels_; staged.sampleRate = rate_; staged.samples = 512;
    for (unsigned channel = 0; channel < channels_; ++channel) {
        if (!frame_->extended_data || !frame_->extended_data[channel]) return Result::CodecFailure;
        std::memcpy(staged.planar[channel].data(), frame_->extended_data[channel], 512 * sizeof(float));
        for (auto value : staged.planar[channel]) if (!std::isfinite(value)) return Result::CodecFailure;
    }
    output = staged;
    return Result::Decoded;
}
} // namespace recomp::audio
