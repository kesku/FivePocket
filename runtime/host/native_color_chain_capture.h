#pragma once
#include "native_color_chain_inputs.h"
#include "native_color_chain_program.h"
#include "native_color_vertex_geometry.h"
#include "native_color_render_state.h"
#include "native_depth_clear_request.h"
#include "native_color_chain_memory.h"
#include "xenos_draw_command_plan.h"

namespace recomp::gpu {
// Own raw resources selected from three command-order draw CANDIDATES. This is
// a binding-layout/shader-key/predicate gate, NOT whole-batch GPU admission.
// Mode/primitive/count checks identify the input ABI only. Full fixed-function
// state, depth pixel/copy vertex, intervening commands, cache/wait effects and
// root/child/input/output ownership still need their separate admission gates.
// The opt-in padded program additionally requires two known coordinate/index/
// window requests and whole W1 rectangle geometry under the bounded CPU policy.
class NativeColorChainCapture {
public:
    NativeColorChainCapture(const NativeColorChainCapture&) = delete;
    NativeColorChainCapture& operator=(const NativeColorChainCapture&) = delete;
    NativeColorChainCapture(NativeColorChainCapture&&) noexcept = default;
    NativeColorChainCapture& operator=(NativeColorChainCapture&&) noexcept = default;

    // All metadata, identities and predicates reject before the first resource
    // read. Reader must enforce allocation ownership, with every guest writer
    // parked for the complete plan + capture interval; a returned vector alone
    // is not proof of GPU-time contents. Source bytes are never modified here.
    static std::optional<NativeColorChainCapture> create(const DrawCommandPlan& plan,
            const std::array<unsigned,3>& indices, const NativeColorChainProgram& program,
            const Pm4PhysicalReader& reader) noexcept {
        if (!reader || plan.shaders.size()>8 || !(indices[0]<indices[1] && indices[1]<indices[2]) ||
            indices[2]>=plan.draws.size()) return std::nullopt;
        try {
            NativeColorChainCapture candidate;candidate.coordinates_=program.coordinates();
            const auto known = [](const DrawRegisterCandidates& registers, std::uint32_t index,
                                  NativeInputWord& output) {
                const auto it=registers.find(index);
                if (it==registers.end() || it->second.bits.known!=0xFFFFFFFF) return false;
                output={it->second.bits.value,it->second.bits.known}; return true;
            };
            const auto descriptor = [&](const auto& registers,std::uint32_t first,auto& output) {
                for (unsigned w=0;w<output.size();++w) if (!known(registers,first+w,output[w])) return false;
                return true;
            };
            const auto constants = [&](const auto& registers,std::uint32_t base,const auto& required,auto& output) {
                for (auto index:required) {
                    NativeColorConstant c;c.index=index;
                    if (!descriptor(registers,base+index*4,c.words)) return false;
                    output.push_back(c);
                }
                return true;
            };
            for (unsigned stage=0;stage<3;++stage) {
                const auto& draw=plan.draws[indices[stage]];
                if ((stage && draw.offsetWords<=plan.draws[indices[stage-1]].offsetWords) ||
                    (draw.opcode!=0x22 && draw.opcode!=0x36) || draw.primitive!=(stage?6u:8u) ||
                    draw.indexCount!=(stage?4u:3u) ||
                    evaluateDrawBinPredicate(draw.predicate.state,draw.predicate.predicated)!=DrawBinDecision::Pass)
                    return std::nullopt;
                NativeInputWord mode;
                if (!known(draw.registers,0x2208,mode) || mode.bits!=(stage?4u:5u)) return std::nullopt;
                if(!stage && candidate.coordinates_==NativeColorCoordinateProfile::Padded1280W1 &&
                    !planNativeDepthClearState(plan,indices[stage],candidate.depthState_))return std::nullopt;
                candidate.predicates_[stage]=draw.predicate;
                if (stage && candidate.coordinates_==NativeColorCoordinateProfile::Padded1280W1 &&
                    !planNativeColorRenderState(draw,stage-1,candidate.renderStates_[stage-1])) return std::nullopt;
                if(stage)candidate.viewports_[stage-1]=candidate.renderStates_[stage-1].viewport;
                for (unsigned shaderStage=0;shaderStage<(stage?2u:1u);++shaderStage) {
                    const auto index=draw.shaders[shaderStage];
                    if (index>=plan.shaders.size()) return std::nullopt;
                    const auto& shader=plan.shaders[index];
                    if (shader.stage!=shaderStage || shader.offsetWords>=draw.offsetWords ||
                        shader.bigEndianCode.empty() || shader.bigEndianCode.size()>4096 || shader.bigEndianCode.size()%12)
                        return std::nullopt;
                    candidate.active_[stage?1+(stage-1)*2+shaderStage:0]={shader.stage,shader.bigEndianCode};
                }
                auto& vertex=stage?candidate.metadata_.pairs[stage-1].vertices:candidate.metadata_.depth;
                vertex.slot=stage?95:0;
                if (!descriptor(draw.registers,0x4800+vertex.slot*2,vertex.descriptor)) return std::nullopt;
                const auto address=vertex.descriptor[0].bits&~3u;
                const auto size=stage?144u:84u;
                if ((vertex.descriptor[0].bits&3)!=3 || vertex.descriptor[1].bits!=(0x10000002|size) ||
                    std::uint64_t{address}+size>0x20000000) return std::nullopt;
                vertex.source={address,nullptr,size};
                if (!stage) continue;
                auto& pair=candidate.metadata_.pairs[stage-1];
                if (stage==1) {
                    if (!constants(draw.registers,0x4000,std::array<unsigned,5>{8,9,10,11,36},pair.vertexConstants) ||
                        !constants(draw.registers,0x4400,std::array<unsigned,5>{32,33,34,35,255},pair.pixelConstants)) return std::nullopt;
                } else if (!constants(draw.registers,0x4000,std::array<unsigned,4>{8,9,10,11},pair.vertexConstants) ||
                           !constants(draw.registers,0x4400,std::array<unsigned,2>{16,62},pair.pixelConstants)) return std::nullopt;
                for (unsigned i=0;i<(stage==1?3u:1u);++i) {
                    NativeColorTextureBinding t;t.slot=stage==1?i:2;
                    if (!descriptor(draw.registers,0x4800+t.slot*6,t.descriptor)) return std::nullopt;
                    std::array<std::uint32_t,6> words{};
                    for (unsigned w=0;w<6;++w) words[w]=t.descriptor[w].bits;
                    if (stage==1) {
                        LinearR8Plane plane;
                        if (!decodeLinearR8Plane(words,plane) || plane.width!=(i?640u:1280u)) return std::nullopt;
                        t.source={plane.physicalAddress,nullptr,plane.pitch*plane.height};
                    } else {
                        if(isNativeConstantZeroTexture(words)) {
                            t.sourceKind=NativeColorTextureBinding::SourceKind::ConstantZeroSwizzle;
                        } else {
                            Tiled32Texture tile;
                            if (!decodeTiled32Texture(words,tile)) return std::nullopt;
                            t.source={tile.physicalAddress,nullptr,4096};
                        }
                    }
                    pair.textures.push_back(t);
                }
            }
            if (!program.matches(candidate.active_)) return std::nullopt;
            if(!collectNativeColorSourceRanges(plan,candidate.metadata_,candidate.sources_))return std::nullopt;
            for (unsigned i=0;i<7;++i) {
                if(i==6 && color_chain_inputs::constantZeroTexture(candidate.metadata_.pairs[1].textures[0]))continue;
                const auto& span=source(candidate.metadata_,i);
                auto bytes=reader(span.physicalAddress,static_cast<std::uint32_t>(span.size));
                if (!bytes || bytes->size()!=span.size) return std::nullopt;
                candidate.raw_[i]=std::move(*bytes);
            }
            if(candidate.coordinates_==NativeColorCoordinateProfile::Padded1280W1){
                NativeColorChainBindings view;
                if(!candidate.bindings(view))return std::nullopt;
                if(!planNativeDepthClearGeometry(candidate.active_[0],view.depth,candidate.depthGeometry_))return std::nullopt;
                for(unsigned i=0;i<2;++i)
                    if(!planNativeColorVertexGeometry(candidate.active_[1+i*2],view.pairs[i],i,candidate.geometry_[i]))return std::nullopt;
            }
            return candidate;
        } catch (...) {return std::nullopt;}
    }
    const NativeColorChainProgram::Identities& active() const noexcept {return active_;}
    const std::array<DrawBinPredicate,3>& predicates() const noexcept {return predicates_;}
    NativeColorCoordinateProfile coordinates() const noexcept {return coordinates_;}
    const std::array<NativeColorViewportRequest,2>& viewports() const noexcept {return viewports_;}
    const std::array<NativeColorVertexGeometry,2>& geometry() const noexcept {return geometry_;}
    const std::array<NativeColorRenderStateRequest,2>& renderStates() const noexcept {return renderStates_;}
    const NativeDepthClearStateRequest& depthState() const noexcept {return depthState_;}
    const NativeDepthClearGeometry& depthGeometry() const noexcept {return depthGeometry_;}
    const NativeColorSourceRanges& sourceRanges() const noexcept {return sources_;}

