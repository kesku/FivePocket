// Apple implementation of the existing runtime interface. Compile this instead
// of guest_address_space.cpp from the attempt, never alongside it. Uses the
// Mach alias mechanism measured on the mini in M0; no POSIX shared-memory name
// or sandbox-external file is needed. No generated game source is copied here.
#include "guest_address_space.h"
#include <mach/mach.h>
#include <sys/mman.h>

namespace recomp::bootstrap {
GuestAddressSpace::~GuestAddressSpace() { release(); }

bool GuestAddressSpace::reserve() noexcept {
    if (base_) return true;
    void* p = mmap(nullptr, Size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return false;
    base_ = static_cast<std::uint8_t*>(p);
    return true;
}

bool GuestAddressSpace::reserveWithPhysicalAcAlias() noexcept {
    if (base_) return false; // Never overwrite an already populated guest.
    if (!reserve()) return false;
    constexpr vm_size_t bytes = 0x20000000;
    const auto source = reinterpret_cast<vm_address_t>(base_ + 0xA0000000);
    const auto wanted = reinterpret_cast<vm_address_t>(base_ + 0xC0000000);
    vm_address_t destination = wanted;
    vm_prot_t current = 0, maximum = 0;
    boolean_t copy = FALSE;
#if defined(APPLE_MEMORY_MUTANT_COPY)
    copy = TRUE;
#endif
    const auto rc = vm_remap(mach_task_self(), &destination, bytes, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), source,
        copy, &current, &maximum, VM_INHERIT_NONE);
    if (rc != KERN_SUCCESS || destination != wanted ||
        (current & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE)) {
        release();
        return false;
    }
    return true;
}

void GuestAddressSpace::release() noexcept {
    if (!base_) return;
#if !defined(APPLE_MEMORY_MUTANT_RETAIN)
    munmap(base_, Size);
    base_ = nullptr;
#endif
}
bool GuestAddressSpace::isReserved() const noexcept { return base_ != nullptr; }
std::uint8_t* GuestAddressSpace::base() const noexcept { return base_; }
std::uint8_t* GuestAddressSpace::translate(std::uint32_t address) const noexcept {
    return base_ ? base_ + address : nullptr;
}
} // namespace recomp::bootstrap
