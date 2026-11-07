#pragma once
#include "native_scaler_wrapper.h"
#include <functional>
#include <vector>

namespace recomp::gpu {
struct NativeScalerTailResult {
    bool configured=false;
    std::uint32_t nativeFences=0;
    std::array<std::uint8_t,8> eventBytes{};
};
inline bool validNativeScalerTailResult(const NativeScalerWrapper& plan,const NativeScalerTailResult& result) noexcept {
    if (!result.configured || result.nativeFences!=3) return false;
    for (unsigned i=0;i<2;++i) for (unsigned b=0;b<4;++b)
        if (result.eventBytes[i*4+b]!=std::uint8_t(plan.operations()[24+i].value>>(24-b*8))) return false;
    return true;
}
// Explicit native logical scanout policy, NOT Xenos display registers/vblank.
// The reviewed wrapper's enable and identity-configuration transactions each
// require a fresh native queue fence. The wait concerns this native pending
// transaction; it does not manufacture a retail1973 pending-bit readback.
// The original native command installs an actual one-to-one RGBA mapping for
// the presentation consumer. Configuration itself draws/presents no image.
class NativeScalerDisplayState final {
public:
    using Fence=std::function<bool()>;
    bool apply(const NativeScalerWrapper::Operation& op,const NativeScalerCommand& command,const Fence& fence) {
        using Kind=NativeScalerWrapper::Kind;
        if (failed_ || configured() || !fence) return false;
        const auto matches=[&](Kind kind,unsigned word,unsigned target,unsigned value) {
            return op.kind==kind && op.wordOffset==word && op.target==target && op.value==value;
        };
        switch (next_) {
        case 15:if (!matches(Kind::ScalerUpdateLock,53,0x1973,0x10000)) return false;break;
        case 16:if (!matches(Kind::ScalerEnable,55,0x1964,1)) return false;break;
        case 17:if (!matches(Kind::ScalerUpdateLock,57,0x1973,0)) return false;break;
        case 18:
            if (!matches(Kind::WaitScalerUpdate,59,0x1973,0) || !complete(fence)) return false;
            break;
        case 19:if (!matches(Kind::FlipControl,65,0x1852,0)) return false;break;
        case 20:if (!matches(Kind::MasterUpdateLock,67,0x1838,1)) return false;break;
        case 21:
            if (op.kind!=Kind::IdentityScaler || op.wordOffset!=69 || op.value!=NativeScalerCommand::Words ||
                (op.target&3) || std::uint64_t{op.target}+NativeScalerCommand::Bytes>0x20000000) return false;
            // Immutable plan already owns and validates the complete inputs.
            // Fence old native work before publishing the logical mapping.
            if (!complete(fence)) return false;
            plan_=command.plan();break;
        default:return false;
        }
        ++next_;return true;
    }
    bool configured() const noexcept {return !failed_ && next_==22 && plan_.has_value();}
    bool failed() const noexcept {return failed_;}
    unsigned nativeFences() const noexcept {return fences_;}
    unsigned nextOperation() const noexcept {return next_;}
    // Actual output operation. Dimensions/bytes must exactly match the plan;
    // arbitrary scaling/filtering remains unsupported. Supports input/output
    // alias; failure leaves output unchanged. Never alters the guest source.
    bool mapRgba(const std::uint8_t* input,std::size_t bytes,unsigned width,unsigned height,
            std::vector<std::uint8_t>& output) const {
        using C=NativeIdentityScalerCandidate;
        constexpr auto PixelBytes=C::Width*C::Height*4;
        if (!configured() || !input || bytes!=PixelBytes || width!=C::Width || height!=C::Height ||
            reinterpret_cast<std::uintptr_t>(input)>std::numeric_limits<std::uintptr_t>::max()-bytes) return false;
        try {
            std::vector<std::uint8_t> pixels(PixelBytes);
            for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
                const auto source=plan_->rgbaSourceOffset(x,y);
                if (!source || *source>bytes-4) return false;
                std::memcpy(pixels.data()+(y*width+x)*4,input+*source,4);
            }
            output.swap(pixels);return true;
        } catch (...) {return false;}
    }
private:
    bool complete(const Fence& fence) {
        failed_=true; // False/throw cannot be retried as a completed transaction.
        if (!fence()) return false;
        ++fences_;failed_=false;return true;
    }
    unsigned next_=15,fences_=0;
    bool failed_=false;
    std::optional<NativeIdentityScalerPlan> plan_;
};
}
