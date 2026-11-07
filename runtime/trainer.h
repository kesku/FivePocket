#pragma once
// trainer (2026-10-25; shared library code for every port: Thor, Fold QoL, iOS, Duo. Owner 2026-10-25: "two finger swipe the bottom
// screen for a trainer menu ... and the Script mod support in the main options menu"). Single player only.
//
// What it is: a small queue of requests from the app (recomp_trainer_v1) that the library executes on the game's own script thread,
// by calling the game's RECOMPILED native handlers with the arguments its scripts pass. Nothing here writes game state directly.
//
// Where it runs (indirect_call_resolver.cpp, the glue): right after a script's PLAYER_PED_ID (fa92e226) returns through the script
// VM's native call (sub_83439990, return address 0x8343A208; the call context at r1+128: +0 result pointer, +4 argument count,
// +8 argument pointer, +12 = 0). That is the script thread, inside a script's own native call, so the natives see a current script
// thread exactly as for the game's own calls. At most once per game frame (the frame counter GET_FRAME_COUNT reads, u32 0x83C5D10C).
// The glue saves the whole register context and restores it afterwards; argument blocks live below the VM's stack pointer.
//
// Native ABI (from our recompiled handlers, e.g. GET_ENTITY_COORDS sub_82D773A0, CREATE_VEHICLE sub_82E06618): r3 = the context;
// arguments are 4-byte big-endian slots (int, float bits, BOOL, guest pointer); a Vector3 argument is 3 slots; a Vector3 result is
// written as 3 words at the result pointer. Hash = joaat of the lower-case name (the 360 build's rule, natives/registrations.json).
//
// Safety: OFF until files/settings/trainer.txt says "trainer=on" (read at the first API call and at every Sync). Requests and toggles
// act only in gameplay (player playing, no pause menu, no cutscene, no character switch, screen not faded out, player alive, no network
// game). Every request is logged (RENDER_TRAINER). A request that stays blocked for BlockedFramesMax game frames is dropped.
// This header is pure: no guest memory, no platform calls. The glue implements Natives; the tests implement it with mocks.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <mutex>

