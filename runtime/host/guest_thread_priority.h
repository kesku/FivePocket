#pragma once
#include <cstdint>
#include <optional>

namespace recomp::bootstrap {
// Explicit NT-compatible subset: normal relative increments and saturation.
// KeSetBasePriorityThread documents saturation at |increment| >= 16 and
// query/previous-value reporting of +/-16. Process-class changes and boosts
// are not modeled. This is not an assertion about Xbox dispatch timing.
constexpr std::optional<std::int32_t> normalizePortableThreadPriority(std::int64_t increment,bool nativeHighPriorityTiers=false) {
    if (increment >= 16) return 16;
    if (increment <= -16) return -16;
    if (increment >= -2 && increment <= 2) return static_cast<std::int32_t>(increment);
    if (nativeHighPriorityTiers && (increment==3 || increment==4)) return static_cast<std::int32_t>(increment);
    return std::nullopt;
}
constexpr bool validPortableSchedulerPriority(std::int32_t priority,bool nativeHighPriorityTiers=false) {
    return (priority >= -2 && priority <= 2) || priority == -16 || priority == 16 || (nativeHighPriorityTiers && (priority==3 || priority==4));
}
// Fair serialized port policy: saturation is one bounded slice tier beyond
// normal +/-2, never a huge shift by the guest's raw input or OS real-time mode.
// Explicit native +3/+4 share the upper bounded eight-times slice weight. They
// remain separately queryable, not saturated16 sentinels or Xbox priority classes.
constexpr std::int32_t portablePrioritySliceShift(std::int32_t priority) {
    return priority >= 3 ? 3 : priority == -16 ? -3 : priority;
}
}
