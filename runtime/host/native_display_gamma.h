#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace recomp::gpu {
// Native RGB8 scanout policy: 256 independent R/G/B unsigned10-bit LUT samples,
// indexed by input byte and normalized to8-bit with round-to-nearest. Alpha is
// unchanged. Not a claim of retail analog output/dithering/filter precision.
class NativeDisplayGammaRamp final {
public:
    using Packed=std::array<std::uint32_t,256>;
    static std::optional<NativeDisplayGammaRamp> make(const Packed& packed) noexcept {
        for (const auto value:packed) if (value&0xC0000000) return std::nullopt;
        return NativeDisplayGammaRamp(packed);
    }
    std::uint16_t sample(unsigned index,unsigned channel) const noexcept {
        return index<256 && channel<3?static_cast<std::uint16_t>((packed_[index]>>(20-channel*10))&1023):0;
    }
    std::array<std::uint8_t,4> pixel(const std::array<std::uint8_t,4>& input) const noexcept {
        auto result=input;
        for (unsigned c=0;c<3;++c) result[c]=static_cast<std::uint8_t>((sample(input[c],c)*255u+511u)/1023u);
        return result;
    }
    bool mapRgba(const std::uint8_t* input,std::size_t bytes,std::vector<std::uint8_t>& output) const {
        if (!input || !bytes || bytes%4 || bytes>1280*720*4 ||
            reinterpret_cast<std::uintptr_t>(input)>std::numeric_limits<std::uintptr_t>::max()-bytes) return false;
        try {
            std::vector<std::uint8_t> mapped(bytes);
            for (std::size_t i=0;i<bytes;i+=4) {
                const auto p=pixel({input[i],input[i+1],input[i+2],input[i+3]});
                for (unsigned c=0;c<4;++c) mapped[i+c]=p[c];
            }
            output.swap(mapped);return true;
        } catch (...) {return false;}
    }
private:
    explicit NativeDisplayGammaRamp(const Packed& packed):packed_(packed){}
    Packed packed_;
};

// Owns one full observed RGB8 table configuration. Pure validation: it does
// not unlock a display, satisfy the scaler wait, present or publish events.
// REG_RMW only clears format bits8..10; unknown other control bits remain
// unknown. COND_WRITE info7 is unconditional, not a guessed register poll.
// Primary field references: Xenia register_table.inc and command_processor.cc.
class NativeDisplayGammaWrapper final {
public:
    static constexpr unsigned Words=2339,Bytes=Words*4,TableOffset=23,TableStride=9,TailOffset=2327;
    static std::optional<NativeDisplayGammaWrapper> parse(std::uint32_t physical,const std::uint8_t* bytes,
            std::size_t size,std::uint32_t frontbuffer) noexcept {
        if (!bytes || size!=Bytes || (physical&3) || std::uint64_t{physical}+size>0x20000000 ||
            (frontbuffer&3) || frontbuffer>=0x20000000) return std::nullopt;
        std::array<std::uint32_t,Words> words{};
        for (unsigned i=0;i<Words;++i) for (unsigned b=0;b<4;++b) words[i]=(words[i]<<8)|bytes[i*4+b];
        const std::array<std::uint32_t,23> prefix{0xC0022100,0x1841,0xFFFFF8FF,0,
            0x1930,0,0x1844,frontbuffer,0x1838,0,0xC0043C00,3,0x1973,0,1,0x100,
            0x1852,1,0x11921,0,0,0x1927,7};
        for (unsigned i=0;i<prefix.size();++i) if (words[i]!=prefix[i]) return std::nullopt;
        NativeDisplayGammaRamp::Packed packed{};
        for (unsigned i=0;i<256;++i) {
            const auto at=TableOffset+i*TableStride;packed[i]=words[at+1];
            const std::array<std::uint32_t,9> entry{0x1925,packed[i],0xC0054500,7,0x1925,packed[i],0xFFFFFFFF,0x1922,i+1};
            for (unsigned j=0;j<entry.size();++j) if (words[at+j]!=entry[j]) return std::nullopt;
        }
        const auto ramp=NativeDisplayGammaRamp::make(packed);if (!ramp) return std::nullopt;
        const std::array<std::uint32_t,12> tail{0xC0003B00,0x7FFF,0xC0001000,0x77777777,
            0xC0025800,3,words[2333],words[2334],0xC0025800,3,words[2337],words[2338]};
        for (unsigned i=0;i<tail.size();++i) if (words[TailOffset+i]!=tail[i]) return std::nullopt;
        for (unsigned offset:{2333u,2337u}) {
            if ((words[offset]&3)!=2 || (words[offset]&~3u)>=0x20000000) return std::nullopt;
            const auto address=words[offset]&~3u;
            if (std::uint64_t{address}<std::uint64_t{physical}+Bytes && std::uint64_t{physical}<std::uint64_t{address}+4) return std::nullopt;
        }
        if (words[2333]==words[2337]) return std::nullopt;
        return NativeDisplayGammaWrapper(physical,frontbuffer,words,*ramp);
    }
    const NativeDisplayGammaRamp& ramp() const noexcept {return ramp_;}
    std::uint32_t frontbuffer() const noexcept {return frontbuffer_;}
    std::uint32_t physical() const noexcept {return physical_;}
    std::uint32_t eventPhysical(unsigned i) const noexcept {return i<2?words_[2333+i*4]&~3u:0;}
    std::uint32_t eventValue(unsigned i) const noexcept {return i<2?words_[2334+i*4]:0;}
    std::uint32_t lastPackedSample() const noexcept {return words_[TableOffset+255*TableStride+1];}
    bool matches(std::uint32_t physical,const std::uint8_t* bytes,std::size_t size) const noexcept {
        if (!bytes || physical!=physical_ || size!=Bytes) return false;
        for (unsigned i=0;i<Words;++i) for (unsigned b=0;b<4;++b)
            if (bytes[i*4+b]!=static_cast<std::uint8_t>(words_[i]>>(24-b*8))) return false;
        return true;
    }
private:
    NativeDisplayGammaWrapper(std::uint32_t physical,std::uint32_t frontbuffer,
            const std::array<std::uint32_t,Words>& words,const NativeDisplayGammaRamp& ramp)
        :physical_(physical),frontbuffer_(frontbuffer),words_(words),ramp_(ramp){}
    std::uint32_t physical_,frontbuffer_;
    std::array<std::uint32_t,Words> words_;
    NativeDisplayGammaRamp ramp_;
};
struct NativeDisplayGammaResult {
    bool presentQueued=false;
    std::uint32_t nativeFences=0;
    std::array<std::uint8_t,8> eventBytes{};
};
inline bool validNativeDisplayGammaResult(const NativeDisplayGammaWrapper& plan,const NativeDisplayGammaResult& result) noexcept {
    if (!result.presentQueued || result.nativeFences!=2) return false;
    for (unsigned i=0;i<2;++i) for (unsigned b=0;b<4;++b)
        if (result.eventBytes[i*4+b]!=static_cast<std::uint8_t>(plan.eventValue(i)>>(24-b*8))) return false;
    return true;
}
}
