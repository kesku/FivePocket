#pragma once
#include "gta5_tile_recording_diagnostic.h"
#include "xenos_dma_triangle_draw.h"
#include <functional>
#include <span>

namespace recomp::gpu {
// CURRENT, independently validated mappings borrowed under the executor gate.
// Physical/virtual identity is supplied by the allocation owner, never granted
// by this parser. The caller retains that ownership and parks writers throughout
// prepare -> native scene execution -> publish -> original worker resume.
struct Gta5TileExtentRange {
    std::uint32_t address=0,physical=0;
    std::span<const std::uint8_t> bytes;
};
// Single sealed bounds block produced by the ordinary tiling path. This is a
// conservative XY culling policy, NOT measured coverage or occlusion. Original
// 836BDF50 reads only XY and patches mask+8. Enclosing every validated tile loses
// culling efficiency but cannot remove a tile's draw. Z words are retained;
// their hardware meaning and other consumers are not implemented here.
// Every record must bind an exact mask/DMA/first-pass-mask/EXT sequence before
// any publication. A successful plan does NOT execute the remaining PM4, retire
// the ring, or justify skipping the enclosed draws (including the zero draw's
// surrounding effects). Linked bounds blocks remain explicitly unsupported.
class Gta5TileExtentPublication final {
public:
    enum class Phase { Prepared, Complete, Failed };
    struct Binding {std::uint32_t drawOffsetWords,eventOffsetWords,maskAddress,destinationPhysical;};
    using Read=Gta5ResourceReader;
    using Fence=std::function<bool()>;
    using Publish=std::function<bool(std::uint32_t,const std::vector<std::uint8_t>&,const std::vector<std::uint8_t>&)>;
    Gta5TileExtentPublication(const Gta5TileExtentPublication&)=delete;
    Gta5TileExtentPublication& operator=(const Gta5TileExtentPublication&)=delete;
    Gta5TileExtentPublication(Gta5TileExtentPublication&&)=default;
    Gta5TileExtentPublication& operator=(Gta5TileExtentPublication&&)=default;
    static std::optional<Gta5TileExtentPublication> prepare(const Gta5TileExtentRange& commands,
            const Gta5TileExtentRange& table,std::span<const std::array<std::uint32_t,4>> tiles) noexcept {
        try {
            const auto valid=[](const auto& r,std::size_t limit) {
                return r.address>=0xA0000000 && r.address<0xC0000000 && !(r.address&3) && !(r.physical&3) &&
                    !r.bytes.empty() && !(r.bytes.size()%4) && r.bytes.size()<=limit &&
                    std::uint64_t{r.address}+r.bytes.size()<=0xC0000000 &&
                    std::uint64_t{r.physical}+r.bytes.size()<=0x20000000;
            };
            if(!valid(commands,4*1024*1024) || !valid(table,65536) || table.bytes.size()<24 ||
               (table.bytes.size()-8)%16 || tiles.empty() || tiles.size()>15)return std::nullopt;
            const auto overlap=[](std::uint32_t a,std::size_t an,std::uint32_t b,std::size_t bn) {
                return a<std::uint64_t{b}+bn && b<std::uint64_t{a}+an;
            };
            if(overlap(commands.address,commands.bytes.size(),table.address,table.bytes.size()) ||
               overlap(commands.physical,commands.bytes.size(),table.physical,table.bytes.size()))return std::nullopt;
            const auto word=[](auto bytes,std::size_t at) {
                return (std::uint32_t{bytes[at]}<<24)|(std::uint32_t{bytes[at+1]}<<16)|
                    (std::uint32_t{bytes[at+2]}<<8)|bytes[at+3];
            };
            const auto end=static_cast<std::uint32_t>(table.bytes.size()-4);
            if(word(table.bytes,0)!=Gta5TileRecordingDiagnostic::originalLink(table.address+end) ||
               word(table.bytes,end)!=0xC0000000)return std::nullopt;
            unsigned left=UINT32_MAX,top=UINT32_MAX,right=0,bottom=0;
            for(const auto& r:tiles) {
                if(r[0]>=r[2] || r[1]>=r[3] || r[2]>524288 || r[3]>524288)return std::nullopt;
                left=std::min(left,r[0]);top=std::min(top,r[1]);right=std::max(right,r[2]);bottom=std::max(bottom,r[3]);
            }
            const std::array<unsigned,4> extent{left/8,(right-1)/8,top/8,(bottom-1)/8};
            Pm4Snapshot stream;Pm4Error error;
            if(!decodePm4(commands.bytes.data(),commands.bytes.size(),{},stream,error))return std::nullopt;
            unsigned events=0;
            for(const auto& p:stream.packets())if(p.kind==Pm4Kind::Command) {
                switch(p.opcode) {
                case 0x5A:++events;break;
                case 0x22:case 0x23:case 0x2B:case 0x36:case 0x3B:case 0x3C:
                case 0x46:case 0x55:case 0x60:case 0x61:case 0x62:case 0x63:break;
                default:return std::nullopt; // No hidden command traversal.
                }
            }
            if(events!=(table.bytes.size()-8)/16)return std::nullopt;
            Gta5TileExtentPublication plan;
            plan.commandAddress_=commands.address;plan.tableAddress_=table.address;
            plan.commands_.assign(commands.bytes.begin(),commands.bytes.end());
            plan.expected_.assign(table.bytes.begin(),table.bytes.end());plan.completed_=plan.expected_;
            std::size_t cursor=0;
            const auto& packets=stream.packets();const auto& w=stream.words();
            for(unsigned at=4;at<end;at+=16) {
                const auto target=word(table.bytes,at);
                if((target&3) || target<commands.address || std::uint64_t{target}+12>
                   std::uint64_t{commands.address}+commands.bytes.size())return std::nullopt;
                const auto offset=(target-commands.address)/4+1;
                while(cursor<packets.size() && packets[cursor].offsetWords<offset)++cursor;
                if(cursor+4>packets.size() || packets[cursor].offsetWords!=offset)return std::nullopt;
                const auto& mask=packets[cursor];const auto& draw=packets[cursor+1];
                const auto& first=packets[cursor+2];const auto& event=packets[cursor+3];
                const auto exact=[&](const auto& p,unsigned header,unsigned value) {
                    return p.kind==Pm4Kind::Command && p.payloadWords==1 && w[p.offsetWords]==header && w[p.offsetWords+1]==value;
                };
                if(!exact(mask,0xC0006000,UINT32_MAX) || !exact(first,0xC0006000,0x80000000) ||
                   event.kind!=Pm4Kind::Command || event.payloadWords!=2 || w[event.offsetWords]!=0xC0015A01 ||
                   w[event.offsetWords+1]!=0x1A || w[event.offsetWords+2]!=((table.physical+at+4u)|1u))return std::nullopt;
                DmaTriangleRequest dma;DmaTriangleFailure why;
                if(!inspectDmaTriangleLayout(stream,cursor+1,dma,why)) {
                    if(draw.kind!=Pm4Kind::Command || draw.payloadWords!=2 ||
                       w[draw.offsetWords]!=0xC0012201 || w[draw.offsetWords+1] || w[draw.offsetWords+2]!=0x84)return std::nullopt;
                } else if(!draw.predicated)return std::nullopt;
                plan.bindings_.push_back({draw.offsetWords,event.offsetWords,target+8,table.physical+at+4});
                for(unsigned i=0;i<4;++i) {
                    plan.completed_[at+4+i*2]=static_cast<std::uint8_t>(extent[i]>>8);
                    plan.completed_[at+5+i*2]=static_cast<std::uint8_t>(extent[i]);
                }
                cursor+=4; // Reject duplicate/out-of-order record targets.
            }
            return plan;
        } catch(...) {return std::nullopt;}
    }
    const auto& bindings()const noexcept{return bindings_;}
    const auto& expected()const noexcept{return expected_;}
    const auto& completed()const noexcept{return completed_;}
    Phase phase()const noexcept{return phase_;}
    // Caller establishes all referenced first-pass draws have run, then this
    // same-queue fence must really complete. Validate CURRENT command/table
    // bytes before and after the fence. The publisher must itself revalidate
    // writable allocation ownership and compare the entire expected block
    // atomically before stores (GuestImportRegistry's existing batch API).
    // Only XY culling records publish: no VGT event/status or RPTR completion.
    bool publish(const Read& read,const Fence& fence,const Publish& write) noexcept {
        if(phase_!=Phase::Prepared)return false;
        phase_=Phase::Failed; // One attempt, including exceptions/reentry.
        try {
            if(!read || !fence || !write || commands_.empty() || expected_.empty() || bindings_.empty())return false;
            const auto unchanged=[&] {
                const auto commands=read(commandAddress_,static_cast<unsigned>(commands_.size()));
                const auto table=read(tableAddress_,static_cast<unsigned>(expected_.size()));
                return commands && table && *commands==commands_ && *table==expected_;
            };
            if(!unchanged() || !fence() || !unchanged() || !write(tableAddress_,expected_,completed_))return false;
            phase_=Phase::Complete;return true;
        } catch(...) {return false;}
    }
private:
    Gta5TileExtentPublication()=default;
    Phase phase_=Phase::Prepared;
    std::uint32_t commandAddress_=0,tableAddress_=0;
    std::vector<std::uint8_t> commands_,expected_,completed_;
    std::vector<Binding> bindings_;
};
}