namespace recomp::trainer {

// The capability marker the apps may look for (zero-terminated); the exports below are the API.
inline constexpr char Marker[]="recomp-trainer-v1";

constexpr std::uint32_t joaat(const char* s) noexcept {
    std::uint32_t h=0;
    for(;*s;++s){unsigned char c=static_cast<unsigned char>(*s);if(c>='A' && c<='Z')c=static_cast<unsigned char>(c+32);
        h+=c;h+=h<<10;h^=h>>6;}
    h+=h<<3;h^=h>>11;h+=h<<15;return h;
}

// The natives the trainer calls (hash = joaat of the name; each handler's argument use was read from its asm, see the README).
namespace n {
constexpr std::uint32_t PlayerId=joaat("PLAYER_ID"),PlayerPedId=joaat("PLAYER_PED_ID"),IsPlayerPlaying=joaat("IS_PLAYER_PLAYING"),
    IsPauseMenuActive=joaat("IS_PAUSE_MENU_ACTIVE"),IsCutsceneActive=joaat("IS_CUTSCENE_ACTIVE"),
    IsSwitchInProgress=joaat("IS_PLAYER_SWITCH_IN_PROGRESS"),IsScreenFadedOut=joaat("IS_SCREEN_FADED_OUT"),IsEntityDead=joaat("IS_ENTITY_DEAD"),
    NetworkGame=joaat("NETWORK_IS_GAME_IN_PROGRESS"),
    SetPlayerInvincible=joaat("SET_PLAYER_INVINCIBLE"),SetWantedLevel=joaat("SET_PLAYER_WANTED_LEVEL"),
    SetWantedLevelNow=joaat("SET_PLAYER_WANTED_LEVEL_NOW"),SetMaxWantedLevel=joaat("SET_MAX_WANTED_LEVEL"),
    ClearWantedLevel=joaat("CLEAR_PLAYER_WANTED_LEVEL"),GetWantedLevel=joaat("GET_PLAYER_WANTED_LEVEL"),
    SetEntityHealth=joaat("SET_ENTITY_HEALTH"),GetEntityMaxHealth=joaat("GET_ENTITY_MAX_HEALTH"),SetPedArmour=joaat("SET_PED_ARMOUR"),
    GetPlayerMaxArmour=joaat("GET_PLAYER_MAX_ARMOUR"),ClearPedBlood=joaat("CLEAR_PED_BLOOD_DAMAGE"),
    SetInfiniteAmmoClip=joaat("SET_PED_INFINITE_AMMO_CLIP"),GiveWeapon=joaat("GIVE_WEAPON_TO_PED"),IsWeaponValid=joaat("IS_WEAPON_VALID"),
    IsModelInCdimage=joaat("IS_MODEL_IN_CDIMAGE"),IsModelAVehicle=joaat("IS_MODEL_A_VEHICLE"),RequestModel=joaat("REQUEST_MODEL"),
    HasModelLoaded=joaat("HAS_MODEL_LOADED"),CreateVehicle=joaat("CREATE_VEHICLE"),SetPedIntoVehicle=joaat("SET_PED_INTO_VEHICLE"),
    VehicleNoLongerNeeded=joaat("SET_VEHICLE_AS_NO_LONGER_NEEDED"),ModelNoLongerNeeded=joaat("SET_MODEL_AS_NO_LONGER_NEEDED"),
    GetEntityCoords=joaat("GET_ENTITY_COORDS"),GetEntityHeading=joaat("GET_ENTITY_HEADING"),IsPedInAnyVehicle=joaat("IS_PED_IN_ANY_VEHICLE"),
    GetVehiclePedIsIn=joaat("GET_VEHICLE_PED_IS_IN"),SetVehicleFixed=joaat("SET_VEHICLE_FIXED"),
    SetVehicleDeformationFixed=joaat("SET_VEHICLE_DEFORMATION_FIXED"),SetVehicleDirt=joaat("SET_VEHICLE_DIRT_LEVEL"),
    SetVehicleOnGround=joaat("SET_VEHICLE_ON_GROUND_PROPERLY"),SetVehicleEngineOn=joaat("SET_VEHICLE_ENGINE_ON"),
    FirstBlipInfoId=joaat("GET_FIRST_BLIP_INFO_ID"),DoesBlipExist=joaat("DOES_BLIP_EXIST"),BlipInfoIdCoord=joaat("GET_BLIP_INFO_ID_COORD"),
    GroundZ=joaat("GET_GROUND_Z_FOR_3D_COORD"),SetPedCoordsKeepVehicle=joaat("SET_PED_COORDS_KEEP_VEHICLE"),
    RequestCollisionAtCoord=joaat("REQUEST_COLLISION_AT_COORD"),SetClockTime=joaat("SET_CLOCK_TIME"),PauseClock=joaat("PAUSE_CLOCK"),
    SetWeatherNowPersist=joaat("SET_WEATHER_TYPE_NOW_PERSIST"),ClearWeatherPersist=joaat("CLEAR_WEATHER_TYPE_PERSIST"),
    SuperJumpThisFrame=joaat("SET_SUPER_JUMP_THIS_FRAME"),RunSprintMultiplier=joaat("SET_RUN_SPRINT_MULTIPLIER_FOR_PLAYER"),
    SwimMultiplier=joaat("SET_SWIM_MULTIPLIER_FOR_PLAYER"),
    // v834 (thor-radio, the Quick Save button without the phone): SET_SAVE_MENU_ACTIVE (sub_82D7F9E8 -> sub_82D7E5E0: with arg 0 it does
    // nothing while the ped is in a vehicle; else the ped's velocities zeroed, then sub_825EBCE0(joaat("FE_MENU_VERSION_SAVEGAME") 809275cc,
    // 1, -1, 0) = the frontend's Save Game menu), GET_MISSION_FLAG, IS_AUTO_SAVE_IN_PROGRESS (refusals).
    SetSaveMenuActive=joaat("SET_SAVE_MENU_ACTIVE"),GetMissionFlag=joaat("GET_MISSION_FLAG"),IsAutoSaveInProgress=joaat("IS_AUTO_SAVE_IN_PROGRESS"),
    // v717 (the Fold's phone pad; bug-fix's contract via main, 2026-10-25): a ringing call (handler 0x82D6CA70) and a call ongoing
    // (handler 0x82D54080), read only while the phone is up (phoneCall).
    IsPedRingtonePlaying=joaat("IS_PED_RINGTONE_PLAYING"),IsMobilePhoneCallOngoing=joaat("IS_MOBILE_PHONE_CALL_ONGOING");
static_assert(IsPedRingtonePlaying==0xfe576ee4u && IsMobilePhoneCallOngoing==0x4ed1400au,"the contract's 360 hashes");
}
// Every hash the trainer may call: the glue records their handlers when the game registers its natives.
inline constexpr std::uint32_t UsedNatives[]{
    n::PlayerId,n::PlayerPedId,n::IsPlayerPlaying,n::IsPauseMenuActive,n::IsCutsceneActive,n::IsSwitchInProgress,n::IsScreenFadedOut,
    n::IsEntityDead,n::NetworkGame,n::SetPlayerInvincible,n::SetWantedLevel,n::SetWantedLevelNow,n::SetMaxWantedLevel,n::ClearWantedLevel,
    n::GetWantedLevel,n::SetEntityHealth,n::GetEntityMaxHealth,n::SetPedArmour,n::GetPlayerMaxArmour,n::ClearPedBlood,
    n::SetInfiniteAmmoClip,n::GiveWeapon,n::IsWeaponValid,n::IsModelInCdimage,n::IsModelAVehicle,n::RequestModel,n::HasModelLoaded,
    n::CreateVehicle,n::SetPedIntoVehicle,n::VehicleNoLongerNeeded,n::ModelNoLongerNeeded,n::GetEntityCoords,n::GetEntityHeading,
    n::IsPedInAnyVehicle,n::GetVehiclePedIsIn,n::SetVehicleFixed,n::SetVehicleDeformationFixed,n::SetVehicleDirt,n::SetVehicleOnGround,
    n::SetVehicleEngineOn,n::FirstBlipInfoId,n::DoesBlipExist,n::BlipInfoIdCoord,n::GroundZ,n::SetPedCoordsKeepVehicle,
    n::RequestCollisionAtCoord,n::SetClockTime,n::PauseClock,n::SetWeatherNowPersist,n::ClearWeatherPersist,n::SuperJumpThisFrame,
    n::RunSprintMultiplier,n::SwimMultiplier,n::SetSaveMenuActive,n::GetMissionFlag,n::IsAutoSaveInProgress,
    n::IsPedRingtonePlaying,n::IsMobilePhoneCallOngoing};
inline bool used(std::uint32_t hash) noexcept {
    return std::find(std::begin(UsedNatives),std::end(UsedNatives),hash)!=std::end(UsedNatives);
}

// One call of a game native. Implemented by the glue (guest memory, the recompiled handler) and by the tests (mocks).
struct Natives {
    virtual ~Natives()=default;
    // n argument words (n <= 12); up to 3 result words in out (zeroed first). False: the native is not available (not registered).
    virtual bool call(std::uint32_t hash,const std::uint32_t* args,unsigned n,std::uint32_t* out)=0;
    // Guest scratch storage valid until the end of this tick: a zero-terminated copy of s / one word; 0 = no room.
    virtual std::uint32_t text(const char* s)=0;
    virtual std::uint32_t word(std::uint32_t value)=0;
    virtual std::uint32_t load(std::uint32_t address)=0;   // a scratch word back (big-endian guest word)
};
inline std::uint32_t bits(float f) noexcept {std::uint32_t w;std::memcpy(&w,&f,4);return w;}
inline float asFloat(std::uint32_t w) noexcept {float f;std::memcpy(&f,&w,4);return f;}

// ---------------------------------------------------------------------------------------------------------------- the API's words
enum Action : int {
    Sync=0,            // re-read files/settings/trainer.txt; allowed while off
    Toggle=1,          // a0 = Toggles bit index, a1 = 0 off / 1 on
    Wanted=2,          // a0 = 0..5 stars, now
    Heal=3,            // full health, full armour, blood cleaned
    GiveWeapons=4,     // every weapon of WeaponList that the game knows, with ammo
    SpawnVehicle=5,    // a1 = 0: a0 = the model hash (joaat of the model name; any 32-bit pattern); a1 = 1: a0 = index into VehicleList
    FixVehicle=6,      // the current vehicle repaired, deformation fixed and cleaned
    TeleportWaypoint=7,// to the map's waypoint, onto the ground
    TeleportLandmark=8,// a0 = index into LandmarkList
    SetTime=9,         // a0 = hour 0..23, a1 = minute 0..59
    SetWeather=10,     // a0 = index into WeatherList (kept until ClearWeather)
    ClearWeather=11,   // back to the game's own weather cycle
    OpenSaveMenu=12,   // v834: the game's Save Game menu now (SET_SAVE_MENU_ACTIVE); allowed while the trainer is OFF (the Quick Save button);
                       // refused on a mission, with stars, during an autosave
    ActionCount=13
};
enum ToggleBit : int {God=0,NeverWanted=1,InfiniteAmmo=2,SuperJump=3,FastRun=4,FastSwim=5,FreezeTime=6,ToggleCount=7};
inline const char* toggleName(int t) noexcept {
    static const char* const names[]={"god","never-wanted","infinite-ammo","super-jump","fast-run","fast-swim","freeze-time"};
    return t>=0 && t<ToggleCount?names[t]:"?";
}
inline const char* actionName(int a) noexcept {
    static const char* const names[]={"sync","toggle","wanted","heal","give-weapons","spawn-vehicle","fix-vehicle","teleport-waypoint",
        "teleport-landmark","set-time","set-weather","clear-weather","open-save-menu"};
    return a>=0 && a<ActionCount?names[a]:"?";
}
// Return values of recomp_trainer_v1 (>= 0: the request's sequence number).
constexpr int ErrOff=-1,ErrArgs=-2,ErrFull=-3;
// recomp_trainer_state_v1: toggles in bits 0..6, then these.
constexpr std::uint32_t StateEnabled=1u<<16,StateGameplay=1u<<17,StateBusy=1u<<18,StateWeatherLocked=1u<<19;

// ------------------------------------------------------------------------------------------------------------------ the lists
struct Vehicle {const char* label;const char* model;};
// Popular models of the base game (the 360 disc). A model the game does not have is refused at spawn time (IS_MODEL_IN_CDIMAGE).
inline constexpr std::array<Vehicle,24> VehicleList{{
    {"Adder","adder"},{"Entity XF","entityxf"},{"Infernus","infernus"},{"Cheetah","cheetah"},{"Banshee","banshee"},{"Comet","comet2"},
    {"Sultan","sultan"},{"Elegy RH8","elegy2"},{"Bati 801","bati"},{"Sanchez","sanchez"},{"Faggio","faggio2"},{"BMX","bmx"},
    {"Police Cruiser","police"},{"Ambulance","ambulance"},{"Fire Truck","firetruk"},{"Bus","bus"},{"Rhino Tank","rhino"},
    {"Buzzard","buzzard"},{"Maverick","maverick"},{"Cargobob","cargobob"},{"P-996 Lazer","lazer"},{"Duster","duster"},
    {"Jetmax","jetmax"},{"Seashark","seashark"}}};
struct Landmark {const char* label;float x,y,z;};
// Public map coordinates (approximate): the teleport probes the ground near z, so a few metres off is harmless.
inline constexpr std::array<Landmark,12> LandmarkList{{
    {"Michael's house",-852.4f,160.0f,65.6f},{"Franklin's house",7.4f,535.5f,176.0f},{"Trevor's trailer",1985.7f,3812.2f,32.2f},
    {"Maze Bank Tower roof",-75.0f,-818.2f,326.2f},{"Mount Chiliad",501.8f,5604.0f,797.9f},{"Los Santos Airport",-1102.3f,-2894.5f,13.9f},
    {"Sandy Shores airfield",1747.0f,3273.7f,41.1f},{"Fort Zancudo",-2047.4f,3132.1f,32.8f},{"Del Perro Pier",-1850.1f,-1231.8f,13.0f},
    {"Vinewood sign",711.4f,1198.1f,348.5f},{"Paleto Bay",-446.0f,6012.0f,31.7f},{"Los Santos Customs",-365.4f,-131.8f,37.9f}}};
inline constexpr std::array<const char*,13> WeatherList{"EXTRASUNNY","CLEAR","CLOUDS","SMOG","FOGGY","OVERCAST","RAIN","THUNDER",
    "CLEARING","NEUTRAL","SNOW","BLIZZARD","SNOWLIGHT"};
// Base-game weapons; IS_WEAPON_VALID filters any this build lacks.
inline constexpr std::array<const char*,36> WeaponList{"WEAPON_KNIFE","WEAPON_NIGHTSTICK","WEAPON_HAMMER","WEAPON_BAT","WEAPON_CROWBAR",
    "WEAPON_GOLFCLUB","WEAPON_PISTOL","WEAPON_COMBATPISTOL","WEAPON_APPISTOL","WEAPON_PISTOL50","WEAPON_MICROSMG","WEAPON_SMG",
    "WEAPON_ASSAULTSMG","WEAPON_ASSAULTRIFLE","WEAPON_CARBINERIFLE","WEAPON_ADVANCEDRIFLE","WEAPON_MG","WEAPON_COMBATMG",
    "WEAPON_PUMPSHOTGUN","WEAPON_SAWNOFFSHOTGUN","WEAPON_ASSAULTSHOTGUN","WEAPON_BULLPUPSHOTGUN","WEAPON_STUNGUN","WEAPON_SNIPERRIFLE",
    "WEAPON_HEAVYSNIPER","WEAPON_GRENADELAUNCHER","WEAPON_RPG","WEAPON_MINIGUN","WEAPON_GRENADE","WEAPON_STICKYBOMB",
    "WEAPON_SMOKEGRENADE","WEAPON_BZGAS","WEAPON_MOLOTOV","WEAPON_FIREEXTINGUISHER","WEAPON_PETROLCAN","WEAPON_FLARE"};
constexpr std::uint32_t WeaponAmmo=9999;
enum List : int {Vehicles=0,Landmarks=1,Weathers=2,Toggles=3,Actions=4};
// The label of one list entry (for the app's menu): the list's size, writing the label when index is in range.
inline int listEntry(int list,int index,char* out,int cap) noexcept {
    const char* label=nullptr;int count=0;
    switch(list) {
    case Vehicles: count=int(VehicleList.size());if(index>=0 && index<count)label=VehicleList[std::size_t(index)].label;break;
    case Landmarks: count=int(LandmarkList.size());if(index>=0 && index<count)label=LandmarkList[std::size_t(index)].label;break;
    case Weathers: count=int(WeatherList.size());if(index>=0 && index<count)label=WeatherList[std::size_t(index)];break;
    case Toggles: count=ToggleCount;if(index>=0 && index<count)label=toggleName(index);break;
    case Actions: count=ActionCount;if(index>=0 && index<count)label=actionName(index);break;
    default: return -1;
    }
    if(label && out && cap>0)std::snprintf(out,std::size_t(cap),"%s",label);
    return count;
}

// files/settings/trainer.txt: words "key=value"; "trainer=on" (or 1 / true / yes) enables, anything else = off.
inline bool parseEnabled(const char* text,std::size_t n) noexcept {
    std::size_t i=0;bool on=false;
    while(i<n) {
        while(i<n && (text[i]==' ' || text[i]=='\n' || text[i]=='\r' || text[i]=='\t'))++i;
        std::size_t j=i;while(j<n && text[j]!=' ' && text[j]!='\n' && text[j]!='\r' && text[j]!='\t')++j;
        char word[48]{};const std::size_t len=std::min<std::size_t>(j-i,sizeof word-1);
        for(std::size_t k=0;k<len;++k){char c=text[i+k];if(c>='A' && c<='Z')c=char(c+32);word[k]=c;}
        if(!std::strncmp(word,"trainer=",8)) {
            const char* v=word+8;
            on=!std::strcmp(v,"on") || !std::strcmp(v,"1") || !std::strcmp(v,"true") || !std::strcmp(v,"yes");
        }
        i=j;
    }
    return on;
}

// ------------------------------------------------------------------------------------------------------------------ the engine
struct Request {int action=0,a0=0,a1=0,a2=0;int seq=0;std::uint32_t blockedFrames=0;};
constexpr std::size_t QueueMax=16;
constexpr std::uint32_t BlockedFramesMax=150;      // ~5 s at 30 fps: a request blocked this long is dropped
constexpr std::uint32_t ModelWaitFramesMax=300;    // a model that does not load in ~10 s: the spawn fails
constexpr std::uint32_t CollisionWaitFrames=3;     // frames between a teleport probe's move and its ground test
constexpr std::uint32_t WaypointSprite=8;          // the map waypoint's blip sprite
inline constexpr std::array<float,17> ProbeHeights{100,150,50,0,200,250,300,350,400,450,500,550,600,650,700,750,800};

class Trainer {
public:
    using LogFn=void(*)(const char* line);
    explicit Trainer(LogFn log=nullptr) noexcept:log_(log) {}

