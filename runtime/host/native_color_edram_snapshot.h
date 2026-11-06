#pragma once
#include "native_color_chain_readback.h"
#include <optional>
#include <type_traits>
namespace recomp::gpu {
// Owned final packed-color EDRAM bytes for this exact reference profile, not
// the whole 10 MiB backing. The diagnostic canaries outside the fully written
// tile region must NEVER become known guest graphics memory. No fence or live
// command admission is inferred here; caller supplies stable host-visible data.
class NativeColorEdramSnapshot {
public:
    static constexpr std::uint32_t Begin=720*5120,Bytes=1280*720*4;
    NativeColorEdramSnapshot(const NativeColorEdramSnapshot&)=delete;
    NativeColorEdramSnapshot& operator=(const NativeColorEdramSnapshot&)=delete;
    NativeColorEdramSnapshot(NativeColorEdramSnapshot&&) noexcept=default;
    NativeColorEdramSnapshot& operator=(NativeColorEdramSnapshot&&) noexcept=default;
    static std::optional<NativeColorEdramSnapshot> capture(const std::uint8_t* half,std::size_t halfBytes,
            const std::uint8_t* edram,std::size_t edramBytes,const std::uint8_t* tiled,std::size_t tiledBytes) noexcept {
        AliasFootprint footprint;
        if(!fullAliasFootprint({0x14000500,0xC02D0,1280,720,false},footprint) || footprint.begin!=Begin || footprint.bytes!=Bytes ||
            !auditNativeColorChainReadback(half,halfBytes,edram,edramBytes,tiled,tiledBytes))return std::nullopt;
        try {
            NativeColorEdramSnapshot result;result.bytes_.assign(edram+16+Begin,edram+16+Begin+Bytes);return result;
        }catch(...){return std::nullopt;}
    }
    // EDRAM byte offset, NOT a guest physical address. Reads outside the known
    // region, including adjacent diagnostic canaries, fail with output intact.
    bool readWord(std::uint32_t offset,std::uint32_t& output) const noexcept {
        if(bytes_.size()!=Bytes || (offset&3) || offset<Begin || std::uint64_t{offset}+4>Begin+Bytes)return false;
        const auto* p=bytes_.data()+offset-Begin;
        output=p[0]|(std::uint32_t{p[1]}<<8)|(std::uint32_t{p[2]}<<16)|(std::uint32_t{p[3]}<<24);return true;
    }
    // Const borrowed bytes are valid only while this owner remains alive and
    // unmoved. Their native EDRAM offset is Begin, never zero.
    const std::vector<std::uint8_t>& knownBytes() const noexcept{return bytes_;}
private:
    NativeColorEdramSnapshot()=default;
    std::vector<std::uint8_t> bytes_;
};
static_assert(std::is_nothrow_move_assignable_v<NativeColorEdramSnapshot>);
}
