// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>

// A third app-owned source, not RecordedInputPolicy or an OS gamepad. This
// policy contains no I/O, clock calls, game/OS headers, or implicit input retry.
// All calls MUST be serialized by the integrating multiplexer. Copy State and
// Token together under that same lock. Only the actual menu/guest consumer may
// call acknowledge(); setters, capabilities(), observers and ordinary snapshots
// are NOT consumer acknowledgements.
namespace recomp::apple::agentinput {

enum class Stage : std::uint8_t { Menu=0, Guest=1 };
enum class Phase : std::uint8_t { Unavailable, InitialNeutral, Ready, Holding, ReleasePending, Revoked, Closed };
enum class PublicationPhase : std::uint8_t { Unavailable, InitialNeutral, Held, Released, Revoked };
enum class Reason : std::uint8_t {
    None, Malformed, Closed, Busy, NoLease, InvalidStage, Inactive, Owner,
    SourceStale, Clock, ObservationStale, Progress, Authorization, Session,
    NewStageRequired, Sequence, Expiry, Bounds, StageMismatch, Observation,
    ConsumerToken, ConsumerState, ConsumerProgress, ConsumerPhase,
    InitialAckTimeout, MissingHeldAck, ReleaseAckTimeout, Exhausted
};

inline constexpr std::uint32_t AllowedButtons=0xf3ffu; // D-pad, Start/Back, L3/R3, shoulders, ABXY; NOT Guide.
inline constexpr std::uint32_t MaxSequence=4096;
inline constexpr std::uint64_t SourceFreshMs=1500;
inline constexpr std::uint64_t ObservationFreshMs=2000;
inline constexpr std::uint64_t MaxExpiryAheadMs=5000;
inline constexpr std::uint64_t MaxClockStepMs=5000;
inline constexpr std::uint64_t AckTimeoutMs=2500;
inline constexpr std::uint64_t MaxGuestObservationLag=120;
using Nonce=std::array<char,32>;

struct State {
    std::uint32_t buttons=0;
    std::array<int,6> axes{}; // LX LY RX RY [-1000,1000]; LT RT [0,1000].
    bool operator==(const State&) const = default;
    bool neutral() const noexcept {return *this==State{};}
};
struct Command {
    Nonce nonce{};
    std::uint32_t sequence=0;
    std::uint64_t expiryMs=0;
    Stage stage=Stage::Menu;
    std::uint64_t observedGeneration=0;
    State state{};
    std::uint32_t holdMs=0;
    bool operator==(const Command&) const = default;
};
struct Environment {
    Stage stage=Stage::Menu;
    std::uint64_t stageGeneration=0;
    // Stage-typed producer progress: a menu UI-consumption revision for Menu,
    // and an actual guest-input frame/revision for Guest. Never use a guest
    // frame as evidence that the launcher consumed input.
    std::uint64_t progressRevision=0;
    std::uint64_t sourceSampleMs=0;
    std::uint64_t nowMs=0; // One monotonic clock domain, milliseconds.
    std::uint64_t observedGeneration=0;
    std::uint64_t observedProgress=0;
    std::uint64_t observedAtMs=0;
    std::uint64_t ownerActivityGeneration=0; // Increment on meaningful manual activity, even if later neutral.
    bool active=false;
    bool manualNeutral=false; // BOTH touch and physical controller are neutral.
};
struct Acquisition {
    Nonce nonce{};
    std::uint64_t authorizationRevision=0; // Explicit fresh authorization; never inferred from foreground.
};
struct Token {
    std::uint64_t publicationGeneration=0;
    std::uint64_t leaseGeneration=0;
    std::uint64_t stageGeneration=0;
    std::uint64_t observedGeneration=0;
    std::uint32_t sequence=0;
    Stage stage=Stage::Menu;
    PublicationPhase phase=PublicationPhase::Unavailable;
    bool operator==(const Token&) const = default;
};
struct Publication {
    State state{};
    Token token{};
    bool available=false;
    bool operator==(const Publication&) const = default;
};
struct Completion {
    std::uint32_t sequence=0;
    std::uint64_t heldGeneration=0,releaseGeneration=0;
    std::uint64_t heldProgress=0,releaseProgress=0;
    bool operator==(const Completion&) const = default;
};
struct Result {
    Reason reason=Reason::None;
    explicit operator bool() const noexcept {return reason==Reason::None;}
};
struct Limits {
    // Lower limits permit finite exhaustion tests; they can only stop delivery
    // sooner, never weaken publication identity or replay checks.
    std::uint64_t publications=std::numeric_limits<std::uint64_t>::max();
    std::uint64_t leases=std::numeric_limits<std::uint64_t>::max();
};

inline bool validStage(Stage stage) noexcept {return stage==Stage::Menu || stage==Stage::Guest;}
inline bool validNonce(const Nonce& nonce) noexcept {
    for(char c:nonce)if(!((c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F')))return false;
    return true;
}
inline bool parseNonce(std::string_view text,Nonce& out) noexcept {
    if(text.size()!=out.size())return false;
    Nonce next{};for(std::size_t i=0;i<next.size();++i)next[i]=text[i];
    if(!validNonce(next))return false;
    out=next;return true;
}
inline bool validCommandBounds(const Command& c) noexcept {
#if !defined(APPLE_AGENT_INPUT_MUTANT_SEQUENCE_BOUNDS)
    if(c.sequence<1 || c.sequence>MaxSequence)return false;
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_STAGE_BOUNDS)
    if(!validStage(c.stage))return false;
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_BUTTON_MASK)
    if(c.state.buttons&~AllowedButtons)return false;
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_OPPOSING_DPAD)
    if((c.state.buttons&3u)==3u || (c.state.buttons&12u)==12u)return false;
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_HOLD_BOUNDS)
    if(c.holdMs<50 || c.holdMs>2000)return false;
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_AXIS_BOUNDS)
    for(unsigned i=0;i<c.state.axes.size();++i)
        if(c.state.axes[i]>1000 || c.state.axes[i]<(i<4?-1000:0))return false;
