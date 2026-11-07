// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "private_trace_api.h"
#include <functional>

namespace recomp::input::delivery::embedding {
using NativeReader=std::function<int(std::uint32_t,recomp::input::GamepadState&)>;
// Bind-time choice only, before guest workers. Default-off returns the original
// legacy function unchanged. A dedicated observer selects the direct V5 source,
// so neither diag-stick polling nor the legacy OR/max/packet mutations execute.
NativeReader selectNativeGamepadReader(const RecompPrivateTraceCallbacksV5* input,
    NativeReader legacy) noexcept;
}