    // ---- the app's side (any thread)
    void setEnabled(bool on) noexcept {
        const bool was=enabled_.exchange(on,std::memory_order_acq_rel);
        if(was!=on)logf("RENDER_TRAINER enabled=%d",on?1:0);
        if(!on) { // everything back off at the next safe point: toggles off, queue dropped
            std::lock_guard l(m_); // (v834: a pending save-menu request stays)
            for(auto it=queue_.begin();it!=queue_.end();)if(it->action!=OpenSaveMenu)it=queue_.erase(it);else ++it;
            toggles_.store(0,std::memory_order_relaxed);
        }
        refreshWork();
    }
    bool enabled() const noexcept {return enabled_.load(std::memory_order_acquire);}
    // A request: >= 0 its sequence number, else ErrOff / ErrArgs / ErrFull.
    int request(int action,int a0,int a1,int a2) noexcept {
        if(action==Sync)return 0; // (handled by the export: it re-reads the settings file)
        if(!enabled() && action!=OpenSaveMenu)return ErrOff; // (v834: the save menu is not a cheat)
        if(!valid(action,a0,a1))return ErrArgs;
        if(action==Toggle) { // toggles are state, not queued work
            const std::uint32_t bit=1u<<a0;
            if(a1)toggles_.fetch_or(bit,std::memory_order_acq_rel);else toggles_.fetch_and(~bit,std::memory_order_acq_rel);
            const int seq=++seq_;
            logf("RENDER_TRAINER request seq=%d action=toggle %s=%d",seq,toggleName(a0),a1?1:0);
            refreshWork();
            return seq;
        }
        std::lock_guard l(m_);
        if(queue_.size()>=QueueMax)return ErrFull;
        Request r{action,a0,a1,a2,++seq_,0};
        queue_.push_back(r);
        logf("RENDER_TRAINER request seq=%d action=%s a0=%d a1=%d",r.seq,actionName(action),a0,a1);
        work_.store(true,std::memory_order_release);
        return r.seq;
    }
    std::uint32_t state() const noexcept {
        std::uint32_t s=toggles_.load(std::memory_order_relaxed)&((1u<<ToggleCount)-1);
        if(enabled())s|=StateEnabled;
        if(gameplay_.load(std::memory_order_relaxed))s|=StateGameplay;
        if(busy_.load(std::memory_order_relaxed))s|=StateBusy;
        if(weatherLocked_.load(std::memory_order_relaxed))s|=StateWeatherLocked;
        return s;
    }
    // The last result line ("seq=N ok|fail|blocked|dropped ...") and its sequence number.
    int status(char* out,int cap) const noexcept {
        std::lock_guard l(m_);
        if(out && cap>0)std::snprintf(out,std::size_t(cap),"%s",status_);
        return statusSeq_;
    }
    // A save was committed (the save store): logged when toggles are on (the owner allows it; the log records it).
    void noteSave(const char* name) noexcept {
        const auto t=toggles_.load(std::memory_order_relaxed);
        if(enabled() && t)logf("RENDER_TRAINER save name=%s toggles=%#x (saved with trainer toggles on)",name?name:"?",t);
    }

