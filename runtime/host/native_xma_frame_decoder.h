#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace recomp::audio {
// Owns the actual Xenia FFmpeg XMAFRAMES decoder. One instance per native
// context; serialized by its caller. Packet joining, loops and the guest PCM
// ring belong to the stream adapter, not this compressed-frame codec.
class NativeXmaFrameDecoder final {
public:
    struct Frame {
        std::array<std::array<float, 512>, 2> planar{};
        std::uint32_t channels = 0, sampleRate = 0, samples = 0;
    };
    enum class Result { Decoded, InvalidInput, CodecFailure };
    NativeXmaFrameDecoder();
    ~NativeXmaFrameDecoder();
    NativeXmaFrameDecoder(const NativeXmaFrameDecoder&) = delete;
    NativeXmaFrameDecoder& operator=(const NativeXmaFrameDecoder&) = delete;
    bool available() const noexcept;
    bool configure(std::uint32_t sampleRate, std::uint32_t channels);
    // Input is one assembled XMA frame, starting at bit0..7 of its first byte.
    // Its original 15-bit length must equal bitCount, including that header.
    // Output is untouched unless the decoder produces one valid complete frame.
    Result decode(const std::uint8_t* data, std::size_t bytes,
                  std::uint32_t firstBit, std::uint32_t bitCount, Frame& output);
    void reset() noexcept;
private:
    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::uint32_t rate_ = 0, channels_ = 0;
};
} // namespace recomp::audio
