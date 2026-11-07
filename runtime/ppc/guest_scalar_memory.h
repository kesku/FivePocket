// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>
#include <functional>
#include <optional>

// Guest memory model, fixed at compile time. Default 0 keeps the serialized-executor path.
//   RECOMP_GUEST_MEMORY_ATOMIC=1  naturally aligned ordinary-RAM scalar accesses are relaxed
//     atomics: single-copy atomic, never torn, merged or hoisted out of guest spin-waits.
//     Misaligned accesses stay plain byte-aligned accesses (Xenon gives them no atomicity either).
//   RECOMP_GUEST_MEMORY_STRONG=1  implies ATOMIC; aligned accesses are seq_cst (AArch64
//     LDAR/STLR). That subsumes the corpus's sync/lwsync/eieio, which emit no code. Bring-up
//     only. Not acquire/release: an acquire load may lower to LDAPR (RCpc), which lets a
//     later load pass an earlier release store, so it would not cover PowerPC sync.
// Set the same switches for every translation unit of one binary. The per-model inline
// namespace below keeps a mismatch from silently ODR-merging the inline templates.
#if defined(RECOMP_GUEST_MEMORY_MODEL)
#error "RECOMP_GUEST_MEMORY_MODEL is derived; define RECOMP_GUEST_MEMORY_ATOMIC or RECOMP_GUEST_MEMORY_STRONG"
#endif
#if defined(RECOMP_GUEST_MEMORY_STRONG) && RECOMP_GUEST_MEMORY_STRONG
#define RECOMP_GUEST_MEMORY_MODEL 2
#elif defined(RECOMP_GUEST_MEMORY_ATOMIC) && RECOMP_GUEST_MEMORY_ATOMIC
#define RECOMP_GUEST_MEMORY_MODEL 1
#else
#define RECOMP_GUEST_MEMORY_MODEL 0
#endif

// The ordinary-RAM fast path is forced inline, even at -O0 where the corpus is built. The
// MMIO/special cases stay out of line and cold.
#if defined(__GNUC__) || defined(__clang__)
#define RECOMP_GUEST_SCALAR_INLINE [[gnu::always_inline]] inline
#define RECOMP_GUEST_SCALAR_COLD [[gnu::noinline, gnu::cold]]
#else
#define RECOMP_GUEST_SCALAR_INLINE inline
#define RECOMP_GUEST_SCALAR_COLD
#endif

