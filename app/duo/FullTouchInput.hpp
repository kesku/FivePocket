// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "FullTouchPolicy.hpp"
#include "AgentInputPolicy.hpp"
#include "apple/gamepad_input.h"
#include "ios_qol_gesture.h" // ios-qol (iPad 231): the long presses on the game picture (android-qol v700 rules)
#include "ios_touch_pad.h" // ios-touchpad (iPad 235): the universal touch controls (touch-controls-spec SPEC v2)
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace recomp::apple::fulltouch {
// App-owned input. This is not a GCVirtualController or a recorded-input lease.
// Writers and observers belong to the constructing main thread. Guest reads
// copy a coherent packet under a mutex and never invoke an OS/UI callback.
enum class Origin {None,Touch,ControllerEvent,Agent};
struct AgentReceipt {
    agentinput::Publication consumed{};
    recomp::input::GamepadState native{};
    agentinput::Result acknowledgement{agentinput::Reason::NoLease};
    agentinput::Completion completion{};
    agentinput::Phase phase=agentinput::Phase::Unavailable;
    int result=0;
};
// One coherent app-source record. Separate phase/completion/publication getters
// cannot serve a handoff when guest confirmation can run concurrently.
struct AgentStatus {
    agentinput::Phase phase=agentinput::Phase::Unavailable;
    agentinput::Publication publication{};
    agentinput::Completion completion{};
    std::uint64_t ownerGeneration=0;
    bool active=false,manualNeutral=false;
};
// Deliberate app-source selection policy, not GCButtonInput.isPressed evidence.
// Small analog trigger noise must not take control away from held touch.
#if defined(APPLE_FULL_TOUCH_MUTANT_TRIGGER_NOISE)
inline constexpr unsigned SelectionTriggerThreshold=1;
#else
inline constexpr unsigned SelectionTriggerThreshold=128;
#endif
class Input final {
public:
    Input() {publish();}
    ~Input() {close();}
    Input(const Input&)=delete;
    Input& operator=(const Input&)=delete;
    bool touch(Element element,double x,double y,double value) noexcept {
        owner();
        if(closed_ || !active_ || !available_ || static_cast<unsigned>(element)>=static_cast<unsigned>(Element::Count))return false;
        const float px=finite(x,-1,1),py=finite(y,-1,1),v=finite(value,0,1);
        using namespace gamepad;
        switch(element) {
        case Element::LeftStick:touch_.leftX=px;touch_.leftY=py;break;
        case Element::RightStick:touch_.rightX=px;touch_.rightY=py;break;
        case Element::Dpad:
            touch_.buttons[Up]=py>0;touch_.buttons[Down]=py<0;
            touch_.buttons[Left]=px<0;touch_.buttons[Right]=px>0;break;
        case Element::A:touch_.buttons[A]=v>0;break;
        case Element::B:touch_.buttons[B]=v>0;break;
        case Element::X:touch_.buttons[X]=v>0;break;
        case Element::Y:touch_.buttons[Y]=v>0;break;
        case Element::L1:touch_.buttons[LeftShoulder]=v>0;break;
        case Element::R1:touch_.buttons[RightShoulder]=v>0;break;
        case Element::L2:touch_.leftTrigger=v;break;
        case Element::R2:touch_.rightTrigger=v;break;
        case Element::Start:touch_.buttons[Start]=v>0;break;
        case Element::Back:touch_.buttons[Back]=v>0;break;
        case Element::L3:touch_.buttons[LeftStick]=v>0;break;
        case Element::R3:touch_.buttons[RightStick]=v>0;break;
        case Element::Count:return false;
        }
#if defined(APPLE_FULL_TOUCH_MUTANT_BINDING)
        if(element==Element::Start) {touch_.buttons[Start]=false;touch_.buttons[Back]=v>0;}
#endif
        // A late release belongs to its old source; it must not adopt touch.
        if(px!=0 || py!=0 || v>0)selected_=Origin::Touch;
#if defined(APPLE_FULL_TOUCH_MUTANT_RELEASE_ADOPTS)
        selected_=Origin::Touch;
#endif
        publish(px!=0 || py!=0 || v>0);return true;
    }
    void controllerEvent(std::uint64_t id,bool connected,const recomp::input::GamepadState& state,
                         const recomp::input::GamepadState& capabilities) noexcept {
        owner();if(closed_)return;
        connected=connected && id!=0;
        const auto before=(controllerConnected_ && id==controllerId_)?controller_:recomp::input::GamepadState{};
        const bool fresh=connected && ((state.buttons&~before.buttons)!=0 ||
            (state.leftTrigger>=SelectionTriggerThreshold && before.leftTrigger<SelectionTriggerThreshold) ||
            (state.rightTrigger>=SelectionTriggerThreshold && before.rightTrigger<SelectionTriggerThreshold));
        controllerId_=connected?id:0;controllerConnected_=connected;
        controller_=connected?state:recomp::input::GamepadState{};
        controllerCaps_=connected?capabilities:recomp::input::GamepadState{};
        const bool agentAxisActivity=connected && (
            std::abs(int(state.leftX))>=8192 || std::abs(int(state.leftY))>=8192 ||
            std::abs(int(state.rightX))>=8192 || std::abs(int(state.rightY))>=8192);
        // Preserve ordinary button/trigger-only arbitration. A meaningful stick
        // move cancels agent ownership, but must not steal touch when disabled.
        bool agentOwned;
        {std::lock_guard lock(mutex_);agentOwned=agent_.leased();}
        if((fresh || (agentOwned && agentAxisActivity)) && active_)selected_=Origin::ControllerEvent;
#if defined(APPLE_FULL_TOUCH_MUTANT_AXIS_ADOPTS)
        if(connected && active_)selected_=Origin::ControllerEvent;
#endif
        if(!connected && selected_==Origin::ControllerEvent)selected_=Origin::Touch;
#if defined(APPLE_AGENT_CONNECTION_MUTANT_AXIS_OWNER)
        publish(fresh);
#else
        publish(fresh || agentAxisActivity);
#endif
    }
    void setTouchAvailable(bool available) noexcept {
        owner();if(closed_)return;
        available_=available;
        if(!available)touch_={};
        if(!available && selected_==Origin::Touch)selected_=Origin::ControllerEvent;
        publish();
    }
    // ios-qol (iPad 231): the long-press gesture (Main.mm's PortQolTouchView drives it on this owner thread, then qolChanged()).
    recomp::apple::qol::Gesture& qol() noexcept {owner();return qol_;}
    void qolChanged() noexcept {owner();if(!closed_)publish();}
    // ios-touchpad (iPad 235): the universal touch controls' model (Main.mm's PortQolTouchView routes the touches and draws it on this
    // owner thread, then qolChanged()). Mode Off: the published pad is the old one (the controller or the FullTouch elements).
    recomp::apple::touchpad::Pad& pad() noexcept {owner();return pad_;}
    // ios-sidemenu (SPEC 12.1): while the side menu is open the game gets none of the input (a neutral pad; the held state is let go
    // when it opens), and the menu reads the physical pad's buttons (XInput bits) to drive itself.
    void setMenuOpen(bool open) noexcept {owner();if(closed_ || menuOpen_==open)return;menuOpen_=open;publish();}
    unsigned controllerButtons() const noexcept {owner();return controllerConnected_?controller_.buttons:0u;}
    static long long qolNow() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void setApplicationActive(bool active) noexcept {
        owner();if(closed_)return;
        if(!active){qol_.cancel();pad_.cancel(qolNow());} // (ios-qol / ios-touchpad: focus lost, everything held released)
        active_=active;
        if(!active) {
#if !defined(APPLE_FULL_TOUCH_MUTANT_BACKGROUND_HELD)
            touch_={};
#endif
        }
        publish();
    }
    void close() noexcept {
        owner();if(closed_)return;
        closed_=true;touch_={};controller_={};controllerCaps_={};controllerConnected_=false;publish();
    }
    int snapshot(std::uint32_t user,recomp::input::GamepadState* result) noexcept {return read(user,result,false);}
    int capabilities(std::uint32_t user,recomp::input::GamepadState* result) noexcept {return read(user,result,true);}
    Origin origin() const noexcept {std::lock_guard lock(mutex_);return publishedOrigin_;}
    // Explicit opt-in; no lease is acquired by constructing the normal source.
    // Caller supplies authoritative stage/progress/observation/monotonic times.
    // Source presence, manual neutrality and activity generations come from this
    // multiplexer, never from a transport claiming the owner was idle.
    agentinput::Result acquireAgent(const agentinput::Acquisition& request,agentinput::Environment e) noexcept {
        owner();agentinput::Result result;bool changed;
        {std::lock_guard lock(mutex_);result=agent_.acquire(request,environmentLocked(e));changed=refreshLocked();
#if defined(APPLE_AGENT_CONNECTION_MUTANT_SNAPSHOT_ACK) || defined(APPLE_AGENT_CONNECTION_MUTANT_CAPS_ACK) || defined(APPLE_AGENT_CONNECTION_MUTANT_OBSERVER_ACK)
            mutantEnvironment_=environmentLocked(e);
#endif
        }
        if(changed)emitPublished();
        return result;
    }
    agentinput::Result agentCommand(std::string_view text,agentinput::Environment e) noexcept {
        owner();agentinput::Result result;bool changed;
        {std::lock_guard lock(mutex_);result=agent_.acceptText(text,environmentLocked(e));changed=refreshLocked();}
        if(changed)emitPublished();
        return result;
    }
    agentinput::Result updateAgent(agentinput::Environment e) noexcept {
        owner();agentinput::Result result;bool changed;
        {std::lock_guard lock(mutex_);result=agent_.update(environmentLocked(e));changed=refreshLocked();}
        if(changed)emitPublished();
        return result;
    }
    // Explicit app authorization withdrawal; preserve manual sources and their
    // activity generation. Publish fallback only after leaving the input lock.
    void revokeAgent(agentinput::Reason reason=agentinput::Reason::Authorization) noexcept {
        owner();bool changed;
        {std::lock_guard lock(mutex_);agent_.revoke(reason);changed=refreshLocked();}
        if(changed)emitPublished();
    }
    agentinput::Phase agentPhase() const noexcept {std::lock_guard lock(mutex_);return agent_.phase();}
    agentinput::Completion agentCompletion() const noexcept {std::lock_guard lock(mutex_);return agent_.completion();}
    agentinput::Publication agentPublication() const noexcept {std::lock_guard lock(mutex_);return publishedAgent_;}
    AgentStatus agentStatus() const noexcept {
        std::lock_guard lock(mutex_);
        return {agent_.phase(),publishedAgent_,agent_.completion(),ownerActivityGeneration_,sourceActive_,manualNeutral_};
    }
    // Menu taps outside the on-screen gamepad use this before their UI action.
    void ownerActivity() noexcept {
        owner();
#if !defined(APPLE_AGENT_CONNECTION_MUTANT_MENU_OWNER)
        publish(true);
#endif
    }
    // NOT an ordinary snapshot. Bind this only at the actual stage consumer.
    // It ticks expiry, copies native state+token, and acknowledges that same
    // publication under the one input lock. No callback/OS/UI runs under it.
    int consumeAgent(std::uint32_t user,recomp::input::GamepadState* state,
                     agentinput::Environment e,AgentReceipt* receipt) noexcept {
        return readForConsumer(user,state,e,receipt,true);
    }
    // Guest reader phase: copy state/token together, but do NOT acknowledge.
    // The runner may override this state and the registry may reject its output.
    int sampleAgent(std::uint32_t user,recomp::input::GamepadState* state,
                    agentinput::Environment e,AgentReceipt* receipt) noexcept {
        return readForConsumer(user,state,e,receipt,false);
    }
    // Only the qualified post-copy hook may call this. Callback never waits for
    // the input lock or calls UI/OS/logging; contention is a missing ACK, not a
    // fabricated success. The caller must retain the exact sample for THIS
    // reader invocation, exclude legacy overrides, and supply real progress.
    agentinput::Result confirmAgent(const AgentReceipt& sample,
        const recomp::input::GamepadState& delivered,agentinput::Environment e) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock())return {agentinput::Reason::Busy};
        if(sample.result!=1 || !sample.consumed.available || publishedOrigin_!=Origin::Agent)
            return {agentinput::Reason::NoLease};
#if !defined(APPLE_AGENT_CONNECTION_MUTANT_FINAL_NATIVE)
        if(recomp::input::encodeGamepadState(sample.native)!=recomp::input::encodeGamepadState(delivered))
            return {agentinput::Reason::ConsumerState};
#else
        (void)delivered;
#endif
#if !defined(APPLE_AGENT_CONNECTION_MUTANT_SAMPLE_NATIVE)
        if(recomp::input::encodeGamepadState(published_)!=recomp::input::encodeGamepadState(sample.native))
            return {agentinput::Reason::ConsumerState};
#endif
        return agent_.acknowledge(sample.consumed,environmentLocked(e));
    }
    std::unique_ptr<gamepad::EventObservation> observe(gamepad::EventObserver callback,void* user) {
        owner();
        auto observation=closed_?nullptr:events_.observe(callback,user);
#if !defined(APPLE_FULL_TOUCH_MUTANT_REPLAY_OMIT)
        // Preserve initial presence without changing the guest packet. A new
        // menu sees neutral/held source state before its first fresh press;
        // held-source admission remains the launcher's responsibility.
        if(observation) {
            emitPublished();
        }
#endif
        return observation;
    }
