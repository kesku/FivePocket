#pragma once

#include <cstdint>
#include <optional>

namespace recomp::bootstrap {

struct NativeAudioConfiguration {
    std::uint32_t sampleRate{0}, channels{0}, bufferCapacityFrames{0}, framesPerBurst{0};
};
inline bool validNativeAudioConfiguration(const NativeAudioConfiguration& c) noexcept {
    return c.sampleRate == 48000 && c.channels == 2 && c.framesPerBurst > 0 &&
        c.framesPerBurst <= c.bufferCapacityFrames && c.bufferCapacityFrames <= 48000;
}

// Output-only native stream. Querying an open stream is not playing samples,
// registering a guest callback, or verifying an audible physical speaker.
// Invoked under the registry lock; implementations must not reenter guest code.
class GuestAudioBackend {
public:
    virtual ~GuestAudioBackend() = default;
    virtual std::optional<NativeAudioConfiguration> configuration() const noexcept = 0;
};
}