#endif
    return true;
}

// Exact fourteen-token ASCII line. Single spaces, optional final LF; no tabs,
// CR, scripts, paths, exponent/hex numbers or extra fields. On ANY parse error
// out is unchanged. Commands remain validated again at policy admission.
inline bool parseCommand(std::string_view text,Command& out) noexcept {
#if !defined(APPLE_AGENT_INPUT_MUTANT_TEXT_LENGTH)
    if(text.empty() || text.size()>512)return false;
#endif
    if(!text.empty() && text.back()=='\n')text.remove_suffix(1);
    std::array<std::string_view,14> fields{};
    unsigned count=0;
    while(!text.empty()) {
        if(count==fields.size()) {
#if !defined(APPLE_AGENT_INPUT_MUTANT_FIELD_COUNT)
            return false;
#else
            break;
#endif
        }
        const auto end=text.find(' ');
        auto field=text.substr(0,end);
        if(field.empty())return false;
        // Protocol/nonce/numeric whole-token validation below admits ASCII
        // only. Avoid a second check whose deletion could not change behavior.
        fields[count++]=field;
        if(end==text.npos)break;
        text.remove_prefix(end+1);
        if(text.empty())return false;
    }
#if !defined(APPLE_AGENT_INPUT_MUTANT_FIELD_COUNT)
    if(count!=fields.size())return false;
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_PROTOCOL)
    if(fields[0]!="PORT_AGENT_V1")return false;
#endif
    Command next{};
#if !defined(APPLE_AGENT_INPUT_MUTANT_NONCE)
    if(!parseNonce(fields[1],next.nonce))return false;
#else
    for(std::size_t i=0;i<next.nonce.size() && i<fields[1].size();++i)next.nonce[i]=fields[1][i];
#endif
    auto number=[](std::string_view value,auto& dest) noexcept {
        if(value.empty())return false;
        auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),dest);
#if defined(APPLE_AGENT_INPUT_MUTANT_NUMBER_END)
        (void)end;
#endif
#if defined(APPLE_AGENT_INPUT_MUTANT_NUMBER_ERROR)
        (void)error;
#endif
        return true
#if !defined(APPLE_AGENT_INPUT_MUTANT_NUMBER_ERROR)
            && error==std::errc{}
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_NUMBER_END)
            && end==value.data()+value.size()
#endif
            ;
    };
    unsigned stage=0;
    if(!number(fields[2],next.sequence) || !number(fields[3],next.expiryMs) || !number(fields[4],stage) ||
       !number(fields[5],next.observedGeneration) || !number(fields[6],next.state.buttons) || !number(fields[13],next.holdMs))return false;
