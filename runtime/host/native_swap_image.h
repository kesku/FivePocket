#pragma once
#include "xenos_color_copy.h"
#include <array>
#include <optional>
#include <vector>

namespace recomp::gpu {
// Layout-only contract for the first observed VdSwap image. The header contains
// a CPU virtual address, unlike a shader's physical fetch descriptor. Explicit
// physical translation is caller evidence and MUST be checked against current
// allocation ownership before reading live RAM. This class creates no lease,
// fence certificate, native texture, guest commands or presentation receipt.
class NativeSwapImagePlan final {
public:
    static constexpr std::uint32_t Width=1280, Height=720;
    static constexpr std::uint32_t Capacity=Width*736*4, PixelBytes=Width*Height*4;
    static std::optional<NativeSwapImagePlan> make(const std::array<std::uint32_t,6>& fetch,
            std::uint32_t virtualBase,std::uint32_t physicalBase,std::uint32_t format,
            std::uint32_t colorSpace,std::uint32_t width,std::uint32_t height) noexcept {
        // Xenos fetch layout reference: xenia/xenos.h at95a5c3ee250f80c3b9d139658649d9ffb6db3eec.
        // Type2, unsigned, pitch40*32, tiled; format6/none endian; 2D base mip;
        // normalized channels .zyx1 with no exponent/filter/mip modifiers.
        // Require every observed field, not just enough bits to guess an image.
        if(fetch[0]!=0x8A000002 || fetch[1]!=(virtualBase|6) || fetch[2]!=0x0059E4FF ||
            fetch[3]!=0x1414 || fetch[4]!=0 || fetch[5]!=0x200 || format!=6 || colorSpace ||
            width!=Width || height!=Height || !virtualBase || (virtualBase&4095) || (physicalBase&4095) ||
            std::uint64_t{virtualBase}+Capacity>0x100000000ull || std::uint64_t{physicalBase}+Capacity>0x20000000)
            return std::nullopt;
        return NativeSwapImagePlan(virtualBase,physicalBase);
    }
    std::uint32_t virtualBase() const noexcept { return virtualBase_; }
    std::uint32_t physicalBase() const noexcept { return physicalBase_; }
    // Independent owned RGBA8 output. Reads active texels ONLY, never unused
    // last-tile rows. Failure leaves output intact; in-place input/output alias
    // is safe because every input read precedes the final nonthrowing swap.
    bool decode(const std::uint8_t* tiled,std::size_t bytes,std::vector<std::uint8_t>& output) const {
        if(!tiled || bytes!=Capacity || reinterpret_cast<std::uintptr_t>(tiled)>
            std::numeric_limits<std::uintptr_t>::max()-Capacity)return false;
        try {
            std::vector<std::uint8_t> pixels(PixelBytes);
            for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){
                std::uint32_t offset=0;if(!colorCopyOffset(Width,Height,x,y,offset) || offset>Capacity-4)return false;
                const auto at=(y*Width+x)*4;
                pixels[at]=tiled[offset+2];pixels[at+1]=tiled[offset+1];pixels[at+2]=tiled[offset];pixels[at+3]=255;
            }
            output.swap(pixels);return true;
        } catch(...) {return false;}
    }
private:
    NativeSwapImagePlan(std::uint32_t v,std::uint32_t p):virtualBase_(v),physicalBase_(p){}
    std::uint32_t virtualBase_,physicalBase_;
};
}
