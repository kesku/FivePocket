// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_audio_render_client.h"
#include <memory>

namespace recomp::audio {
struct AppleAudioLifecycleStatus {
    bool available{false};
    bool applicationActive{true};
    bool running{false};
    bool sessionActive{false};
    bool stopPending{false};
    bool pauseRequested{false};
    bool interrupted{false};
    bool resumePermitted{true};
    bool terminal{false};
    bool invalidated{false};
};
// iOS control requests, not a guest-wide pause or a synchronous drain. They do
// not discard PCM or change the published stream configuration/counters.
// pauseRequested=true with running=true or stopPending=true means the queued
// stop has not completed. Only !running && !stopPending acknowledges stopped.
// terminate is irreversible for the process and must accompany a real terminal
// runtime result, not a temporary application-state transition.
// On macOS these leave the baseline backend unchanged.
void setAppleAudioApplicationActive(bool active) noexcept;
void terminateAppleAudioLifecycle() noexcept;
AppleAudioLifecycleStatus appleAudioLifecycleStatus() noexcept;

// Apple replacement behind the existing playback interface. Not wired into the
// shared runner until its Apple branch is proposed/adopted; no Android ABI edit.
class AppleAudioOutputStream final : public NativeAudioPlaybackBackend {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    AppleAudioOutputStream();
    ~AppleAudioOutputStream() override;
    bool open();
    void close() noexcept;
    bool requestStart() noexcept override;
    bool started() const noexcept override;
    std::int32_t writeStereo(const float*,std::uint32_t) noexcept override;
    std::optional<std::uint64_t> framesWritten() const noexcept override;
    // Frames supplied to AudioUnit, not proof of physical speaker presentation.
    std::optional<std::uint64_t> framesRead() const noexcept override;
    std::optional<bootstrap::NativeAudioConfiguration> configuration() const noexcept override;
};
}
