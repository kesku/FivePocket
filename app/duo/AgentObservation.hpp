// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AgentInputPolicy.hpp"
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>

namespace recomp::apple::agentobservation {
namespace ai=agentinput;
using Digest=std::array<std::uint8_t,32>;
using RunId=ai::Nonce;
struct Reference {
    RunId run{};
    std::uint64_t captureId=0;
    Digest sha256{};
    bool operator==(const Reference&) const=default;
};
struct Metadata {
    Reference reference{};
    ai::Stage stage=ai::Stage::Menu;
    std::uint64_t stageGeneration=0,ownerActivityGeneration=0,progress=0;
    std::uint64_t capturedAtMs=0,sourceSampleMs=0,frameSequence=0;
    std::uint32_t width=0,height=0;
    std::size_t byteCount=0;
};
enum class Reason : std::uint8_t {
    None,Busy,Closed,Bounds,Source,Clock,Expired,Identity,Review,Stage,Owner,Progress,Exhausted,Hash
};
struct Result {
    Reason reason=Reason::None;
    explicit operator bool() const noexcept {return reason==Reason::None;}
};
struct Limits {std::uint64_t captures=UINT64_MAX;};

// One outstanding tightly packed RGBA8 image per immutable app run. Construction
// allocates once; invalid configuration is closed, allocation failure throws
// before a usable ledger exists. Methods never allocate or wait for another
// caller. Close is for shutdown AFTER producers/review dispatch have joined.
//
// Only the trusted capture adapter may register actual pixels and their original
// app-clock capture time. Source must include the real multiplexer owner/neutral
// fields, not host assertions. The adapter must supply the coherent source
// snapshot belonging to the actual capture. Observed* fields are deliberately unused: a new
// validated capture must precede Record::image, not depend on an older image.
//
// Clock and SHA256 run under this ledger's lock. They must be bounded, noexcept,
// non-reentrant, and use no UI/I/O/allocation or other blocking lock. SHA256 hashes
// the owned copy, never the producer's borrowed pointer or a claimed digest.
// The actual capture, clock and hashing adapters are not supplied here.
//
// Attestation is only a single-use metadata binding. The transport must establish
// that a separate trusted reviewer actually saw these exact bytes. This class
// neither verifies that human claim nor authorizes input, updates Record::image,
// or acknowledges any command. Ingress must still use the existing input policy.
class Ledger final {
public:
    static constexpr std::size_t MaxBytes=16u*1024u*1024u;
    using Clock=std::uint64_t(*)(void*) noexcept;
    using Sha256=bool(*)(void*,const std::uint8_t*,std::size_t,Digest&) noexcept;
    Ledger(RunId run,std::size_t capacity,Clock clock,void* clockContext,
           Sha256 hash,void* hashContext,Limits limits={})
        :run_(run),capacity_(capacity),clock_(clock),clockContext_(clockContext),
         hash_(hash),hashContext_(hashContext),limits_(limits) {
        if(!ai::validNonce(run_) || !capacity_ || capacity_>MaxBytes || !clock_ || !hash_) {
            closed_=true;return;
        }
        bytes_.reset(new std::uint8_t[capacity_]);
    }
    Ledger(const Ledger&)=delete;
    Ledger& operator=(const Ledger&)=delete;