#if !defined(APPLE_AGENT_INPUT_MUTANT_STAGE_TRUNCATION)
    if(stage>std::numeric_limits<std::uint8_t>::max())return false;
#endif
    next.stage=static_cast<Stage>(stage);
    for(unsigned i=0;i<next.state.axes.size();++i)if(!number(fields[7+i],next.state.axes[i]))return false;
    if(!validCommandBounds(next))return false;
    out=next;return true;
}

class Policy final {
public:
    explicit Policy(Limits limits={}) noexcept :limits_(limits) {}
    Phase phase() const noexcept {return phase_;}
    Reason revocationReason() const noexcept {return revocationReason_;}
    const Publication& publication() const noexcept {return publication_;}
    const Completion& completion() const noexcept {return completion_;}
    std::uint32_t nextSequence() const noexcept {return nextSequence_;}
    std::uint64_t heldAcknowledgedGeneration() const noexcept {return heldAckGeneration_;}
    std::uint64_t releaseAcknowledgedGeneration() const noexcept {return releaseAckGeneration_;}
    bool leased() const noexcept {
        return phase_==Phase::InitialNeutral || phase_==Phase::Ready || phase_==Phase::Holding || phase_==Phase::ReleasePending;
    }

    Result acquire(const Acquisition& request,const Environment& e) noexcept {
#if !defined(APPLE_AGENT_INPUT_MUTANT_CLOSED)
        if(phase_==Phase::Closed)return {Reason::Closed};
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_ACQUIRE_BUSY)
        if(leased())return {Reason::Busy};
#endif
        if(const auto reason=sourceReason(e);reason!=Reason::None)return {reason};
        if(const auto reason=observationReason(e);reason!=Reason::None)return {reason};
#if !defined(APPLE_AGENT_INPUT_MUTANT_AUTHORIZATION)
        if(!request.authorizationRevision || request.authorizationRevision<=authorizationRevision_)return {Reason::Authorization};
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_ACQUIRE_NONCE)
        if(!validNonce(request.nonce) || (everAcquired_ && request.nonce==nonce_))return {Reason::Session};
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_NEW_STAGE)
        if(everAcquired_ && e.stageGeneration<=stageGeneration_)return {Reason::NewStageRequired};
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_ACQUIRE_CLOCK)
        if(everAcquired_ && e.nowMs<lastNow_)return {Reason::Clock};
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_ACQUIRE_EXHAUSTION)
        if(publicationGeneration_>=limits_.publications || leaseGeneration_>=limits_.leases)
            return {Reason::Exhausted};
#endif
        nonce_=request.nonce;authorizationRevision_=request.authorizationRevision;
        stage_=e.stage;stageGeneration_=e.stageGeneration;ownerGeneration_=e.ownerActivityGeneration;
        ++leaseGeneration_;everAcquired_=true;nextSequence_=1;command_={};completion_={};
        heldAckGeneration_=releaseAckGeneration_=heldAckProgress_=0;
        lastConsumerProgress_=e.progressRevision;lastProgress_=e.progressRevision;
        lastNow_=e.nowMs;lastObservedGeneration_=e.observedGeneration;
        acquiredAt_=e.nowMs;phase_=Phase::InitialNeutral;revocationReason_=Reason::None;
        if(!publish({},PublicationPhase::InitialNeutral,0,e.observedGeneration,true))return {Reason::Exhausted};
        return {};
    }

    // Parse before touching the lease: even invalid text arriving after a
    // deadline cannot secretly tick, revoke or acknowledge the policy.
    Result acceptText(std::string_view text,const Environment& e) noexcept {
        Command c{};if(!parseCommand(text,c))return {Reason::Malformed};
        return accept(c,e);
    }
    Result accept(const Command& c,const Environment& e) noexcept {
        if(!validNonce(c.nonce) || !validCommandBounds(c))return {Reason::Bounds};
        if(const auto result=update(e);!result)return result;
#if !defined(APPLE_AGENT_INPUT_MUTANT_COMMAND_READY)
        if(phase_!=Phase::Ready)return {leased()?Reason::Busy:Reason::NoLease};
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_SESSION_MATCH)
        if(c.nonce!=nonce_)return {Reason::Session};
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_SEQUENCE_MATCH)
        if(c.sequence!=nextSequence_)return {Reason::Sequence};
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_COMMAND_STAGE)
        if(c.stage!=stage_)return {Reason::StageMismatch};
