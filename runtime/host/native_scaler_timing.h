#pragma once
#include <array>
#include <cstdint>

namespace recomp::gpu {
// Explicit native compatibility hypothesis, not measured Xbox/scaler hardware.
// Logical CTA720p60 timing:1650 total pixels *750 lines *60Hz =74.25MHz.
// Guest scaler uses video-mode+0x1C as an integer divisor of VdGpuClockInMHz.
// Treat that field as whole-MHz pixel bandwidth (truncate to74). Xenia's
// independently named unknown field also returns74, but does not prove meaning.
// The500MHz numerator is the SAME nominal scale as our explicit native clock,
// not the Android GPU's measured frequency. No mode change, vblank or PLL write.
struct NativeScalerTiming {
    static constexpr std::uint32_t TotalWidth=1650,TotalHeight=750,RefreshHz=60;
    static constexpr std::uint32_t PixelClockHz=TotalWidth*TotalHeight*RefreshHz;
    static constexpr std::uint32_t PixelClockWholeMHz=PixelClockHz/1'000'000;
    static constexpr std::uint32_t NominalGpuMHz=500;
    static constexpr std::array<std::uint8_t,4> GpuClockBytes{0,0,1,0xF4};
    static_assert(PixelClockHz==74'250'000 && PixelClockWholeMHz==74);
};
}
