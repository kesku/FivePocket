// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>
namespace recomp::apple {
struct RecordedInputCommand {
    std::uint32_t sequence=0,frame=0,buttons=0,holdMs=0;
    std::uint64_t expires=0;
    std::array<int,6> axes{}; // LX,LY,RX,RY [-1000,1000]; LT,RT [0,1000].
};
// ASCII, exactly13 fields. No arbitrary paths, scripts, save commands or repeats.
inline bool parseRecordedInput(std::string_view text,std::string_view session,RecordedInputCommand& out) {
    if(text.size()>512 || session.size()!=32)return false;
    std::array<std::string_view,13> fields{};std::size_t count=0;
    while(!text.empty()) {
        const auto start=text.find_first_not_of(" \r\n\t");if(start==text.npos)break;text.remove_prefix(start);
        if(count==fields.size())return false;
        const auto end=text.find_first_of(" \r\n\t");fields[count++]=text.substr(0,end);
        if(end==text.npos)break;text.remove_prefix(end);
    }
    if(count!=fields.size() || fields[0]!="PORT_INPUT_V1" || fields[1]!=session)return false;
    auto number=[](std::string_view value,auto& dest) {auto [p,e]=std::from_chars(value.data(),value.data()+value.size(),dest);
        return e==std::errc{} && p==value.data()+value.size();};
    RecordedInputCommand candidate;
    if(!number(fields[2],candidate.sequence) || !number(fields[3],candidate.expires) ||
       !number(fields[4],candidate.frame) || !number(fields[5],candidate.buttons) || !number(fields[12],candidate.holdMs))return false;
    for(unsigned n=0;n<6;++n)if(!number(fields[6+n],candidate.axes[n]))return false;
    // Only face/shoulder buttons and the four explicitly synthetic D-pad bits.
    // Opposing directions are not a meaningful physical-pad command.
    if(!candidate.sequence || candidate.sequence>1024 || (candidate.buttons&~0xf30fu) ||
       (candidate.buttons&3u)==3u || (candidate.buttons&12u)==12u ||
       candidate.holdMs<50 || candidate.holdMs>2000)return false;
    for(unsigned n=0;n<6;++n)if(candidate.axes[n]>(1000) || candidate.axes[n]<(n<4?-1000:0))return false;
    out=candidate;return true;
}
class RecordedInputPolicy {
    std::uint32_t next_=1;bool active_=false,closed_=false;
public:
    std::uint32_t next() const noexcept{return next_;}
    bool admit(const RecordedInputCommand& c,std::uint64_t now,std::uint32_t frame,bool foreground,bool connected) noexcept {
        if(closed_ || active_ || !foreground || !connected ||
#if !defined(APPLE_PORT_MUTANT_INPUT_REPLAY)
           c.sequence!=next_ ||
#endif
           !frame ||
           c.frame>frame || frame-c.frame>300 || c.expires<now || c.expires-now>30)return false;
        ++next_;active_=true;return true;
    }
    void release() noexcept{active_=false;}
    void close() noexcept{closed_=true;active_=false;}
};
}