    // ---- the script thread's side
    // Anything to do? (one relaxed load: the glue's test before it builds a tick)
    bool wanted() const noexcept {return work_.load(std::memory_order_relaxed);}
    // One tick at the safe point (at most once per game frame; the glue checks the frame counter).
    void tick(Natives& nat,std::uint32_t frame) noexcept {
        if(!enabled()) { // turned off: a running job ends, owed off actions run (in gameplay)
            if(job_.kind!=Job::None)finishJob(nat,false,"trainer-off",true);
            if(applied_)offAll(nat);
            bool pending=false;{std::lock_guard l(m_);pending=!queue_.empty();}
            if(pending) { // v834: a save-menu request (the only kind queued while off)
                const Gate g=gate(nat);
                if(!g.ok)ageQueue(g.reason);
                else {Request r{};bool have=false;
                    {std::lock_guard l(m_);if(!queue_.empty()){r=queue_.front();queue_.pop_front();have=true;}}
                    if(have && r.action==OpenSaveMenu)start(nat,g,r,frame);}
            }
            publishScriptState();return;
        }
        const Gate g=gate(nat);
        gameplay_.store(g.ok,std::memory_order_relaxed);
        if(!g.ok) {
            if(job_.kind!=Job::None)finishJob(nat,false,g.reason,true);
            ageQueue(g.reason);
            publishScriptState();
            return;
        }
        applyToggles(nat,g);
        if(job_.kind!=Job::None)runJob(nat,g,frame);
        else {
            Request r{};bool have=false;
            {std::lock_guard l(m_);if(!queue_.empty()){r=queue_.front();queue_.pop_front();have=true;}}
            if(have)start(nat,g,r,frame);
        }
        publishScriptState();
    }

private:
    struct Gate {bool ok=false;const char* reason="";std::uint32_t player=0,ped=0;};
    struct Job {
        enum Kind:std::uint8_t {None,Spawn,Teleport} kind=None;
        int seq=0;std::uint32_t model=0,startFrame=0,stepFrame=0;unsigned step=0;
        float x=0,y=0,zHint=0;bool hasHint=false;float fromX=0,fromY=0,fromZ=0;const char* what="";
    };

