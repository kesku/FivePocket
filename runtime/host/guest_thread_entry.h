#pragma once

#include "guest_thread_storage_pool.h"

namespace recomp::bootstrap {

struct GuestThreadEntryState {
    std::uint32_t address;
    std::uint32_t stackPointer;
    std::uint32_t pcrAddress;
    std::uint32_t argument0;
    std::uint32_t argument1;
    bool directEntry;
};

// Reserve a caller frame/home area inside the allocated guest stack. Reference:
// Xenia 95a5c3ee... processor.cc Execute uses 112 bytes + 64 bytes of padding;
// xthread.cc Execute passes (routine, context) to XAPI, or just context directly.
inline std::optional<GuestThreadEntryState> makeGuestThreadEntry(const GuestPreparedThread& thread) noexcept {
    constexpr std::uint32_t EntryFrame = 112 + 64;
    const auto& layout = thread.layout;
    const auto top = static_cast<std::uint64_t>(layout.stackBase) + layout.stackSize;
    const bool direct = thread.xapiStartup == 0;
    const auto entry = direct ? thread.startRoutine : thread.xapiStartup;
    if (entry == 0 || (entry & 3U) != 0 || layout.pcrAddress == 0
            || (layout.pcrAddress & 3U) != 0 || layout.stackSize < EntryFrame
            || top > UINT32_MAX || (top & 15U) != 0) {
        return std::nullopt;
    }
    return GuestThreadEntryState{entry, static_cast<std::uint32_t>(top - EntryFrame),
            layout.pcrAddress, direct ? thread.startContext : thread.startRoutine,
            direct ? 0U : thread.startContext, direct};
}

// Command-interrupt ABI: r3=source1, r4=registered user data. CPU identity and
// interrupt/IRQL authority are separate lifecycle checks, not inferred here.
// This must never reinterpret audio's wrapped-context page as device context.
inline std::optional<GuestThreadEntryState> makeGuestCommandInterruptEntry(const GuestPreparedThread& thread) noexcept {
    auto entry=makeGuestThreadEntry(thread);
    if (!entry || !entry->directEntry || thread.wrappedContextAddress || thread.creationFlags!=0) return std::nullopt;
    entry->argument0=1;entry->argument1=thread.startContext;
    return entry;
}

} // namespace recomp::bootstrap
