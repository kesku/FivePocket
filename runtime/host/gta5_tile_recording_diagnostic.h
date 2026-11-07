#pragma once
#include "gta5_driver_scissor.h"
#include "gta5_logical_tile_clear.h"

namespace recomp::gpu {
// Capture-only first-draw frontier. NEVER grants GPU execution, guest pixel
// publication, a ring receipt, a wait completion or permission to present.
// The caller must stop at those boundaries while pending() is true. No method
// completes this recording: reset is allowed only after guest workers retire.
class Gta5TileRecordingDiagnostic {
public:
    enum class Kind { Scissor, Clear, Indexed, Resolve };
    struct Event {Kind kind;unsigned index;};
private:
    std::optional<Gta5LogicalTileClear> identity_;
    Gta5ClearCapture seed_;
    std::array<std::vector<std::uint8_t>,7> initialRecords_;
    std::vector<Gta5ClearCapture> clears_;
    std::vector<std::array<std::uint32_t,4>> scissors_;
    std::vector<Event> events_;
    unsigned indexedCount_=0,lastIndexedSequence_=0;
    unsigned resolveCount_=0;
    static unsigned word(const std::vector<std::uint8_t>& b,unsigned at) {
        return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
    }
    bool sameSurfaces(const Gta5ClearCapture& capture)const {
        if(!identity_ || capture.call.device!=seed_.call.device)return false;
        const auto p=describeGta5LogicalTileSurfaces(capture);
        if(!p || p->layout.width!=identity_->layout.width || p->layout.height!=identity_->layout.height ||
           p->layout.beginFlags!=identity_->layout.beginFlags || p->layout.targets!=identity_->layout.targets ||
           p->layout.tiles.size()!=identity_->layout.tiles.size())return false;
        for(unsigned i=0;i<p->layout.tiles.size();++i)if(p->layout.tiles[i].logical!=identity_->layout.tiles[i].logical ||
            p->layout.tiles[i].origin!=identity_->layout.tiles[i].origin)return false;
        for(unsigned i=0;i<5;++i)if(capture.surfaces[i].object!=seed_.surfaces[i].object ||
            capture.surfaces[i].tileCache!=seed_.surfaces[i].tileCache || capture.surfaces[i].header!=seed_.surfaces[i].header)return false;
        return true;
    }
public:
    // Host capture budget, not an original command-list or GPU completion limit.
    static constexpr unsigned MaxClears=16;
    static constexpr unsigned MaxIndexed=128;
    // Original829322B8 records four resolves;82312788 records up to six more.
    // This bounds retained diagnostics, not GPU or worker completion.
    static constexpr unsigned MaxResolves=10;
    static constexpr std::array<unsigned,7> DescriptorOffsets{13760,13616,13736,13640,13664,13688,13712};
    // Exact pointer transformation used by original82431888, NOT a registry
    // ownership/address alias policy. Used only to compare a stored link word.
    static unsigned originalLink(unsigned address) {
        return (address&0x1FFFFFFFu)+(((address>>20)+512u)&0x1000u)-0x40000000u;
    }
    bool pending()const{return identity_.has_value();}
    unsigned device()const{return pending()?seed_.call.device:0;}
    const auto& initialRecords()const{return initialRecords_;}
    const auto& seed()const{return seed_;}
    const auto& clears()const{return clears_;}
    const auto& scissors()const{return scissors_;}
    const auto& events()const{return events_;}
    unsigned indexedCount()const{return indexedCount_;}
    bool begin(const Gta5ClearCapture& capture,const std::array<std::uint32_t,4>& rectangle,
            const Gta5ResourceReader& ownedRecords) {
        if(pending() || !ownedRecords || !capture.call.device || !capture.state || capture.state->size()<13860)return false;
        auto zero=capture;zero.call.flags=0;
        const auto p=planGta5LogicalTileClear(zero);
        const auto s=planGta5DriverScissor(*capture.state,rectangle);
        if(!p || !s || !s->binned || s->auxiliaryRectangle || p->layout.beginFlags!=1 ||
           p->layout.tiles.size()!=2 || rectangle!=std::array<unsigned,4>{0,0,p->layout.width,p->layout.height})return false;
        std::array<std::vector<std::uint8_t>,7> records;
        // Validate all seven original single-block seeds before publishing any
        // capture state. Original header slack is uninitialized: do not test it.
        for(unsigned i=0;i<7;++i) {
            const auto at=DescriptorOffsets[i],head=word(*capture.state,at),used=i?4u:260u;
            if(!head || head%128 || head>UINT32_MAX-4224 || word(*capture.state,at+4)!=head ||
               word(*capture.state,at+8)!=4224 || word(*capture.state,at+12)!=head+used ||
               word(*capture.state,at+16)!=head+4220 || word(*capture.state,at+20)!=capture.call.device)return false;
            const auto bytes=ownedRecords(head,used);
            if(!bytes || bytes->size()!=used || word(*bytes,0)!=originalLink(head+4))return false;
            records[i]=*bytes;
        }
        for(unsigned at=13784;at<13808;at+=4)if(word(*capture.state,at))return false;
        const auto& header=records[0];
        if(word(header,4)!=0x80000000 || word(header,8)!=p->layout.beginFlags ||
           word(header,12)!=p->layout.tiles.size() || word(header,256)!=0x86000000)return false;
        for(unsigned i=0;i<p->layout.tiles.size();++i)for(unsigned c=0;c<4;++c)
            if(word(header,16+i*16+c*4)!=p->layout.tiles[i].logical[c]-p->layout.tiles[0].origin[c%2])return false;
        seed_=capture;identity_=p;initialRecords_=std::move(records);scissors_.push_back(rectangle);
        events_.push_back({Kind::Scissor,0});return true;
    }
    bool recordScissor(const Gta5ClearCapture& current,const std::array<std::uint32_t,4>& rectangle) {
        if(scissors_.size()>=8 || !sameSurfaces(current))return false;
        const auto p=planGta5DriverScissor(*current.state,rectangle);
        if(!p || !p->binned || p->auxiliaryRectangle || rectangle[0]>rectangle[2] || rectangle[1]>rectangle[3] ||
           rectangle[2]>identity_->layout.width || rectangle[3]>identity_->layout.height)return false;
        events_.push_back({Kind::Scissor,static_cast<unsigned>(scissors_.size())});
        scissors_.push_back(rectangle);return true;
    }
    bool recordClear(const Gta5ClearCapture& capture) {
        if(clears_.size()>=MaxClears || !sameSurfaces(capture))return false;
        const auto p=planGta5LogicalTileClear(capture);
        if(!p || p->aspects!=capture.call.flags || p->empty())return false;
        // Preserve initial-clear admission. Later original82475F10(0) resets
        // bin selection, and queued822F6500 can clear both bins. Retain only
        // fully planned, same-surface effects in call order; this still grants
        // no execution/worker replay. Draw/submit/wait/present must still stop.
        if(clears_.empty() && (p->tiles!=1 || (capture.call.flags!=0x30 && capture.call.flags!=0x31)))return false;
        events_.push_back({Kind::Clear,static_cast<unsigned>(clears_.size())});
        clears_.push_back(capture);return true; // Recorded, never executed/completed.
    }
    // Index refers to an owned prepared snapshot, not a draw completion. Caller
    // retains shaders/constants/geometry and must validate original CPU-write
    // windows; original emission still runs and all submit/wait guards remain.
    bool recordIndexed(const Gta5ClearCapture& current,unsigned sequence) {
        if(indexedCount_>=MaxIndexed || !sequence || sequence<=lastIndexedSequence_ ||
           clears_.empty() || !sameSurfaces(current) || (*current.state)[11068]!=0x21)return false;
        events_.push_back({Kind::Indexed,sequence});lastIndexedSequence_=sequence;++indexedCount_;return true;
    }
    // Caller separately admits original CPU writes and retains a complete
    // before/after capture. Index is capture order, never a GPU completion.
    bool recordResolve(const Gta5ClearCapture& current,unsigned index) {
        if(resolveCount_>=MaxResolves || index!=resolveCount_ || !indexedCount_ || !sameSurfaces(current))return false;
        events_.push_back({Kind::Resolve,index});++resolveCount_;return true;
    }
};
}
