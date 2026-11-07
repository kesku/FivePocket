// SPDX-License-Identifier: GPL-3.0-or-later
#include "AgentDiscardStream.hpp"
#include <atomic>
#include <cerrno>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace recomp::apple::agentdiscardstream {
namespace {
struct Fd {int value=-1;~Fd(){if(value>=0)::close(value);}void reset(){if(value>=0)::close(value);value=-1;}};
enum class Phase {Bind,Capture,Busy,Review};
struct State {
    ao::RunId run;ai::Nonce capability;Server::Clock clock;void* context;
    const std::thread::id owner=std::this_thread::get_id();
    std::atomic<Failure> failure{Failure::None};std::atomic<std::uint16_t> port{0};
    std::atomic<bool> stopped{false};
    std::mutex mutex;
    Phase phase=Phase::Bind;Request request{};bool available=false;
    bool bound=false,reviewConsumed=false;ao::Reference exportedReference{};
    std::uint64_t correlation=0,sendSequence=0;
    std::array<std::uint8_t,HeaderBytes> txHeader{};
    std::array<std::uint8_t,MetadataBytes> txSmall{};
    std::unique_ptr<std::uint8_t[]> image{new std::uint8_t[ao::Ledger::MaxBytes]};
    std::size_t smallBytes=0,imageBytes=0;std::uint64_t txStarted=0;
    bool sending=false;Type txType{};
    State(ao::RunId r,ai::Nonce cap,Server::Clock c,void* p):run(r),capability(cap),clock(c),context(p){}
    void fail(Failure reason) noexcept {auto expected=Failure::None;failure.compare_exchange_strong(expected,reason);}
    bool send(Type type,std::uint64_t id,std::size_t small,std::size_t pixels,std::uint64_t stamp) noexcept {
        if(sending || !stamp || sendSequence==UINT64_MAX){fail(Failure::Protocol);return false;}
        txHeader=encode({type,std::uint32_t(small+pixels),++sendSequence,id});
        smallBytes=small;imageBytes=pixels;txStarted=stamp;txType=type;sending=true;return true;
    }
};
bool setup(int fd) noexcept {
    const int flags=fcntl(fd,F_GETFL,0),descriptor=fcntl(fd,F_GETFD,0);
    if(flags<0 || descriptor<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0 ||
       fcntl(fd,F_SETFD,descriptor|FD_CLOEXEC)<0)return false;
    const int enabled=1;
    return setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled))==0;
}
void run(const std::shared_ptr<State>& state) noexcept {
    struct Complete {State& state;~Complete(){state.port.store(0);state.stopped.store(true);}} complete{*state};
    Fd listener,peer;
    if(state->failure.load()!=Failure::None)return;
    listener.value=socket(AF_INET,SOCK_STREAM,0);
    if(listener.value<0 || !setup(listener.value)){state->fail(Failure::Socket);return;}
    sockaddr_in address{};address.sin_len=sizeof(address);address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;
    if(bind(listener.value,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(listener.value,1)) {
        state->fail(Failure::Socket);return;
    }
    socklen_t length=sizeof(address);
    if(getsockname(listener.value,reinterpret_cast<sockaddr*>(&address),&length) ||
       length!=sizeof(address) || address.sin_addr.s_addr!=htonl(INADDR_LOOPBACK) || !address.sin_port) {
        state->fail(Failure::Socket);return;
    }
    state->port.store(ntohs(address.sin_port));
    while(state->failure.load()==Failure::None && peer.value<0) {
        pollfd fd{listener.value,POLLIN,0};const auto result=poll(&fd,1,20);
        if(result<0 && errno==EINTR)continue;
        if(result<0 || (fd.revents&(POLLERR|POLLHUP|POLLNVAL))){state->fail(Failure::Socket);return;}
        if(!(fd.revents&POLLIN))continue;
        sockaddr_in remote{};socklen_t count=sizeof(remote);
        peer.value=accept(listener.value,reinterpret_cast<sockaddr*>(&remote),&count);
        if(peer.value<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))continue;
        if(peer.value<0 || count!=sizeof(remote) || remote.sin_family!=AF_INET ||
           remote.sin_addr.s_addr!=htonl(INADDR_LOOPBACK) || !setup(peer.value)) {
            state->fail(Failure::Socket);return;
        }
    }
    listener.reset(); // Exactly one accepted connection, never reopen/reconnect.
    if(state->failure.load()!=Failure::None)return;
    {
        std::lock_guard lock(state->mutex);
        std::copy(state->run.begin(),state->run.end(),state->txSmall.begin());
        if(!state->send(Type::Hello,0,32,0,state->clock(state->context)))return;
    }
    std::array<std::uint8_t,HeaderBytes> header{};Request incoming{};
    std::size_t headerRead=0,payloadRead=0,sent=0;
    std::uint64_t expected=1,receiveStarted=0,lastNow=0;
    while(state->failure.load()==Failure::None) {
        const auto now=state->clock(state->context);
        if(!now || now<lastNow){state->fail(Failure::Deadline);break;}lastNow=now;
        bool sending=false;
        {std::lock_guard lock(state->mutex);sending=state->sending;
         if(sending && (now<state->txStarted || now-state->txStarted>ai::ObservationFreshMs))state->fail(Failure::Deadline);}
        if(receiveStarted && now-receiveStarted>ai::ObservationFreshMs)state->fail(Failure::Deadline);
        if(state->failure.load()!=Failure::None)break;
        pollfd fd{peer.value,short(POLLIN|(sending?POLLOUT:0)),0};const auto ready=poll(&fd,1,20);
        if(ready<0 && errno==EINTR)continue;
        if(ready<0 || (fd.revents&(POLLERR|POLLHUP|POLLNVAL))){state->fail(Failure::Disconnected);break;}
        if(fd.revents&POLLIN) {
            auto* destination=headerRead<HeaderBytes?header.data()+headerRead:incoming.bytes.data()+payloadRead;
            const auto remaining=headerRead<HeaderBytes?HeaderBytes-headerRead:incoming.header.bytes-payloadRead;
            const auto count=recv(peer.value,destination,remaining,0);
            if(count==0){state->fail(Failure::Disconnected);break;}
            if(count<0 && errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK){state->fail(Failure::Socket);break;}
            if(count>0) {
                if(!receiveStarted)receiveStarted=now;
                if(headerRead<HeaderBytes) {
                    headerRead+=std::size_t(count);
                    if(headerRead==HeaderBytes && !decodeClient(header,expected,incoming.header)) {
                        state->fail(Failure::Protocol);break;
                    }
                } else payloadRead+=std::size_t(count);
                if(headerRead==HeaderBytes && payloadRead==incoming.header.bytes) {
                    if(!validate(incoming,state->run)){state->fail(Failure::Protocol);break;}
                    if(incoming.header.type==Type::Cancel){state->fail(Failure::Cancelled);break;}
                    std::lock_guard lock(state->mutex);
                    if(incoming.header.type==Type::BindReview) {
                        if(state->bound || state->phase!=Phase::Bind || state->sending || expected!=1 ||
                           !std::equal(state->capability.begin(),state->capability.end(),incoming.bytes.begin()+32)) {
                            state->fail(Failure::Protocol);break;
                        }
                        // This acknowledges only the connection capability. No
                        // image, input policy or game-consumer ACK is changed.
                        state->bound=true;state->capability={};state->phase=Phase::Busy;
                        state->txSmall={};put(state->txSmall.data()+24,now,8);
                        if(!state->send(Type::Result,incoming.header.correlation,32,0,now))break;
                    } else {
                        if(!state->bound || state->available || state->sending ||
                           (incoming.header.type==Type::Capture && state->phase!=Phase::Capture) ||
                           (incoming.header.type==Type::ReviewedRequest && state->phase!=Phase::Review) ||
                           (incoming.header.type==Type::Discard && (state->phase!=Phase::Review || incoming.reference!=state->exportedReference))) {
                            state->fail(Failure::Protocol);break;
                        }
                        state->request=incoming;state->available=true;state->phase=Phase::Busy;
                        state->correlation=incoming.header.correlation;state->reviewConsumed=false;
                        if(incoming.header.type==Type::Capture)state->exportedReference={};
                    }
                    ++expected;incoming={};headerRead=payloadRead=0;receiveStarted=0;
                }
            }
        }
        if(fd.revents&POLLOUT) {
            std::lock_guard lock(state->mutex);
            if(!state->sending)continue;
            const std::uint8_t* bytes=nullptr;std::size_t remaining=0;
            if(sent<HeaderBytes){bytes=state->txHeader.data()+sent;remaining=HeaderBytes-sent;}
            else if(sent<HeaderBytes+state->smallBytes){const auto offset=sent-HeaderBytes;bytes=state->txSmall.data()+offset;remaining=state->smallBytes-offset;}
            else {const auto offset=sent-HeaderBytes-state->smallBytes;bytes=state->image.get()+offset;remaining=state->imageBytes-offset;}
            const auto count=::send(peer.value,bytes,remaining,0);
            if(count<0 && errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK){state->fail(Failure::Socket);break;}
            if(count>0)sent+=std::size_t(count);
            if(sent==HeaderBytes+state->smallBytes+state->imageBytes) {
                state->sending=false;sent=0;
                if(state->txType==Type::Observation)state->phase=Phase::Review;
                else if(state->txType==Type::Result || state->txType==Type::Discarded)state->phase=Phase::Capture;
            }
        }
    }
    state->port.store(0);
}
}
struct Server::Impl {
    std::shared_ptr<State> state;dispatch_group_t group;
    bool failureConsumed=false;
    Impl(ao::RunId run,ai::Nonce capability,Clock clock,void* context):state(std::make_shared<State>(run,capability,clock,context)),group(dispatch_group_create()) {
        if(!group){state->fail(Failure::Socket);state->stopped.store(true);return;}
        if(!ai::validNonce(run) || !ai::validNonce(capability) || capability==run || !clock){state->fail(Failure::Protocol);state->stopped.store(true);return;}
        const auto keep=state;
        dispatch_group_async(group,dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{runWorker(keep);});
    }
    static void runWorker(const std::shared_ptr<State>& state) noexcept {run(state);}
    ~Impl() {
        state->fail(Failure::Cancelled);
        // Off-main group completion is the join boundary. Captured state keeps
        // descriptors/buffers alive through the worker's final close.
        if(!group)return;
        const auto keep=state;const auto owned=group;
        dispatch_group_notify(owned,dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
            (void)keep;dispatch_release(owned);
        });
    }
};
Server::Server(ao::RunId r,ai::Nonce cap,Clock c,void* p):impl_(std::make_unique<Impl>(r,cap,c,p)){}
Server::~Server()=default;
std::uint16_t Server::port() const noexcept {return impl_->state->port.load();}
Failure Server::failure() const noexcept {return impl_->state->failure.load();}
bool Server::stopped() const noexcept {return impl_->state->stopped.load();}
bool Server::consumeFailure(void(*consume)(void*) noexcept,void* context) noexcept {
    if(std::this_thread::get_id()!=impl_->state->owner || !consume || impl_->failureConsumed ||
       failure()==Failure::None)return false;
    impl_->failureConsumed=true;consume(context);return true;
}
bool Server::acceptReview(const ao::Reference& reference,std::string_view originalWire) noexcept {
    auto& s=*impl_->state;
    if(std::this_thread::get_id()!=s.owner || failure()!=Failure::None)return false;
    std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock || !s.bound || s.available || s.sending || s.phase!=Phase::Busy || s.reviewConsumed ||
       s.request.header.type!=Type::ReviewedRequest || s.request.header.correlation!=s.correlation ||
       reference!=s.exportedReference || reference!=s.request.reference || originalWire!=s.request.wire())return false;
    s.reviewConsumed=true;return true;
}
void Server::close() noexcept {impl_->state->fail(Failure::Cancelled);}
bool Server::take(Request& out) noexcept {
    out={};auto& s=*impl_->state;
    if(std::this_thread::get_id()!=s.owner || failure()!=Failure::None)return false;
    std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock || !s.available)return false;
    out=s.request;s.available=false;return true;
}
std::span<std::uint8_t> Server::imageBuffer(std::uint64_t correlation) noexcept {
    auto& s=*impl_->state;
    if(std::this_thread::get_id()!=s.owner || failure()!=Failure::None)return {};
    std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock || s.available || s.sending || s.phase!=Phase::Busy || correlation!=s.correlation)return {};
    return {s.image.get(),ao::Ledger::MaxBytes};
}
bool Server::observation(std::uint64_t correlation,const agentcapture::Publication& p,const ag::Grant& g,std::uint64_t now) noexcept {
    auto& s=*impl_->state;if(std::this_thread::get_id()!=s.owner || failure()!=Failure::None)return false;
    std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock || s.available || s.sending || s.phase!=Phase::Busy || correlation!=s.correlation ||
       !metadata(p,g,now,s.txSmall))return false;
    s.exportedReference=p.observation.metadata.reference;
    return s.send(Type::Observation,correlation,MetadataBytes,p.observation.metadata.byteCount,p.observation.metadata.capturedAtMs);
}
bool Server::result(std::uint64_t correlation,std::uint32_t status,ar::Result result,std::uint64_t now) noexcept {
    auto& s=*impl_->state;if(std::this_thread::get_id()!=s.owner || failure()!=Failure::None)return false;
    std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock || s.available || s.sending || s.phase!=Phase::Busy || correlation!=s.correlation || s.request.header.type==Type::Discard || status>3)return false;
    s.txSmall={};const std::array<std::uint32_t,5> values{status,unsigned(result.reason),unsigned(result.ingress.reason),
        unsigned(result.ingress.ledger),unsigned(result.ingress.input)};
    for(unsigned i=0;i<values.size();++i)put(s.txSmall.data()+4*i,values[i],4);
    put(s.txSmall.data()+24,now,8);return s.send(Type::Result,correlation,32,0,now);
}
bool Server::discarded(std::uint64_t correlation,const ao::Reference& reference,std::uint64_t now) noexcept {
    auto& s=*impl_->state;if(std::this_thread::get_id()!=s.owner || failure()!=Failure::None)return false;
    std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock || s.available || s.sending || s.phase!=Phase::Busy || correlation!=s.correlation ||
       s.request.header.type!=Type::Discard || reference!=s.request.reference || reference!=s.exportedReference)return false;
    s.txSmall={};std::copy(reference.run.begin(),reference.run.end(),s.txSmall.begin());
    put(s.txSmall.data()+32,reference.captureId,8);
    std::copy(reference.sha256.begin(),reference.sha256.end(),s.txSmall.begin()+40);
    if(!s.send(Type::Discarded,correlation,72,0,now))return false;
    s.exportedReference={};return true;
}

}
