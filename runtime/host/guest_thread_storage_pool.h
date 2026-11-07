#pragma once

#include "guest_thread_environment.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace recomp::bootstrap {

struct GuestThreadMemoryRange {
    std::uint32_t base{0};
    std::uint32_t size{0};
};

struct GuestThreadStorageConfig {
    GuestThreadMemoryRange arena;
    GuestThreadMemoryRange image;
    GuestThreadMemoryRange code;
    std::uint32_t tlsRawDataAddress{0};
    std::uint32_t tlsRawDataSize{0};
    std::uint32_t tlsDataSize{0};
    std::uint32_t tlsSlotCount{0};
    // Parser-validated title default, not a silently invented stack size.
    std::uint32_t defaultStackSize{0};
    // The caller reserves a disjoint ID namespace (initial title thread is 1).
    std::uint32_t firstThreadId{2};
};

struct GuestThreadStorageRequest {
    std::uint32_t stackSize{0}; // Zero selects the explicitly bound title default.
    std::uint32_t xapiStartup{0}; // Zero permits a direct start routine.
    std::uint32_t startRoutine{0};
    std::uint32_t startContext{0}; // Opaque guest value, never dereferenced here.
    std::uint32_t creationFlags{1}; // Bit 0 suspended; high byte singleton CPU mask. Storage never schedules.
    // Native direct-callback worker: reserve a separate owned page containing
    // one BE context word and pass its address. Never steal caller stack/PCR/TLS
    // padding. Incompatible with XAPI startup. Normal title requests leave false.
    bool wrapStartContext{false};
};

struct GuestPreparedThread {
    GuestThreadEnvironmentLayout layout;
    GuestThreadMemoryRange allocation;
    std::uint32_t dynamicTlsAddress{0};
    std::uint32_t xapiStartup{0};
    std::uint32_t startRoutine{0};
    std::uint32_t startContext{0};
    std::uint32_t creationFlags{1};
    std::uint32_t wrappedContextAddress{0}; // Zero unless explicitly requested.
};

enum class GuestThreadStorageState {
    Prepared,
    Unavailable,
    InvalidRequest,
    CapacityExceeded,
    ArenaExhausted,
    InitializationFailed,
};

struct GuestThreadStorageResult {
    GuestThreadStorageState state{GuestThreadStorageState::Unavailable};
    std::optional<GuestPreparedThread> thread;
    [[nodiscard]] bool succeeded() const noexcept {
        return state == GuestThreadStorageState::Prepared && thread.has_value();
    }
};

// Owns bounded storage for prepared, non-running guest threads. This does NOT
// export ExCreateThread, issue kernel handles, bind registry TLS, construct a
// runnable PPCContext, or schedule anything. Code addresses are range-checked;
// the execution adapter must additionally resolve exact translated functions.
// No host thread is created and no guest callback is invoked.
//
// The caller must exclude every other runtime arena/scratch range at initialize
// and keep them disjoint afterward. The address-space object AND its reservation
// must outlive this pool, or reset must be called before releasing the mapping.
// Calls are serialized by the owner; this is not a concurrent scheduler.
class GuestThreadStoragePool final {
public:
    static constexpr std::uint32_t PageSize = 0x1000U;
    static constexpr std::uint32_t MinimumStackSize = 0x4000U;
    static constexpr std::uint32_t MaximumStackSize = 16U * 1024U * 1024U;
    static constexpr std::uint32_t MaximumArenaSize = 256U * 1024U * 1024U;
    static constexpr std::size_t MaximumThreads = 64;
    static constexpr std::size_t MaximumExcludedRanges = 64;

    // Xenia xthread.cc GetFakeCpuNumber: the high byte is a one-hot logical
    // processor mask (six processors), not a processor index. Other low flags
    // have separate priority/object semantics and are not silently discarded.
    [[nodiscard]] static constexpr bool validCreationFlags(std::uint32_t flags) noexcept {
        const auto mask = flags >> 24U;
        return (flags & ~0x3F000001U) == 0 && (mask == 0 || (mask & (mask - 1U)) == 0);
    }

    GuestThreadStoragePool() = default;
    ~GuestThreadStoragePool();
    GuestThreadStoragePool(const GuestThreadStoragePool&) = delete;
    GuestThreadStoragePool& operator=(const GuestThreadStoragePool&) = delete;
    GuestThreadStoragePool(GuestThreadStoragePool&&) = delete;
    GuestThreadStoragePool& operator=(GuestThreadStoragePool&&) = delete;

    // Validation only; successful binding does not write guest memory. Refuses
    // implicit rebinding, even if no threads have yet been prepared.
    [[nodiscard]] bool initialize(
            GuestAddressSpace& addressSpace,
            const GuestThreadStorageConfig& config,
            const std::vector<GuestThreadMemoryRange>& excludedRanges = {});
    [[nodiscard]] GuestThreadStorageResult prepare(const GuestThreadStorageRequest& request);
    [[nodiscard]] std::optional<GuestPreparedThread> lookup(std::uint32_t threadId) const;
    // Scrubs the complete allocation, including stack and padding. Only call
    // after all external references/TLS bindings are gone. This is NOT NtClose:
    // closing a handle must not destroy a still-live thread's storage.
    [[nodiscard]] bool release(std::uint32_t threadId);
    void reset() noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    [[nodiscard]] std::optional<std::uint32_t> findBlock(std::uint32_t size) const;
    void scrub(const GuestPreparedThread& thread) noexcept;
    GuestAddressSpace* addressSpace_{nullptr};
    GuestThreadStorageConfig config_{};
    std::array<std::optional<GuestPreparedThread>, MaximumThreads> threads_{};
    // Never recycle identities across release/reset during this pool's life.
    std::uint64_t nextThreadId_{2};
    std::uint32_t tlsAllocationSize_{0};
};

} // namespace recomp::bootstrap
