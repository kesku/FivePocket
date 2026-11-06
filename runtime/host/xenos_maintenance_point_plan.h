#pragma once
#include "xenos_native_initialization.h"
#include "gta5_tile_deferred_tail.h"

namespace recomp::gpu {
// A plan, NOT submission authority. An executor still needs current allocation
// ownership, alias checks, native cache/target ownership, ordered GPU work,
// audited event bytes, and an atomic state/guest-completion commit. This must
// never be used by the disabled side-effect-free acknowledgement path.
enum class MaintenancePointFailure {
    None, Limits, Graph, Header, Register, ShaderLayout, ShaderProof, DrawState,
    DrawLayout, Opcode, Event, EventOrdering, Incomplete, Allocation
};
struct MaintenancePointError {
    MaintenancePointFailure failure=MaintenancePointFailure::None;
    std::uint32_t node=0,offset=0,detail=0;
};
struct MaintenancePointPlan {
    struct Point { std::uint32_t node,offset; ConstantShaderPair pair; };
    struct Event { std::uint32_t node,offset,address,value; };
    struct Barrier { std::uint32_t precedingPoints; bool tileFlush; std::uint32_t invalidateMask=0; };
    std::vector<Point> points;
    std::vector<Event> events;
    std::vector<Barrier> barriers;
    std::map<std::uint32_t,std::uint32_t> registers;
    // Retain the final loaded microcode even after derived-state invalidation. Loading
    // an embedded program is not executing it: only programs active at a draw
    // receive the strict constant/no-memory proof below.
    std::array<std::vector<std::uint8_t>,2> finalPrograms;
    NativeWordUpdatePlan updates;
    std::uint32_t shaderLoads=0,invalidations=0,registerWrites=0;
    bool experimentalLeadingIdlePair=false;
    // Separate runtime profile: masked point setup -> idle -> raw uploads ->
    // original deferred fence tail. This is never startup completion authority.
    std::optional<Gta5TileDeferredTail> runtimeTail;
};

enum class MaintenancePointProfile { LeadingIdle, RuntimeProgramUploads };

inline std::optional<Gta5TileDeferredTail> inspectRuntimeMaintenanceEnvelope(
        const Pm4Snapshot& root,const Pm4IndirectGraph& graph) {
    // Actual97->100 capture: three source-produced setup/GPR/point IBs followed
    // by CPU-emitted program/constants and original82309890's completion tail.
    // Counts are bounded profile requirements, NOT an instruction to skip them.
    if(root.words().size()!=3 || root.words()[0]!=0xC0013F00 || graph.nodes.size()!=4)return std::nullopt;
    const auto& parent=graph.nodes[0];const auto& w=parent.snapshot.words();
    if(parent.parent || parent.parentOffsetWords || parent.depth!=1 ||
       parent.physicalAddress!=root.words()[1] || w.size()!=root.words()[2] ||
       w.size()<32 || w.size()>32768)return std::nullopt;
    constexpr std::array<unsigned,4> scissor{0x22080,0,0,0x01800500};
    constexpr std::array<unsigned,6> idle{0x5C8,0x20000,0xE00,1,0xE40,1};
    if(!std::equal(scissor.begin(),scissor.end(),w.begin()) ||
       !std::equal(idle.begin(),idle.end(),w.begin()+13))return std::nullopt;
    constexpr unsigned counts[]{74,13,48};
    for(unsigned i=0;i<3;++i) {
        const auto& child=graph.nodes[i+1];const unsigned at=4+3*i;
        if(child.parent!=1 || child.parentOffsetWords!=at || child.depth!=2 ||
           child.snapshot.words().size()!=counts[i] || w[at]!=0xC0013F00 ||
           w[at+1]!=child.physicalAddress || w[at+2]!=counts[i])return std::nullopt;
    }
    constexpr std::array<unsigned,13> gpr{0x5C8,0x20000,0x78D00,
        0x40401,0x40401,0x40401,0x40401,0x40401,0x40401,0x40401,0x40401,0xD00,0x40400};
    if(!std::equal(gpr.begin(),gpr.end(),graph.nodes[2].snapshot.words().begin()))return std::nullopt;
    const auto& points=graph.nodes[3].snapshot.words();
    for(unsigned i=0;i<48;i+=2)if(points[i]!=0xC0003600 || points[i+1]!=0x10081)return std::nullopt;
    // No hidden draws, memory operations or extra IBs in the upload suffix.
    // Full packet framing, raw state and shader validation still run below.
    for(const auto& p:parent.snapshot.packets()) {
        if(p.offsetWords<19 || p.offsetWords>=w.size()-12)continue;
        if(p.kind==Pm4Kind::Filler)continue;
        if(p.kind!=Pm4Kind::Command || (p.opcode!=0x2B && p.opcode!=0x56 && p.opcode!=0x3B))return std::nullopt;
        if(p.opcode==0x3B && (p.payloadWords!=1 || w[p.offsetWords+1]!=0x100))return std::nullopt;
    }
    std::array<std::uint8_t,48> tail{};
    for(unsigned i=0;i<12;++i)for(unsigned b=0;b<4;++b)tail[i*4+b]=w[w.size()-12+i]>>(24-b*8);
    const auto physical=std::uint64_t{parent.physicalAddress}+w.size()*4-48;
    if(physical>UINT32_MAX)return std::nullopt;
    return Gta5TileDeferredTail::parse(static_cast<unsigned>(physical),tail);
}

inline bool maintenancePointRegisterValue(std::uint32_t index,std::uint32_t value) {
    // No scratch, gamma, indirect memory or generic register fallback.
    if(index==0x1DC || index==0x1DD || (index>=0x578 && index<=0x57F) || index==0x5C8)return false;
    if(index==0xD00) {
        // Source command-stream SQ_GPR_MANAGEMENT allocation controls; retain
        // exact supported values. They do not become Adreno register writes.
        return value==0x40400 || value==0x40401 || value==0x245C0 || value==0x245C1 ||
            value==0x20600 || value==0x20601;
    }
    if(index==0x2203)return value==0 || value==0x10; // Retained RB_HIZCONTROL.
    return nativeInitializationRegisterValue(index,value);
}

inline bool planMaintenancePointsProfile(const Pm4Snapshot& root,const Pm4IndirectGraph& graph,
        MaintenancePointPlan& output,MaintenancePointError& error,bool allowExperimentalIdlePair,
        MaintenancePointProfile profile) {
    const auto fail=[&](MaintenancePointFailure why,std::uint32_t node=0,std::uint32_t at=0,std::uint32_t detail=0) {
        error={why,node,at,detail};return false;
    };
    if(!allowExperimentalIdlePair || root.words().empty() || graph.nodes.size()>64)
        return fail(MaintenancePointFailure::Limits);
    try {
        const bool runtime=profile==MaintenancePointProfile::RuntimeProgramUploads;
        const auto runtimeTail=runtime?inspectRuntimeMaintenanceEnvelope(root,graph):std::nullopt;
        if(runtime && !runtimeTail)return fail(MaintenancePointFailure::Incomplete);
        const unsigned wordLimit=runtime?32768:4096,shaderLimit=runtime?128:32,barrierLimit=runtime?128:64;
        std::map<std::pair<std::uint32_t,std::uint32_t>,std::uint32_t> children;
        std::uint64_t wordCount=root.words().size(),packetCount=root.packets().size();
        for(std::uint32_t i=0;i<graph.nodes.size();++i) {
            const auto& n=graph.nodes[i];
            if(n.parent>i || n.depth!=1+(n.parent?graph.nodes[n.parent-1].depth:0) || n.depth>8 ||
               n.snapshot.words().empty() || (n.physicalAddress&3) ||
               std::uint64_t{n.physicalAddress}+n.snapshot.words().size()*4>0x20000000 ||
               !children.emplace(std::make_pair(n.parent,n.parentOffsetWords),i+1).second)
                return fail(MaintenancePointFailure::Graph,i+1);
            wordCount+=n.snapshot.words().size();packetCount+=n.snapshot.packets().size();
        }
        if(wordCount>wordLimit || packetCount>4096 || wordCount!=graph.totalWords || packetCount!=graph.totalPackets)
            return fail(MaintenancePointFailure::Limits);
        MaintenancePointPlan plan;plan.runtimeTail=runtimeTail;
        std::array<NativeWordRequest,64> requests{};
        unsigned visited=0,fresh=0,leafPackets=0;
        std::array<bool,2> freshPrograms{};
        constexpr std::array<std::array<std::uint32_t,2>,7> required{{
            {{0x2180,0x1000000E}},{{0x2181,0}},{{0x2104,0}},{{0x2200,0}},
            {{0x2208,4}},{{0x2293,0}},{{0x2280,0x80008}}}};
        const auto walk=[&](auto&& self,const Pm4Snapshot& stream,std::uint32_t node)->bool {
            const auto& words=stream.words();const auto& packets=stream.packets();
            for(std::size_t k=0;k<packets.size();++k) {
                const auto& p=packets[k];const auto at=p.offsetWords;
                const auto* data=words.data()+at+1;
                if(p.predicated ||
                   (p.kind==Pm4Kind::Command && words[at]!=(0xC0000000u|((p.payloadWords-1)<<16)|(p.opcode<<8))) ||
                   (p.kind==Pm4Kind::RegisterSequence && words[at]!=(((p.payloadWords-1)<<16)|(p.repeatRegister?0x8000u:0)|p.firstRegister)) ||
                   (p.kind==Pm4Kind::RegisterPair && words[at]!=(0x40000000u|p.firstRegister|(p.secondRegister<<11))) ||
                   (p.kind==Pm4Kind::Filler && words[at]!=0x80000000u))
                    return fail(MaintenancePointFailure::Header,node,at);
                if(p.kind==Pm4Kind::Command && (p.opcode==0x3F || p.opcode==0x37)) {
                    const auto found=children.find({node,at});
                    if(p.payloadWords!=2 || found==children.end())return fail(MaintenancePointFailure::Graph,node,at);
                    const auto& child=graph.nodes[found->second-1];
                    if(child.physicalAddress!=data[0] || child.snapshot.words().size()!=data[1])
                        return fail(MaintenancePointFailure::Graph,node,at);
                    ++visited;if(!self(self,child.snapshot,found->second))return false;
                    continue;
                }
                if(!leafPackets && p.kind==Pm4Kind::RegisterSequence && words.size()-at>=6 && k+2<packets.size()) {
                    constexpr std::array<std::uint32_t,6> idle{0x5C8,0x20000,0xE00,1,0xE40,1};
                    if(std::equal(idle.begin(),idle.end(),words.begin()+at) && packets[k+1].offsetWords==at+2 && packets[k+2].offsetWords==at+4) {
                        plan.experimentalLeadingIdlePair=true;plan.barriers.push_back({0,false});
                        leafPackets+=3;k+=2;continue;
                    }
                }
                ++leafPackets;
                if(!runtime && !plan.experimentalLeadingIdlePair)return fail(MaintenancePointFailure::Incomplete,node,at);
                if(runtime && node==1 && at==13) {
                    // The validated idle/cache pair occurs AFTER all24 points.
                    // Keep this dependency in order; never prepend a fake wait.
                    if(plan.points.size()!=24 || k+2>=packets.size() ||
                       packets[k+1].offsetWords!=at+2 || packets[k+2].offsetWords!=at+4)
                        return fail(MaintenancePointFailure::Incomplete,node,at);
                    plan.barriers.push_back({24,false});
                    plan.registers[0x5C8]=0x20000;plan.registers[0xE00]=1;plan.registers[0xE40]=1;
                    plan.registerWrites+=3;leafPackets+=2;k+=2;continue;
                }
                if(!plan.events.empty() && !(p.kind==Pm4Kind::Command && (p.opcode==0x58 || p.opcode==0x10)) && p.kind!=Pm4Kind::Filler)
                    return fail(MaintenancePointFailure::EventOrdering,node,at);
                if(p.kind==Pm4Kind::RegisterSequence && p.firstRegister==0x5C8) {
                    // Exact WAIT_UNTIL=WAIT_3D_IDLE sites between point groups.
                    // The executor must issue an actual native dependency at
                    // this position; this is not an ignored register write.
                    if(p.repeatRegister || p.payloadWords!=1 || data[0]!=0x20000 || plan.barriers.size()==barrierLimit)
                        return fail(MaintenancePointFailure::Register,node,at,0x5C8);
                    if(runtime){plan.registers[0x5C8]=data[0];++plan.registerWrites;}
                    plan.barriers.push_back({static_cast<unsigned>(plan.points.size()),false});continue;
                }
                if(p.kind==Pm4Kind::RegisterSequence || p.kind==Pm4Kind::RegisterPair) {
                    for(unsigned i=0;i<p.payloadWords;++i) {
                        const auto index=p.kind==Pm4Kind::RegisterPair?(i?p.secondRegister:p.firstRegister):p.firstRegister+(p.repeatRegister?0:i);
                        const bool runtimeScissor=runtime && node==1 && at==0 && index==0x2082 && data[i]==0x01800500;
                        if(!runtimeScissor && !maintenancePointRegisterValue(index,data[i]))return fail(MaintenancePointFailure::Register,node,at,index);
                        plan.registers[index]=data[i];++plan.registerWrites;
                        for(unsigned f=0;f<required.size();++f)if(index==required[f][0])fresh|=1u<<f;
                    }
                    continue;
                }
                if(p.kind==Pm4Kind::Filler)continue;
                if(p.kind!=Pm4Kind::Command)return fail(MaintenancePointFailure::Opcode,node,at);
                switch(p.opcode) {
                case 0x10:break;
                case 0x2B: {
                    if(p.payloadWords<2 || data[0]>1 || !data[1] || data[1]%3 || data[1]>1024 || data[1]!=p.payloadWords-2 || plan.shaderLoads==shaderLimit)
                        return fail(MaintenancePointFailure::ShaderLayout,node,at);
                    auto& code=plan.finalPrograms[data[0]];code.resize(data[1]*4);
                    for(unsigned i=0;i<data[1];++i)for(unsigned b=0;b<4;++b)code[i*4+b]=data[i+2]>>(24-b*8);
                    freshPrograms[data[0]]=true;++plan.shaderLoads;break;
                }
                case 0x36: {
                    if(p.payloadWords!=1 || data[0]!=0x10081 || plan.points.size()==128)
                        return fail(MaintenancePointFailure::DrawLayout,node,at);
                    if(fresh!=127)return fail(MaintenancePointFailure::DrawState,node,at);
                    for(const auto& r:required)if(!plan.registers.count(r[0]) || plan.registers.at(r[0])!=r[1])
                        return fail(MaintenancePointFailure::DrawState,node,at,r[0]);
                    std::array<CopyShaderProof,2> shaders;CopyShaderError why;
                    for(unsigned stage=0;stage<2;++stage)if(!freshPrograms[stage] || !analyzeCopyShader(plan.finalPrograms[stage].data(),plan.finalPrograms[stage].size(),
                        static_cast<CopyShaderStage>(stage),shaders[stage],why))return fail(MaintenancePointFailure::ShaderProof,node,at,stage);
                    ConstantShaderPair pair;
                    if(!linkConstantShaderPair(shaders[0],shaders[1],0x1000000E,0,pair) || pair.position!=std::array<std::uint8_t,4>{0,0,0,1})
                        return fail(MaintenancePointFailure::ShaderProof,node,at);
                    plan.points.push_back({node,at,pair});break;
                }
                case 0x3B:
                    if(p.payloadWords!=1 || (data[0]!=0x100 && data[0]!=0x300 && data[0]!=0x7FFF) || plan.barriers.size()==barrierLimit)
                        return fail(MaintenancePointFailure::Opcode,node,at,p.opcode);
                    // Source INVALIDATE_STATE invalidates derived pointers,
                    // not raw loaded microcode. Drop proof freshness, preserve
                    // selected programs, and request a real native dependency.
                    ++plan.invalidations;freshPrograms={};if(data[0]==0x7FFF)fresh=0;
                    plan.barriers.push_back({static_cast<unsigned>(plan.points.size()),false,data[0]});break;
                case 0x46:
                    if(p.payloadWords!=1 || data[0]!=15 || plan.barriers.size()==barrierLimit)
                        return fail(MaintenancePointFailure::Event,node,at);
                    plan.barriers.push_back({static_cast<unsigned>(plan.points.size()),true});plan.registers[0x21F9]=15;break;
                case 0x56:
                    // Captured matrix-constant uploads to ALU slots64..207.
                    // Preserve every raw word, including NaN bit patterns. No
                    // scratch/trigger/register alias or generic selector allowed.
                    if(!runtime || node!=1 || at<19 || p.payloadWords<17 ||
                       p.payloadWords>577 || (p.payloadWords-1)%16 || data[0]!=0x4100)
                        return fail(MaintenancePointFailure::Register,node,at,p.opcode);
                    for(unsigned i=1;i<p.payloadWords;++i){plan.registers[0x4100+i-1]=data[i];++plan.registerWrites;}
                    break;
                case 0x58: {
                    if(p.payloadWords!=3 || data[0]!=3 || (data[1]&3)!=2 || (data[1]&~3u)>=0x20000000 || plan.events.size()==64 || plan.points.empty())
                        return fail(MaintenancePointFailure::Event,node,at);
                    const auto address=data[1]&~3u;
                    for(const auto& event:plan.events)if(event.address==address)return fail(MaintenancePointFailure::Event,node,at);
                    const auto slot=static_cast<unsigned>(plan.events.size());requests[slot]={slot*4,data[2],NativeWordPhase::AfterGraphics};
                    plan.events.push_back({node,at,address,data[2]});plan.registers[0x21F9]=3;break;
                }
                default:return fail(MaintenancePointFailure::Opcode,node,at,p.opcode);
                }
            }
            return true;
        };
        if(!walk(walk,root,0))return false;
        if(visited!=graph.nodes.size())return fail(MaintenancePointFailure::Graph);
        if((!runtime && !plan.experimentalLeadingIdlePair) || plan.points.empty() || plan.events.empty() ||
           !planNativeWordUpdates(requests.data(),plan.events.size(),static_cast<unsigned>(plan.events.size()*4),plan.updates))
            return fail(MaintenancePointFailure::Incomplete);
        if(runtime && (plan.points.size()!=24 || plan.events.size()!=2 ||
           plan.events[0].address!=runtimeTail->fencePhysical()+4 || plan.events[0].value!=runtimeTail->commandCursor() ||
           plan.events[1].address!=runtimeTail->fencePhysical() || plan.events[1].value!=runtimeTail->completionValue()))
            return fail(MaintenancePointFailure::Event);
        output=std::move(plan);error={};return true;
    } catch(const std::bad_alloc&) {return fail(MaintenancePointFailure::Allocation);}
}

inline bool planMaintenancePoints(const Pm4Snapshot& root,const Pm4IndirectGraph& graph,
        MaintenancePointPlan& output,MaintenancePointError& error,bool allowExperimentalIdlePair=false) {
    return planMaintenancePointsProfile(root,graph,output,error,allowExperimentalIdlePair,MaintenancePointProfile::LeadingIdle);
}
inline bool planRuntimeMaintenancePoints(const Pm4Snapshot& root,const Pm4IndirectGraph& graph,
        MaintenancePointPlan& output,MaintenancePointError& error) {
    return planMaintenancePointsProfile(root,graph,output,error,true,MaintenancePointProfile::RuntimeProgramUploads);
}
}