    static bool valid(int action,int a0,int a1) noexcept {
        switch(action) {
        case Toggle: return a0>=0 && a0<ToggleCount && (a1==0 || a1==1);
        case Wanted: return a0>=0 && a0<=5;
        case SpawnVehicle: return a1==0 || (a1==1 && a0>=0 && a0<int(VehicleList.size()));
        case TeleportLandmark: return a0>=0 && a0<int(LandmarkList.size());
        case SetTime: return a0>=0 && a0<=23 && a1>=0 && a1<=59;
        case SetWeather: return a0>=0 && a0<int(WeatherList.size());
        case Heal: case GiveWeapons: case FixVehicle: case TeleportWaypoint: case ClearWeather: case OpenSaveMenu: return true;
        default: return false;
        }
    }
    // Called by both threads: reads atomics only. The script thread publishes busy_ / owed_ first (publishScriptState).
    void refreshWork() noexcept {
        bool w=busy_.load(std::memory_order_relaxed) || owed_.load(std::memory_order_relaxed); // a job, or off actions still owed
        if(!w && enabled())w=toggles_.load(std::memory_order_relaxed)!=0;
        if(!w){std::lock_guard l(m_);w=!queue_.empty();} // (v834: off, the queue holds save-menu requests only)
        work_.store(w,std::memory_order_release);
    }
    void publishScriptState() noexcept {
        busy_.store(job_.kind!=Job::None,std::memory_order_relaxed);owed_.store(applied_!=0,std::memory_order_relaxed);
        refreshWork();
    }
    template<class... A> void logf(const char* f,A... a) const noexcept {
        if(!log_)return;
        char line[240];std::snprintf(line,sizeof line,f,a...);log_(line);
    }
    void result(int seq,const char* kind,const char* what,const char* detail) noexcept {
        {
            std::lock_guard l(m_);
            std::snprintf(status_,sizeof status_,"seq=%d %s %s%s%s",seq,kind,what,detail && *detail?" ":"",detail?detail:"");
            statusSeq_=seq;
        }
        logf("RENDER_TRAINER result seq=%d %s %s%s%s",seq,kind,what,detail && *detail?" ":"",detail?detail:"");
    }

