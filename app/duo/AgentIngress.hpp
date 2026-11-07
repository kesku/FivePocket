// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AgentEnvironment.hpp"
#include "AgentObservation.hpp"
#include <thread>

namespace recomp::apple::agentingress {
namespace ai=agentinput;
namespace ao=agentobservation;

// Supplied only by the app's authorization provider, never by a transport.
// A stage transition requires the provider to explicitly authorize that stage.
struct Grant {
    ao::RunId run{};
    ai::Stage stage=ai::Stage::Menu;
    std::uint64_t stageGeneration=0;
    ai::Acquisition acquisition{};
};
struct Observation {
    ao::Metadata metadata{};
    std::uint64_t observedGeneration=0;
};
struct AcquireRequest {ao::Reference reference{};ai::Acquisition acquisition{};};
// Synchronous borrowed text. The transport must own it throughout dispatch.
struct CommandRequest {ao::Reference reference{};std::string_view text{};};
enum class Reason : std::uint8_t {
    None,Thread,Busy,Closed,Provider,Image,Identity,Stage,Owner,Observation,Authorization,Input
};
struct Result {
    Reason reason=Reason::None;
    ao::Reason ledger=ao::Reason::None;
    ai::Reason input=ai::Reason::None;
    explicit operator bool() const noexcept {return reason==Reason::None;}
};

// Session-owned from before its first image; the only Record::image writer.
// Source is the real FullTouchInput Input/Gamepad, not a policy substitute.
// All public calls belong to the constructing source-owner thread. Referents
// outlive this dispatcher. Providers are trusted app-owned, bounded, noexcept
// and non-reentrant; environment reads the same Record and may additionally
// deny activity. No caller-supplied environment, clock or grant is accepted.
//
// This class neither captures pixels nor attests that anyone reviewed them.
// Only the separate trusted review path may call Ledger::attest. A consumed
// review is single-use even if the real input policy subsequently refuses it.
// No method acknowledges delivery or advances source progress.
template<class Source> class Dispatcher final {
public:
    using EnvironmentProvider=bool(*)(void*,ai::Environment&) noexcept;
    using GrantProvider=bool(*)(void*,Grant&) noexcept;
    Dispatcher(ao::Ledger& ledger,agentenvironment::Record& record,Source& source,
               EnvironmentProvider environment,void* environmentContext,
               GrantProvider grant,void* grantContext) noexcept
        :ledger_(ledger),record_(record),source_(source),environment_(environment),
         environmentContext_(environmentContext),grant_(grant),grantContext_(grantContext),
         closed_(!environment || !grant) {}
    Dispatcher(const Dispatcher&)=delete;
    Dispatcher& operator=(const Dispatcher&)=delete;

    // Trusted capture adapter uses this, not host assertions about neutrality.
    Result read(ai::Environment& out) noexcept {
        out={};Scope scope(*this);if(!scope)return {scope.reason};
        if(!current(out)){out={};return {Reason::Provider};}return {};
    }

    // Export the exact ledger-owned pixels while publishing their observation
    // once. Failure clears returned metadata; export bytes alone confer no use.
    Result publish(const ao::Reference& reference,std::uint8_t* pixels,
                   std::size_t capacity,Observation& out) noexcept {
        out={};Scope scope(*this);if(!scope)return {scope.reason};
        if(bound_)return {Reason::Busy};
        ao::Metadata metadata{};
        if(const auto result=ledger_.copyImage(reference,pixels,capacity,metadata);!result)
            return {Reason::Image,result.reason};
        ai::Environment before{},e{};
        if(!record_.read(before) || !current(e))return {Reason::Provider};
        if(before.observedGeneration>lastObserved_)lastObserved_=before.observedGeneration;
        if(!sameObservation(before,e))return {Reason::Observation};
        if(const auto reason=captureReason(metadata,e);reason!=Reason::None)return {reason};
        if(lastObserved_==UINT64_MAX)return {Reason::Observation};
        const auto expected=lastObserved_+1;
        if(!record_.image(metadata.stage,metadata.stageGeneration,metadata.reference.captureId,
                          metadata.progress,metadata.capturedAtMs))return {Reason::Image};
        ai::Environment after{};
        if(!record_.read(after))return {Reason::Provider};
        if(after.observedGeneration>lastObserved_)lastObserved_=after.observedGeneration;
        if(after.observedGeneration!=expected || !imageMatches(after,metadata))return {Reason::Observation};
        if(!current(e))return {Reason::Provider};
        if(!sameObservation(after,e))return {Reason::Observation};
        if(const auto reason=captureReason(metadata,e);reason!=Reason::None)return {reason};
        observation_={metadata,after.observedGeneration};bound_=true;out=observation_;return {};
    }

    Result submit(const AcquireRequest& request) noexcept {
        Scope scope(*this);if(!scope)return {scope.reason};
        ai::Environment e{};Grant grant{};
        if(const auto result=prepare(request.reference,e,grant);!result)return result;
        if(!sameAcquisition(request.acquisition,grant.acquisition))return {Reason::Authorization};
        if(const auto result=consume(request.reference,e,grant);!result)return result;
        const auto result=source_.acquireAgent(request.acquisition,e);
        if(!result)return {Reason::Input,ao::Reason::None,result.reason};
        acquired_=grant;hasAcquired_=true;return {};
    }

    Result submit(const CommandRequest& request) noexcept {
        Scope scope(*this);if(!scope)return {scope.reason};
        ai::Environment e{};Grant grant{};
        if(const auto result=prepare(request.reference,e,grant);!result)return result;
        if(!hasAcquired_ || !sameGrant(grant,acquired_))return {Reason::Authorization};
        ai::Command parsed{};
        if(!ai::parseCommand(request.text,parsed))return {Reason::Input,ao::Reason::None,ai::Reason::Malformed};
        if(parsed.nonce!=grant.acquisition.nonce || parsed.stage!=grant.stage ||
           parsed.observedGeneration!=observation_.observedGeneration)return {Reason::Observation};
        if(const auto result=consume(request.reference,e,grant);!result)return result;
        // Preserve the original text. Policy alone decides sequence/expiry/state.
        const auto result=source_.agentCommand(request.text,e);
        return result?Result{}:Result{Reason::Input,ao::Reason::None,result.reason};
    }

    // Discard only the pending observation. Never restart Record or a lease to
    // evade a stage/reacquisition restriction. A failed publication is resettable.
    Result reset() noexcept {
        Scope scope(*this);if(!scope)return {scope.reason};
        bound_=false;observation_={};
        const auto result=ledger_.discard();
        return result?Result{}:Result{Reason::Image,result.reason};
    }
    Result close() noexcept {
        if(std::this_thread::get_id()!=owner_)return {Reason::Thread};
        if(entered_)return {Reason::Busy};
        bound_=false;observation_={};hasAcquired_=false;acquired_={};closed_=true;return {};
    }
private:
    struct Scope {
        Dispatcher& self;Reason reason=Reason::None;
        explicit Scope(Dispatcher& value) noexcept:self(value) {
            if(std::this_thread::get_id()!=self.owner_)reason=Reason::Thread;
            else if(self.entered_)reason=Reason::Busy;
            else if(self.closed_)reason=Reason::Closed;
            else self.entered_=true;
        }
        ~Scope(){if(reason==Reason::None)self.entered_=false;}
        explicit operator bool() const noexcept{return reason==Reason::None;}
    };
    bool current(ai::Environment& e) noexcept {
        e={};if(!environment_(environmentContext_,e))return false;
        const auto status=source_.agentStatus();
        e.active=e.active && status.active;e.manualNeutral=status.manualNeutral;
        e.ownerActivityGeneration=status.ownerGeneration;return true;
    }
    static bool sameObservation(const ai::Environment& a,const ai::Environment& b) noexcept {
        return a.stage==b.stage && a.stageGeneration==b.stageGeneration &&
            a.observedGeneration==b.observedGeneration && a.observedProgress==b.observedProgress &&
            a.observedAtMs==b.observedAtMs;
    }
    static bool imageMatches(const ai::Environment& e,const ao::Metadata& m) noexcept {
        return e.stage==m.stage && e.stageGeneration==m.stageGeneration &&
            e.observedProgress==m.progress && e.observedAtMs==m.capturedAtMs;
    }
    static Reason captureReason(const ao::Metadata& m,const ai::Environment& e) noexcept {
        if(!e.active || !ai::validStage(e.stage) || !e.stageGeneration ||
           e.stage!=m.stage || e.stageGeneration!=m.stageGeneration)return Reason::Stage;
        if(!e.manualNeutral || e.ownerActivityGeneration!=m.ownerActivityGeneration)return Reason::Owner;
        if(!e.nowMs || !e.sourceSampleMs || e.sourceSampleMs>e.nowMs ||
           e.nowMs-e.sourceSampleMs>ai::SourceFreshMs || !m.capturedAtMs ||
           m.capturedAtMs>e.nowMs || e.nowMs-m.capturedAtMs>ai::ObservationFreshMs)return Reason::Observation;
        const auto lag=e.stage==ai::Stage::Menu?0:ai::MaxGuestObservationLag;
        if(!m.progress || e.progressRevision<m.progress || e.progressRevision-m.progress>lag)
            return Reason::Observation;
        return Reason::None;
    }
    static bool sameAcquisition(const ai::Acquisition& a,const ai::Acquisition& b) noexcept {
        return a.nonce==b.nonce && a.authorizationRevision==b.authorizationRevision;
    }
    static bool sameGrant(const Grant& a,const Grant& b) noexcept {
        return a.run==b.run && a.stage==b.stage && a.stageGeneration==b.stageGeneration &&
            sameAcquisition(a.acquisition,b.acquisition);
    }
    Result prepare(const ao::Reference& reference,ai::Environment& e,Grant& grant) noexcept {
        if(!bound_ || reference!=observation_.metadata.reference)return {Reason::Identity};
        if(!current(e))return {Reason::Provider};
        if(e.observedGeneration!=observation_.observedGeneration || !imageMatches(e,observation_.metadata))
            return {Reason::Observation};
        if(const auto reason=captureReason(observation_.metadata,e);reason!=Reason::None)return {reason};
        if(!grant_(grantContext_,grant) || !ai::validNonce(grant.run) ||
           !ai::validNonce(grant.acquisition.nonce) || !grant.acquisition.authorizationRevision ||
           grant.run!=reference.run || grant.stage!=e.stage || grant.stageGeneration!=e.stageGeneration)
            return {Reason::Authorization};
        return {};
    }
    Result consume(const ao::Reference& reference,ai::Environment& e,const Grant& grant) noexcept {
        ao::Metadata actual{};
        if(const auto result=ledger_.takeReviewed(reference,e,actual);!result)return {Reason::Image,result.reason};
        const auto observation=observation_;
        const auto expected=observation.metadata;bound_=false;observation_={};
        if(actual.reference!=expected.reference || actual.stage!=expected.stage ||
           actual.stageGeneration!=expected.stageGeneration || actual.progress!=expected.progress ||
           actual.capturedAtMs!=expected.capturedAtMs || actual.ownerActivityGeneration!=expected.ownerActivityGeneration ||
           actual.sourceSampleMs!=expected.sourceSampleMs || actual.frameSequence!=expected.frameSequence ||
           actual.width!=expected.width || actual.height!=expected.height || actual.byteCount!=expected.byteCount)
            return {Reason::Identity};
        // Consumption/provider work may take time or race a real guest copy.
        // Re-read after it; never pass an earlier clock/progress snapshot on.
        Grant latest{};
        if(!grant_(grantContext_,latest) || !sameGrant(latest,grant))return {Reason::Authorization};
        if(!current(e))return {Reason::Provider};
        if(e.observedGeneration!=observation.observedGeneration || !imageMatches(e,expected))
            return {Reason::Observation};
        if(const auto reason=captureReason(expected,e);reason!=Reason::None)return {reason};
        return {};
    }
    ao::Ledger& ledger_;agentenvironment::Record& record_;Source& source_;
    const EnvironmentProvider environment_;void* const environmentContext_;
    const GrantProvider grant_;void* const grantContext_;
    const std::thread::id owner_=std::this_thread::get_id();
    Observation observation_{};Grant acquired_{};
    std::uint64_t lastObserved_=0;
    bool bound_=false,hasAcquired_=false,entered_=false,closed_=false;
};
}
