#pragma once
#include "native_offscreen_draw_plan.h"
#include <bitset>
#include <map>
#include <utility>

namespace recomp::gpu {
// Bounded single-sample RGBA8 colour writes -> D24FS8 raw-word alias view.
// Only full 80x16 physical tiles become known.
// The caller MUST invalidate every intervening writer before submission and
// provide only completed, fenced RGBA8 output here. A saved image or matching
// object address is not evidence of completion. This class does not supply it.
class NativeEdram32Bands {
    static constexpr unsigned TileWords=80*16,Tiles=2048;
    std::vector<std::uint32_t> words_;
    std::bitset<Tiles> known_;
    // Only incompletely known tiles need word masks. Full-tile writers retain
    // the compact original representation; reset does not scan 10 MiB.
    std::map<unsigned,std::array<std::uint64_t,TileWords/64>> partial_;
    std::uint64_t serial_=0;
public:
    void reset(){known_.reset();partial_.clear();serial_=0;}
    bool invalidate(unsigned base,unsigned tiles) {
        if(!tiles || base>=Tiles || tiles>Tiles-base){reset();return false;}
        for(unsigned i=base;i<base+tiles;++i){known_.reset(i);partial_.erase(i);}
        return true;
    }
    unsigned knownTiles()const{return known_.count();}
    std::uint64_t nextSerial()const{return serial_==UINT64_MAX?0:serial_+1;}
    void swap(NativeEdram32Bands& other)noexcept {
        words_.swap(other.words_);std::swap(known_,other.known_);partial_.swap(other.partial_);std::swap(serial_,other.serial_);
    }
    // Completed, tightly packed RAW 32-bit colour words, little-byte-first.
    // This is storage publication, NOT format conversion or a completion
    // receipt. Call only with fenced bytes from a separately validated writer.
    // Pixel columns [width,pitch) and rows outside the written rectangle are
    // untouched, not DONT_CARE. In particular a 360-row pass does not clear its
    // final tile's remaining eight rows. An unknown writer still invalidates
    // its whole possible footprint before any contents may be consumed.
    // No 64bpp addressing convention is selected here.
    bool recordCompletedRawColorRows(unsigned base,unsigned pitch,unsigned width,unsigned height,
            const std::vector<std::uint8_t>& bytes,std::uint64_t serial,unsigned firstRow=0) {
        const auto lastRow=std::uint64_t{firstRow}+height;
        if(!serial || serial<=serial_ || !width || !height || pitch<width || pitch%80 ||
           base>=Tiles || std::uint64_t{pitch/80}*((lastRow+15)/16)>Tiles-base ||
           bytes.size()!=std::uint64_t{width}*height*4)return false;
        if(words_.empty())words_.resize(TileWords*Tiles);
        for(unsigned row=0;row<height;++row)for(unsigned x=0;x<width;++x) {
            const unsigned y=firstRow+row,tile=base+(y/16)*(pitch/80)+x/80;
            const unsigned index=(y%16)*80+x%80;
            const auto at=4*(std::size_t{row}*width+x);
            words_[tile*TileWords+index]=std::uint32_t{bytes[at]}|
                (std::uint32_t{bytes[at+1]}<<8)|(std::uint32_t{bytes[at+2]}<<16)|(std::uint32_t{bytes[at+3]}<<24);
            if(!known_[tile])partial_[tile][index/64]|=std::uint64_t{1}<<(index%64);
        }
        for(auto i=partial_.begin();i!=partial_.end();) {
            if(std::all_of(i->second.begin(),i->second.end(),[](auto m){return m==UINT64_MAX;})) {
                known_.set(i->first);i=partial_.erase(i);
            } else ++i;
        }
        serial_=serial;return true;
    }
    bool recordCompletedColorBand(const NativeOffscreenDrawPlan& p,
            const std::vector<std::uint8_t>& rgba,std::uint64_t serial) {
        if(!serial || serial<=serial_ || p.format || !p.object || !p.width || !p.height ||
           p.width!=p.pitch || p.pitch%80 || p.height%16 || p.baseTile>=Tiles ||
           p.tileCount!=std::uint64_t{p.pitch/80}*(p.height/16) || p.tileCount>Tiles-p.baseTile ||
           rgba.size()!=std::uint64_t{p.width}*p.height*4)return false;
        if(words_.empty())words_.resize(TileWords*Tiles);
        for(unsigned y=0;y<p.height;++y)for(unsigned x=0;x<p.width;++x) {
            const unsigned tile=p.baseTile+(y/16)*(p.pitch/80)+x/80;
            const auto at=4*(std::size_t{y}*p.width+x);
            words_[tile*TileWords+(y%16)*80+x%80]=std::uint32_t{rgba[at]}|
                (std::uint32_t{rgba[at+1]}<<8)|(std::uint32_t{rgba[at+2]}<<16)|(std::uint32_t{rgba[at+3]}<<24);
        }
        for(unsigned i=p.baseTile;i<p.baseTile+p.tileCount;++i){known_.set(i);partial_.erase(i);}
        serial_=serial;return true;
    }
    // Completed stencil writes rejoin the same physical owner. No depth
    // encoding or clear is performed here; the GPU target's readback must
    // independently have proved that all high24 depth bits were preserved.
    bool recordCompletedDepthWords(unsigned base,unsigned pitch,unsigned width,unsigned height,
            const std::vector<std::uint32_t>& raw,std::uint64_t serial) {
        if(!serial || serial<=serial_ || !width || !height || pitch<width || pitch%80 ||
           width%80 || height%16 || base>=Tiles ||
           std::uint64_t{pitch/80}*(height/16)>Tiles-base ||
           raw.size()!=std::uint64_t{width}*height)return false;
        if(words_.empty())words_.resize(TileWords*Tiles);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const unsigned tile=base+(y/16)*(pitch/80)+x/80;
            words_[tile*TileWords+(y%16)*80+(x%80+40)%80]=raw[std::size_t{y}*width+x];
        }
        for(unsigned ty=0;ty<height/16;++ty)for(unsigned tx=0;tx<width/80;++tx)
            {const auto tile=base+ty*(pitch/80)+tx;known_.set(tile);partial_.erase(tile);}
        serial_=serial;return true;
    }
    // Returned words retain depth bits8..31 and stencil bits0..7. The depth
    // address uses the opposite 40-pixel half of each 80-pixel physical tile
    // (pinned Xenia/ReX edram.xesli). This is not depth float quantization.
private:
    std::optional<std::vector<std::uint32_t>> readWords(unsigned base,unsigned pitch,
            unsigned width,unsigned height,bool depth)const {
        if(!width || !height || pitch<width || pitch%80 || width%80 || height%16 ||
           base>=Tiles || std::uint64_t{pitch/80}*(height/16)>Tiles-base)return std::nullopt;
        for(unsigned ty=0;ty<height/16;++ty)for(unsigned tx=0;tx<width/80;++tx)
            if(!known_[base+ty*(pitch/80)+tx])return std::nullopt;
        std::vector<std::uint32_t> result(std::size_t{width}*height);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const unsigned tile=base+(y/16)*(pitch/80)+x/80;
            const unsigned halfX=depth?(x%80+40)%80:x%80;
            result[std::size_t{y}*width+x]=words_[tile*TileWords+(y%16)*80+halfX];
        }
        return result;
    }