    // A fresh BORROWED view. It is valid only while this owner stays alive and
    // unmoved. No internal self-pointers survive moves/copies; moved-from owners
    // fail the shape checks rather than producing a stale source pointer.
    bool bindings(NativeColorChainBindings& output) const noexcept {
        try {
            if (metadata_.pairs[0].textures.size()!=3 || metadata_.pairs[1].textures.size()!=1) return false;
            auto candidate=metadata_;
            for (unsigned i=0;i<7;++i) {
                if(i==6 && color_chain_inputs::constantZeroTexture(candidate.pairs[1].textures[0])) {
                    if(!raw_[i].empty())return false;
                    continue;
                }
                auto& span=source(candidate,i);
                if (!span.size || raw_[i].size()!=span.size) return false;
                span.bytes=raw_[i].data();
            }
            output=std::move(candidate); return true;
        } catch (...) {return false;}
    }
    bool pack(PackedNativeColorChainInputs& output) const noexcept {
        NativeColorChainBindings view;
        return bindings(view) && packNativeColorChainInputs(view,output);
    }
private:
    NativeColorChainCapture() = default;
    static NativeColorSource& source(NativeColorChainBindings& b,unsigned i) noexcept {
        if (i<3) return i?b.pairs[i-1].vertices.source:b.depth.source;
        return i<6?b.pairs[0].textures[i-3].source:b.pairs[1].textures[0].source;
    }
    NativeColorChainBindings metadata_;
    std::array<std::vector<std::uint8_t>,7> raw_;
    NativeColorChainProgram::Identities active_;
    std::array<DrawBinPredicate,3> predicates_{};
    NativeColorCoordinateProfile coordinates_=NativeColorCoordinateProfile::DiagnosticPositive;
    std::array<NativeColorViewportRequest,2> viewports_{};
    std::array<NativeColorVertexGeometry,2> geometry_{};
    std::array<NativeColorRenderStateRequest,2> renderStates_{};
    NativeDepthClearStateRequest depthState_{};
    NativeDepthClearGeometry depthGeometry_{};
    NativeColorSourceRanges sources_{};
};
}
