#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace recomp::gpu {
// Canonical unpredicated IM_LOAD_IMMEDIATE, whole program at instruction0.
// Retains exact raw guest bytes, not SPIR-V or proof the program can execute.
// Partial instruction-memory updates and malformed/trailing packets reject.
class NativeInlineShaderLoad final {
public:
    using Programs=std::array<std::vector<std::uint8_t>,2>;
    static std::optional<NativeInlineShaderLoad> parse(std::span<const std::uint8_t> bytes) noexcept {
        if(bytes.size()<16 || bytes.size()%4 || bytes.size()>65540)return std::nullopt;
        const auto word=[&](unsigned at){return (std::uint32_t{bytes[at]}<<24)|(std::uint32_t{bytes[at+1]}<<16)|
            (std::uint32_t{bytes[at+2]}<<8)|bytes[at+3];};
        const auto words=bytes.size()/4-3;
        if(words>16382 || word(0)!=(0xC0002B00u|((words+1)<<16)) || word(4)>1 || word(8)!=words)return std::nullopt;
        try{return NativeInlineShaderLoad(word(4),{bytes.begin()+12,bytes.end()});}catch(...){return std::nullopt;}
    }
    void apply(Programs& programs) const {
        auto replacement=bytes_;programs[stage_].swap(replacement);
    }
    unsigned stage() const noexcept{return stage_;}
    const std::vector<std::uint8_t>& bytes() const noexcept{return bytes_;}
private:
    NativeInlineShaderLoad(unsigned stage,std::vector<std::uint8_t> bytes):stage_(stage),bytes_(std::move(bytes)){}
    unsigned stage_;std::vector<std::uint8_t> bytes_;
};
}
