#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace recomp::gpu {
// Borrowed CURRENT writable mappings. The caller owns allocation leases and
// excludes every guest writer through validation/publication. Expected and
// completed bytes are independent snapshots. This is not a GPU fence receipt,
// cache invalidation, ring retirement, or permission to resolve stale captures.
struct NativeSerializedResourceUpdate {
    std::uint32_t address=0;
    struct Destination {
        std::uint8_t* bytes=nullptr;std::size_t count=0;
        auto* data()const noexcept{return bytes;}
        auto size()const noexcept{return count;}
        bool empty()const noexcept{return !count;}
    } destination;
    const std::vector<std::uint8_t>* expected=nullptr;
    const std::vector<std::uint8_t>* completed=nullptr;
};
// Validate the WHOLE batch before its first store. Publish in supplied order:
// native texture data first, then command cursor, then resource completion ID.
// Call only after real native execution/fences and host visibility. Consumers
// must remain parked; failure atomicity here is not concurrent atomicity.
inline bool publishNativeSerializedResourceBatch(const NativeSerializedResourceUpdate* updates,std::size_t count) noexcept {
    constexpr std::size_t Limit=64,ResourceLimit=64u*1024*1024,TotalLimit=256u*1024*1024;
    if(!updates || !count || count>Limit)return false;
    const auto valid=[](const auto& bytes) {
        return bytes.data() && !bytes.empty() && reinterpret_cast<std::uintptr_t>(bytes.data())<=
            std::numeric_limits<std::uintptr_t>::max()-bytes.size();
    };
    const auto overlaps=[](const auto& a,const auto& b) {
        const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
        return x<y+b.size() && y<x+a.size();
    };
    struct Copy {std::uint8_t* target;const std::uint8_t* source;std::size_t size;};
    std::array<Copy,Limit> copies{};
    std::size_t total=0;
    for(std::size_t i=0;i<count;++i) {
        const auto& u=updates[i];const auto size=u.destination.size();
        if(!u.expected || !u.completed || !valid(u.destination) || !valid(*u.expected) || !valid(*u.completed) ||
           size!=u.expected->size() || size!=u.completed->size() || size>ResourceLimit ||
           (u.address&3) || std::uint64_t{u.address}+size>0x100000000ULL || total>TotalLimit-size)return false;
        total+=size;
        if(std::memcmp(u.destination.data(),u.expected->data(),size))return false;
        copies[i]={u.destination.data(),u.completed->data(),size};
    }
    for(std::size_t i=0;i<count;++i) {
        const auto& u=updates[i];
        for(std::size_t j=0;j<count;++j) {
            const auto& v=updates[j];
            // Sources must survive ALL earlier stores, not just their own.
            if(overlaps(u.destination,*v.expected) || overlaps(u.destination,*v.completed))return false;
            if(j<i && (overlaps(u.destination,v.destination) ||
                (u.address<std::uint64_t{v.address}+v.destination.size() &&
                 v.address<std::uint64_t{u.address}+u.destination.size())))return false;
        }
    }
    // No caller descriptors, allocations, callbacks or fallible checks after
    // the first write. Descriptor arrays may themselves live in a target span.
    for(std::size_t i=0;i<count;++i)std::memcpy(copies[i].target,copies[i].source,copies[i].size);
    return true;
}
template<std::size_t N>inline bool publishNativeSerializedResourceBatch(
        const std::array<NativeSerializedResourceUpdate,N>& updates) noexcept {
    return publishNativeSerializedResourceBatch(updates.data(),updates.size());
}
}
