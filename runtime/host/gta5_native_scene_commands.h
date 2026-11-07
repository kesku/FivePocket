#pragma once
#include "gta5_native_scene_resolves.h"
#include "native_tile_bin_command.h"
#include "native_tile_cache_barrier.h"
#include "native_tile_register_state.h"
#include "native_tile_register_writeback.h"
#include "native_tile_state_invalidation.h"
#include "native_inline_shader_load.h"
#include <variant>

namespace recomp::gpu {
// Ordered, typed connection of the ORIGINAL pending stream to native producers.
// A plan is NOT an execution receipt. Extent events explicitly remain deferred
// until current owned bounds storage and its original consumer are connected.
// No unknown packet becomes a successful no-op, including between two draws.
struct Gta5NativeSceneCommands {
    enum class Kind {Raw,Cache,Bin,Invalidate,Program,Query,Indexed,Resolve,
        RegisterWriteback,ZeroRaster,DeferredExtentReset,DeferredExtentWrite};
    struct Command {
        Kind kind=Kind::Raw;
        unsigned offsetBytes=0,endBytes=0,index=UINT32_MAX,value=0;
        bool predicated=false;
        std::variant<std::monostate,NativeTileRegisterState,NativeTileCacheBarrier,
            NativeTileBinCommand,NativeTileStateInvalidation,NativeInlineShaderLoad,
            NativeTileRegisterWriteback> operation;
    };
    std::vector<Command> commands;
    // Exact bytes validated by the original-query/draw matcher. Retirement
    // must bind execution to these bytes, not just operation counts.
    std::vector<std::uint8_t> sourceBytes;
    unsigned accountedPackets=0,deferredExtentPackets=0;
};
inline std::optional<Gta5NativeSceneCommands> prepareGta5NativeSceneCommands(
        const Gta5TileQueryCommands& inputs,const Gta5NativeSceneResolves& resolves,
        std::span<const std::uint8_t> bytes,unsigned* rejectedOffset=nullptr) {
    if(rejectedOffset)*rejectedOffset=0;
    const auto& stream=inputs.commands;
    const auto& packets=stream.packets();const auto& words=stream.words();
    if(bytes.empty() || bytes.size()>65536 || bytes.size()!=words.size()*4 ||
       !inputs.matchedDraws || resolves.resolves.empty() ||
       inputs.actions.size()!=packets.size())return std::nullopt;
    const auto word=[&](unsigned at) {
        return (unsigned(bytes[at])<<24)|(unsigned(bytes[at+1])<<16)|
            (unsigned(bytes[at+2])<<8)|bytes[at+3];
    };
    for(unsigned i=0;i<words.size();++i)if(word(i*4)!=words[i])return std::nullopt;
    Gta5NativeSceneCommands result;result.sourceBytes.assign(bytes.begin(),bytes.end());unsigned indexed=0,copies=0;
    for(unsigned i=0;i<packets.size();) {
        const auto& p=packets[i];const auto at=p.offsetWords*4;
        const auto end=(i+1<packets.size()?packets[i+1].offsetWords*4:bytes.size());
        if(rejectedOffset)*rejectedOffset=at;
        if(at>=end || end>bytes.size())return std::nullopt;
        const auto packet=bytes.subspan(at,end-at);const auto header=word(at);
        using Kind=Gta5NativeSceneCommands::Kind;
        Gta5NativeSceneCommands::Command c;c.offsetBytes=at;c.endBytes=end;c.predicated=p.predicated;
        const auto& query=inputs.actions[i];
        if(query.offsetWords!=p.offsetWords)return std::nullopt;
        // Aggregate the original cache/coherency producer including its WAIT,
        // not a stored BUSY value followed by an unimplemented wait packet.
        if(header==0xA31 || header==0x5C8 ||
           (header==0xC0004600 && p.payloadWords==1 && word(at+4)==6)) {
            unsigned length=header==0xA31?44:8;
            if(header==0x5C8 && bytes.size()-at>=24 && NativeTileCacheBarrier::parse(bytes.subspan(at,24)))length=24;
            if(length>bytes.size()-at)return std::nullopt;
            auto cache=NativeTileCacheBarrier::parse(bytes.subspan(at,length));if(!cache)return std::nullopt;
            c.kind=Kind::Cache;c.endBytes=at+length;c.operation=std::move(*cache);
        } else if(p.kind==Pm4Kind::RegisterSequence) {
            auto raw=NativeTileRegisterState::parse(packet);if(!raw)return std::nullopt;
            c.kind=Kind::Raw;c.operation=std::move(*raw);
        } else if(p.kind==Pm4Kind::Command) {
            if(auto bin=NativeTileBinCommand::parse(packet)) {c.kind=Kind::Bin;c.operation=*bin;}
            else if(auto invalidation=NativeTileStateInvalidation::parse(packet)) {c.kind=Kind::Invalidate;c.operation=*invalidation;}
            else if(auto program=NativeInlineShaderLoad::parse(packet)) {c.kind=Kind::Program;c.operation=std::move(*program);}
            else if(auto write=NativeTileRegisterWriteback::parse(packet)) {c.kind=Kind::RegisterWriteback;c.operation=*write;}
            else if(header==0xC0002300 && p.payloadWords==1 &&
                    (query.kind==Gta5TileQueryCommands::Kind::QueryBegin || query.kind==Gta5TileQueryCommands::Kind::QueryEnd) &&
                    query.payload==word(at+4)) {c.kind=Kind::Query;c.value=query.payload;}
            else if(header==0xC0032201 && p.payloadWords==4 && query.kind==Gta5TileQueryCommands::Kind::Indexed &&
                    query.drawIndex==indexed && indexed<inputs.matchedDraws) {
                c.kind=Kind::Indexed;c.index=indexed++;c.value=query.payload;
            }
            else if(header==0xC0012201 && p.payloadWords==2 && !word(at+4) && word(at+8)==0x84)c.kind=Kind::ZeroRaster;
            else if(header==0xC0003601 && p.payloadWords==1 && word(at+4)==0x30088 &&
                    copies<resolves.resolves.size() && resolves.resolves[copies].drawBytes==at) {
                c.kind=Kind::Resolve;c.index=copies++;
            } else if((header==0xC0004600 || header==0xC0004601) && p.payloadWords==1 && word(at+4)==0x19) {
                c.kind=Kind::DeferredExtentReset;++result.deferredExtentPackets;
            } else if(header==0xC0015A01 && p.payloadWords==2 && word(at+4)==0x1A &&
                    (word(at+8)&3)==1 && word(at+8)<0x20000000) {
                c.kind=Kind::DeferredExtentWrite;c.value=word(at+8);++result.deferredExtentPackets;
            } else return std::nullopt;
        } else return std::nullopt;
        // Every byte in an aggregate must end on an actual packet boundary.
        unsigned next=i;
        while(next<packets.size() && packets[next].offsetWords*4<c.endBytes)++next;
        if((next<packets.size()?packets[next].offsetWords*4:bytes.size())!=c.endBytes)return std::nullopt;
        result.accountedPackets+=next-i;i=next;result.commands.push_back(std::move(c));
    }
    if(indexed!=inputs.matchedDraws || copies!=resolves.resolves.size())return std::nullopt;
    if(rejectedOffset)*rejectedOffset=UINT32_MAX;
    return result;
}
// Rebind a later ORIGINAL worker submission, not a replay of the first saved
// buffer. Only the original bounds consumer's per-DMA bin masks may differ;
// every other command word and every native upload/query binding stays exact.
inline std::optional<Gta5NativeSceneCommands> prepareGta5NativeSceneReplayCommands(
        const Gta5TileQueryCommands& captured,const Gta5NativeSceneResolves& resolves,
        std::span<const std::uint8_t> submitted,unsigned tileCount,bool afterBounds) {
    const auto& packets=captured.commands.packets();const auto& words=captured.commands.words();
    if(!tileCount || tileCount>15 || submitted.size()!=words.size()*4 ||
       captured.actions.size()!=packets.size())return std::nullopt;
    std::vector<bool> maskWords(words.size(),false);
    std::vector<Gta5TileQueryDrawBinding> bindings;
    for(unsigned i=0;i<packets.size();++i) {
        const auto& p=packets[i];const auto& a=captured.actions[i];
        if(a.offsetWords!=p.offsetWords)return std::nullopt;
        const bool indexed=a.kind==Gta5TileQueryCommands::Kind::Indexed;
        const bool zero=p.kind==Pm4Kind::Command && p.opcode==0x22 && p.payloadWords==2;
        if(indexed || zero) {
            if(p.offsetWords<2 || words[p.offsetWords-2]!=0xC0006000 ||
               words[p.offsetWords-1]!=UINT32_MAX)return std::nullopt;
            maskWords[p.offsetWords-1]=true;
        }
        if(indexed) {
            DmaTriangleRequest dma;DmaTriangleFailure why;
            if(a.drawIndex!=bindings.size() || !inspectDmaTriangleLayout(captured.commands,i,dma,why))return std::nullopt;
            bindings.push_back({dma.physicalAddress,dma.indexCount,a.payload});
        }
    }
    if(bindings.size()!=captured.matchedDraws)return std::nullopt;
    // Original836BDF50 ORISes bit31 into the computed paired tile mask before
    // storing mask+8. It is mandatory even after the first pass, not unknown
    // padding and not permission to admit other high bits.
    const auto allowed=0x80000000u|((1u<<(tileCount*2))-1);
    for(unsigned i=0;i<words.size();++i) {
        const auto* b=submitted.data()+i*4;
        const unsigned actual=(unsigned(b[0])<<24)|(unsigned(b[1])<<16)|(unsigned(b[2])<<8)|b[3];
        if(actual!=words[i] && (!afterBounds || !maskWords[i] || !(actual&0x80000000u) || (actual&~allowed)))return std::nullopt;
    }
    Pm4Snapshot stream;Pm4Error error;Gta5TileQueryCommands matched;
    if(!decodePm4(submitted.data(),submitted.size(),{},stream,error) ||
       !matchGta5TileQueryCommands(stream,bindings,matched))return std::nullopt;
    return prepareGta5NativeSceneCommands(matched,resolves,submitted);
}
// Shared real engine consumer for STATE operations, in the caller's exact scene
// order. nullopt means this command still needs its geometry/copy/memory consumer,
// NOT success. A false value is a terminal rejected/failed state operation.
template<class Engine,class Target>
std::optional<bool> executeGta5NativeSceneStateCommand(
        const Gta5NativeSceneCommands::Command& c,Engine& engine,Target& target,unsigned* evicted=nullptr) {
    using Kind=Gta5NativeSceneCommands::Kind;
    switch(c.kind) {
    case Kind::Raw:return engine.executeLogicalRegisterState(std::get<NativeTileRegisterState>(c.operation));
    case Kind::Cache:return engine.executeLogicalCacheBarrier(std::get<NativeTileCacheBarrier>(c.operation),target,evicted);
    case Kind::Bin:return engine.executeLogicalBinCommand(std::get<NativeTileBinCommand>(c.operation));
    case Kind::Invalidate:return engine.executeLogicalStateInvalidation(std::get<NativeTileStateInvalidation>(c.operation),target,evicted);
    case Kind::Program:return engine.executeLogicalInlineShaderLoad(std::get<NativeInlineShaderLoad>(c.operation));
    case Kind::Query:return engine.executeLogicalVisibilityCommand(c.value);
    default:return std::nullopt;
    }
}
}