    // A native returning one word (0 when it is unavailable: the callers treat 0 as "no").
    static std::uint32_t call1(Natives& nat,std::uint32_t hash,std::initializer_list<std::uint32_t> a,bool* ok=nullptr) noexcept {
        std::uint32_t out[3]{};const bool r=nat.call(hash,a.begin(),unsigned(a.size()),out);
        if(ok)*ok=r;
        return r?out[0]:0u;
    }
    static void call0(Natives& nat,std::uint32_t hash,std::initializer_list<std::uint32_t> a) noexcept {
        std::uint32_t out[3]{};(void)nat.call(hash,a.begin(),unsigned(a.size()),out);
    }

    Gate gate(Natives& nat) noexcept {
        Gate g;bool ok=false;
        g.player=call1(nat,n::PlayerId,{},&ok);
        if(!ok){g.reason="natives-unavailable";return g;}
        g.ped=call1(nat,n::PlayerPedId,{});
        if(call1(nat,n::NetworkGame,{})){g.reason="network-game";return g;}
        if(!call1(nat,n::IsPlayerPlaying,{g.player})){g.reason="no-player";return g;}
        if(!g.ped){g.reason="no-ped";return g;}
        if(call1(nat,n::IsPauseMenuActive,{})){g.reason="pause-menu";return g;}
        if(call1(nat,n::IsCutsceneActive,{})){g.reason="cutscene";return g;}
        if(call1(nat,n::IsSwitchInProgress,{})){g.reason="character-switch";return g;}
        if(call1(nat,n::IsScreenFadedOut,{})){g.reason="faded-out";return g;}
        if(call1(nat,n::IsEntityDead,{g.ped})){g.reason="player-dead";return g;}
        g.ok=true;return g;
    }

    void applyToggles(Natives& nat,const Gate& g) noexcept {
        const std::uint32_t t=toggles_.load(std::memory_order_acquire);
        const auto on=[&](int b){return (t>>b)&1u;};
        // turned off since they were applied: their off action, once
        const std::uint32_t off=applied_&~t;
        if(off)offToggles(nat,g,off);
        if(on(God))call0(nat,n::SetPlayerInvincible,{g.player,1});
        if(on(NeverWanted)) {
            if(call1(nat,n::GetWantedLevel,{g.player}))call0(nat,n::ClearWantedLevel,{g.player});
            call0(nat,n::SetMaxWantedLevel,{0});
        }
        if(on(InfiniteAmmo))call0(nat,n::SetInfiniteAmmoClip,{g.ped,1});
        if(on(SuperJump))call0(nat,n::SuperJumpThisFrame,{g.player});
        if(on(FastRun))call0(nat,n::RunSprintMultiplier,{g.player,bits(1.49f)});
        if(on(FastSwim))call0(nat,n::SwimMultiplier,{g.player,bits(1.49f)});
        if(on(FreezeTime))call0(nat,n::PauseClock,{1});
        const std::uint32_t newly=t&~applied_;
        for(int b=0;b<ToggleCount;++b)if((newly>>b)&1u)logf("RENDER_TRAINER toggle %s=on applied",toggleName(b));
        applied_=t;
    }
    void offToggles(Natives& nat,const Gate& g,std::uint32_t off) noexcept {
        const auto has=[&](int b){return (off>>b)&1u;};
        if(has(God))call0(nat,n::SetPlayerInvincible,{g.player,0});
        if(has(NeverWanted))call0(nat,n::SetMaxWantedLevel,{5});
        if(has(InfiniteAmmo))call0(nat,n::SetInfiniteAmmoClip,{g.ped,0});
        if(has(FastRun))call0(nat,n::RunSprintMultiplier,{g.player,bits(1.0f)});
        if(has(FastSwim))call0(nat,n::SwimMultiplier,{g.player,bits(1.0f)});
        if(has(FreezeTime))call0(nat,n::PauseClock,{0});
        for(int b=0;b<ToggleCount;++b)if((off>>b)&1u)logf("RENDER_TRAINER toggle %s=off applied",toggleName(b));
        applied_&=~off;
    }
    // The trainer was turned off: every applied toggle's off action (only in gameplay, like every action), the weather released.
    void offAll(Natives& nat) noexcept {
        const Gate g=gate(nat);
        if(g.ok)offToggles(nat,g,applied_);
    }

    void ageQueue(const char* reason) noexcept {
        std::lock_guard l(m_);
        for(auto it=queue_.begin();it!=queue_.end();) {
            if(++it->blockedFrames>=BlockedFramesMax) {
                const int seq=it->seq;const int action=it->action;it=queue_.erase(it);
                std::snprintf(status_,sizeof status_,"seq=%d dropped %s blocked=%s",seq,actionName(action),reason);statusSeq_=seq;
                logf("RENDER_TRAINER result seq=%d dropped %s blocked=%s",seq,actionName(action),reason);
            } else ++it;
        }
    }