private:
    int readForConsumer(std::uint32_t user,recomp::input::GamepadState* state,
        agentinput::Environment e,AgentReceipt* receipt,bool acknowledge) noexcept {
        if(receipt)*receipt={};
        if(!state)return -1;
        *state={};if(user)return 0;
        std::lock_guard lock(mutex_);
        if(agent_.leased()) {agent_.update(environmentLocked(e));refreshLocked();}
        if(!publishedConnected_)return 0;
        *state=published_;
        AgentReceipt actual;actual.result=1;actual.native=*state;
        if(publishedOrigin_==Origin::Agent) {
            actual.consumed=publishedAgent_;
#if defined(APPLE_AGENT_CONNECTION_MUTANT_TOKEN)
            ++actual.consumed.token.publicationGeneration;
#endif
#if defined(APPLE_AGENT_CONNECTION_MUTANT_STATE)
            actual.consumed.state.buttons^=0x1000;
#endif
            auto expected=agentNative(actual.consumed.state);expected.packet=state->packet;
            if(recomp::input::encodeGamepadState(expected)==recomp::input::encodeGamepadState(*state)) {
#if defined(APPLE_AGENT_CONNECTION_MUTANT_SAMPLE_ACK)
                (void)acknowledge;
                actual.acknowledgement=agent_.acknowledge(actual.consumed,environmentLocked(e));
#else
                if(acknowledge)actual.acknowledgement=agent_.acknowledge(actual.consumed,environmentLocked(e));
#endif
            }
            else actual.acknowledgement={agentinput::Reason::ConsumerState};
        }
        actual.phase=agent_.phase();actual.completion=agent_.completion();
        if(receipt)*receipt=actual;
        return 1;
    }
    static float finite(double value,double low,double high) noexcept {
        return std::isfinite(value)?float(std::clamp(value,low,high)):0;
    }
    void owner() const noexcept {if(thread_!=std::this_thread::get_id())std::_Exit(4);}
    static recomp::input::GamepadState touchCapabilities() noexcept {
        gamepad::Supported supported;supported.buttons.fill(true);supported.buttons[gamepad::Guide]=false;
        supported.leftX=supported.leftY=supported.rightX=supported.rightY=true;
        supported.leftTrigger=supported.rightTrigger=true;
        return gamepad::capabilities(supported);
    }
    int read(std::uint32_t user,recomp::input::GamepadState* result,bool caps) noexcept {
        if(!result)return -1;
        *result={};std::lock_guard lock(mutex_);
        if(user || !publishedConnected_)return 0;
#if defined(APPLE_AGENT_CONNECTION_MUTANT_SNAPSHOT_ACK)
        if(!caps)fakeAcknowledgementLocked();
#endif
#if defined(APPLE_AGENT_CONNECTION_MUTANT_CAPS_ACK)
        if(caps)fakeAcknowledgementLocked();
#endif
        *result=caps?publishedCaps_:published_;return 1;
    }
    agentinput::Environment environmentLocked(agentinput::Environment e) const noexcept {
#if !defined(APPLE_AGENT_CONNECTION_MUTANT_SOURCE_ACTIVE)
        e.active=e.active && sourceActive_;
#endif
#if !defined(APPLE_AGENT_CONNECTION_MUTANT_MANUAL_NEUTRAL)
        e.manualNeutral=manualNeutral_;
#endif
        e.ownerActivityGeneration=ownerActivityGeneration_;
        return e;
    }
    static bool neutralManual(const recomp::input::GamepadState& s) noexcept {
        return !s.buttons && s.leftTrigger<SelectionTriggerThreshold && s.rightTrigger<SelectionTriggerThreshold &&
            std::abs(int(s.leftX))<8192 && std::abs(int(s.leftY))<8192 &&
            std::abs(int(s.rightX))<8192 && std::abs(int(s.rightY))<8192;
    }
    static recomp::input::GamepadState agentNative(const agentinput::State& state) noexcept {
        recomp::input::GamepadState next;
        next.buttons=static_cast<std::uint16_t>(state.buttons);
        next.leftX=gamepad::stick(float(state.axes[0])/1000.0f);
        next.leftY=gamepad::stick(float(state.axes[1])/1000.0f);
        next.rightX=gamepad::stick(float(state.axes[2])/1000.0f);
        next.rightY=gamepad::stick(float(state.axes[3])/1000.0f);
        next.leftTrigger=gamepad::trigger(float(state.axes[4])/1000.0f);
        next.rightTrigger=gamepad::trigger(float(state.axes[5])/1000.0f);
#if defined(APPLE_AGENT_CONNECTION_MUTANT_MAPPING)
        next.rightY=std::int16_t(-next.rightY);
#endif
        return next;
    }
    bool refreshLocked() noexcept {
        auto origin=manualOrigin_;
        auto next=manual_;auto caps=manualCaps_;bool connected=manualConnected_;
        const auto before=publishedAgent_;
        publishedAgent_={};
        if(agent_.leased() && agent_.publication().available && sourceActive_) {
            origin=Origin::Agent;connected=true;caps=touchCapabilities();
            publishedAgent_=agent_.publication();next=agentNative(publishedAgent_.state);
        }
        next.packet=published_.packet;
        const auto sourceId=origin==Origin::Touch?TouchId:origin==Origin::Agent?AgentId:manualSourceId_;
        // A neutral controller replacement still needs an event so observers
        // can admit the new source before its first button press arrives.
        const bool changed=connected!=publishedConnected_ || origin!=publishedOrigin_ ||
            (connected && sourceId!=publishedSourceId_) ||
#if !defined(APPLE_AGENT_CONNECTION_MUTANT_TOKEN_EVENT)
            publishedAgent_.token!=before.token ||
#endif
            recomp::input::encodeGamepadState(next)!=recomp::input::encodeGamepadState(published_) ||
            recomp::input::encodeGamepadState(caps)!=recomp::input::encodeGamepadState(publishedCaps_);
        if(changed)++next.packet;
        published_=next;publishedCaps_=caps;publishedOrigin_=origin;publishedConnected_=connected;
        publishedSourceId_=sourceId;
#if defined(APPLE_AGENT_CONNECTION_MUTANT_TOKEN_EVENT)
        (void)before;
#endif
        return changed;
    }
    void emitPublished() noexcept {
        recomp::input::GamepadState current;bool connected;std::uint64_t id;
        {std::lock_guard lock(mutex_);current=published_;connected=publishedConnected_;id=publishedSourceId_;}
        // Synchronous menu observers may re-enter consumeAgent(). Never call
        // them under the input lock or treat their mere presence as an ACK.
        events_.publish(id,connected,current);
#if defined(APPLE_AGENT_CONNECTION_MUTANT_OBSERVER_ACK)
        {std::lock_guard lock(mutex_);fakeAcknowledgementLocked();}
#endif
    }
