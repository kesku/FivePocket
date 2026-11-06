#pragma once
#include <cstdint>
#include <optional>

namespace recomp::gpu {
// RB_DEPTHCONTROL and RB_STENCILREFMASK[_BF], pinned Xenia95a5c3ee
// registers.h. Decoding does not prove attachment identity or depth format.
struct XenosStencilFace {
    unsigned compare=0,fail=0,pass=0,depthFail=0,reference=0,readMask=0,writeMask=0;
    bool operator==(const XenosStencilFace&)const=default;
};
struct XenosDepthStencilState {
    bool stencilEnable=false,depthEnable=false,depthWrite=false,backfaceEnable=false;
    unsigned depthCompare=0;
    XenosStencilFace front,back;
    bool operator==(const XenosDepthStencilState&)const=default;
};
inline std::optional<XenosDepthStencilState> decodeXenosDepthStencilState(
        std::uint32_t control,std::uint32_t front,std::uint32_t back) {
    if((control&8) || (front&0xFF000000u) || (back&0xFF000000u))return std::nullopt;
    XenosDepthStencilState result;
    result.stencilEnable=control&1;result.depthEnable=control&2;result.depthWrite=control&4;
    result.backfaceEnable=control&128;result.depthCompare=(control>>4)&7;
    const auto face=[](unsigned bits,unsigned ref) {
        return XenosStencilFace{bits&7,(bits>>3)&7,(bits>>6)&7,(bits>>9)&7,
            ref&255,(ref>>8)&255,(ref>>16)&255};
    };
    result.front=face(control>>8,front);
    result.back=result.backfaceEnable?face(control>>20,back):result.front;
    return result;
}

// Finite comparison/reference semantics for tests and draw-state auditing.
// No implicit D24FS8 quantization: the caller supplies already represented
// depths. The eventual native raster path must choose its representation.
template<class T> inline bool xenosCompare(unsigned function,T reference,T stored) {
    switch(function) {
    case 0:return false;case 1:return reference<stored;case 2:return reference==stored;
    case 3:return reference<=stored;case 4:return reference>stored;case 5:return reference!=stored;
    case 6:return reference>=stored;case 7:return true;default:return false;
    }
}
inline unsigned applyXenosStencilOperation(unsigned operation,unsigned reference,unsigned stored) {
    stored&=255;reference&=255;
    switch(operation) {
    case 0:return stored;case 1:return 0;case 2:return reference;
    case 3:return stored==255?255:stored+1;case 4:return stored?stored-1:0;
    case 5:return stored^255;case 6:return (stored+1)&255;case 7:return (stored-1)&255;
    default:return stored;
    }
}
struct XenosDepthStencilResult {bool colorPass=false,depthWritten=false;unsigned stencil=0;};
inline XenosDepthStencilResult evaluateXenosDepthStencil(const XenosDepthStencilState& state,
        float incoming,float stored,unsigned stencil,bool backface) {
    const auto& face=backface && state.backfaceEnable?state.back:state.front;
    const bool stencilPass=!state.stencilEnable || xenosCompare(face.compare,face.reference&face.readMask,stencil&face.readMask);
    const bool depthPass=!state.depthEnable || xenosCompare(state.depthCompare,incoming,stored);
    const auto operation=!stencilPass?face.fail:!depthPass?face.depthFail:face.pass;
    const auto next=state.stencilEnable?applyXenosStencilOperation(operation,face.reference,stencil):stencil;
    return {stencilPass && depthPass,stencilPass && depthPass && state.depthEnable && state.depthWrite,
        ((stencil&~face.writeMask)|(next&face.writeMask))&255};
}
}
