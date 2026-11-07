// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "diag_log_sample.h"
#include "duo_input.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace recomp::duo::diagnostics {
enum class Target {File, Console};

class TouchLog {
public:
    using Sink = void (*)(void*, Target, const char*, std::size_t) noexcept;
    using Clock = long long (*)(void*) noexcept;
    static constexpr std::size_t EntryBytes = 384;
    static constexpr std::size_t DefaultBudget = 512 * 1024;

    TouchLog(Sink sink, Clock clock, void* context = nullptr,
             bool enabled = recomp::diag::verboseDiagnosticsEnabled(),
             std::size_t budget = DefaultBudget) noexcept
        : sink_(sink), clock_(clock), context_(context), enabled_(enabled), budget_(budget), stopped_(budget < EntryBytes) {}

    bool active() const noexcept {
        return enabled_ && sink_ && !stopped_.load(std::memory_order_relaxed);
    }

    template<class Formatter> void write(Target target, Formatter&& format) noexcept {
        if (!active()) return;
        std::lock_guard lock(mutex_);
        if (budget_ - bytes_ < EntryBytes) {
            stopped_.store(true, std::memory_order_relaxed);
            return;
        }
        char message[256]{};
        std::forward<Formatter>(format)(message, sizeof message);
        message[sizeof message - 1] = '\0';
        char line[EntryBytes]{};
        if (target == Target::File)
            std::snprintf(line, sizeof line, "%lld %s\n", clock_ ? clock_(context_) : 0, message);
        else
            std::snprintf(line, sizeof line, "%s\n", message);
        const auto size = std::strlen(line);
        sink_(context_, target, line, size);
        bytes_ += size;
        if (budget_ - bytes_ < EntryBytes) stopped_.store(true, std::memory_order_relaxed);
    }

    // Gesture and pad snapshots are inspected while the owning Input mutex is held.
    void states(const WheelGesture& lower, const MapTouch& top) noexcept {
        if (!active()) return;
        const std::array<int, 8> current{int(lower.state()), lower.buttons(), lower.rightX(), lower.rightY(),
                                       int(top.state()), top.buttons(), top.rightX(), top.rightY()};
        if (!haveGesture_ || current != gesture_) {
            gesture_ = current; haveGesture_ = true;
            write(Target::File, [&](char* line, std::size_t size) {
                std::snprintf(line, size, "gesture lower=%d buttons=%04x stick=%d,%d top=%d buttons=%04x rs=%d,%d",
                              current[0], unsigned(current[1]), current[2], current[3], current[4],
                              unsigned(current[5]), current[6], current[7]);
            });
        }
        if (current[0] != lastLower_) {
            lastLower_ = current[0];
            write(Target::Console, [&](char* line, std::size_t size) {
                std::snprintf(line, size, "APPLE_IOS_DUO_WHEEL_TOUCH state=%d buttons=%04x stick=%d,%d (touch input)",
                              current[0], unsigned(current[1]), current[2], current[3]);
            });
        }
        if (current[4] != lastTop_) {
            lastTop_ = current[4];
            write(Target::Console, [&](char* line, std::size_t size) {
                std::snprintf(line, size, "APPLE_IOS_DUO_MAP_TOUCH state=%d rx=%d ry=%d (touch input)",
                              current[4], current[6], current[7]);
            });
        }
    }

    void pads(const Pad& in, const Pad& out) noexcept {
        if (!active()) return;
        const std::array<int, 14> current{in.buttons, in.leftX, in.leftY, in.rightX, in.rightY, in.leftTrigger, in.rightTrigger,
                                        out.buttons, out.leftX, out.leftY, out.rightX, out.rightY, out.leftTrigger, out.rightTrigger};
        if (havePad_ && current == pad_) return;
        pad_ = current; havePad_ = true;
        write(Target::File, [&](char* line, std::size_t size) {
            std::snprintf(line, size, "pad in=%04x ls=%d,%d rs=%d,%d t=%u,%u out=%04x rs=%d,%d ls=%d,%d t=%d,%d",
                          unsigned(in.buttons), in.leftX, in.leftY, in.rightX, in.rightY,
                          unsigned(in.leftTrigger), unsigned(in.rightTrigger), unsigned(out.buttons),
                          out.rightX, out.rightY, out.leftX, out.leftY, out.leftTrigger, out.rightTrigger);
        });
    }

private:
    Sink sink_;
    Clock clock_;
    void* context_;
    bool enabled_;
    std::size_t budget_, bytes_ = 0;
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    bool haveGesture_ = false, havePad_ = false;
    std::array<int, 8> gesture_{};
    std::array<int, 14> pad_{};
    int lastLower_ = int(WheelGesture::State::IDLE), lastTop_ = int(MapTouch::State::IDLE);
};
}

namespace recomp::duo::app {
// The application and host tests share this locked input-to-diagnostics boundary.
template<class State, class Clock>
inline void mergeInput(Input& in, State& state, Clock&& inputClock, diagnostics::TouchLog& log) noexcept {
    std::lock_guard lock(in.m);
    if (!log.active()) {
        mergeCore(in, state, inputClock());
        return;
    }
    Pad before{}, after{};
    mergeCore(in, state, inputClock(), &before, &after);
    log.states(in.lower, in.top);
    log.pads(before, after);
}

}
