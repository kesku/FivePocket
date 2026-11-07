// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_diagnostics.h"
#include "host/native_gamepad_state.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
using recomp::duo::app::Input;
using recomp::duo::app::mergeInput;
using recomp::duo::diagnostics::Target;
using recomp::duo::diagnostics::TouchLog;
using recomp::input::GamepadState;

struct Output {
    unsigned clocks = 0;
    std::vector<std::string> lines;
    static long long clock(void* context) noexcept {
        auto& out = *static_cast<Output*>(context);
        return ++out.clocks;
    }
    static void sink(void* context, Target, const char* text, std::size_t size) noexcept {
        static_cast<Output*>(context)->lines.emplace_back(text, size);
    }
};

void disabledWork() {
    Output output;
    TouchLog log(Output::sink, Output::clock, &output, false);
    Input input;
    unsigned inputClocks = 0, formats = 0;
    for (unsigned i = 0; i != 20; ++i) {
        GamepadState pad{};pad.packet = i;
        mergeInput(input, pad, [&] { ++inputClocks; return 1000LL; }, log);
        assert(pad.packet == i);
        log.write(Target::File, [&](char*, std::size_t) { ++formats; });
    }
    assert(inputClocks == 20);
    assert(formats == 0 && output.clocks == 0 && output.lines.empty());
}

std::vector<std::array<std::uint8_t, 16>> sequence(bool verbose, Output& output) {
    TouchLog log(Output::sink, Output::clock, &output, verbose);
    Input input;
    input.lower.nativeMap(true);
    unsigned inputClocks = 0;
    std::vector<std::array<std::uint8_t, 16>> result;
    auto sample = [&](long long time, std::uint32_t packet, std::uint16_t buttons = 0) {
        GamepadState state{};
        state.packet = packet;state.buttons = buttons;
        state.leftX = 1234;state.leftY = -2345;state.rightX = 19;state.rightY = -29;
        state.leftTrigger = 45;state.rightTrigger = 67;
        mergeInput(input, state, [&] { ++inputClocks; return time; }, log);
        result.push_back(recomp::input::encodeGamepadState(state));
        assert(state.leftX == 1234 && state.leftY == -2345);
        assert(state.leftTrigger == 45 && state.rightTrigger == 67);
        return state;
    };
    assert(sample(0, 7).packet == 7);
    input.top.down(100, 1000, 400, 1920, 1080);
    assert(sample(100, 8).packet == 8);
    input.top.up(150);
    const auto tap = sample(150, 9, 0x1000);
    assert((tap.buttons & 0x1c00) == 0x1c00);
    assert((tap.packet & 0xff000000u) == 0x41000000u);
    assert(sample(200, 10, 0x1000).packet == 0x4100000au);
    assert(sample(301, 11).packet == 11);

    input.top.down(1000, 900, 500, 1920, 1080);
    input.top.move(1010, 1100, 500);
    const auto drag = sample(1010, 12);
    assert((drag.buttons & 0x0400) != 0 && (drag.packet & 0x40000000u) != 0);
    input.top.move(1026, 1200, 480);
    const auto moved = sample(1026, 13);
    assert(moved.rightX != drag.rightX);
    assert((moved.packet & 0x3f000000u) != (drag.packet & 0x3f000000u));
    const auto repeated = sample(1026, 14);
    assert((repeated.packet & 0xff000000u) == (moved.packet & 0xff000000u));

    input.top.pointerDown(1030, 1350, 550);
    input.top.move2(1046, 850, 480, 1450, 580);
    const auto pinch = sample(1046, 15);
    assert(pinch.rightX == recomp::duo::MapTouch::PINCH);
    input.top.pointerUp(1050);input.top.up(1051);
    assert(sample(1051, 16).packet == 16);

    assert(sample(2000, 17, recomp::duo::WheelGesture::LB).packet == 17);
    input.lower.down(2001, 1600, 540, 1920, 1080, true);
    const auto wheel = sample(2001, 18, recomp::duo::WheelGesture::LB);
    assert(wheel.rightX > 0 && (wheel.buttons & recomp::duo::WheelGesture::LB));
    input.lower.move(2020, 100, 540, 1920, 1080);
    assert(sample(2020, 19, recomp::duo::WheelGesture::LB).rightX < 0);
    input.lower.up(2021);
    sample(2155, 20, recomp::duo::WheelGesture::LB);
    input.lower.cancel();input.top.cancel();
    assert(sample(2200, 21).packet == 21);
    assert(inputClocks == result.size());
    return result;
}