#if defined(APPLE_AGENT_CONNECTION_MUTANT_SNAPSHOT_ACK) || defined(APPLE_AGENT_CONNECTION_MUTANT_CAPS_ACK) || defined(APPLE_AGENT_CONNECTION_MUTANT_OBSERVER_ACK)
    void fakeAcknowledgementLocked() noexcept {
        auto e=mutantEnvironment_;++e.progressRevision;
        agent_.acknowledge(publishedAgent_,e);
    }
    agentinput::Environment mutantEnvironment_{};
#endif
    void publish(bool meaningfulOwner=false) noexcept {
        auto origin=selected_;
        if(!active_ || closed_)origin=Origin::None;
        else if(origin==Origin::Touch && !available_)origin=controllerConnected_?Origin::ControllerEvent:Origin::None;
        else if(origin==Origin::ControllerEvent && !controllerConnected_)origin=available_?Origin::Touch:Origin::None;
        const bool connected=origin!=Origin::None;
        // Owner-thread touch state cannot change before the callback below.
        // Share its mapping with the neutrality check and keep it off the lock.
        const auto touchState=gamepad::map(touch_);
        auto next=origin==Origin::Touch?touchState:
            origin==Origin::ControllerEvent?controller_:recomp::input::GamepadState{};
        if(connected && pad_.mode!=recomp::apple::touchpad::Mode::Off && available_) {
            // ios-touchpad (iPad 235; SPEC v2 5, TouchPad.arbitrate): the physical pad wins input by input - buttons OR'd, a physical stick
            // past XInput's dead zone or a trigger above 30 replaces the touch one; the long-press gesture's right stick before the touch
            // pad's. A fresh physical input hides the touch controls (setting hide=1, the default).
            namespace tp=recomp::apple::touchpad;
            const long long now=qolNow();
            tp::State physical;
            if(controllerConnected_)physical={controller_.buttons,controller_.leftX,controller_.leftY,controller_.rightX,controller_.rightY,
                                              controller_.leftTrigger,controller_.rightTrigger};
            if(tp::freshPhysical(physical,lastPhysical_))pad_.physical(now,true);
            lastPhysical_=physical.buttons;
            qol_.pad(now,physical.buttons);
            // (owner ~21:00: the sticks are off while a wheel is open - the gesture holds or latched one, or the physical pad holds its button)
            pad_.overlay(qol_.wheelOpen() || tp::Pad::physicalWheel(pad_.context(),physical.buttons)); // (SPEC 3.8)
            const auto g=recomp::apple::qol::Gesture::merge(&qol_,physical.buttons,physical.rx,physical.ry);
            const auto m=tp::arbitrate(physical,tp::Gesture{g.buttons,g.rightX,g.rightY},pad_.state(now));
            const auto axis=[](int v){return static_cast<std::int16_t>(std::clamp(v,-32768,32767));};
            const auto trig=[](int v){return static_cast<std::uint8_t>(std::clamp(v,0,255));};
            next.buttons=static_cast<std::uint16_t>(m.buttons);next.leftX=axis(m.lx);next.leftY=axis(m.ly);
            next.rightX=axis(m.rx);next.rightY=axis(m.ry);next.leftTrigger=trig(m.lt);next.rightTrigger=trig(m.rt);
        } else if(connected) { // ios-qol (iPad 231): the pad (on-screen controls or controller) wins; the gesture's buttons OR'd, its stick while it pushes
            qol_.pad(qolNow(),next.buttons);
            const auto m=recomp::apple::qol::Gesture::merge(&qol_,next.buttons,next.rightX,next.rightY);
            next.buttons=static_cast<std::uint16_t>(m.buttons);
            next.rightX=static_cast<std::int16_t>(m.rightX);next.rightY=static_cast<std::int16_t>(m.rightY);
        }
        if(menuOpen_ && connected){const auto packet=next.packet;next=recomp::input::GamepadState{};next.packet=packet;} // (ios-sidemenu)
        auto caps=origin==Origin::Touch?touchCapabilities():
            origin==Origin::ControllerEvent?controllerCaps_:recomp::input::GamepadState{};
        bool changed;
        {
            std::lock_guard lock(mutex_);
            manual_=next;manualCaps_=caps;manualOrigin_=origin;manualConnected_=connected;
            manualSourceId_=controllerId_;
            sourceActive_=active_ && !closed_;
            // Both underlying sources count, not only the selected one.
            manualNeutral_=!pad_.active() && !touchState.buttons && !touchState.leftX && !touchState.leftY &&
                !touchState.rightX && !touchState.rightY && !touchState.leftTrigger && !touchState.rightTrigger &&
                (!controllerConnected_ || neutralManual(controller_));
#if !defined(APPLE_AGENT_CONNECTION_MUTANT_OWNER)
            if(meaningfulOwner) {++ownerActivityGeneration_;agent_.revoke(agentinput::Reason::Owner);}
#else
            (void)meaningfulOwner;
#endif
#if !defined(APPLE_AGENT_CONNECTION_MUTANT_LIFECYCLE)
            if(!sourceActive_)agent_.revoke(agentinput::Reason::Inactive);
            if(closed_)agent_.close();
#endif
            changed=refreshLocked();
        }
#if !defined(APPLE_FULL_TOUCH_MUTANT_OBSERVER_OMIT)
        if(changed)emitPublished();
#else
        (void)changed;
#endif
    }
    static constexpr std::uint64_t TouchId=UINT64_MAX;
    static constexpr std::uint64_t AgentId=UINT64_MAX-1;
    const std::thread::id thread_=std::this_thread::get_id();
    gamepad::Normalized touch_{};
    recomp::apple::qol::Gesture qol_; // ios-qol (iPad 231)
    recomp::apple::touchpad::Pad pad_; // ios-touchpad (iPad 235)
    int lastPhysical_=0;
    bool menuOpen_=false; // (ios-sidemenu)
    bool available_=true,active_=true,closed_=false,controllerConnected_=false;
    std::uint64_t controllerId_=0;
    recomp::input::GamepadState controller_{},controllerCaps_{};
    Origin selected_=Origin::Touch;
    mutable std::mutex mutex_;
    agentinput::Policy agent_;
    agentinput::Publication publishedAgent_{};
    recomp::input::GamepadState manual_{},manualCaps_{};
    Origin manualOrigin_=Origin::None;
    bool manualConnected_=false,manualNeutral_=true,sourceActive_=true;
    std::uint64_t ownerActivityGeneration_=0;
    std::uint64_t manualSourceId_=0;
    std::uint64_t publishedSourceId_=0;
    recomp::input::GamepadState published_{},publishedCaps_{};
    bool publishedConnected_=false;
    Origin publishedOrigin_=Origin::None;
    gamepad::EventChannel events_;
};

