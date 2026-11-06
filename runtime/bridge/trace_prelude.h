#pragma once

#include "indirect_call_resolver.h"
#include "guest_scalar_memory.h"
#include "guest_timebase.h"

// Generated TB/TBU reads and KeQueryPerformanceFrequency use the same rate.
#define PPC_READ_TIMEBASE() recomp::bootstrap::readGuestTimebase()

// All generated scalar accesses, not only the heuristic MMIO-store annotations.
// MM_* aliases in ppc_context.h inherit these guarded ordinary access macros.
#define PPC_LOAD_U8(x) recomp::bootstrap::loadGuestScalar<std::uint8_t>(base, static_cast<std::uint32_t>(x))
#define PPC_LOAD_U16(x) recomp::bootstrap::loadGuestScalar<std::uint16_t>(base, static_cast<std::uint32_t>(x))
#define PPC_LOAD_U32(x) recomp::bootstrap::loadGuestScalar<std::uint32_t>(base, static_cast<std::uint32_t>(x))
#define PPC_LOAD_U64(x) recomp::bootstrap::loadGuestScalar<std::uint64_t>(base, static_cast<std::uint32_t>(x))
#define PPC_STORE_U8(x, y) recomp::bootstrap::storeGuestScalar<std::uint8_t>(base, static_cast<std::uint32_t>(x), static_cast<std::uint8_t>(y))
#define PPC_STORE_U16(x, y) recomp::bootstrap::storeGuestScalar<std::uint16_t>(base, static_cast<std::uint32_t>(x), static_cast<std::uint16_t>(y))
#define PPC_STORE_U32(x, y) recomp::bootstrap::storeGuestScalar<std::uint32_t>(base, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y))
#define PPC_STORE_U64(x, y) recomp::bootstrap::storeGuestScalar<std::uint64_t>(base, static_cast<std::uint32_t>(x), static_cast<std::uint64_t>(y))

// eieio. XenonRecomp emits PPC_MM_STORE_* exactly for stores immediately followed by eieio
// (18 in the api26 corpus), and eieio itself emits no code. Under a concurrent memory model
// (guest_scalar_memory.h), order such a store before every later store. Model 0 keeps the
// ppc_context.h aliases, which are the plain guarded stores above.
#if RECOMP_GUEST_MEMORY_MODEL != 0
#define RECOMP_GUEST_EIEIO() __atomic_thread_fence(__ATOMIC_RELEASE)
#define PPC_MM_STORE_U8(x, y) do { PPC_STORE_U8(x, y); RECOMP_GUEST_EIEIO(); } while (false)
#define PPC_MM_STORE_U16(x, y) do { PPC_STORE_U16(x, y); RECOMP_GUEST_EIEIO(); } while (false)
#define PPC_MM_STORE_U32(x, y) do { PPC_STORE_U32(x, y); RECOMP_GUEST_EIEIO(); } while (false)
#define PPC_MM_STORE_U64(x, y) do { PPC_STORE_U64(x, y); RECOMP_GUEST_EIEIO(); } while (false)
#endif

// dcbz (32 bytes, or 128 with RECOMP_DCBZ_128=1) and dcbz128. The api26 corpus still emits
// both inline as memset(base + ((ea) & ~31), 0, 32) / (..., 128); these hooks take effect
// only after those lines are rewritten to PPC_DCBZ(ea) / PPC_DCBZ128(ea).
#if defined(RECOMP_DCBZ_128) && RECOMP_DCBZ_128
#define PPC_DCBZ(x) recomp::bootstrap::zeroGuestBlock<128>(base, static_cast<std::uint32_t>(x))
#else
#define PPC_DCBZ(x) recomp::bootstrap::zeroGuestBlock<32>(base, static_cast<std::uint32_t>(x))
#endif
#define PPC_DCBZ128(x) recomp::bootstrap::zeroGuestBlock<128>(base, static_cast<std::uint32_t>(x))

// Included before every translation unit. The generated ppc_context.h keeps
// this opt-in definition and invokes it from PPC_FUNC_PROLOGUE.
extern "C" void recomp_guest_execution_checkpoint_named(const char* functionLabel); // native callers
// WP1 fast path (guest_parallel_wake.h): decrement a countdown kept in the context's tail
// padding and do one relaxed load, with the executor's cold path behind them. The label length
// is a compile-time constant, so no strlen runs per entry. In cooperative builds the attention
// word stays nonzero, so every entry takes the cold path: GuestExecutionGuard::checkpoint with
// the label bounded to 128 bytes, as recomp_guest_execution_checkpoint_named did. Generated
// code therefore links against an executor TU that defines recomp_guest_checkpoint_attention
// and recomp_guest_execution_checkpoint_slow.
#include "guest_parallel_wake.h"
#define PPC_RUNTIME_CHECKPOINT() ::recomp::bootstrap::guestCheckpoint(ctx, __func__, sizeof(__func__) - 1)
extern "C" [[noreturn]] void recomp_guest_execution_trap();
#define PPC_RUNTIME_TRAP() recomp_guest_execution_trap()

// The generated default indexes a host-pointer table in guest memory. This
// local harness keeps the table on the host and accepts only exact validated
// guest function addresses, leaving image-tail guest memory untouched.
#define PPC_CALL_INDIRECT_FUNC(target) do { \
    recomp_host_guest_call_indirect( \
            static_cast<std::uint32_t>(target), ctx, base); \
} while (false)
