// Precise-reservation overrides for the split 64-bit LL/SC helper pair of the GTA V corpus.
//
//   sub_836DE760: mfmsr r5 | stdx r5,0,r4 | mtmsrd r13,1 | ldarx r3,0,r3 | blr
//                 Load-reserved; returns to its caller with the reservation held.
//   sub_836DE778: stdcx. r4,0,r3 | mtmsrd r5,1 | li r3,1 | beqlr+ | li r3,0 | blr
//                 Store-conditional; r3 = 1 on success.
//
// The lock-free stack pop sub_834710D8 loads head->next between these calls. The generated
// translation checks only that the 64-bit {head, depth} header still holds the loaded value,
// so pop A, pop B, push X, push A by other threads (the ABA pattern) lets a stale pop install
// a node another thread owns. Xenon fails the stdcx. because stores to the header killed the
// reservation. This file restores that failure. Every store-conditional made through this
// pair publishes a new generation for its 128-byte granule (hashed to a stripe). A reservation
// holds only while its generation is unchanged and the value compares equal.
//
// Both generated definitions are PPC_WEAK_FUNC, so these strong definitions replace them at
// link time, for direct calls and PPCFuncMappings alike. The file compiles to nothing unless
// RECOMP_GUEST_PRECISE_RESERVATION is 1. That defaults to 1 when a concurrent guest memory
// model is selected (RECOMP_GUEST_MEMORY_ATOMIC or _STRONG), so a default build keeps the
// generated helpers.
//
// Not covered: plain stores and inline (generated) stwcx./stdcx. to the same granule do not
// publish generations. Those are caught only if they leave a different value.

#include "trace_prelude.h"

#include "ppc_config.h"
#include "ppc_context.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#ifndef RECOMP_GUEST_PRECISE_RESERVATION
#if RECOMP_GUEST_MEMORY_MODEL != 0
#define RECOMP_GUEST_PRECISE_RESERVATION 1
#else
#define RECOMP_GUEST_PRECISE_RESERVATION 0
#endif
#endif

#if RECOMP_GUEST_PRECISE_RESERVATION

PPC_EXTERN_FUNC(sub_836DE760);
PPC_EXTERN_FUNC(sub_836DE778);

namespace {

constexpr std::size_t ReservationStripeCount = 4096; // 128-byte granules, hashed

// Even: idle. Odd: a store-conditional is between claiming the stripe and publishing.
struct alignas(64) ReservationStripe {
    std::atomic<std::uint64_t> generation{0};
};
ReservationStripe reservationStripes[ReservationStripeCount];

struct HelperReservation {
    std::uint32_t address{0};
    std::uint64_t generation{0};
    bool valid{false};
};
// One reservation per host thread, like one per hardware thread. Each guest thread owns a
// host thread in every executor mode, and a native callback that runs on the same host
// thread replaces the reservation exactly as an interrupt would.
thread_local HelperReservation helperReservation;

ReservationStripe& reservationStripe(std::uint32_t address) noexcept {
    return reservationStripes[(address >> 7) & (ReservationStripeCount - 1)];
}

// The internal implementations keep the generated names, so PPC_FUNC_PROLOGUE reports the
// same checkpoint label (__func__) as the generated __imp__ functions.
void __imp__sub_836DE760(PPCContext& __restrict ctx, uint8_t* base) {
    PPC_FUNC_PROLOGUE();
    // mfmsr r5
    ctx.r5.u64 = ctx.msr;
    // stdx r5,0,r4
    PPC_STORE_U64(ctx.r4.u32, ctx.r5.u64);
    // mtmsrd r13,1
    ctx.msr = (ctx.r13.u32 & 0x8020) | (ctx.msr & ~0x8020);
    // ldarx r3,0,r3. Sample the generation before the value: a value written by a
    // store-conditional that completes after this sample fails that generation check.
    const std::uint32_t address = ctx.r3.u32;
    const std::uint64_t generation = reservationStripe(address).generation.load(std::memory_order_acquire);
    ctx.reserved.u64 = __atomic_load_n(reinterpret_cast<const std::uint64_t*>(base + address), __ATOMIC_ACQUIRE);
    helperReservation = {address, generation, true};
    ctx.r3.u64 = __builtin_bswap64(ctx.reserved.u64);
    // blr
}

void __imp__sub_836DE778(PPCContext& __restrict ctx, uint8_t* base) {
    PPC_FUNC_PROLOGUE();
    // stdcx. r4,0,r3. Any store-conditional consumes the reservation, successful or not.
    const std::uint32_t address = ctx.r3.u32;
    const HelperReservation reservation = helperReservation;
    helperReservation.valid = false;
    bool stored = false;
    if (reservation.valid && reservation.address == address && (reservation.generation & 1) == 0) {
        ReservationStripe& stripe = reservationStripe(address);
        std::uint64_t claimed = reservation.generation;
        // Fails if any helper store-conditional in this granule completed, or is in progress,
        // since the load-reserved.
        if (stripe.generation.compare_exchange_strong(claimed, reservation.generation + 1,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            std::uint64_t expected = ctx.reserved.u64;
            stored = __atomic_compare_exchange_n(reinterpret_cast<std::uint64_t*>(base + address), &expected,
                    __builtin_bswap64(ctx.r4.u64), false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
            // Success invalidates every reservation in the granule. A value mismatch stored
            // nothing, so it restores the claimed generation.
            stripe.generation.store(reservation.generation + (stored ? 2 : 0), std::memory_order_release);
        }
    }
    ctx.cr0.lt = 0;
    ctx.cr0.gt = 0;
    ctx.cr0.eq = stored;
    ctx.cr0.so = ctx.xer.so;
    // mtmsrd r5,1
    ctx.msr = (ctx.r5.u32 & 0x8020) | (ctx.msr & ~0x8020);
    // li r3,1 | beqlr+ | li r3,0 | blr
    ctx.r3.s64 = stored ? 1 : 0;
}

} // namespace

// Same shape as the generated weak wrappers, but strong.
PPC_FUNC(sub_836DE760) {
    __imp__sub_836DE760(ctx, base);
}

PPC_FUNC(sub_836DE778) {
    __imp__sub_836DE778(ctx, base);
}

#endif // RECOMP_GUEST_PRECISE_RESERVATION
