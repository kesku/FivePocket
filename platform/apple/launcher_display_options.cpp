// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/launcher_display_options.h"
#include <atomic>
#include <pthread.h>
#if !defined(__APPLE__)
#error "Apple launcher provider belongs only to the Apple shell"
#endif
namespace {
recomp::apple::launcher::DisplayOptionsChannel options;
// ios-sidemenu (SPEC §12): the in-game menu's live word. The start's frozen word stays as it is (the one-way handoff); the menu
// publishes a second word with its live fields changed and every restart field at its start value (ios_side_menu.h liveChoices);
// the reader returns it once set. Only after the freeze, only from the main thread.
std::atomic<std::uint32_t> live{0};
std::atomic<bool> frozen{false}; // (set by both freezes: the game has started)
bool mainOwner() noexcept {
#if defined(APPLE_LAUNCHER_OPTIONS_MUTANT_MAIN_THREAD)
    return true;
#else
    return pthread_main_np()!=0;
#endif
}
}
namespace recomp::apple::launcher {
bool publishSavedLauncherDisplayOptions(const StoredDisplay& snapshot) noexcept {
    if(!mainOwner() || !snapshot.generation)return false;
    return options.publishSaved(snapshot.choices);
}
bool freezeLauncherDisplayOptions() noexcept {
#if !defined(APPLE_LAUNCHER_OPTIONS_MUTANT_FREEZE_THREAD)
    if(!mainOwner())return false;
#endif
    options.freeze();frozen.store(true,std::memory_order_release);return true;
}
bool freezeLegacyLauncherDisplayOptions() noexcept {
    if(!mainOwner())return false;
#if !defined(APPLE_LAUNCHER_OPTIONS_MUTANT_LEGACY)
    if(!options.publishSaved(std::nullopt))return false;
#endif
    options.freeze();frozen.store(true,std::memory_order_release);return true;
}
}
namespace recomp::apple::launcher {
bool publishLiveLauncherDisplayOptions(const DisplayChoices& choices) noexcept {
    if(!mainOwner() || !frozen.load(std::memory_order_acquire))return false;
    const auto word=encode(choices);if(!word)return false;
    live.store(*word,std::memory_order_release);return true;
}
}
extern "C" std::uint32_t recomp_apple_launcher_display_options() noexcept {
    const auto l=live.load(std::memory_order_acquire);
    return l?l:options.read();
}
