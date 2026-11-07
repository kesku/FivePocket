// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AgentIngress.hpp"
#include <array>
#include <charconv>
#include <limits>
#include <string_view>

namespace recomp::apple::agentrequest {
namespace ai=agentinput;
namespace ao=agentobservation;
namespace ag=agentingress;

inline constexpr std::string_view Protocol="PORT_AGENT_REQUEST_V1";
// AgentInputPolicy::parseCommand admits at most 512 bytes, including its LF.
// Keep the entire existing command space; prefix decimal IDs use uint64_t.
inline constexpr std::size_t MaxCommandBytes=512;
inline constexpr std::size_t MaxNumberBytes=std::numeric_limits<std::uint64_t>::digits10+1;
inline constexpr std::size_t MaxBytes=Protocol.size()+5+ai::Nonce{}.size()+
    MaxNumberBytes+ao::Digest{}.size()*2+std::string_view("COMMAND").size()+MaxCommandBytes;

enum class Kind : std::uint8_t { Acquire,Command };
enum class Reason : std::uint8_t { None,Oversize,Envelope,Reference,Acquisition,Command,Ingress };
struct Request {
    Kind kind=Kind::Acquire;
    ao::Reference reference{};
    ai::Acquisition acquisition{};
    std::string_view command{};
};
struct Result {
    Reason reason=Reason::None;
    ag::Result ingress{};
    explicit operator bool() const noexcept {return reason==Reason::None;}
};

namespace detail {
inline bool number(std::string_view text,std::uint64_t& out) noexcept {
    if(text.empty() || text.front()<'1' || text.front()>'9')return false;
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),out);
    return error==std::errc{} && end==text.data()+text.size();
}
inline int hex(char value) noexcept {
    if(value>='0' && value<='9')return value-'0';
    if(value>='a' && value<='f')return value-'a'+10;
    if(value>='A' && value<='F')return value-'A'+10;
    return -1;
}
inline bool digest(std::string_view text,ao::Digest& out) noexcept {
    if(text.size()!=out.size()*2)return false;
    for(std::size_t i=0;i<out.size();++i) {
        const auto high=hex(text[2*i]),low=hex(text[2*i+1]);
        if(high<0 || low<0)return false;
        out[i]=std::uint8_t(high*16+low);
    }
    return true;
}
}

// Exact single-space outer prefix:
// PORT_AGENT_REQUEST_V1 run32 captureId sha256hex64 ACQUIRE nonce32 revision[LF]
// PORT_AGENT_REQUEST_V1 run32 captureId sha256hex64 COMMAND <PORT_AGENT_V1 bytes>
// IDs/revision are canonical nonzero decimal; SHA256 decodes to 32 raw bytes.
// Run/nonce spelling retains the existing case-sensitive nonce identity.
// No allocation, I/O, authorization, review attestation or source/ACK update.
// On failure out is empty. On success command borrows the original suffix:
// its backing storage must stay alive and immutable until dispatch completes.
inline Result parse(std::string_view text,Request& out) noexcept {
    out={};
    if(text.size()>MaxBytes)return {Reason::Oversize};
    std::array<std::string_view,5> fields{};
    for(auto& field:fields) {
        const auto end=text.find(' ');
        if(end==text.npos || !end)return {Reason::Envelope};
        field=text.substr(0,end);text.remove_prefix(end+1);
    }
    if(fields[0]!=Protocol)return {Reason::Envelope};
    Request next{};
    if(!ai::parseNonce(fields[1],next.reference.run) ||
       !detail::number(fields[2],next.reference.captureId) ||
       !detail::digest(fields[3],next.reference.sha256))return {Reason::Reference};
    if(fields[4]=="ACQUIRE") {
        if(!text.empty() && text.back()=='\n')text.remove_suffix(1);
        const auto separator=text.find(' ');
        if(separator==text.npos || !ai::parseNonce(text.substr(0,separator),next.acquisition.nonce) ||
           !detail::number(text.substr(separator+1),next.acquisition.authorizationRevision))return {Reason::Acquisition};
    } else if(fields[4]=="COMMAND") {
        ai::Command command{};
        if(!ai::parseCommand(text,command))return {Reason::Command};
        next.kind=Kind::Command;next.command=text;
    } else return {Reason::Envelope};
    out=next;return {};
}

// Call on the actual Dispatcher's owner thread. The complete envelope remains
// immutable throughout this synchronous call, including trusted app callbacks.
// Ingress alone obtains the current app environment/grant and consumes review.
template<class Dispatcher> Result submit(Dispatcher& dispatcher,std::string_view text) noexcept {
    Request request{};
    if(const auto result=parse(text,request);!result)return result;
    const auto result=request.kind==Kind::Acquire?
        dispatcher.submit(ag::AcquireRequest{request.reference,request.acquisition}):
        dispatcher.submit(ag::CommandRequest{request.reference,request.command});
    return result?Result{}:Result{Reason::Ingress,result};
}
}