#endif
        if(const auto reason=observationReason(e);reason!=Reason::None)return {reason};
#if !defined(APPLE_AGENT_INPUT_MUTANT_OBSERVATION_MATCH)
        if(c.observedGeneration!=e.observedGeneration)return {Reason::Observation};
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_EXPIRY)
        if(c.expiryMs<=e.nowMs || c.expiryMs-e.nowMs>MaxExpiryAheadMs || c.expiryMs-e.nowMs<=c.holdMs)return {Reason::Expiry};
#endif
        command_=c;heldUntil_=e.nowMs+c.holdMs;phase_=Phase::Holding;
        heldAckGeneration_=releaseAckGeneration_=heldAckProgress_=0;
        if(!publish(c.state,PublicationPhase::Held,c.sequence,c.observedGeneration,true))return {Reason::Exhausted};
        return {};
    }

    // Invoke on source/lifecycle changes and bounded timer ticks, not only when
    // accepting the next command. A revoked lease never resurrects on neutral
    // or foreground; acquire() requires a NEW stage and explicit authorization.
    Result update(const Environment& e) noexcept {
        if(!leased())return {phase_==Phase::Closed?Reason::Closed:Reason::NoLease};
        auto fail=[this](Reason reason) noexcept {revoke(reason);return Result{reason};};
        if(const auto reason=sourceReason(e);reason!=Reason::None)return fail(reason);
#if !defined(APPLE_AGENT_INPUT_MUTANT_LEASE_STAGE)
        if(e.stage!=stage_ || e.stageGeneration!=stageGeneration_)return fail(Reason::StageMismatch);
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_OWNER_GENERATION)
        if(e.ownerActivityGeneration!=ownerGeneration_)return fail(Reason::Owner);
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_CLOCK)
        if(e.nowMs<lastNow_ || e.nowMs-lastNow_>MaxClockStepMs)return fail(Reason::Clock);
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_PROGRESS_MONOTONIC)
        if(e.progressRevision<lastProgress_ || e.observedGeneration<lastObservedGeneration_)return fail(Reason::Progress);
#endif
        lastNow_=e.nowMs;lastProgress_=e.progressRevision;lastObservedGeneration_=e.observedGeneration;
#if !defined(APPLE_AGENT_INPUT_MUTANT_INITIAL_TIMEOUT)
        if(phase_==Phase::InitialNeutral && e.nowMs-acquiredAt_>AckTimeoutMs)return fail(Reason::InitialAckTimeout);
#endif
        if(phase_==Phase::Holding) {
            if(e.nowMs>=heldUntil_) {
#if !defined(APPLE_AGENT_INPUT_MUTANT_HELD_REQUIRED)
                if(!heldAckGeneration_)return fail(Reason::MissingHeldAck);
#endif
                phase_=Phase::ReleasePending;releasedAt_=e.nowMs;
                if(!publish({},PublicationPhase::Released,command_.sequence,command_.observedGeneration,true))return {Reason::Exhausted};
            }
        }
#if !defined(APPLE_AGENT_INPUT_MUTANT_RELEASE_TIMEOUT)
        if(phase_==Phase::ReleasePending && (e.nowMs>=command_.expiryMs || e.nowMs-releasedAt_>AckTimeoutMs))
            return fail(Reason::ReleaseAckTimeout);
#endif
        return {};
    }

    // consumed MUST be the coherent State+Token actually returned to the real
    // stage consumer, not a snapshot obtained by a setter/observer. Progress is
    // that consumer's real revision in e, not a synthetic ACK counter. Repeated
    // acknowledgement of one held token returns false and cannot advance it.
    Result acknowledge(const Publication& consumed,const Environment& e) noexcept {
        if(const auto result=update(e);!result)return result;
#if !defined(APPLE_AGENT_INPUT_MUTANT_ACK_TOKEN)
        if(!consumed.available || consumed.token!=publication_.token)return {Reason::ConsumerToken};
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_ACK_STATE)
        if(consumed.state!=publication_.state)return {Reason::ConsumerState};
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_ACK_PROGRESS)
        if(e.progressRevision<=lastConsumerProgress_)return {Reason::ConsumerProgress};