    void start(Natives& nat,const Gate& g,const Request& r,std::uint32_t frame) noexcept {
        const char* what=actionName(r.action);
        switch(r.action) {
        case Wanted:
            if(r.a0>0 && (toggles_.load(std::memory_order_relaxed)>>NeverWanted)&1u){result(r.seq,"fail",what,"never-wanted-is-on");return;}
            if(r.a0==0)call0(nat,n::ClearWantedLevel,{g.player});
            else {call0(nat,n::SetWantedLevel,{g.player,std::uint32_t(r.a0),0});call0(nat,n::SetWantedLevelNow,{g.player,0});}
            {char d[16];std::snprintf(d,sizeof d,"stars=%d",r.a0);result(r.seq,"ok",what,d);}
            return;
        case Heal: {
            const std::uint32_t maxHealth=call1(nat,n::GetEntityMaxHealth,{g.ped});
            if(maxHealth)call0(nat,n::SetEntityHealth,{g.ped,maxHealth});
            const std::uint32_t maxArmour=call1(nat,n::GetPlayerMaxArmour,{g.player});
            call0(nat,n::SetPedArmour,{g.ped,maxArmour});
            call0(nat,n::ClearPedBlood,{g.ped});
            char d[40];std::snprintf(d,sizeof d,"health=%u armour=%u",maxHealth,maxArmour);result(r.seq,"ok",what,d);
            return;}
        case GiveWeapons: {
            unsigned given=0;
            for(const char* w:WeaponList) {
                const std::uint32_t h=joaat(w);
                if(!call1(nat,n::IsWeaponValid,{h}))continue;
                call0(nat,n::GiveWeapon,{g.ped,h,WeaponAmmo,0,0});++given;
            }
            char d[24];std::snprintf(d,sizeof d,"weapons=%u",given);result(r.seq,given?"ok":"fail",what,d);
            return;}
        case FixVehicle: {
            if(!call1(nat,n::IsPedInAnyVehicle,{g.ped,0})){result(r.seq,"fail",what,"not-in-a-vehicle");return;}
            const std::uint32_t veh=call1(nat,n::GetVehiclePedIsIn,{g.ped,0});
            if(!veh){result(r.seq,"fail",what,"no-vehicle");return;}
            call0(nat,n::SetVehicleFixed,{veh});call0(nat,n::SetVehicleDeformationFixed,{veh});call0(nat,n::SetVehicleDirt,{veh,bits(0.0f)});
            result(r.seq,"ok",what,"");
            return;}
        case SetTime:
            call0(nat,n::SetClockTime,{std::uint32_t(r.a0),std::uint32_t(r.a1),0});
            {char d[24];std::snprintf(d,sizeof d,"time=%02d:%02d",r.a0,r.a1);result(r.seq,"ok",what,d);}
            return;
        case SetWeather: {
            const char* name=WeatherList[std::size_t(r.a0)];
            const std::uint32_t s=nat.text(name);
            if(!s){result(r.seq,"fail",what,"no-scratch");return;}
            call0(nat,n::SetWeatherNowPersist,{s});weatherLocked_.store(true,std::memory_order_relaxed);
            result(r.seq,"ok",what,name);
            return;}
        case ClearWeather:
            call0(nat,n::ClearWeatherPersist,{});weatherLocked_.store(false,std::memory_order_relaxed);
            result(r.seq,"ok",what,"");
            return;
        case OpenSaveMenu: // v834: refused where the game's own phone offers no save (a mission, stars, an autosave running)
            if(call1(nat,n::GetMissionFlag,{})){result(r.seq,"fail",what,"on-a-mission");return;}
            if(call1(nat,n::GetWantedLevel,{g.player})){result(r.seq,"fail",what,"wanted");return;}
            if(call1(nat,n::IsAutoSaveInProgress,{})){result(r.seq,"fail",what,"autosave");return;}
            call0(nat,n::SetSaveMenuActive,{1}); // (1: also in a vehicle - with 0 the native does nothing there)
            result(r.seq,"ok",what,"");
            return;
        case SpawnVehicle: {
            const std::uint32_t model=r.a1==1?joaat(VehicleList[std::size_t(r.a0)].model):std::uint32_t(r.a0);
            if(!call1(nat,n::IsModelInCdimage,{model}) || !call1(nat,n::IsModelAVehicle,{model})){
                char d[32];std::snprintf(d,sizeof d,"model=%08x not-a-vehicle",model);result(r.seq,"fail",what,d);return;}
            call0(nat,n::RequestModel,{model});
            job_=Job{};job_.kind=Job::Spawn;job_.seq=r.seq;job_.model=model;job_.startFrame=frame;job_.what=what;
            logf("RENDER_TRAINER spawn seq=%d model=%08x requested",r.seq,model);
            return;}
        case TeleportWaypoint: case TeleportLandmark: {
            float x=0,y=0,z=0;bool hint=false;
            if(r.action==TeleportWaypoint) {
                const std::uint32_t blip=call1(nat,n::FirstBlipInfoId,{WaypointSprite});
                if(!blip || !call1(nat,n::DoesBlipExist,{blip})){result(r.seq,"fail",what,"no-waypoint");return;}
                std::uint32_t out[3]{};
                if(!nat.call(n::BlipInfoIdCoord,&blip,1,out)){result(r.seq,"fail",what,"no-coords");return;}
                x=asFloat(out[0]);y=asFloat(out[1]);
            } else {const auto& l=LandmarkList[std::size_t(r.a0)];x=l.x;y=l.y;z=l.z;hint=true;}
            if(!std::isfinite(x) || !std::isfinite(y) || std::fabs(x)>20000.f || std::fabs(y)>20000.f){result(r.seq,"fail",what,"bad-coords");return;}
            std::uint32_t here[3]{};const std::uint32_t args[2]{g.ped,1};
            (void)nat.call(n::GetEntityCoords,args,2,here);
            job_=Job{};job_.kind=Job::Teleport;job_.seq=r.seq;job_.x=x;job_.y=y;job_.zHint=z;job_.hasHint=hint;job_.startFrame=frame;
            job_.fromX=asFloat(here[0]);job_.fromY=asFloat(here[1]);job_.fromZ=asFloat(here[2]);job_.what=what;job_.step=0;
            moveProbe(nat,g,frame);
            return;}
        default: result(r.seq,"fail",what,"unknown");return;
        }
    }

