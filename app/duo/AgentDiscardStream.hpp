// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Explicit protocol v3; v1/v2 sources and wire behavior remain unchanged.
#include "AgentStream.hpp"
#include <algorithm>
#include <array>
#include <memory>
#include <span>

namespace recomp::apple::agentdiscardstream {
namespace ao=agentobservation;
namespace ai=agentinput;
namespace ag=agentingress;
namespace ar=agentrequest;
enum class Type : std::uint16_t { Hello=1,Capture=2,Observation=3,ReviewedRequest=4,Result=5,Ack=6,Cancel=7,BindReview=8,Discard=9,Discarded=10 };
enum class Failure : std::uint8_t { None,Cancelled,Socket,Protocol,Deadline,Disconnected };
inline constexpr std::size_t HeaderBytes=32,MetadataBytes=200,MaxReviewBytes=74+ar::MaxBytes;
inline constexpr std::array<std::uint8_t,8> Magic{'P','A','G','E','N','T',0,0};
inline void put(std::uint8_t* out,std::uint64_t value,unsigned bytes) noexcept {
    for(unsigned i=0;i<bytes;++i){out[bytes-i-1]=std::uint8_t(value);value>>=8;}
}
inline std::uint64_t get(const std::uint8_t* in,unsigned bytes) noexcept {
    std::uint64_t value=0;for(unsigned i=0;i<bytes;++i)value=(value<<8)|in[i];return value;
}
struct Header {Type type{};std::uint32_t bytes=0;std::uint64_t sequence=0,correlation=0;};
inline std::array<std::uint8_t,HeaderBytes> encode(Header value) noexcept {
    std::array<std::uint8_t,HeaderBytes> out{};std::copy(Magic.begin(),Magic.end(),out.begin());
    put(out.data()+8,3,2);put(out.data()+10,unsigned(value.type),2);put(out.data()+12,value.bytes,4);
    put(out.data()+16,value.sequence,8);put(out.data()+24,value.correlation,8);return out;
}
// Client frames only. ACK is deliberately unsupported until real consumer
// tokens have a source-bound correlation; socket writes are never input ACKs.
inline bool decodeClient(std::span<const std::uint8_t> bytes,std::uint64_t expected,Header& out) noexcept {
    out={};if(bytes.size()!=HeaderBytes || !expected || expected==UINT64_MAX ||
        !std::equal(Magic.begin(),Magic.end(),bytes.begin()) || get(bytes.data()+8,2)!=3)return false;
    Header next{Type(get(bytes.data()+10,2)),std::uint32_t(get(bytes.data()+12,4)),
                get(bytes.data()+16,8),get(bytes.data()+24,8)};
    if(next.sequence!=expected || next.correlation!=expected)return false;
    if((next.type==Type::BindReview && next.bytes==64) ||
       (next.type==Type::Capture && next.bytes==32) ||
       (next.type==Type::Discard && next.bytes==72) ||
       (next.type==Type::ReviewedRequest && next.bytes>=74 && next.bytes<=MaxReviewBytes) ||
       (next.type==Type::Cancel && !next.bytes)){out=next;return true;}
    return false;
}
struct Request {
    Header header{};
    std::array<std::uint8_t,MaxReviewBytes> bytes{};
    ao::Reference reference{};
    std::string_view wire() const noexcept {
        return header.type==Type::ReviewedRequest?std::string_view(reinterpret_cast<const char*>(bytes.data()+74),header.bytes-74):std::string_view{};
    }
};
inline bool validate(Request& request,const ao::RunId& run) noexcept {
    if(request.header.type==Type::Cancel)return !request.header.bytes;
    if(request.header.bytes<32 || !std::equal(run.begin(),run.end(),request.bytes.begin()))return false;
    if(request.header.type==Type::BindReview)return request.header.bytes==64;
    if(request.header.type==Type::Capture)return request.header.bytes==32;
    if(request.header.type==Type::Discard) {
        if(request.header.bytes!=72)return false;
        std::copy_n(reinterpret_cast<const char*>(request.bytes.data()),32,request.reference.run.begin());
        request.reference.captureId=get(request.bytes.data()+32,8);
        std::copy_n(request.bytes.data()+40,32,request.reference.sha256.begin());
        return request.reference.captureId!=0;
    }
    if(request.header.type!=Type::ReviewedRequest || request.header.bytes<74 ||
       request.header.bytes>MaxReviewBytes || get(request.bytes.data()+72,2)!=request.header.bytes-74)return false;
    std::copy_n(reinterpret_cast<const char*>(request.bytes.data()),32,request.reference.run.begin());
    request.reference.captureId=get(request.bytes.data()+32,8);
    std::copy_n(request.bytes.data()+40,32,request.reference.sha256.begin());
    ar::Request parsed{};
    return bool(ar::parse(request.wire(),parsed)) && parsed.reference==request.reference;
}
// Cancellation deliberately carries no review, environment, grant or input API.
// Main must check this before queuing Discarded; failed cancellation closes its run.
template<class Capture> bool cancelDiscard(Capture* capture) noexcept {
    return capture && bool(capture->cancel());
}

// The v1 image metadata contract and all freshness/shape checks are unchanged.
using agentstream::metadata;

enum class Launch : std::uint8_t { Disabled,Enabled,Invalid };
inline Launch launch(std::span<const std::string_view> arguments,bool normalMode,bool measurement) noexcept {
    unsigned trust=0,input=0,authorize=0;
    for(auto argument:arguments) {
        if(argument=="--agent-trust-host-review-v3")trust=std::min(trust+1,2u);
        if(argument=="--agent-input-v1")input=std::min(input+1,2u);
        if(argument=="--agent-authorize-run-v1")authorize=std::min(authorize+1,2u);
    }
    if(!trust)return Launch::Disabled;
    return trust==1 && input==1 && authorize==1 && normalMode && !measurement?Launch::Enabled:Launch::Invalid;
}

// Construct/service/close on Main. Construction allocates one 16MiB send
// buffer, then starts a serial off-main worker. No network/file I/O is performed
// on Main or in the accepted frame callback. One connection for this lifetime;
// failure never starts another listener. Destruction requests stop; the dispatch
// group retains state until off-main I/O has drained and descriptors are closed.
class Server final {
public:
    using Clock=ao::Ledger::Clock;
    // Capability is generated once by the explicitly enabled app, never by a peer.
    Server(ao::RunId,ai::Nonce capability,Clock,void*);
    ~Server();
    Server(const Server&)=delete;Server& operator=(const Server&)=delete;
    std::uint16_t port() const noexcept;
    Failure failure() const noexcept;
    bool stopped() const noexcept;
    bool consumeFailure(void(*)(void*) noexcept,void*) noexcept;
    bool take(Request&) noexcept;
    std::span<std::uint8_t> imageBuffer(std::uint64_t correlation) noexcept;
    bool observation(std::uint64_t,const agentcapture::Publication&,const ag::Grant&,std::uint64_t) noexcept;
    bool result(std::uint64_t,std::uint32_t status,ar::Result,std::uint64_t) noexcept;
    // Call only after checked capture cancellation on Main. Echoes the exact
    // abandoned reference, not review, input admission, or consumption ACK.
    bool discarded(std::uint64_t,const ao::Reference&,std::uint64_t) noexcept;
    // Consume the current request's trusted-host assertion exactly once. This
    // proves capability/connection/ref/wire binding, not that a human or model
    // viewed the pixels. It neither attests the ledger nor admits/ACKs input.
    bool acceptReview(const ao::Reference&,std::string_view originalWire) noexcept;
    void close() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