    Result capture(ai::Environment source,std::uint64_t frameSequence,
                   std::uint64_t capturedAtMs,const std::uint8_t* bytes,std::size_t count,
                   std::uint32_t width,std::uint32_t height,Reference& out) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock())return {Reason::Busy};
        if(closed_)return {Reason::Closed};
        if(phase_!=Phase::Vacant)return {Reason::Busy};
        // Division guards both multiplications, even on 32-bit size_t.
        if(!bytes || !width || !height || width>capacity_/4)return {Reason::Bounds};
        const auto rowBytes=std::size_t(width)*4;
        if(height>capacity_/rowBytes || count!=rowBytes*height)return {Reason::Bounds};
        auto now=timeLocked();if(!now)return {Reason::Clock};
        if(const auto r=sourceReason(source,now);r!=Reason::None)return {r};
        if(!capturedAtMs || capturedAtMs>now || now-capturedAtMs>ai::ObservationFreshMs ||
           capturedAtMs<lastCapturedAt_ || source.sourceSampleMs>capturedAtMs)return {Reason::Expired};
        if(captureId_>=limits_.captures) {closed_=true;return {Reason::Exhausted};}
        std::memcpy(bytes_.get(),bytes,count);
        Digest digest{};
        if(!hash_(hashContext_,bytes_.get(),count,digest))return {Reason::Hash};
        now=timeLocked();if(!now)return {Reason::Clock};
        if(const auto r=sourceReason(source,now);r!=Reason::None)return {r};
        if(now-capturedAtMs>ai::ObservationFreshMs)return {Reason::Expired};
        Metadata next{};
        next.reference={run_,captureId_+1,digest};next.stage=source.stage;
        next.stageGeneration=source.stageGeneration;next.ownerActivityGeneration=source.ownerActivityGeneration;
        next.progress=source.progressRevision;next.capturedAtMs=capturedAtMs;
        next.sourceSampleMs=source.sourceSampleMs;next.frameSequence=frameSequence;
        next.width=width;next.height=height;next.byteCount=count;
        metadata_=next;++captureId_;lastCapturedAt_=capturedAtMs;phase_=Phase::Captured;
        out=next.reference;return {};
    }

    // Copies exact owned bytes. Exporting never refreshes time or marks review.
    Result copyImage(const Reference& reference,std::uint8_t* out,std::size_t capacity,
                     Metadata& metadata) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock())return {Reason::Busy};
        if(closed_)return {Reason::Closed};
        if(!matches(reference))return {Reason::Identity};
        if(!out || capacity<metadata_.byteCount)return {Reason::Bounds};
        std::memcpy(out,bytes_.get(),metadata_.byteCount);metadata=metadata_;return {};
    }
    Result attest(const Reference& reference,const ai::Environment& current) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock())return {Reason::Busy};
        if(closed_)return {Reason::Closed};
        if(!matches(reference))return {Reason::Identity};
        if(phase_!=Phase::Captured)return {Reason::Review};
        if(const auto r=currentReason(current);r!=Reason::None)return {r};
        phase_=Phase::Reviewed;return {};
    }
    // Consumes the matching attestation once; this is NOT command admission.
    Result takeReviewed(const Reference& reference,const ai::Environment& current,Metadata& out) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock())return {Reason::Busy};
        if(closed_)return {Reason::Closed};
        if(!matches(reference))return {Reason::Identity};
        if(phase_!=Phase::Reviewed)return {Reason::Review};
        if(const auto r=currentReason(current);r!=Reason::None)return {r};
        out=metadata_;phase_=Phase::Vacant;return {};
    }
    Result discard() noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock())return {Reason::Busy};
        if(closed_)return {Reason::Closed};
        phase_=Phase::Vacant;return {};
    }
    void close() noexcept {
        std::lock_guard lock(mutex_);closed_=true;phase_=Phase::Vacant;
    }
private:
    enum class Phase : std::uint8_t {Vacant,Captured,Reviewed};
    bool matches(const Reference& reference) const noexcept {
        return phase_!=Phase::Vacant && reference==metadata_.reference;
    }
    static Reason sourceReason(const ai::Environment& e,std::uint64_t now) noexcept {
        if(!ai::validStage(e.stage) || !e.stageGeneration || !e.active)return Reason::Source;
        if(!e.manualNeutral)return Reason::Owner;
        if(!e.progressRevision)return Reason::Progress;
        if(!e.sourceSampleMs || e.sourceSampleMs>now || now-e.sourceSampleMs>ai::SourceFreshMs)return Reason::Source;
        return Reason::None;
    }
    Reason currentReason(const ai::Environment& e) noexcept {
        const auto now=timeLocked();if(!now)return Reason::Clock;
        if(const auto r=sourceReason(e,now);r!=Reason::None)return r;
        if(e.stage!=metadata_.stage || e.stageGeneration!=metadata_.stageGeneration)return Reason::Stage;
        if(e.ownerActivityGeneration!=metadata_.ownerActivityGeneration)return Reason::Owner;
        if(now<metadata_.capturedAtMs || now-metadata_.capturedAtMs>ai::ObservationFreshMs)return Reason::Expired;
        const auto lag=e.stage==ai::Stage::Menu?0:ai::MaxGuestObservationLag;
        if(e.progressRevision<metadata_.progress || e.progressRevision-metadata_.progress>lag)return Reason::Progress;
        return Reason::None;
    }
    std::uint64_t timeLocked() noexcept {
        const auto now=clock_(clockContext_);
        if(!now || now<lastNow_) {closed_=true;phase_=Phase::Vacant;return 0;}
        lastNow_=now;return now;
    }
    const RunId run_;
    const std::size_t capacity_;
    const Clock clock_;
    void* const clockContext_;
    const Sha256 hash_;
    void* const hashContext_;
    const Limits limits_;
    std::unique_ptr<std::uint8_t[]> bytes_;
    std::mutex mutex_;
    Metadata metadata_{};
    Phase phase_=Phase::Vacant;
    std::uint64_t captureId_=0,lastCapturedAt_=0,lastNow_=0;
    bool closed_=false;
};
}
