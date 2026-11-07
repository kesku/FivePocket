#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>

namespace recomp::gpu {
// Boolean VIZ_QUERY lifecycle, NOT EVENT_WRITE_ZPD sample counts or a complete
// DRAW_INDX decoder. Original begin/end keep scan-converter state separate from
// readable status. Each draw callback must identify its measurement stage;
// unknown/error results cannot become hidden or visible. Queue/guest ownership
// and packet/bin predicates are the caller's separate obligations.
class NativeVisibilityQueries final {
public:
    using Journal=std::map<std::uint32_t,std::pair<std::uint32_t,std::uint32_t>>;
    enum class Stage { FinalSamples, PotentialVisibility };
    struct Measurement { bool visible; Stage stage; };
    using Draw=std::function<std::optional<Measurement>()>;
    bool command(std::uint32_t payload,Journal& journal) noexcept {
        if(failed_ || busy_ || (payload&~0x13Fu))return fail();
        const unsigned id=payload&63;const auto bit=std::uint64_t{1}<<id;
        const bool end=payload&0x100;
        if(bool(active_&bit)!=end)return fail();
        try {
            // Stage allocations before any lifecycle/status mutation. End
            // replaces only this query's bit; unrelated unknown bits stay
            // unknown, rather than inventing a zeroed status bank.
            auto next=journal;
            next[0x21F9]={end?8u:7u,0xFFFFFFFFu};
            if(end) {
                auto& status=next[0xC44+(id>>5)];const auto mask=std::uint32_t{1}<<(id&31);
                status.first=(status.first&~mask)|((visible_&bit)?mask:0u);
                status.second|=mask;
            }
            journal.swap(next);
            knownConditions_|=bit;
            if(end) {
                active_&=~bit;
                keep_=(keep_&~bit)|(visible_&bit);
            } else {
                active_|=bit;visible_&=~bit;keep_|=bit;
            }
            return true;
        } catch(...) {return fail();}
    }
    bool draw(std::uint32_t control,const Draw& execute) noexcept {
        // kill_pix_post_hi_z/detail_mask require distinct raster semantics.
        // The measured game profile has neither. Do not render through them.
        if(failed_ || busy_ || !execute || (control&~0x7Fu))return fail();
        const auto bit=std::uint64_t{1}<<((control>>1)&63);
        if((control&1) && !(active_&bit))return fail();
        busy_=true;
        try {
            const auto measured=execute();busy_=false;
            if(failed_ || !measured)return fail();
            // A final Vulkan sample count is not the earlier Xenos potential
            // visibility test. In particular zero after pixel KILL/stencil
            // cannot justify discarding future geometry. Do not publish it.
            if((control&1) && measured->stage!=Stage::PotentialVisibility)return fail();
            if((control&1) && measured->visible)visible_|=bit;
            return true;
        } catch(...) {busy_=false;return fail();}
    }
    // CP DISCARD is independent of the readable SC status: begin clears it
    // (KEEP) while the old readable result is retained. Synchronous callbacks
    // finish before end; no asynchronous END_RCVD window is admitted here.
    // Unknown/unseen conditions remain unsupported. No DMA admission here.
    std::optional<bool> condition(std::uint32_t token) const noexcept {
        if(failed_ || busy_)return std::nullopt;
        if(!token)return true;
        if((token&~0x13Fu) || !(token&0x100))return std::nullopt;
        const auto bit=std::uint64_t{1}<<(token&63);
        if(!(knownConditions_&bit))return std::nullopt;
        return (keep_&bit)!=0;
    }
    bool idle() const noexcept {return !failed_ && !busy_ && !active_;}
    bool failed() const noexcept {return failed_;}
private:
    bool fail() noexcept {failed_=true;return false;}
    std::uint64_t active_=0,visible_=0,keep_=0,knownConditions_=0;
    bool busy_=false,failed_=false;
};
}
