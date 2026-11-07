#pragma once
#include "native_color_chain_program.h"
#include "private_input.h"
#include <string>
namespace recomp::gpu {
// Bounded, read-only diagnostic transport. Validating program framing and guest
// keys is NOT authentication of translations, SPIR-V validation or GPU work.
inline std::optional<NativeColorChainProgram> readNativeColorProgram(const std::string& directory,
        NativeColorCoordinateProfile coordinates) noexcept {
    if(directory.empty() || directory.front()!='/' || directory.size()>4096)return std::nullopt;
    try {
        const auto module=[&](const char* role,std::vector<std::uint32_t>& words){
            std::vector<std::uint8_t> bytes;
            if(!readInput((directory+"/"+role+"/dataflow.spv").c_str(),bytes,65536) || bytes.size()<20 || bytes.size()%4)return false;
            words.resize(bytes.size()/4);
            for(unsigned i=0;i<words.size();++i)words[i]=bytes[i*4]|(std::uint32_t{bytes[i*4+1]}<<8)|
                (std::uint32_t{bytes[i*4+2]}<<16)|(std::uint32_t{bytes[i*4+3]}<<24);
            return true;
        };
        NativeColorChainProgram::Translations translations;
        constexpr std::array<const char*,5> roles{{"depth/vertex","r8/vertex","r8/fragment","small/vertex","small/fragment"}};
        for(unsigned i=0;i<5;++i){auto& t=translations[i];t.guest.stage=i==2 || i==4?1:0;
            if(!readInput((directory+"/"+roles[i]+"/guest-code.bin").c_str(),t.guest.bigEndianCode,4096) || !module(roles[i],t.nativeWords))return std::nullopt;}
        NativeColorChainProgram::Support support;
        constexpr std::array<const char*,4> helpers{{"store","copy","load","depth/store"}};
        for(unsigned i=0;i<4;++i)if(!module(helpers[i],support[i]))return std::nullopt;
        return NativeColorChainProgram::create(translations,support,1,coordinates);
    }catch(...){return std::nullopt;}
}
}
