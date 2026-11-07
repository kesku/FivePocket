// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace recomp::bootstrap {
class GuestImportRegistry;
}

extern "C" void recomp_bind_generated_guest_import_registry(
    recomp::bootstrap::GuestImportRegistry* registry,
    std::uint8_t* expectedGuestBase) noexcept;
extern "C" void recomp_unbind_generated_guest_import_registry() noexcept;
