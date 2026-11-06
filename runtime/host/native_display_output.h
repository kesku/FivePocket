#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace recomp::gpu {
// Captured XDVO_FORCE_OUTPUT_CNTL bit0-clear request. Xenon-emu's register
// table names 1E4E; Free60 clears byte-offset7938 bit0 around ANA programming
// and leaves it clear after mode setup. Normal-video selection is an explicit
// native policy, NOT proof of the retail electrical/blanking/force-data ABI.
// Only this exact operation is admitted; setting force bits remains unsupported.
class NativeDisplayOutputWrapper final {
public:
    static constexpr unsigned Words=16,Bytes=Words*4,Register=0x1E4E;
    static std::optional<NativeDisplayOutputWrapper> parse(std::uint32_t physical,
            const std::uint8_t* bytes,std::size_t size) noexcept {
        if (!bytes || size!=Bytes || (physical&3) || std::uint64_t{physical}+Bytes>0x20000000) return std::nullopt;
        std::array<std::uint32_t,Words> words{};
        for (unsigned i=0;i<Words;++i) for(unsigned b=0;b<4;++b) words[i]=(words[i]<<8)|bytes[i*4+b];
        const std::array<std::uint32_t,Words> expected{0xC0022100,Register,0xFFFFFFFE,0,
            0xC0003B00,0x7FFF,0xC0001000,0x77777777,
            0xC0025800,3,words[10],words[11],0xC0025800,3,words[14],words[15]};
        if (words!=expected || words[10]==words[14]) return std::nullopt;
        for (unsigned at:{10u,14u}) {
            const auto address=words[at]&~3u;
            if ((words[at]&3)!=2 || address>=0x20000000 ||
                (std::uint64_t{address}<std::uint64_t{physical}+Bytes && std::uint64_t{physical}<std::uint64_t{address}+4)) return std::nullopt;
        }
        return NativeDisplayOutputWrapper(physical,words);
    }
    // Partial knowledge: changing bit0 does not invent values for31 other bits.
    static std::pair<std::uint32_t,std::uint32_t> clearForceBit(
            std::pair<std::uint32_t,std::uint32_t> old) noexcept {return {old.first&~1u,old.second|1u};}
    std::uint32_t physical() const noexcept {return physical_;}
    std::uint32_t eventPhysical(unsigned i) const noexcept {return i<2?words_[10+i*4]&~3u:0;}
    std::uint32_t eventValue(unsigned i) const noexcept {return i<2?words_[11+i*4]:0;}
    bool matches(std::uint32_t physical,const std::uint8_t* bytes,std::size_t size) const noexcept {
        if (physical!=physical_ || !bytes || size!=Bytes) return false;
        for(unsigned i=0;i<Words;++i)for(unsigned b=0;b<4;++b)
            if(bytes[i*4+b]!=static_cast<std::uint8_t>(words_[i]>>(24-b*8)))return false;
        return true;
    }
private:
    NativeDisplayOutputWrapper(std::uint32_t physical,const std::array<std::uint32_t,Words>& words):physical_(physical),words_(words){}
    std::uint32_t physical_;
    std::array<std::uint32_t,Words> words_;
};
struct NativeDisplayOutputResult {
    bool presentQueued=false;
    std::uint32_t nativeFences=0;
    std::array<std::uint8_t,8> eventBytes{};
};
inline bool validNativeDisplayOutputResult(const NativeDisplayOutputWrapper& plan,const NativeDisplayOutputResult& result) noexcept {
    if(!result.presentQueued || result.nativeFences!=2)return false;
    for(unsigned i=0;i<2;++i)for(unsigned b=0;b<4;++b)
        if(result.eventBytes[i*4+b]!=static_cast<std::uint8_t>(plan.eventValue(i)>>(24-b*8)))return false;
    return true;
}
}
