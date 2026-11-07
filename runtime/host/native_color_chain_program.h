#pragma once
#include "native_color_chain_profile.h"
#include "native_color_viewport.h"
#include <optional>
#include <utility>

namespace recomp::gpu {
// Order is part of ABI version 1, not an arbitrary shader-cache index.
enum class NativeColorShaderRole : unsigned { DepthVertex, R8Vertex, R8Pixel, SmallVertex, SmallPixel };
struct NativeColorShaderIdentity {
    std::uint32_t stage=0; // Guest stage: 0 vertex, 1 pixel.
    std::vector<std::uint8_t> bigEndianCode;
};
struct NativeColorShaderTranslation {
    NativeColorShaderIdentity guest;
    std::vector<std::uint32_t> nativeWords;
};

// Exact association of locally validated translations with five ACTIVE guest
// stage identities. Not an authentication mechanism, translator, spirv-val,
// descriptor reflection, PM4 fixed-function admission or fidelity certificate.
// The offline producer must validate all module instructions and their ABI.
// Coordinate metadata also belongs to that association: it must describe the
// actual two color vertex modules, not merely the caller's preferred viewport.
// Depth pixel and copy vertex support are separate command-profile obligations;
// they are NOT silently accepted just because these five translated roles match.
class NativeColorChainProgram {
public:
    using Profile=NativeColorChainProfile<true,true,true>;
    using Identities=std::array<NativeColorShaderIdentity,5>;
    using Translations=std::array<NativeColorShaderTranslation,5>;
    // Native support roles: half store, tiled copy, half load, depth store.
    using Support=std::array<std::vector<std::uint32_t>,4>;
    static std::optional<NativeColorChainProgram> create(const Translations& stages,
            const Support& support,unsigned abiVersion=1,
            NativeColorCoordinateProfile coordinates=NativeColorCoordinateProfile::DiagnosticPositive) {
        if(abiVersion!=1 || !validNativeColorCoordinateProfile(coordinates))return std::nullopt;
        for(unsigned i=0;i<5;++i)
            if(!validIdentity(stages[i].guest,i) || !framing(stages[i].nativeWords,guestStage(i)?4u:0u,false))return std::nullopt;
        for(unsigned i=0;i<4;++i)if(!framing(support[i],5,i==3))return std::nullopt;
        try {
            NativeColorChainProgram p;p.coordinates_=coordinates;
            for(unsigned i=0;i<5;++i)p.identities_[i]=stages[i].guest;
            p.code_={stages[2].nativeWords,stages[1].nativeWords,stages[4].nativeWords,stages[3].nativeWords,
                support[0],support[1],support[2]};
            p.depthCode_={stages[0].nativeWords,support[3]};
            return p;
        } catch (...) {return std::nullopt;}
    }
    bool matches(const Identities& active) const noexcept {
        for(unsigned i=0;i<5;++i)
            if(!validIdentity(active[i],i) || active[i].bigEndianCode!=identities_[i].bigEndianCode)return false;
        return true;
    }
    const Profile::Code& code()const noexcept{return code_;}
    const Identities& identities()const noexcept{return identities_;}
    const Profile::DepthCode& depthCode()const noexcept{return depthCode_;}
    NativeColorCoordinateProfile coordinates()const noexcept{return coordinates_;}
private:
    NativeColorChainProgram()=default;
    static unsigned guestStage(unsigned role)noexcept{return role==2 || role==4?1:0;}
    static bool validIdentity(const NativeColorShaderIdentity& key,unsigned role)noexcept{
        return key.stage==guestStage(role) && !key.bigEndianCode.empty() && key.bigEndianCode.size()<=4096 && key.bigEndianCode.size()%12==0;
    }
    static bool framing(const std::vector<std::uint32_t>& words,unsigned model,bool imageQuery)noexcept{
        if(words.size()<5 || words.size()>16384 || words[0]!=0x07230203 || words[1]!=0x10300 ||
           !words[3] || words[3]>1'000'000 || words[4])return false;
        unsigned entries=0,capabilities=0;
        for(std::size_t at=5;at<words.size();){const auto n=words[at]>>16,op=words[at]&65535;
            if(!n || n>words.size()-at)return false;
            if(op==17){
                if(n!=2)return false;
                const unsigned bit=words[at+1]==1?1:(imageQuery && words[at+1]==50?2:0);
                if(!bit || (capabilities&bit))return false;capabilities|=bit;
            }else if(op==15){
                if(n<5 || words[at+1]!=model || !words[at+2] || words[at+2]>=words[3] ||
                   words[at+3]!=0x6E69616D || words[at+4]!=0 || ++entries!=1)return false;
            }else if(op==10)return false; // No extensions in this bounded module profile.
            at+=n;
        }
        return entries==1 && (capabilities&1);
    }
    Identities identities_;
    Profile::Code code_;
    Profile::DepthCode depthCode_;
    NativeColorCoordinateProfile coordinates_=NativeColorCoordinateProfile::DiagnosticPositive;
};
}