    float probeHeight(unsigned step) const noexcept {
        if(job_.hasHint){if(step==0)return job_.zHint+5.0f;--step;}
        return step<ProbeHeights.size()?ProbeHeights[step]:-1.0f;
    }
    unsigned probeCount() const noexcept {return unsigned(ProbeHeights.size())+(job_.hasHint?1u:0u);}
    void moveProbe(Natives& nat,const Gate& g,std::uint32_t frame) noexcept {
        const float h=probeHeight(job_.step);
        call0(nat,n::SetPedCoordsKeepVehicle,{g.ped,bits(job_.x),bits(job_.y),bits(h)});
        call0(nat,n::RequestCollisionAtCoord,{bits(job_.x),bits(job_.y),bits(h)});
        job_.stepFrame=frame;
    }

    void runJob(Natives& nat,const Gate& g,std::uint32_t frame) noexcept {
        if(job_.kind==Job::Spawn) {
            if(!call1(nat,n::HasModelLoaded,{job_.model})) {
                if(frame-job_.startFrame>=ModelWaitFramesMax)finishJob(nat,false,"model-timeout",false);
                else call0(nat,n::RequestModel,{job_.model});
                return;
            }
            std::uint32_t at[3]{};const std::uint32_t args[2]{g.ped,1};
            (void)nat.call(n::GetEntityCoords,args,2,at);
            const float heading=asFloat(call1(nat,n::GetEntityHeading,{g.ped}));
            float x=asFloat(at[0]),y=asFloat(at[1]);const float z=asFloat(at[2]);
            if(call1(nat,n::IsPedInAnyVehicle,{g.ped,0})) { // in a vehicle: the new one 8 m ahead (the old one stays behind)
                const float rad=heading*3.14159265f/180.0f;x+=-std::sin(rad)*8.0f;y+=std::cos(rad)*8.0f;
            }
            const std::uint32_t veh=call1(nat,n::CreateVehicle,{job_.model,bits(x),bits(y),bits(z),bits(heading),0,0});
            if(!veh){finishJob(nat,false,"create-failed",false);return;}
            call0(nat,n::SetVehicleOnGround,{veh});
            call0(nat,n::SetPedIntoVehicle,{g.ped,veh,std::uint32_t(-1)});
            call0(nat,n::SetVehicleEngineOn,{veh,1,1});
            const std::uint32_t handle=nat.word(veh);
            if(handle)call0(nat,n::VehicleNoLongerNeeded,{handle}); // the world keeps it while used; the game cleans it up later
            char d[40];std::snprintf(d,sizeof d,"model=%08x vehicle=%08x",job_.model,veh);
            finishJob(nat,true,d,false);
            return;
        }
        if(job_.kind==Job::Teleport) {
            if(frame-job_.stepFrame<CollisionWaitFrames)return;
            const float h=probeHeight(job_.step);
            const std::uint32_t outZ=nat.word(0);
            if(outZ && call1(nat,n::GroundZ,{bits(job_.x),bits(job_.y),bits(h),outZ})) {
                const float gz=asFloat(nat.load(outZ));
                if(std::isfinite(gz) && gz>-200.f && gz<2000.f) {
                    call0(nat,n::SetPedCoordsKeepVehicle,{g.ped,bits(job_.x),bits(job_.y),bits(gz+1.0f)});
                    char d[64];std::snprintf(d,sizeof d,"x=%.1f y=%.1f z=%.1f probes=%u",double(job_.x),double(job_.y),double(gz+1.0f),job_.step+1);
                    finishJob(nat,true,d,false);
                    return;
                }
            }
            if(++job_.step>=probeCount()) { // no ground found anywhere: back to where the player was
                if(job_.hasHint)call0(nat,n::SetPedCoordsKeepVehicle,{g.ped,bits(job_.x),bits(job_.y),bits(job_.zHint+1.0f)});
                else call0(nat,n::SetPedCoordsKeepVehicle,{g.ped,bits(job_.fromX),bits(job_.fromY),bits(job_.fromZ)});
                finishJob(nat,job_.hasHint,job_.hasHint?"no-ground (landmark height used)":"no-ground (returned)",false);
                return;
            }
            moveProbe(nat,g,frame);
        }
    }
    void finishJob(Natives& nat,bool ok,const char* detail,bool blocked) noexcept {
        if(job_.kind==Job::Spawn)call0(nat,n::ModelNoLongerNeeded,{job_.model});
        if(blocked && job_.kind==Job::Teleport) {/* left where the last probe put it: the gate closed (pause, cutscene, death) */}
        result(job_.seq,blocked?"blocked":ok?"ok":"fail",job_.what,detail);
        job_=Job{};
    }

    LogFn log_;
    std::atomic<bool> enabled_{false},work_{false},gameplay_{false},busy_{false},owed_{false},weatherLocked_{false};
    std::atomic<std::uint32_t> toggles_{0};
    std::uint32_t applied_=0;   // script thread only: toggles whose per-frame action ran (their off action is still owed)
    Job job_;                   // script thread only
    mutable std::mutex m_;      // queue_ and status_
    std::deque<Request> queue_;
    char status_[128]="none";int statusSeq_=0;
    std::atomic<int> seq_{0};
};

// v717 (the Fold's phone pad, recomp_phone_state_v1): bit 1 = ringing (IS_PED_RINGTONE_PLAYING(PLAYER_PED_ID())), bit 2 = a call
// ongoing (IS_MOBILE_PHONE_CALL_ONGOING()); a native not registered reads as 0.
inline int phoneCall(Natives& nat) noexcept {
    std::uint32_t out[3]{};int b=0;
    if(!nat.call(n::PlayerPedId,nullptr,0,out))return 0;
    const std::uint32_t ped=out[0];
    if(ped && nat.call(n::IsPedRingtonePlaying,&ped,1,out) && out[0])b|=2;
    if(nat.call(n::IsMobilePhoneCallOngoing,nullptr,0,out) && out[0])b|=4;
    return b;
}
} // namespace recomp::trainer