public:
    std::optional<std::vector<std::uint32_t>> readDepthWords(unsigned base,unsigned pitch,
            unsigned width,unsigned height)const {return readWords(base,pitch,width,height,true);}
    // Raw 32-bit colour alias, with NO format conversion or new ownership.
    // Unlike depth, colour uses the unswapped 40-pixel halves. Unknown tiles
    // still refuse; this cannot resurrect an invalid native image.
    std::optional<std::vector<std::uint32_t>> readColorWords(unsigned base,unsigned pitch,
            unsigned width,unsigned height)const {return readWords(base,pitch,width,height,false);}
    // Exact colour rectangle, including partially known final tiles. Every
    // requested WORD must be known. Padding outside the requested rectangle is
    // neither read nor promoted to initialized, unlike rounding height to16.
    std::optional<std::vector<std::uint32_t>> readKnownColorRows(unsigned base,unsigned pitch,
            unsigned width,unsigned height)const {
        if(!width || !height || pitch<width || pitch%80 || base>=Tiles ||
           std::uint64_t{pitch/80}*((std::uint64_t{height}+15)/16)>Tiles-base || words_.empty())return std::nullopt;
        std::vector<std::uint32_t> result(std::size_t{width}*height);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const unsigned tile=base+(y/16)*(pitch/80)+x/80,index=(y%16)*80+x%80;
            if(!known_[tile]) {
                const auto partial=partial_.find(tile);
                if(partial==partial_.end() || !(partial->second[index/64]&(std::uint64_t{1}<<(index%64))))return std::nullopt;
            }
            result[std::size_t{y}*width+x]=words_[tile*TileWords+index];
        }
        return result;
    }
};
// Exclusive-gate transaction: live ownership becomes UNKNOWN before any GPU
// submission. Unhandled writers, early returns and exceptions leave it unknown.
// Only an explicitly classified successful operation may return its candidate
// (with written ranges invalidated or replaced by fenced readback). Never keep
// this across a scheduler yield or recursively enter it for the same owner.
class NativeEdram32Mutation {
    NativeEdram32Bands& live_;
    NativeEdram32Bands candidate_;
    bool committed_=false;
public:
    explicit NativeEdram32Mutation(NativeEdram32Bands& live):live_(live){candidate_.swap(live_);}
    NativeEdram32Mutation(const NativeEdram32Mutation&)=delete;
    NativeEdram32Mutation& operator=(const NativeEdram32Mutation&)=delete;
    NativeEdram32Bands& candidate(){return candidate_;}
    void commit()noexcept {if(!committed_){candidate_.swap(live_);committed_=true;}}
};
// An already completed full-rectangle RGBA8 or R32F writer. Those native
// formats have the same raw 32-bit colour words in this storage model. Do not
// pass normalized k_16_16 staging or an eight-byte RGBA16F target here.
// Unlike an unknown writer, this replaces only its actual pixel rectangle,
// preserving old padding provenance. The caller retains the transaction until
// EVERY attachment/readback has succeeded, and commits only then.
inline bool recordCompletedNativeRaw32Draw(NativeEdram32Mutation& mutation,
        const NativeOffscreenDrawPlan& plan,const std::vector<std::uint8_t>& bytes) {
    const auto count=nativeOffscreenTileCount(plan.pitch,plan.height,plan.format);
    if(!plan.object || (plan.format!=0 && plan.format!=14) || !count || plan.tileCount!=*count)
        return false;
    auto& candidate=mutation.candidate();
    return candidate.recordCompletedRawColorRows(plan.baseTile,plan.pitch,plan.width,plan.height,bytes,candidate.nextSerial());
}
// Explicit emulation model, NOT a measured Xbox 32/64bpp alias permutation.
// Xenia/XenDroid XeEdramOffsetBytes uses40x16 64bpp samples per5120-byte tile
// and documents that convention as unverified relative to32bpp hardware.
// A caller must opt in; the raw32 helper above never infers a wide format.
enum class NativeEdram64Model { Unspecified, Xenia40x16 };
inline bool recordCompletedNativeColorDraw(NativeEdram32Mutation& mutation,
        const NativeOffscreenDrawPlan& plan,const std::vector<std::uint8_t>& bytes,
        NativeEdram64Model model=NativeEdram64Model::Unspecified) {
    if(plan.format!=7)return recordCompletedNativeRaw32Draw(mutation,plan,bytes);
    const auto count=nativeOffscreenTileCount(plan.pitch,plan.height,plan.format);
    if(model!=NativeEdram64Model::Xenia40x16 || !plan.object || !count || plan.tileCount!=*count ||
       plan.width>UINT32_MAX/2 || plan.pitch>UINT32_MAX/2)return false;
    // Native RGBA16F readback already supplies two little-byte-first words per
    // pixel (RG,BA). Under this model a row of40 wide pixels is exactly a row
    // of80 raw words. Reuse partial-word provenance so padded rows/columns
    // remain untouched and unknown words are never silently initialized.
    auto& candidate=mutation.candidate();
    return candidate.recordCompletedRawColorRows(plan.baseTile,plan.pitch*2,plan.width*2,
        plan.height,bytes,candidate.nextSerial());
}
// The dispatcher invalidates its known display owner on entry. A proved
// no-output draw must return that already-valid owner as well as physical
// content. Supply the pointer ONLY for an owner valid on entry, never for an
// unknown/stale image. Failed proofs restore neither owner.
inline bool finishNativeReadOnlyDraw(bool proved,NativeEdram32Mutation& mutation,
        bool* previouslyValidDisplay=nullptr)noexcept {
    if(!proved)return false;
    mutation.commit();
    if(previouslyValidDisplay)*previouslyValidDisplay=true;
    return true;
}
template<class Submit,class Readback,class CompletedRows>
bool executeNativeEdram32ColorRows(NativeEdram32Mutation& mutation,const NativeOffscreenDrawPlan& plan,
        Submit&& submit,Readback&& readback,CompletedRows&& completedRows) {
    auto& candidate=mutation.candidate();
    if(!candidate.invalidate(plan.baseTile,plan.tileCount) || !submit())return false;
    const unsigned rows=completedRows();
    if(!rows || rows>plan.height || rows%16)return false;
    auto written=plan;written.height=rows;written.tileCount=(written.pitch/80)*(rows/16);
    const auto rgba=readback();
    // The reader returns ONLY the proved completed prefix. The rest of the
    // DONT_CARE attachment stays unknown even if readback happened to be zero.
    if(!rgba || !candidate.recordCompletedColorBand(written,*rgba,candidate.nextSerial()))return false;
    mutation.commit();return true;
}
template<class Submit,class Readback>
bool executeNativeEdram32ColorBand(NativeEdram32Mutation& mutation,const NativeOffscreenDrawPlan& plan,
        Submit&& submit,Readback&& readback) {
    return executeNativeEdram32ColorRows(mutation,plan,std::forward<Submit>(submit),
        std::forward<Readback>(readback),[&]{return plan.height;});
}
template<class Execute>
bool executeNativeEdram32Stencil(NativeEdram32Mutation& mutation,unsigned base,unsigned pitch,
        unsigned width,unsigned height,Execute&& execute) {
    auto& candidate=mutation.candidate();
    const auto before=candidate.readDepthWords(base,pitch,width,height);
    if(!before)return false;
    if(!candidate.invalidate(base,(pitch/80)*(height/16)))return false;
    const auto after=execute(*before);
    if(!after || after->size()!=before->size())return false;
    // This is exclusively a stencil writer, even if a caller supplies a
    // readback from an incorrectly configured native pipeline.
    for(std::size_t i=0;i<after->size();++i)
        if(((*after)[i]^(*before)[i])&0xFFFFFF00u)return false;
    if(!candidate.recordCompletedDepthWords(base,pitch,width,height,*after,candidate.nextSerial()))return false;
    mutation.commit();return true;
}
}
