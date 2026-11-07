#pragma once
#include "guest_scalar_memory.h"

#include <cstdint>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace recomp::bootstrap {

enum class GuestExecutionStopReason {
    Completed,
    InvalidEntry,
    InvalidBudget,
    NestedRun,
    FunctionEntryBudgetExceeded,
    UnimplementedImport,
    RejectedImport,
    InvalidIndirectTarget,
    HostException,
    NoRunnableTask,
    GuestTrap,
    UnsupportedScalarAccess,
};

// A private diagnostic snapshot, not a guest pointer dereference or full CPU
// state. Generated wrappers capture it immediately before an import stop.
struct GuestImportArguments {
    std::uint64_t stackPointer{0};
    std::array<std::uint64_t, 8> registers{}; // r3-r10, preserve all 64 bits.
};

struct GuestExecutionResult {
    GuestExecutionStopReason reason{GuestExecutionStopReason::InvalidEntry};
    std::uint64_t completedFunctionEntries{0};
    std::string detail;
    std::optional<GuestImportArguments> importArguments{};
    std::optional<GuestScalarAccess> scalarAccess{};

    [[nodiscard]] bool completed() const noexcept {
        return reason == GuestExecutionStopReason::Completed;
    }
};

// A thread-local cooperative guard for private generated guest code. It bounds
// function entries and converts deliberate missing-import stops into data. It
// does not catch memory faults and cannot interrupt a tight loop inside one
// generated function; branch/instruction-level instrumentation is a later gate.
class GuestExecutionGuard final {
public:
    using Entry = std::function<void()>;
    using FunctionTraceSink = std::function<void(std::string_view)>;

    [[nodiscard]] GuestExecutionResult run(
            std::uint64_t maximumFunctionEntries,
            const Entry& entry,
            const FunctionTraceSink& traceSink = {}) noexcept;

    // Generated PPC_FUNC_PROLOGUE instrumentation calls this checkpoint.
    static void checkpoint(std::string_view functionLabel = {});
    // Only retained on an import stop from this run. No capture outside a run.
    static void captureImportArguments(const GuestImportArguments& arguments) noexcept;

    // Generated wrappers for unsupported imports call this fail-closed stop.
    [[noreturn]] static void stopForUnimplementedImport(std::string_view importName);
    // An implemented adapter rejected this call's unsupported arguments/state.
    [[noreturn]] static void stopForRejectedImport(std::string_view importName);

    // A validated host resolver calls this without retaining the private guest
    // address when an indirect branch has no exact generated-code mapping.
    [[noreturn]] static void stopForInvalidIndirectTarget();
    [[noreturn]] static void stopForGuestTrap();

    [[nodiscard]] static bool isActive() noexcept;
};

[[nodiscard]] const char* guestExecutionStopReasonName(
        GuestExecutionStopReason reason) noexcept;

extern "C" void recomp_guest_execution_checkpoint();
extern "C" void recomp_guest_execution_checkpoint_named(const char* functionLabel);
extern "C" [[noreturn]] void recomp_guest_execution_unimplemented_import(const char* importName);
extern "C" [[noreturn]] void recomp_guest_execution_trap();

} // namespace recomp::bootstrap
