#pragma once
#include "native_swap_image.h"
#include <array>
#include <cstring>
#include <limits>
#include <optional>

namespace recomp::gpu {
// Original native-only command envelope, NOT reconstructed retail PM4 or a
// Xenia token. A future explicitly bound native consumer must recognize this
// exact issued record at its ordered command-stream position. Existing PM4
// executors must reject it, never silently retire it as filler/NOP.
//
// This value object only creates/compares OWNED bytes. It does not write guest
// RAM, issue a ticket, pin an owner, submit/present a frame, advance RPTR, or
// acknowledge vblank. Registry admission/ownership and native execution remain
// separate mandatory obligations before any live producer can use these bytes.
class NativeSwapCommand final {
public:
    static constexpr std::uint32_t Words=64,Bytes=Words*4;
    struct Identity {
        std::uint64_t ticket;
        std::uint32_t commandPhysical,imagePhysical,previousReadWord;
    };
    static std::optional<NativeSwapCommand> make(Identity identity) noexcept {
        if(!identity.ticket || (identity.commandPhysical&3) || (identity.imagePhysical&4095) ||
            std::uint64_t{identity.commandPhysical}+Bytes>0x20000000 ||
            std::uint64_t{identity.imagePhysical}+NativeSwapImagePlan::Capacity>0x20000000 ||
            identity.previousReadWord>=4*1024*1024 ||
            (std::uint64_t{identity.commandPhysical}<std::uint64_t{identity.imagePhysical}+NativeSwapImagePlan::Capacity &&
                std::uint64_t{identity.imagePhysical}<std::uint64_t{identity.commandPhysical}+Bytes))return std::nullopt;
        NativeSwapCommand result(identity);
        // Deliberately NOT canonical/executable PM4. Its apparent PM4 payload
        // length is16384words, so a64word command cannot pass normal framing.
        // The second word and all reserved bytes are part of exact matching.
        const std::array<std::uint32_t,13> prefix{0xFFFF5357,0x41500001,Words,1,
            static_cast<std::uint32_t>(identity.ticket>>32),static_cast<std::uint32_t>(identity.ticket),
            identity.commandPhysical,identity.imagePhysical,identity.previousReadWord,
            NativeSwapImagePlan::Width,NativeSwapImagePlan::Height,
            NativeSwapImagePlan::Capacity,NativeSwapImagePlan::PixelBytes};
        for(unsigned i=0;i<prefix.size();++i)for(unsigned lane=0;lane<4;++lane)
            result.bytes_[i*4+lane]=static_cast<std::uint8_t>(prefix[i]>>(24-lane*8));
        return result;
    }
    const Identity& identity() const noexcept {return identity_;}
    const std::array<std::uint8_t,Bytes>& bytes() const noexcept {return bytes_;}
    // Compare with CURRENT lifecycle-held command storage. Ticket and address
    // are identity, not authentication; callers must prevent ticket reuse.
    bool matches(std::uint32_t commandPhysical,const std::uint8_t* bytes,std::size_t count) const noexcept {
        return commandPhysical==identity_.commandPhysical && bytes && count==Bytes &&
            reinterpret_cast<std::uintptr_t>(bytes)<=std::numeric_limits<std::uintptr_t>::max()-Bytes &&
            !std::memcmp(bytes_.data(),bytes,Bytes);
    }
private:
    explicit NativeSwapCommand(Identity identity) noexcept:identity_(identity){}
    Identity identity_;
    std::array<std::uint8_t,Bytes> bytes_{};
};
}
