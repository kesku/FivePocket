#pragma once
#include "gta5_color8_clear_plan.h"
#include "gta5_fixed16_clear.h"

namespace recomp::gpu {
// Separate type: a validated fixed16 surface must never be submitted to the
// color8 image or resolve path merely because both consume32 bits per sample.
struct Gta5Fixed16ClearPlan {
    std::uint32_t object=0,width=0,height=0,pitch=0,baseTile=0,tileCount=0,samples=0,format=0;
    std::uint32_t color=0;
    std::optional<Gta5Depth24ClearPlan> depth;
    bool operator==(const Gta5Fixed16ClearPlan&) const = default;
};
struct Gta5Fixed16ClearAudit {
    Gta5Fixed16ClearPlan plan;
    std::uint32_t issues=0,effectiveFlags=0;
};
inline Gta5Fixed16ClearAudit auditGta5Fixed16Clear(const Gta5ClearCapture& c) {
    return detail::auditGta5Clear32<Gta5Fixed16ClearAudit>(c,
        [](unsigned color){return !(color&~0x000407FFu) && ((color>>16)&15)==4;},
        [](const auto& rgba,unsigned format){return format==4?packGta5Fixed16Clear(rgba):std::nullopt;});
}
}
