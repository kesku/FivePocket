// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>
namespace recomp::apple {
struct MetalCaptureRequest {std::uint64_t expires=0;std::uint32_t frame=0;};
inline bool parseMetalCapture(std::string_view text,std::string_view nonce,MetalCaptureRequest& out) {
    if(text.size()>128 || nonce.size()!=32)return false;
    std::array<std::string_view,4> fields{};std::size_t count=0;
    while(!text.empty()) {
        const auto start=text.find_first_not_of(" \r\n\t");if(start==text.npos)break;text.remove_prefix(start);
        if(count==fields.size())return false;
        const auto end=text.find_first_of(" \r\n\t");fields[count++]=text.substr(0,end);
        if(end==text.npos)break;text.remove_prefix(end);
    }
    if(count!=fields.size() || fields[0]!="PORT_CAPTURE_V1" || fields[1]!=nonce)return false;
    auto number=[](std::string_view s,auto& n){auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),n);
        return e==std::errc{} && p==s.data()+s.size();};
    MetalCaptureRequest r;if(!number(fields[2],r.expires) || !number(fields[3],r.frame))return false;
    out=r;return true;
}
class MetalCapturePolicy {
    bool used_=false,active_=false,closed_=false;
    std::uint32_t startFrame_=0;
    std::uint64_t startMs_=0;
public:
    bool admit(const MetalCaptureRequest& r,std::uint64_t unixNow,std::uint32_t frame,
               std::uint64_t monotonicMs,bool foreground,bool inputNeutral,std::uint64_t available) noexcept {
        if(used_ || closed_ || !foreground || !inputNeutral || !frame || !r.frame || r.frame>frame || frame-r.frame>300 ||
           r.expires<unixNow || r.expires-unixNow>30 || available<(2ull<<30))return false;
        used_=true;active_=true;startFrame_=frame;startMs_=monotonicMs;return true;
    }
    bool stopDue(std::uint32_t frame,std::uint64_t monotonicMs,bool foreground,std::uint64_t available) const noexcept {
        if(!active_)return false;
        // Two accepted-frame boundaries cover at least one full intervening
        // frame when the producer is synchronous. Trace must still confirm it.
        return !foreground || frame<startFrame_ || frame-startFrame_>=2 || monotonicMs<startMs_ ||
#if !defined(APPLE_PORT_MUTANT_CAPTURE_DEADLINE)
               monotonicMs-startMs_>=2000 ||
#endif
               available<(1ull<<30);
    }
    bool active() const noexcept{return active_;}
    bool used() const noexcept{return used_ || closed_;}
    void close() noexcept{closed_=true;active_=false;}
};
}
