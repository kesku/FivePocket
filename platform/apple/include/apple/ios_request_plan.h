// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recomp::apple {
struct IosRequestPlan {
    std::vector<std::string> arguments;
    std::vector<std::uint8_t> serialized;
    std::string imageSourceRelative;
    std::string imageDestinationRelative;
    std::string requestDestinationRelative;
};

// Pins/root/id must be compiled from the authenticated canonical catalog, not
// supplied by a loose writable manifest. Exact template bytes are hashed here.
// newRoot is the already-resolved private directory (94-byte runtime cap).
// newId must be a fresh random request id; this function only checks its syntax
// and difference from the template id. The caller MUST exclusively create the
// new request directory, copy/verify its image, authenticate every required
// asset, and preserve ownership before using these arguments. No readiness
// flag, filesystem mutation, decoder capability or launch authority is created.
// Only absolute paths are rebased; all nonpath arguments remain byte-identical.
// Failure leaves output unchanged, including when parameters alias output.
bool planIosRequest(std::span<const std::uint8_t> raw,
                    const std::array<std::uint8_t,32>& expectedSha256,
                    std::string_view templateRoot,std::string_view templateId,
                    std::string_view newRoot,std::string_view newId,
                    IosRequestPlan& output) noexcept;
}