#endif
        if(phase_==Phase::InitialNeutral) {
            lastConsumerProgress_=e.progressRevision;phase_=Phase::Ready;return {};
        }
        if(phase_==Phase::Holding && !heldAckGeneration_) {
            heldAckGeneration_=consumed.token.publicationGeneration;
            heldAckProgress_=e.progressRevision;lastConsumerProgress_=e.progressRevision;return {};
        }
        if(phase_==Phase::ReleasePending) {
            // update() could reach ReleasePending only after a held ACK.
            // Token equality proves this is the later Released publication;
            // progress > lastConsumerProgress_ proves a later real consumer.
            releaseAckGeneration_=consumed.token.publicationGeneration;
            completion_={command_.sequence,heldAckGeneration_,releaseAckGeneration_,heldAckProgress_,e.progressRevision};
            lastConsumerProgress_=e.progressRevision;nextSequence_=command_.sequence+1;phase_=Phase::Ready;return {};
        }
        return {Reason::ConsumerPhase};
    }

    void revoke(Reason reason=Reason::Owner) noexcept {
        if(phase_==Phase::Closed || phase_==Phase::Revoked || phase_==Phase::Unavailable)return;
        phase_=Phase::Revoked;revocationReason_=reason;
        publish({},PublicationPhase::Revoked,command_.sequence,command_.observedGeneration,false);
    }
    void close() noexcept {
        if(phase_==Phase::Closed)return;
        phase_=Phase::Closed;revocationReason_=Reason::Closed;
        publish({},PublicationPhase::Revoked,command_.sequence,command_.observedGeneration,false);
    }
private:
    static Reason sourceReason(const Environment& e) noexcept {
#if !defined(APPLE_AGENT_INPUT_MUTANT_SOURCE_STAGE)
        if(!validStage(e.stage) || !e.stageGeneration)return Reason::InvalidStage;
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_INACTIVE)
        if(!e.active)return Reason::Inactive;
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_MANUAL_NEUTRAL)
        if(!e.manualNeutral)return Reason::Owner;
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_SOURCE_FRESH)
        if(!e.nowMs || !e.sourceSampleMs || e.sourceSampleMs>e.nowMs || e.nowMs-e.sourceSampleMs>SourceFreshMs || !e.progressRevision)
            return Reason::SourceStale;
#endif
        return Reason::None;
    }
    static Reason observationReason(const Environment& e) noexcept {
#if !defined(APPLE_AGENT_INPUT_MUTANT_OBSERVATION_FRESH)
        if(!e.observedGeneration || !e.observedAtMs || e.observedAtMs>e.nowMs || e.nowMs-e.observedAtMs>ObservationFreshMs)
            return Reason::ObservationStale;
#endif
#if !defined(APPLE_AGENT_INPUT_MUTANT_OBSERVATION_PROGRESS)
        const auto allowed=e.stage==Stage::Menu?0:MaxGuestObservationLag;
        if(!e.observedProgress || e.observedProgress>e.progressRevision || e.progressRevision-e.observedProgress>allowed)
            return Reason::Progress;
#endif
        return Reason::None;
    }
    bool publish(State state,PublicationPhase phase,std::uint32_t sequence,std::uint64_t observation,bool available) noexcept {
#if !defined(APPLE_AGENT_INPUT_MUTANT_PUBLICATION_EXHAUSTION)
        if(publicationGeneration_>=limits_.publications) {
            phase_=Phase::Closed;revocationReason_=Reason::Exhausted;publication_.state={};publication_.available=false;return false;
        }
#endif
        publication_={state,{++publicationGeneration_,leaseGeneration_,stageGeneration_,observation,sequence,stage_,phase},available};
        return true;
    }
    Phase phase_=Phase::Unavailable;
    Limits limits_{};
    Reason revocationReason_=Reason::None;
    Publication publication_{};
    Completion completion_{};
    Command command_{};
    Nonce nonce_{};
    Stage stage_=Stage::Menu;
    bool everAcquired_=false;
    std::uint32_t nextSequence_=1;
    std::uint64_t authorizationRevision_=0,publicationGeneration_=0,leaseGeneration_=0,stageGeneration_=0;
    std::uint64_t ownerGeneration_=0,lastNow_=0,lastProgress_=0,lastObservedGeneration_=0,lastConsumerProgress_=0;
    std::uint64_t acquiredAt_=0,heldUntil_=0,releasedAt_=0,heldAckGeneration_=0,releaseAckGeneration_=0,heldAckProgress_=0;
};

} // namespace recomp::apple::agentinput
