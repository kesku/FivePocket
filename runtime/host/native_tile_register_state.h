#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace recomp::gpu {
// Raw register storage for the ordinary registers present in the captured
// tiled scene. This does NOT establish native raster/shader compatibility.
// Native draws/copies still validate their effective uploaded inputs separately.
// Scratch, coherency, wait/cache triggers and unknown registers are excluded:
// their side effects cannot be implemented by merely remembering their values.
class NativeTileRegisterState final {
public:
    using Journal=std::map<std::uint32_t,std::pair<std::uint32_t,std::uint32_t>>;
    static std::optional<NativeTileRegisterState> parse(std::span<const std::uint8_t> bytes) {
        if(bytes.size()<8 || bytes.size()%4 || bytes.size()>1028)return std::nullopt;
        const auto word=[&](unsigned at) {
            return (std::uint32_t{bytes[at]}<<24)|(std::uint32_t{bytes[at+1]}<<16)|
                (std::uint32_t{bytes[at+2]}<<8)|bytes[at+3];
        };
        const auto header=word(0),first=header&0x7FFF,count=(header>>16)+1;
        if((header>>30) || (header&0x8000) || count+1!=bytes.size()/4)return std::nullopt;
        NativeTileRegisterState result;result.writes_.reserve(count);
        for(unsigned i=0;i<count;++i) {
            if(!ordinary(first+i))return std::nullopt;
            result.writes_.emplace_back(first+i,word(4+i*4));
        }
        return result;
    }
    void apply(Journal& journal) const {
        auto completed=journal;
        for(const auto& [reg,value]:writes_)completed[reg]={value,UINT32_MAX};
        journal.swap(completed);
    }
    const auto& writes() const noexcept{return writes_;}
private:
    static bool ordinary(unsigned reg) noexcept {
        // UCP plane coefficients, vertex fetch constants and fetch-flush
        // configuration words are retained bit-exact, not interpreted as GPU
        // work here. Any non-native-compatible use must fail draw admission.
        if((reg>=0x2388 && reg<0x23A0) || (reg>=0x4800 && reg<0x4806) || (reg>=0x5000 && reg<0x5003))return true;
        switch(reg) {
        case 0xF01:case 0x2007:case 0x200D:
        case 0x2100:case 0x2101:case 0x2102:case 0x2180:case 0x2181:
        case 0x2200:case 0x2203:case 0x2204:case 0x2205:case 0x2206:case 0x2208:
        case 0x2293:case 0x2312:case 0x2318:case 0x2319:case 0x231A:case 0x231B:case 0x231E:case 0x231F:return true;
        default:return false;
        }
    }
    NativeTileRegisterState()=default;
    std::vector<std::pair<unsigned,unsigned>> writes_;
};
}