namespace recomp::bootstrap {
enum class GuestScalarRegion : std::uint32_t {
    Ordinary = 0, GpuRegisters = 1, AddressOverflow = 2, UnboundDataImport = 3, UnsupportedTimestampAccess = 4, XmaRegisters = 5
};
struct GuestScalarAccess {
    std::uint32_t address{0};
    std::uint32_t width{0};
    std::uint64_t value{0}; // Numeric store value, never a speculative read result.
    bool write{false};
    GuestScalarRegion region{GuestScalarRegion::Ordinary};
};

// Exact reference GPU and XMA apertures. This is not an allocation/protection check
// or a complete Xbox hardware map. Partial/cross-boundary accesses also stop.
RECOMP_GUEST_SCALAR_INLINE constexpr GuestScalarRegion classifyGuestScalarAccess(std::uint32_t address, std::uint32_t width) {
    const std::uint64_t end = std::uint64_t{address} + width;
    if (end > (std::uint64_t{1} << 32)) return GuestScalarRegion::AddressOverflow;
    if (address < 0x7FC90000 && end > 0x7FC80000) return GuestScalarRegion::GpuRegisters;
    if (address < 0x7FEB0000 && end > 0x7FEA0000) return GuestScalarRegion::XmaRegisters;
    return GuestScalarRegion::Ordinary;
}

extern "C" [[noreturn]] void recomp_guest_execution_scalar_access(const GuestScalarAccess& access);

// Hull [lo,hi) of the bound special ordinary-RAM ranges (unbound data-import slots and the
// uptime bundle), maintained by their bind/reset functions; empty when nothing is bound.
// Every scalar access used to call both checks out of line (about 10% of process CPU in
// the attempt v4 profile); an access outside the hull cannot touch either range.
inline std::uint32_t guestScalarSpecialLo = 0, guestScalarSpecialHi = 0;
RECOMP_GUEST_SCALAR_INLINE bool touchesGuestScalarSpecial(std::uint32_t address, std::uint32_t width) noexcept {
    return address < guestScalarSpecialHi && std::uint64_t{address} + width > guestScalarSpecialLo;
}

// The two special ranges checked separately by the inline fast path. The combined hull above
// can span hot RAM between them. Each range is [lo, hi) in 64-bit, so hi may be 2^32, and is
// lo = hi = 0 when unbound. updateGuestScalarSpecialHull() (guest_execution_guard.cpp) keeps
// them exact, next to the hull:
//   data-import slots  [front slot, back slot + 4)
//   uptime bundle      [bundle, bundle + 24)
// The initial [0, 2^32) means "not maintained yet". It sends every access to the cold path,
// which re-derives everything itself, so a guard built without the update stays correct.
// One aligned block lets optimized code reach all four bounds from one base register (ldp).
// The names below alias its fields.
struct alignas(32) GuestScalarSpecialRanges {
    std::uint64_t dataSlotLo, dataSlotHi, uptimeLo, uptimeHi;
};
inline GuestScalarSpecialRanges guestScalarSpecialRanges{0, std::uint64_t{1} << 32, 0, std::uint64_t{1} << 32};
inline std::uint64_t& guestScalarDataSlotLo = guestScalarSpecialRanges.dataSlotLo;
inline std::uint64_t& guestScalarDataSlotHi = guestScalarSpecialRanges.dataSlotHi;
inline std::uint64_t& guestScalarUptimeLo = guestScalarSpecialRanges.uptimeLo;
inline std::uint64_t& guestScalarUptimeHi = guestScalarSpecialRanges.uptimeHi;
// v220: the fixed windows that must contain every bound special range when chunks use the constant-window fast path
// (the GTA V launch layout: image page 0x82000000 holds the unbound data-import slots 0x820008DC..0x82000A28; the
// loader's kernel-data page 0x62004000 holds the uptime bundle at 0x62004200). bindGuestScalarDataImportGuard and
// bindGuestScalarUptimeClock refuse ranges outside them (guest_execution_guard.cpp), so a changed layout fails setup.
inline constexpr std::uint64_t guestSpecialWindowA = 0x82000000u, guestSpecialWindowB = 0x62004000u, guestSpecialWindowBytes = 0x1000u;
inline constexpr bool guestSpecialRangeInsideConstWindows(std::uint64_t lo, std::uint64_t hi) noexcept {
    return lo >= hi || (lo >= guestSpecialWindowA && hi <= guestSpecialWindowA + guestSpecialWindowBytes) ||
        (lo >= guestSpecialWindowB && hi <= guestSpecialWindowB + guestSpecialWindowBytes);
}

// Process-wide immutable policy for the serialized diagnostic. Bind before any
// worker is created, reset only after all workers join. No concurrent rebinding.
// A read of an unresolved data slot must not expose its encoded ordinal as a pointer.
[[nodiscard]] bool bindGuestScalarDataImportGuard(const std::vector<std::uint32_t>& slots);
void resetGuestScalarDataImportGuard() noexcept;
[[nodiscard]] bool isUnboundGuestDataImportAccess(std::uint32_t address, std::uint32_t width) noexcept;
// Observed KeTimeStampBundle word at +16 only. Reads query a pure monotonic
// session-millisecond source; other fields and all writes stop before RAM.
// Same before-workers/after-join lifecycle as the data-slot guard above.
[[nodiscard]] bool bindGuestScalarUptimeClock(std::uint32_t bundle,
    std::function<std::optional<std::uint64_t>()> source);
void resetGuestScalarUptimeClock() noexcept;
[[nodiscard]] bool tryReadGuestScalarUptime(std::uint32_t address, std::uint32_t width, std::uint64_t& value);
void checkGuestScalarUptimeWrite(std::uint32_t address, std::uint32_t width, std::uint64_t value);

// ISOLATED HYPOTHESIS, disabled by default: dword 044B as a 500-MHz timer.
// Exact hardware semantics are unresolved (D-063/064). Pure source returns
// elapsed session nanoseconds, or null when no live backend is available.
// Does not authorize any other GPU access, command completion or memory write.
// Serialized diagnostic only; bind before workers, reset after they all join.
[[nodiscard]] bool bindExperimentalGuestGpuClock(std::function<std::optional<std::uint64_t>()> source);
void resetExperimentalGuestGpuClock() noexcept;
[[nodiscard]] std::uint64_t experimentalGuestGpuClockReads() noexcept;
[[nodiscard]] bool tryReadExperimentalGuestGpuClock(std::uint32_t address, std::uint32_t width, std::uint64_t& value);
// Opt-in exact CP_RB_WPTR store routing for the serialized native cold-start
// profile. Default rejects. Successful handling never writes flat MMIO RAM.
[[nodiscard]] bool bindGuestGraphicsSubmission(std::function<bool(std::uint32_t)> handler);
void resetGuestGraphicsSubmission() noexcept;
[[nodiscard]] bool tryWriteGuestGraphicsSubmission(std::uint32_t address, std::uint32_t width, std::uint64_t value);
// Native synchronous XMA registers; same immutable-before-workers lifecycle.
[[nodiscard]] bool bindGuestXmaRegisters(std::function<bool(std::uint32_t,bool,std::uint32_t&)> handler);
void resetGuestXmaRegisters() noexcept;
[[nodiscard]] bool tryAccessGuestXmaRegisters(std::uint32_t address,std::uint32_t width,bool write,std::uint64_t& value);

template<class UInt> RECOMP_GUEST_SCALAR_INLINE UInt swapGuestScalar(UInt value) {
    static_assert(std::is_unsigned_v<UInt> && (sizeof(UInt) == 1 || sizeof(UInt) == 2 ||
        sizeof(UInt) == 4 || sizeof(UInt) == 8));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if constexpr (sizeof(UInt) == 2) return __builtin_bswap16(value);
    if constexpr (sizeof(UInt) == 4) return __builtin_bswap32(value);
    if constexpr (sizeof(UInt) == 8) return __builtin_bswap64(value);
#elif __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error Unsupported host byte order
#endif
    return value;
}

// v220 (attempt copy): RECOMP_GUEST_SPECIAL_CONST_WINDOWS=1 (per chunk, with -iquote of the attempt src) replaces the
// fast path's two runtime special-range tests with two fixed 4 KB windows of this title's launch layout (see
// guestSpecialWindow* below); its own inline namespace keeps the variant from ODR-merging with chunks built without it.
#if RECOMP_GUEST_MEMORY_MODEL == 2
inline namespace guest_memory_strong {
#elif RECOMP_GUEST_MEMORY_MODEL == 1 && defined(RECOMP_GUEST_SPECIAL_CONST_WINDOWS_TREE) && RECOMP_GUEST_SPECIAL_CONST_WINDOWS_TREE
inline namespace guest_memory_atomic_cwt { // v229
#elif RECOMP_GUEST_MEMORY_MODEL == 1 && defined(RECOMP_GUEST_SPECIAL_CONST_WINDOWS) && RECOMP_GUEST_SPECIAL_CONST_WINDOWS
inline namespace guest_memory_atomic_cw {
#elif RECOMP_GUEST_MEMORY_MODEL == 1
inline namespace guest_memory_atomic {
#endif
#if RECOMP_GUEST_MEMORY_MODEL != 0
inline constexpr int guestScalarMemoryOrder = RECOMP_GUEST_MEMORY_MODEL == 2 ? __ATOMIC_SEQ_CST : __ATOMIC_RELAXED;
// Host alignment decides. PPC_FUNC_PROLOGUE assumes a 32-byte aligned base, so in
// generated code this equals the guest address alignment.
template<class UInt> RECOMP_GUEST_SCALAR_INLINE bool isAtomicGuestScalarAddress(const std::uint8_t* host) noexcept {
    return (reinterpret_cast<std::uintptr_t>(host) & (sizeof(UInt) - 1)) == 0;
}
#endif

// A byte-aligned, may_alias view of UInt: the same bytes as memcpy, but one plain load or
// store even at -O0. With _FORTIFY_SOURCE, memcpy there is a __memcpy_chk call, and its
// address-taken temporary also adds a stack-protector canary to every generated function.
template<class UInt> struct UnalignedGuestScalar {
    typedef UInt __attribute__((aligned(1), may_alias)) type;
};

// Ordinary RAM, no guards. Model 0 (serialized executor): a plain possibly-misaligned access
// with no Xenon single-copy atomicity. Models 1 and 2 give it to aligned accesses.
template<class UInt> RECOMP_GUEST_SCALAR_INLINE UInt loadGuestScalarRaw(const std::uint8_t* host) noexcept {
#if RECOMP_GUEST_MEMORY_MODEL != 0
    if (isAtomicGuestScalarAddress<UInt>(host))
        return swapGuestScalar(__atomic_load_n(reinterpret_cast<const UInt*>(host), guestScalarMemoryOrder));
#endif
    // Explicit <UInt>: deducing the aligned(1) typedef as a by-value parameter type crashes
    // GCC 15's AArch64 backend (aarch64_function_arg_alignment).
    return swapGuestScalar<UInt>(*reinterpret_cast<const typename UnalignedGuestScalar<UInt>::type*>(host));
}

template<class UInt> RECOMP_GUEST_SCALAR_INLINE void storeGuestScalarRaw(std::uint8_t* host, UInt value) noexcept {
#if RECOMP_GUEST_MEMORY_MODEL != 0
    if (isAtomicGuestScalarAddress<UInt>(host)) {
        __atomic_store_n(reinterpret_cast<UInt*>(host), swapGuestScalar(value), guestScalarMemoryOrder);
        return;
    }
#endif
    *reinterpret_cast<typename UnalignedGuestScalar<UInt>::type*>(host) = swapGuestScalar(value);
}

// Fast-path predicate: [address, address + width) is ordinary RAM that neither special range
// can touch. Each window test is "last - lo < hi - lo + width - 1" in 64 bits, which is exact:
// the access meets [lo, hi) iff address < hi and address + width > lo. One window covers the
// GPU and XMA apertures and the hardware gap between them. The cold path classifies exactly.
template<class UInt> RECOMP_GUEST_SCALAR_INLINE bool isFastGuestScalarAccess(std::uint32_t address) noexcept {
    constexpr std::uint64_t tail = sizeof(UInt) - 1;
    const std::uint64_t last = std::uint64_t{address} + tail;
#if defined(RECOMP_GUEST_SPECIAL_CONST_WINDOWS) && RECOMP_GUEST_SPECIAL_CONST_WINDOWS
    // v220: the data-import slots and the uptime bundle are bound only inside these windows (bind refuses others:
    // guestSpecialRangesInsideConstWindows); an access inside a window takes the exact cold path, as before.
#if defined(RECOMP_GUEST_SPECIAL_CONST_WINDOWS_TREE) && RECOMP_GUEST_SPECIAL_CONST_WINDOWS_TREE
    // v229: the same predicate as a decision tree over the three sorted windows B=[0x62004000,+4K), the GPU/XMA aperture
    // [0x7FC80000,0x7FEB0000) and A=[0x82000000,+4K): split at the aperture, then each half tests only its own window.
    // The common targets (heap 0x10000000.., guest stacks 0x64000000.., image globals 0x82001000.., the physical views
    // 0xA0000000/0xC0000000) take two or three compares instead of three full window tests (tests/cw_tree_test.cpp:
    // identical to the v220 formula for every width at the window edges and 64 M random addresses).
    if (last < 0x7FC80000u) return last < guestSpecialWindowB || address >= guestSpecialWindowB + guestSpecialWindowBytes;
    if (address < 0x7FEB0000u) return false;
    return last < guestSpecialWindowA || (address >= guestSpecialWindowA + guestSpecialWindowBytes && last <= 0xFFFFFFFFu);
#else
    return last - 0x7FC80000 >= 0x7FEB0000 - 0x7FC80000 + tail && last <= 0xFFFFFFFF &&
        last - guestSpecialWindowA >= guestSpecialWindowBytes + tail && last - guestSpecialWindowB >= guestSpecialWindowBytes + tail;
#endif
#endif
    return last - 0x7FC80000 >= 0x7FEB0000 - 0x7FC80000 + tail && last <= 0xFFFFFFFF &&
        last - guestScalarSpecialRanges.dataSlotLo >=
            guestScalarSpecialRanges.dataSlotHi - guestScalarSpecialRanges.dataSlotLo + tail &&
        last - guestScalarSpecialRanges.uptimeLo >=
            guestScalarSpecialRanges.uptimeHi - guestScalarSpecialRanges.uptimeLo + tail;
}

// Cold path: GPU/XMA apertures, address wrap, unbound data-import slots, and the uptime
// bundle. This is the former single-function logic, statement for statement.
template<class UInt> RECOMP_GUEST_SCALAR_COLD UInt loadGuestScalarSlow(const std::uint8_t* base, std::uint32_t address) {
    auto region = classifyGuestScalarAccess(address, sizeof(UInt));
    const bool special = touchesGuestScalarSpecial(address, sizeof(UInt));
    if (region == GuestScalarRegion::Ordinary && special && isUnboundGuestDataImportAccess(address, sizeof(UInt)))
        region = GuestScalarRegion::UnboundDataImport;
    if (region != GuestScalarRegion::Ordinary) {
        std::uint64_t experimentalValue;
        if (region == GuestScalarRegion::XmaRegisters &&
            tryAccessGuestXmaRegisters(address,sizeof(UInt),false,experimentalValue))
            return static_cast<UInt>(experimentalValue);
        if (region == GuestScalarRegion::GpuRegisters &&
            tryReadExperimentalGuestGpuClock(address, sizeof(UInt), experimentalValue))
            return static_cast<UInt>(experimentalValue);
        recomp_guest_execution_scalar_access({address, sizeof(UInt), 0, false, region});
    }
    std::uint64_t uptime;
    if (special && tryReadGuestScalarUptime(address, sizeof(UInt), uptime)) return static_cast<UInt>(uptime);
    return loadGuestScalarRaw<UInt>(base + address);
}

template<class UInt> RECOMP_GUEST_SCALAR_COLD void storeGuestScalarSlow(std::uint8_t* base, std::uint32_t address, UInt value) {
    auto region = classifyGuestScalarAccess(address, sizeof(UInt));
    const bool special = touchesGuestScalarSpecial(address, sizeof(UInt));
    if (region == GuestScalarRegion::Ordinary && special && isUnboundGuestDataImportAccess(address, sizeof(UInt)))
        region = GuestScalarRegion::UnboundDataImport;
    if (region != GuestScalarRegion::Ordinary) {
        std::uint64_t xmaValue=value;
        if (region == GuestScalarRegion::XmaRegisters && tryAccessGuestXmaRegisters(address,sizeof(UInt),true,xmaValue)) return;
        if (region == GuestScalarRegion::GpuRegisters && tryWriteGuestGraphicsSubmission(address, sizeof(UInt), value)) return;
        recomp_guest_execution_scalar_access({address, sizeof(UInt), value, true, region});
    }
    if (special) checkGuestScalarUptimeWrite(address, sizeof(UInt), value);
    storeGuestScalarRaw<UInt>(base + address, value);
}

// lean-mem (2026-10-04): RECOMP_GUEST_LEAN_RAW=1 (model 1 only; set it for the generated chunks) changes only the
// ordinary-RAM access of the inlined fast path. Model 1 picks a relaxed atomic for an aligned address and a plain
// byte-aligned access otherwise; on AArch64 both are the same LDR/STR, so the machine branch folder merges the arms
// after instruction selection and leaves the alignment TST dead and the host address ADD unfolded (and a second
// copy of every u16 access). Here both arms are the SAME volatile, byte-aligned, may_alias access: AArch64 without
// strict alignment emits one LDR/STR of that width for it (LLVM LangRef: a volatile access of a legal width is not
// split, merged, removed, duplicated or hoisted), single-copy atomic when aligned exactly like the relaxed atomic,
// the same single plain instruction when misaligned. The arms stay so the CFG that PGO hashes is unchanged; the late
// SimplifyCFG merges the identical arms, so the access folds to [base, wEA, uxtw] with no TST. The predicate, the
// slow paths (still the functions above, same names and bodies as without the define, so their PGO records and
// sections are kept) and every special address are untouched. Own nested inline namespace: no ODR merging with TUs
// built without the define.
#if RECOMP_GUEST_MEMORY_MODEL == 1 && defined(RECOMP_GUEST_LEAN_RAW) && RECOMP_GUEST_LEAN_RAW
inline namespace guest_lean_raw {
template<class UInt> RECOMP_GUEST_SCALAR_INLINE UInt loadGuestScalarLeanRaw(const std::uint8_t* host) noexcept {
    using Lean = const volatile typename UnalignedGuestScalar<UInt>::type;
    if (isAtomicGuestScalarAddress<UInt>(host))  // two identical arms on purpose (PGO CFG hash)
        return swapGuestScalar<UInt>(*reinterpret_cast<Lean*>(host));
    return swapGuestScalar<UInt>(*reinterpret_cast<Lean*>(host));
}

template<class UInt> RECOMP_GUEST_SCALAR_INLINE void storeGuestScalarLeanRaw(std::uint8_t* host, UInt value) noexcept {
    using Lean = volatile typename UnalignedGuestScalar<UInt>::type;
    if (isAtomicGuestScalarAddress<UInt>(host)) {
        *reinterpret_cast<Lean*>(host) = swapGuestScalar<UInt>(value);
        return;
    }
    *reinterpret_cast<Lean*>(host) = swapGuestScalar<UInt>(value);
}

template<class UInt> RECOMP_GUEST_SCALAR_INLINE UInt loadGuestScalar(const std::uint8_t* base, std::uint32_t address) {
    if (isFastGuestScalarAccess<UInt>(address)) return loadGuestScalarLeanRaw<UInt>(base + address);
    return loadGuestScalarSlow<UInt>(base, address);
}

template<class UInt> RECOMP_GUEST_SCALAR_INLINE void storeGuestScalar(std::uint8_t* base, std::uint32_t address, UInt value) {
    if (isFastGuestScalarAccess<UInt>(address)) {
        storeGuestScalarLeanRaw<UInt>(base + address, value);
        return;
    }
    storeGuestScalarSlow<UInt>(base, address, value);
}
} // inline namespace guest_lean_raw
#else
// Fast path, inlined into every generated access. Every other address takes the cold path,
// which re-derives the region and special cases exactly as the former single function did.
template<class UInt> RECOMP_GUEST_SCALAR_INLINE UInt loadGuestScalar(const std::uint8_t* base, std::uint32_t address) {
    if (isFastGuestScalarAccess<UInt>(address)) return loadGuestScalarRaw<UInt>(base + address);
    return loadGuestScalarSlow<UInt>(base, address);
}

template<class UInt> RECOMP_GUEST_SCALAR_INLINE void storeGuestScalar(std::uint8_t* base, std::uint32_t address, UInt value) {
    if (isFastGuestScalarAccess<UInt>(address)) {
        storeGuestScalarRaw<UInt>(base + address, value);
        return;
    }
    storeGuestScalarSlow<UInt>(base, address, value);
}
#endif

// dcbz / dcbz128: zero the aligned block that contains address, bypassing the scalar
// guards exactly like the corpus's inline memset. Reached only through PPC_DCBZ/PPC_DCBZ128
// (trace_prelude.h), which the current corpus does not emit yet.
template<std::uint32_t Bytes> void zeroGuestBlock(std::uint8_t* base, std::uint32_t address) noexcept {
    static_assert(Bytes == 32 || Bytes == 128);
    std::uint8_t* const block = base + (address & ~(Bytes - 1));
#if RECOMP_GUEST_MEMORY_MODEL != 0
    for (std::uint32_t offset = 0; offset < Bytes; offset += 8)
        __atomic_store_n(reinterpret_cast<std::uint64_t*>(block + offset), std::uint64_t{0}, guestScalarMemoryOrder);
#else
    __builtin_memset(block, 0, Bytes); // not the _FORTIFY_SOURCE wrapper
#endif
}
#if RECOMP_GUEST_MEMORY_MODEL != 0
} // inline namespace guest_memory_atomic / guest_memory_strong
#endif
}
