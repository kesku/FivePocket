// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>

namespace recomp::apple {

// A control-thread observation of the existing output unit and current route.
// clientFloatPacked means native-endian float32, packed and interleaved; merely
// having the float and packed flag bits is insufficient. This policy neither
// queries hardware nor restarts/reconfigures a unit.
struct AudioRouteSnapshot {
    bool querySucceeded{false};
    bool hasOutput{false};
    bool clientFloatPacked{false};
    double hardwareSampleRate{0};
    double ioBufferDuration{0};
    double clientSampleRate{0};
    std::uint32_t clientChannels{0};
    std::uint32_t clientBytesPerFrame{0};
    std::uint32_t clientFramesPerPacket{0};
    std::uint32_t clientBytesPerPacket{0};
    std::uint32_t clientBitsPerChannel{0};
    std::uint32_t maximumFramesPerSlice{0};
    bool expectedRunning{false};
    bool observedRunning{false};
};

// NativeAudioRenderClient caches its entire configuration at construction.
// Accept only a route that preserves its PCM format and bounded storage. The
// hardware may use a different positive finite rate when RemoteIO continues
// to expose the exact 48 kHz client format; its hardware rate is not substituted
// for the client's rate. The published framesPerBurst is a startup scheduling
// hint, NOT a requirement that every later hardware callback has that length:
// RemoteIO negotiates different periods during Bluetooth route changes. Keep
// the published hint immutable, consume actual callback frames through the
// ring, and validate the live cadence independently against storage capacity.
// No resampling, counter synthesis, reconfiguration or restart happens here.
// All returned strings have static lifetime.
inline const char* audioRouteRejection(const AudioRouteSnapshot& route,
                                      std::uint32_t expectedBurst,
                                      std::uint32_t capacity) noexcept {
    if (!route.querySucceeded) return "query_failed";
    if (!capacity || !expectedBurst || expectedBurst > capacity)
        return "invalid_expected_configuration";
    if (!route.hasOutput) return "missing_output";
    if (!std::isfinite(route.hardwareSampleRate) || !(route.hardwareSampleRate > 0))
        return "invalid_hardware_sample_rate";
    if (!std::isfinite(route.ioBufferDuration) || !(route.ioBufferDuration > 0))
        return "invalid_io_buffer_duration";
    if (!route.clientFloatPacked || route.clientSampleRate != 48000.0 ||
        route.clientChannels != 2 || route.clientBytesPerFrame != 8 ||
        route.clientFramesPerPacket != 1 || route.clientBytesPerPacket != 8 ||
        route.clientBitsPerChannel != 32)
        return "invalid_client_format";
    if (!route.maximumFramesPerSlice || route.maximumFramesPerSlice > capacity)
        return "invalid_maximum_frames";
    // Check in floating point before any integer conversion: an arbitrarily
    // large but finite duration must not overflow an integer or wrap to a
    // seemingly compatible frame count.
    const double frames = route.ioBufferDuration * 48000.0;
    if (!std::isfinite(frames) || !(frames > 0))
        return "invalid_io_buffer_duration";
    if (std::ceil(frames) > static_cast<double>(capacity))
        return "cadence_exceeds_capacity";
    if (route.expectedRunning != route.observedRunning)
        return "unexpected_running_state";
    return nullptr;
}

} // namespace recomp::apple
