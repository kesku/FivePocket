#pragma once
#include "xenos_dma_triangle_draw.h"
#include <span>

namespace recomp::gpu {
struct Gta5TileQueryDrawBinding {
    std::uint32_t indexPhysical=0,indexCount=0,visibilityControl=0;
};
struct Gta5TileQueryCommands {
    enum class Kind { Unimplemented, Indexed, QueryBegin, QueryEnd };
    struct Action {
        Kind kind=Kind::Unimplemented;
        std::uint32_t offsetWords=0,drawIndex=UINT32_MAX,payload=0;
    };
    // Keep EVERY packet and its bytes, including unimplemented effects.
    // A consumer must not interpret successful matching as whole-stream work.
    Pm4Snapshot commands;
    std::vector<Action> actions;
    unsigned matchedDraws=0,queryCommands=0,unimplementedPackets=0;
};
// Connect original VIZ_QUERY packets to prepared native API draw identities.
// Native upload hooks consume shaders/constants/state outside this PM4 stream;
// replaying its register writes alone does NOT reconstruct the actual draws.
// Therefore each binding carries its authoritative draw-time visibility shadow.
// Count/address/order must match the original DMA packet before that shadow can
// select a query. No memory reads, bin admission, GPU execution or retirement.
// All other packets remain explicitly unimplemented; control-flow/unknown
// commands reject because they could hide a query/draw or alter execution order.
inline bool matchGta5TileQueryCommands(const Pm4Snapshot& input,
        std::span<const Gta5TileQueryDrawBinding> draws,Gta5TileQueryCommands& output) noexcept {
    if(input.words().empty() || input.words().size()>1024*1024 || input.packets().size()>65536 ||
       draws.empty() || draws.size()>128)return false;
    try {
        Gta5TileQueryCommands result;result.commands=input;result.actions.reserve(input.packets().size());
        std::uint64_t active=0;
        for(std::size_t i=0;i<input.packets().size();++i) {
            const auto& p=input.packets()[i];const auto* v=input.words().data()+p.offsetWords+1;
            Gta5TileQueryCommands::Action action;action.offsetWords=p.offsetWords;
            if(p.kind==Pm4Kind::Command) {
                if(p.opcode==0x23) {
                    if(p.predicated || p.payloadWords!=1 || input.words()[p.offsetWords]!=0xC0002300u ||
                       (v[0]&~0x13Fu))return false;
                    const auto bit=std::uint64_t{1}<<(v[0]&63);const bool end=v[0]&0x100;
                    if(bool(active&bit)!=end)return false;
                    if(end)active&=~bit;else active|=bit;
                    action.kind=end?Gta5TileQueryCommands::Kind::QueryEnd:Gta5TileQueryCommands::Kind::QueryBegin;
                    action.payload=v[0];++result.queryCommands;
                } else if(p.opcode==0x22 && p.payloadWords==4) {
                    DmaTriangleRequest layout;DmaTriangleFailure error;
                    if(result.matchedDraws==draws.size() || !inspectDmaTriangleLayout(input,i,layout,error))return false;
                    const auto& draw=draws[result.matchedDraws];const auto control=draw.visibilityControl;
                    if(layout.physicalAddress!=draw.indexPhysical || layout.indexCount!=draw.indexCount ||
                       (control&~0x7Fu) || ((control&1) && !(active&(std::uint64_t{1}<<((control>>1)&63)))))return false;
                    action.kind=Gta5TileQueryCommands::Kind::Indexed;
                    action.drawIndex=result.matchedDraws++;action.payload=control;
                } else {
                    switch(p.opcode) {
                    case 0x22:
                        // The zero-index packet still has surrounding effects;
                        // retain it, and never silently omit queried geometry.
                        if(active || p.payloadWords!=2 || v[0] || v[1]!=0x84)return false;
                        break;
                    case 0x36:
                        if(active)return false; // No query binding for resolves.
                        break;
                    case 0x2B: case 0x3B: case 0x3C: case 0x46: case 0x55:
                    case 0x5A: case 0x60: case 0x61: case 0x62: case 0x63:
                        break; // Retained, NOT interpreted or acknowledged.
                    default:return false;
                    }
                }
            }
            if(action.kind==Gta5TileQueryCommands::Kind::Unimplemented)++result.unimplementedPackets;
            result.actions.push_back(action);
        }
        if(active || result.matchedDraws!=draws.size())return false;
        output=std::move(result);return true;
    } catch(...) {return false;}
}
}