// One observer of the existing real adapter, never another GCController handler.
// The adapter must outlive this wrapper. Launcher observes this wrapper instead
// of claiming the adapter's exclusive observation slot a second time.
class Gamepad final {
public:
    struct ControllerSource {
        std::function<std::unique_ptr<gamepad::EventObservation>(gamepad::EventObserver,void*)> observe;
        std::function<int(std::uint32_t,recomp::input::GamepadState*)> capabilities;
    };
    explicit Gamepad(AppleGamepadInput& source):Gamepad(ControllerSource{
        [&source](auto callback,void* user){return source.observe(callback,user);},
        [&source](std::uint32_t user,auto* state){return source.capabilities(user,state);}}) {}
    // Explicit owned-source seam for host integration tests; no GC injection.
    explicit Gamepad(ControllerSource source):source_(std::move(source)) {
        if(!source_.observe || !source_.capabilities)throw std::invalid_argument("full touch controller source missing");
        observation_=source_.observe(controllerEvent,this);
        if(!observation_ || !observation_->active())throw std::invalid_argument("full touch controller observer unavailable");
    }
    ~Gamepad() {close();}
    Gamepad(const Gamepad&)=delete;
    Gamepad& operator=(const Gamepad&)=delete;
    bool touch(Element element,double x,double y,double value) noexcept {return input_.touch(element,x,y,value);}
    recomp::apple::qol::Gesture& qol() noexcept {return input_.qol();}     // ios-qol (iPad 231)
    void qolChanged() noexcept {input_.qolChanged();}
    recomp::apple::touchpad::Pad& pad() noexcept {return input_.pad();} // ios-touchpad (iPad 235)
    void setMenuOpen(bool open) noexcept {input_.setMenuOpen(open);} // ios-sidemenu
    unsigned controllerButtons() const noexcept {return input_.controllerButtons();}
    void setApplicationActive(bool active) noexcept {input_.setApplicationActive(active);}
    void setTouchAvailable(bool available) noexcept {input_.setTouchAvailable(available);}
    void close() noexcept {observation_.reset();input_.close();}
    int snapshot(std::uint32_t user,recomp::input::GamepadState* state) noexcept {return input_.snapshot(user,state);}
    int capabilities(std::uint32_t user,recomp::input::GamepadState* state) noexcept {return input_.capabilities(user,state);}
    Origin origin() const noexcept {return input_.origin();}
    auto acquireAgent(const agentinput::Acquisition& request,agentinput::Environment e) noexcept {return input_.acquireAgent(request,e);}
    auto agentCommand(std::string_view text,agentinput::Environment e) noexcept {return input_.agentCommand(text,e);}
    auto updateAgent(agentinput::Environment e) noexcept {return input_.updateAgent(e);}
    void revokeAgent(agentinput::Reason reason=agentinput::Reason::Authorization) noexcept {input_.revokeAgent(reason);}
    auto agentPhase() const noexcept {return input_.agentPhase();}
    auto agentCompletion() const noexcept {return input_.agentCompletion();}
    auto agentPublication() const noexcept {return input_.agentPublication();}
    auto agentStatus() const noexcept {return input_.agentStatus();}
    void ownerActivity() noexcept {input_.ownerActivity();}
    int consumeAgent(std::uint32_t user,recomp::input::GamepadState* state,agentinput::Environment e,AgentReceipt* receipt) noexcept {
        return input_.consumeAgent(user,state,e,receipt);
    }
    int sampleAgent(std::uint32_t user,recomp::input::GamepadState* state,agentinput::Environment e,AgentReceipt* receipt) noexcept {
        return input_.sampleAgent(user,state,e,receipt);
    }
    auto confirmAgent(const AgentReceipt& sample,const recomp::input::GamepadState& delivered,agentinput::Environment e) noexcept {
        return input_.confirmAgent(sample,delivered,e);
    }
    auto observe(gamepad::EventObserver callback,void* user) {return input_.observe(callback,user);}
private:
    static void controllerEvent(void* user,std::uint64_t id,bool connected,const recomp::input::GamepadState& state) noexcept {
        auto& self=*static_cast<Gamepad*>(user);recomp::input::GamepadState caps{};
        const bool usable=connected && self.source_.capabilities(0,&caps)==1;
        self.input_.controllerEvent(id,usable,state,caps);
    }
    ControllerSource source_;
    Input input_;
    std::unique_ptr<gamepad::EventObservation> observation_;
};
}
