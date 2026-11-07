#pragma once
#include "xenos_texture_2d.h"
#include <deque>
#include <memory>

namespace recomp::gpu {
// CPU conversion reuse only. This does not assert guest/GPU ownership,
// consumption, residency, completion or visibility. Every lookup requires a
// fresh owned source vector and a valid encoded layout, and compares ALL six
// fetch words and ALL source bytes. No address-only or hash-only hits.
// Per draw-target, externally serialized like that target's other CPU state.
class NativeTextureDecodeCache {
    using Pixels=std::vector<std::uint8_t>;
    struct Entry {
        std::array<std::uint32_t,6> fetch;
        Pixels source;
        std::shared_ptr<const Pixels> pixels;
        std::size_t bytes()const noexcept{return source.size()+pixels->size();}
    };
    std::deque<Entry> entries_;
    std::size_t retainedBytes_=0;
    const std::size_t budget_,limit_;
public:
    explicit NativeTextureDecodeCache(std::size_t budget=32u*1024*1024,std::size_t entries=8):
        budget_(std::min(budget,std::size_t{32u*1024*1024})),limit_(std::min(entries,std::size_t{8})){}
    std::size_t retainedBytes()const noexcept{return retainedBytes_;}
    std::size_t entryCount()const noexcept{return entries_.size();}
    std::shared_ptr<const Pixels> linearize(const XenosTextureDescription& description,const Pixels& source) {
        const auto plan=planXenosTexture2D(description);
        if(!plan || source.size()<plan->sourceBytes)return nullptr;
        for(const auto& entry:entries_)
            if(entry.fetch==description.fetch && entry.source==source)return entry.pixels;
        auto decoded=linearizeXenosTexture2D(description,source);
        if(!decoded)return nullptr;
        std::shared_ptr<const Pixels> pixels;
        try {pixels=std::make_shared<const Pixels>(std::move(*decoded));}
        catch(const std::bad_alloc&){return nullptr;}
        // Oversized inputs still convert normally; never broaden the planner
        // just to populate a cache. Account for source AND decoded payloads.
        if(!limit_ || source.size()>budget_ || pixels->size()>budget_-source.size())return pixels;
        const auto bytes=source.size()+pixels->size();
        try {
            Entry entry{description.fetch,source,pixels};
            while(!entries_.empty() && (entries_.size()>=limit_ || bytes>budget_-retainedBytes_)) {
                retainedBytes_-=entries_.front().bytes();entries_.pop_front();
            }
            entries_.push_back(std::move(entry));retainedBytes_+=bytes;
        } catch(const std::bad_alloc&) {
            // Cache retention is optional; the freshly decoded output remains
            // valid even if retaining it failed. Never return a stale entry.
        }
        return pixels;
    }
};
}