void identicalInput() {
    Output quiet, verbose;
    const auto a = sequence(false, quiet);
    const auto b = sequence(true, verbose);
    assert(a == b);
    assert(quiet.lines.empty() && quiet.clocks == 0);
    assert(verbose.lines.size() > 10 && verbose.clocks > 0);
    bool pad = false, gesture = false, wheel = false, map = false;
    for (const auto& line : verbose.lines) {
        pad |= line.find("pad in=") != std::string::npos;
        gesture |= line.find("gesture lower=") != std::string::npos;
        wheel |= line.find("APPLE_IOS_DUO_WHEEL_TOUCH") != std::string::npos;
        map |= line.find("APPLE_IOS_DUO_MAP_TOUCH") != std::string::npos;
    }
    assert(pad && gesture && wheel && map);
}

void deduplicationAndBudget() {
    Output output;
    TouchLog log(Output::sink, Output::clock, &output, true, 2 * TouchLog::EntryBytes);
    Input input;
    GamepadState state{};
    for (unsigned i = 0; i != 20; ++i)
        mergeInput(input, state, [] { return 1000LL; }, log);
    assert(output.lines.size() == 2 && output.clocks == 2);
    unsigned formats = 0;
    while (log.active()) {
        log.write(Target::File, [&](char* text, std::size_t size) {
            ++formats;std::snprintf(text, size, "event %u", formats);
        });
    }
    const auto count = output.lines.size();
    const auto clocks = output.clocks, formatted = formats;
    for (unsigned i = 0; i != 100; ++i) {
        log.write(Target::File, [&](char*, std::size_t) { ++formats; });
        mergeInput(input, state, [] { return 2000LL; }, log);
    }
    assert(output.lines.size() == count && output.clocks == clocks && formats == formatted);
    input.top.down(3000, 1000, 400, 1920, 1080);input.top.up(3030);
    state = {};state.packet = 99;
    mergeInput(input, state, [] { return 3030LL; }, log);
    assert(state.packet == 0x41000063u && (state.buttons & 0x0c00) == 0x0c00);
    state = {};state.packet = 100;
    mergeInput(input, state, [] { return 4000LL; }, log);
    assert(state.packet == 100 && state.buttons == 0);
    assert(output.lines.size() == count && output.clocks == clocks && formats == formatted);
    std::size_t bytes = 0;
    for (const auto& line : output.lines) bytes += line.size();
    assert(bytes <= 2 * TouchLog::EntryBytes);
}

void sharedPolicy() {
    const char* option = std::getenv("RECOMP_VERBOSE_DIAGNOSTICS");
    const bool expected = option && std::strcmp(option, "1") == 0;
    assert(recomp::diag::verboseDiagnosticsEnabled() == expected);
    Output output;
    TouchLog log(Output::sink, Output::clock, &output);
    unsigned formats = 0;
    log.write(Target::File, [&](char* line, std::size_t size) { ++formats;std::snprintf(line, size, "policy"); });
    assert(formats == unsigned(expected));
    assert(output.clocks == unsigned(expected) && output.lines.size() == unsigned(expected));
}
}

int main() {
    disabledWork();
    identicalInput();
    deduplicationAndBudget();
    sharedPolicy();
    std::puts("touch diagnostics: disabled work, input equivalence, packets, budget and policy passed");
}
