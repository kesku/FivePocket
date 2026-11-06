#pragma once
#include "gta5_bound_surface_capture.h"
#include "native_translated_draw.h"
#include "xenos_depth_stencil_state.h"

namespace recomp::gpu {
// ABI25: a direct triangle list whose ONLY output is the stencil buffer.
//
// The captured draw is a box -- 36 vertices, 12 triangles, stride 36 -- with no
// pixel shader, no textures, RB_COLOR_MASK 0 and RB_DEPTHCONTROL 0x00720213:
// depth tested but never written, stencil enabled. The original stencil ref
// is 0x20 with read/write masks 0xFF; only matching samples may be zeroed on
// depth failure. RB_BLENDCONTROL0 (0x2201) is NOT the stencil reference word.
//
// Unlike the rectangle bands this plan owns no colour attachment of its own.
// The colour target is bound and MASKED OFF, and the depth/stencil target is
// existing guest state the draw reads and updates in place, so what the plan
// carries is the identity of both surfaces and the decoded state -- never a
// new target to draw into.
//
// Every predicate names itself on refusal, so a rejection is diagnosable from
// one log line instead of being guessed at.
struct NativeStencilOnlyPlan {
    std::uint32_t colorObject=0,depthObject=0;
    unsigned width=0,height=0,pitch=0;
    unsigned depthBase=0;
    float minDepth=0,maxDepth=1;
    unsigned cull=0;
    bool clockwise=false;
    XenosDepthStencilState state;
    bool operator==(const NativeStencilOnlyPlan&)const=default;
};
inline std::optional<NativeStencilOnlyPlan> planNativeStencilOnlyDraw(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const Gta5BoundSurfaceCapture& bindings,const char** why=nullptr) {
    const auto no=[&](const char* reason){if(why)*why=reason;return std::optional<NativeStencilOnlyPlan>{};};
    if(!validateNativeStencilOnlyProgram(program) || program.stride!=draw.stride || draw.stride%4 ||
       draw.primitive!=4 || draw.vertexCount<3 || draw.vertexCount>4096 || draw.vertexCount%3 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride ||
       draw.deviceState.size()!=13860)return no("program-profile");
    // The absent pixel stage is the band. A draw that bound one is not this.
    if(draw.pixelShader)return no("pixel-stage-present");
    if(!draw.textures.empty())return no("textures-bound");
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    // Colour writes disabled for every channel of every target.
    const auto colorMask=gta5DrawRegister(draw,0x2104);
    if(!colorMask || *colorMask)return no("colour-mask");
    const auto control=gta5DrawRegister(draw,0x2200);
    const auto front=gta5DrawRegister(draw,0x210D);
    if(!control || !front)return no("depth-control-unreadable");
    // RB_STENCILREFMASK_BF only matters when the two faces differ; when they do
    // not, the decoder is handed the front word for both, as the hardware does.
    const auto backface=(*control)&128u;
    const auto back=backface?gta5DrawRegister(draw,0x210C):front;
    if(!back)return no("depth-control-unreadable");
    const auto state=decodeXenosDepthStencilState(*control,*front,*back);
    if(!state)return no("depth-stencil-state");
    // Stencil is the output. Without it the draw writes nothing at all, which
    // is the inert band's claim to make, not this one's.
    if(!state->stencilEnable)return no("stencil-disabled");
    if(state->depthWrite)return no("depth-writes-enabled");
    // A face that can never change the stencil buffer is not an output either.
    const auto writes=[&](const XenosStencilFace& f) {
        return f.writeMask && (f.fail || f.pass || f.depthFail);
    };
    if(!writes(state->front) && !writes(state->back))return no("stencil-writes-nothing");
    // Exact captured raster mode: no culling, no polygon offsets, standard
    // homogeneous clipping/viewport transform. Do not silently substitute the
    // indexed scene band's back-face culling for this two-sided volume.
    const auto mode=gta5DrawRegister(draw,0x2205);
    if(mode!=0x18000u || gta5DrawRegister(draw,0x2204)!=0x80000u ||
       gta5DrawRegister(draw,0x2206)!=0x43Fu)return no("raster-state");
    const auto zscale=gta5DrawRegister(draw,0x2113),zoffset=gta5DrawRegister(draw,0x2114);
    if(!zscale || !zoffset)return no("depth-viewport");
    float scale,offset;std::memcpy(&scale,&*zscale,4);std::memcpy(&offset,&*zoffset,4);
    const auto end=offset+scale;
    if(!std::isfinite(scale) || !std::isfinite(offset) || !std::isfinite(end) ||
       offset<0 || offset>1 || end<0 || end>1)return no("depth-viewport");
    if(draw.deviceState[11068]&0x10)return no("binning-flag");
    const auto scissor=nativeTranslatedScissor(draw);
    const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || !viewport || (*scissor)[0] || (*scissor)[1] || !(*scissor)[2] ||
       !(*scissor)[3])return no("scissor-viewport");
    const auto width=(*scissor)[2],height=(*scissor)[3];
    const auto surfaceInfo=word(draw.deviceState,10368);
    const auto pitch=surfaceInfo&0x3FFFu;
    if(surfaceInfo&0x3C000u)return no("surface-samples");
    if(!pitch || pitch%80 || pitch<width)return no("surface-pitch");
    // Colour target 0 is bound and masked off; 1-3 carry no attachment.
    const auto colorObject=word(draw.deviceState,12816);
    const auto& color=bindings.surfaces[0];
    if(!colorObject || colorObject%4 || color.object!=colorObject)return no("colour-register");
    for(unsigned slot=1;slot<4;++slot)
        if(bindings.surfaces[slot].object || word(draw.deviceState,12816+4*slot))return no("extra-slot-bound");
    // The depth/stencil target is existing guest state, not a target this band
    // creates: its header is required so the draw cannot be admitted against a
    // surface nobody has described.
    const auto depthObject=word(draw.deviceState,12832);
    const auto& depth=bindings.surfaces[4];
    if(!depthObject || depthObject%4 || depthObject>UINT32_MAX-39 || depth.object!=depthObject)return no("depth-register");
    if(!depth.header || depth.header->size()!=40)return no("depth-header");
    if(word(*depth.header,24)!=surfaceInfo)return no("depth-surface-info");
    // register_table.inc: RB_DEPTH_INFO is 0x2002. 0x2004 is RB_COLOR2_INFO!
    // The actual stopped state and depth header both contain 0x00010000:
    // base 0, D24FS8. Do not infer depth from a masked-off colour register.
    const auto depthInfo=gta5DrawRegister(draw,0x2002);
    if(!depthInfo)return no("depth-info-unreadable");
    // Physical EDRAM has 11-bit tile addressing; bit-11 wrapping is outside
    // this captured band. Keep the header/register producer agreement explicit.
    if(*depthInfo&~0x000107FFu)return no("depth-info");
    const auto depthBase=*depthInfo&0x7FFu;
    if(((*depthInfo>>16)&1u)!=1u)return no("depth-format");
    if(word(*depth.header,28)!=*depthInfo)return no("depth-header-register");
    const auto dimensions=word(*depth.header,36);
    if((dimensions>>18)+1!=width || ((dimensions>>3)&0x7FFFu)+1!=height)
        return no("depth-dimensions");
    if((pitch/80)*((height+15)/16)>2048-depthBase)return no("depth-edram-wrap");
    if(depthObject==colorObject)return no("depth-aliases-colour");
    if(why)*why="planned";
    return NativeStencilOnlyPlan{colorObject,depthObject,width,height,pitch,depthBase,
        offset,end,*mode&3u,bool(*mode&4u),*state};
}
}
