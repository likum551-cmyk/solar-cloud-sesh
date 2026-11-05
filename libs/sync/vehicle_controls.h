// Vehicle controls (0.4.22; GI: "add indicators for turn signals, headlights, belt and engine on/off - should be mechanical, come up with
// keys, and add small indicators on the car UI", and "add a fuel gauge somewhere"). Pure C++17, header-only, no game calls: the engine feeds
// the key presses and its native reads in once per script tick, calls the natives Tick names, and pushes what is shown to the overlay.
// Tested offline in tests/vehcontrols_pure_tests.h (the state machine, against a model car) and tests/vehcontrols_wire_tests.h (the real
// server and ClientSession).
//
// KEYS (read by the overlay from ImGui's key events, the same source as Tab / T / Enter: overlay_ui.cpp UiFrame; never while the chat, the
// Tab menu or a text box has the keyboard):
//   Left Arrow   left indicator           (driver)     press again = off; the other side switches; a hazard state becomes this side
//   Right Arrow  right indicator          (driver)
//   Down Arrow   hazards (both sides)     (driver)     toggles
//   Y            engine off / on          (driver)     SET_VEHICLE_ENGINE_ON with disableAutoStart on the OFF call, so throttle does not restart it
//   B            seatbelt                 (any seat)   PED config flag 32 (WillFlyThroughWindscreen): belted = false, unbelted = true
//   H            headlights               GTA's own INPUT_VEH_HEADLIGHT (74). Nothing here touches it: the HUD reads GET_VEHICLE_LIGHTS_STATE.
// GTA controls the engine holds disabled while the local ped is in a vehicle and the keys are on (kVcDisabledControls): 172 / 173 / 174 / 175
// INPUT_CELLPHONE_UP / DOWN / LEFT / RIGHT (the arrow keys), 29 INPUT_SPECIAL_ABILITY_SECONDARY (B), 246 INPUT_MP_TEXT_CHAT_TEAM (Y). The ids are
// the eControlAction numbering (INPUT_NEXT_CAMERA = 0; the same count lands on 99 INPUT_VEH_SELECT_NEXT_WEAPON, 140 melee and 166 character select
// the engine already relies on). Which controls a PC keymap binds to the arrows is INFERRED from the public control table.
//
// WIRE (no change): the indicators ride kVeh2IndicatorL / kVeh2IndicatorR of flags2 (controller-owned, told in full entries on a change), the engine
// state rides kVehEngine of flags (the driver's, read off the car by GET_IS_VEHICLE_ENGINE_RUNNING). Both are bits the protocol has always had
// (kVeh2Known 0xFF, kVehKnown includes kVehEngine): an older client decodes them and applies them like any other. There is no getter for the
// indicators in the 3889 script API (GET_VEHICLE_INDICATOR_LIGHTS is in neither crossmap), so the controller's own state machine is the truth that
// goes on the wire; copies set SET_VEHICLE_INDICATOR_LIGHTS from the bits (sync::DiffBody, on a change).
//
// FUEL (GI: a gauge): fuel is SERVER-OWNED here already (VehicleState.fuelPermille, drained by the server while the engine runs and the car moves,
// engine bit cleared at 0). GET_VEHICLE_FUEL_LEVEL is a FiveM-only native: it is in neither crossmap and not in the public natives list, so there is
// nothing to read from the game. The gauge shows the server's number (FuelHudFor). fuel_use (default off) additionally makes an empty tank real on
// the driver's own car: the engine is cut and will not start (no pumps yet, so off by default).
#pragma once

#include "protocol/protocol.h"

#include <cmath>
#include <cstdint>

namespace gage::sync {

    // ---- keys ---------------------------------------------------------------------------------------------------------
    constexpr unsigned kVcKeyLeft = 1u << 0, kVcKeyRight = 1u << 1, kVcKeyHazard = 1u << 2, kVcKeyEngine = 1u << 3, kVcKeyBelt = 1u << 4;
    constexpr unsigned kVcKeysAll = kVcKeyLeft | kVcKeyRight | kVcKeyHazard | kVcKeyEngine | kVcKeyBelt;
    constexpr int kVcKeyCount = 5;

    constexpr uint8_t kVcIndBoth = proto::kVeh2IndicatorL | proto::kVeh2IndicatorR;
    constexpr int kVcPcfWillFlyThroughWindscreen = 32;   // ePedConfigFlags: the seatbelt (false = belted, the ped stays in the seat)

    // The GTA controls held disabled (DISABLE_CONTROL_ACTION(0, id, true), every script tick) while the keys are on and the local ped is in a vehicle.
    constexpr int kVcDisabledControls[] = { 172, 173, 174, 175, 29, 246 };
    constexpr int kVcDisabledControlCount = (int)(sizeof(kVcDisabledControls) / sizeof(kVcDisabledControls[0]));

    // ---- switches / tuning -------------------------------------------------------------------------------------------------
    struct VehControlsConfig
    {
        bool   keys = true;               // veh_keys: the keys and their mechanics (off = nothing is read, issued or drawn)
        bool   autoCancel = true;         // veh_indicator_autocancel
        bool   fuelUse = false;           // fuel_use: an empty tank (server fuelPermille 0) cuts the engine of our own car and it will not start
        float  cancelTurnDeg = 45.0f;     // a heading change of at least this (either way) is "a completed turn" once the wheel is centred
        float  centreAxis = 0.15f;        // |steering axis| below this = centred
        double centreHoldMs = 400.0;      // ... for this long
        float  centreYawDegPerS = 4.0f;   // centred when the axis cannot be read: the heading changes slower than this
        float  minSpeedMps = 1.0f;        // heading is counted only while rolling at least this fast
        double engineGuardMs = 3000.0;    // a car we switched off: for this long after we sit in it, a running engine is switched off again...
        int    engineGuardMax = 3;        // ... at most this many times
        double fuelCutGapMs = 500.0;      // an empty tank's engine is cut again (when it reads running) no sooner than this
        float  beltWarnMps = 5.56f;       // unbuckled and faster than this (20 km/h): the belt warning
        double engineOffHudMs = 600.0;    // the HUD's engine-off glyph waits this long: the game starts the engine for a new driver about 0.5 s after entry
    };

    // ---- one tick ------------------------------------------------------------------------------------------------------
    struct VehControlsIn
    {
        double   nowMs = 0.0;
        unsigned presses = 0;             // fresh key presses since the last tick (kVcKey* bits; the overlay's edge, no repeats)
        uint32_t vehicle = 0;             // the vehicle the local ped sits in (handle), 0 = none (also: dead, loading)
        int      seat = -1;               // wire seat of the local ped: 0 driver, > 0 passenger, -1 unknown
        bool     blocked = false;         // chat / Tab menu / pause menu / quit card has the keyboard: presses are not ours
        bool     exiting = false;         // the exit-vehicle task is running (F pressed)
        uint8_t  shownInd = 0;            // kVeh2Indicator* bits the car shows when we take it (the server's last flags2 for a networked car)
        float    headingDeg = 0.0f;       // the vehicle's heading (GTA degrees: a left turn raises it)
        float    speedMps = 0.0f;
        bool     steerOk = false;         // INPUT_VEH_MOVE_LR read (-1 left .. +1 right)
        float    steer = 0.0f;
        bool     engineOk = false;        // GET_IS_VEHICLE_ENGINE_RUNNING read
        bool     engineRunning = false;
        bool     beltFlagOk = false;      // GET_PED_CONFIG_FLAG(32) read (the engine reads it about once a second)
        bool     beltFlag = true;
        bool     fuelKnown = false;       // the server's fuelPermille of this car is known (a networked car with a state)
        unsigned fuelPermille = 1000;
    };

    enum class VcEv : uint8_t
    {
        None = 0, Entered, Left, Indicator, AutoCancel, EngineKey, EngineGuard, FuelCut, Belt, BeltAuto, BeltReassert, Rejected
    };
    enum class VcWhy : uint8_t { NotInVehicle = 0, Blocked, NotDriver, NoFuel, Off };

    struct VcEvent
    {
        VcEv    kind = VcEv::None;
        uint8_t a = 0;                    // Indicator: the new bits; EngineKey / EngineGuard / FuelCut: 1 = switched on; Belt: 1 = buckled; Rejected: the key bit index
        uint8_t b = 0;                    // Rejected: VcWhy; Entered: the belt baseline
        int16_t deg = 0;                  // AutoCancel: the net heading change
    };

    struct VehControlsOut
    {
        static constexpr int kMaxEvents = 10;
        bool     setIndicators = false;   // SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, ind & L) and (vehicle, 0, ind & R)
        uint8_t  ind = 0;
        bool     setEngine = false;       // SET_VEHICLE_ENGINE_ON(vehicle, engineOn, instantly = !engineOn, disableAutoStart = !engineOn)
        bool     engineOn = false;
        bool     setBelt = false;         // SET_PED_CONFIG_FLAG(ped, 32, beltFlag)
        bool     beltFlag = true;
        // The vehicle the ped just left (or was replaced in): apply leftInd to it (hazards stay, a single side goes out).
        uint32_t leftVehicle = 0;
        uint8_t  leftInd = 0;
        bool     leftIndChanged = false;
        VcEvent  ev[kMaxEvents];
        int      nev = 0;
        void Add(VcEv k, uint8_t a = 0, uint8_t b = 0, int16_t deg = 0)
        {
            if (nev < kMaxEvents)
                ev[nev++] = VcEvent{ k, a, b, deg };
        }
    };

    struct VehControlsStats
    {
        uint32_t taken[kVcKeyCount] = {};          // presses that did something, per key bit index
        uint32_t rejNotInVehicle = 0, rejBlocked = 0, rejNotDriver = 0, rejNoFuel = 0, rejOff = 0;
        uint32_t entered = 0, left = 0;
        uint32_t indicatorChanges = 0, autoCancels = 0;
        uint32_t engineOff = 0, engineOn = 0, engineGuards = 0, fuelCuts = 0;
        uint32_t beltOn = 0, beltOff = 0, beltAuto = 0, beltReasserts = 0;
        VehControlsStats& operator+=(const VehControlsStats& o)
        {
            for (int i = 0; i < kVcKeyCount; ++i)
                taken[i] += o.taken[i];
            rejNotInVehicle += o.rejNotInVehicle; rejBlocked += o.rejBlocked; rejNotDriver += o.rejNotDriver; rejNoFuel += o.rejNoFuel; rejOff += o.rejOff;
            entered += o.entered; left += o.left; indicatorChanges += o.indicatorChanges; autoCancels += o.autoCancels;
            engineOff += o.engineOff; engineOn += o.engineOn; engineGuards += o.engineGuards; fuelCuts += o.fuelCuts;
            beltOn += o.beltOn; beltOff += o.beltOff; beltAuto += o.beltAuto; beltReasserts += o.beltReasserts;
            return *this;
        }
    };

    // The arguments of SET_VEHICLE_ENGINE_ON(vehicle, value, instantly, disableAutoStart) for a switch to `on`: the OFF call takes effect at once and
    // carries disableAutoStart (so throttle, or the game starting a new driver's engine, does not bring it back); the ON call lets the game run its
    // ignition and leaves auto start allowed.
    struct VcEngineCall
    {
        bool on, instantly, disableAutoStart;
    };
    inline VcEngineCall EngineCallFor(bool on) { return VcEngineCall{ on, !on, !on }; }

    inline int VcKeyIndex(unsigned bit)
    {
        for (int i = 0; i < kVcKeyCount; ++i)
            if (bit == (1u << i))
                return i;
        return -1;
    }

    // A heading difference wrapped to (-180, 180]; a non-finite input is 0 (a failed read must not turn the car).
    inline float VcWrapDeg(float d)
    {
        if (!std::isfinite(d))
            return 0.0f;
        d = std::fmod(d, 360.0f);
        if (d > 180.0f) d -= 360.0f;
        else if (d <= -180.0f) d += 360.0f;
        return d;
    }

    class VehControls
    {
    public:
        explicit VehControls(const VehControlsConfig& cfg = VehControlsConfig()) : cfg_(cfg) {}
        void Configure(const VehControlsConfig& cfg) { cfg_ = cfg; }
        const VehControlsConfig& Config() const { return cfg_; }

        // A new session (Welcome): forget the vehicle and the cars we switched off. Pending outputs are the caller's business.
        void Reset()
        {
            veh_ = 0;
            seat_ = -1;
            ind_ = 0;
            belt_ = false;
            acHave_ = false;
            offN_ = 0;
        }

        uint32_t Vehicle() const { return veh_; }
        int      Seat() const { return seat_; }
        uint8_t  Indicators() const { return ind_; }            // the state of the car we sit in (kVeh2Indicator* bits)
        bool     Belt() const { return belt_; }
        bool     BeltFlagWanted() const { return !belt_; }      // PED config flag 32: false while buckled
        bool     EngineSeenRunning() const { return engineSeen_; }
        // The engine-off glyph: the engine has read stopped for engineOffHudMs (at once after our own Y). False while it reads running or unreadable.
        bool     EngineOffHud(double nowMs) const { return veh_ != 0 && engineOffSince_ >= 0.0 && nowMs - engineOffSince_ >= cfg_.engineOffHudMs; }
        const VehControlsStats& Stats() const { return stats_; }
        VehControlsStats TakeStats()
        {
            const VehControlsStats s = stats_;
            stats_ = VehControlsStats();
            return s;
        }

        VehControlsOut Tick(const VehControlsIn& in)
        {
            VehControlsOut o;
            // Leaving (also: another vehicle, the keys switched off, dead): the old state is closed out before anything else.
            if (veh_ != 0 && (in.vehicle != veh_ || !cfg_.keys))
                Leave(o);
            if (in.presses)
                CountDropped(in, o);
            if (!cfg_.keys || in.vehicle == 0)
                return o;
            if (veh_ == 0)
                Enter(in, o);
            seat_ = in.seat;
            if (in.engineOk)
            {
                engineSeen_ = in.engineRunning;
                if (in.engineRunning)
                    engineOffSince_ = -1.0;
                else if (engineOffSince_ < 0.0)
                    engineOffSince_ = in.nowMs;
            }
            // The exit task: the belt comes off with the player (F auto-unbuckles).
            if (in.exiting && !exitSeen_)
            {
                exitSeen_ = true;
                if (belt_)
                {
                    belt_ = false;
                    o.setBelt = true;
                    o.beltFlag = true;
                    ++stats_.beltAuto;
                    o.Add(VcEv::BeltAuto, 0);
                }
            }
            else if (!in.exiting)
                exitSeen_ = false;
            if (!in.blocked)
                for (int i = 0; i < kVcKeyCount; ++i)
                    if (in.presses & (1u << i))
                        Press(1u << i, in, o);
            AutoCancel(in, o);
            EngineRules(in, o);
            // The seatbelt flag is re-asserted when the game moved it (the engine reads it about once a second).
            if (in.beltFlagOk && in.beltFlag != BeltFlagWanted() && !o.setBelt)
            {
                o.setBelt = true;
                o.beltFlag = BeltFlagWanted();
                ++stats_.beltReasserts;
                o.Add(VcEv::BeltReassert, belt_ ? 1 : 0);
            }
            return o;
        }

    private:
        static constexpr int kOffCap = 8;

        void CountDropped(const VehControlsIn& in, VehControlsOut& o)
        {
            // Presses that cannot be taken: counted by reason (the caller logs a line for the first few).
            const bool taken = cfg_.keys && in.vehicle != 0 && !in.blocked;
            if (taken)
                return;
            for (int i = 0; i < kVcKeyCount; ++i)
            {
                if (!(in.presses & (1u << i)))
                    continue;
                VcWhy why = VcWhy::Off;
                if (!cfg_.keys) { ++stats_.rejOff; why = VcWhy::Off; }
                else if (in.vehicle == 0) { ++stats_.rejNotInVehicle; why = VcWhy::NotInVehicle; }
                else { ++stats_.rejBlocked; why = VcWhy::Blocked; }
                o.Add(VcEv::Rejected, (uint8_t)i, (uint8_t)why);
            }
        }

        void Enter(const VehControlsIn& in, VehControlsOut& o)
        {
            veh_ = in.vehicle;
            seat_ = in.seat;
            ind_ = (uint8_t)(in.shownInd & kVcIndBoth);
            belt_ = false;
            exitSeen_ = false;
            beltBaseline_ = in.beltFlagOk ? in.beltFlag : true;
            acHave_ = false;
            engineSeen_ = in.engineOk ? in.engineRunning : true;
            engineOffSince_ = -1.0;
            guardLeft_ = 0;
            fuelCutAtMs_ = -1e9;
            ++stats_.entered;
            // Unbuckled is the game's fly-through state, said explicitly (the game's own default for this ped is read for the log).
            o.setBelt = true;
            o.beltFlag = true;
            o.Add(VcEv::Entered, ind_, beltBaseline_ ? 1 : 0);
            if (cfg_.keys && IsOff(veh_))
            {
                // A car we switched off ourselves: it stays off when we sit in it again. disableAutoStart was set on the OFF call; the guard
                // re-issues it when the game starts the engine for the new driver anyway (bounded).
                guardLeft_ = cfg_.engineGuardMax;
                guardUntilMs_ = in.nowMs + cfg_.engineGuardMs;
            }
        }

        void Leave(VehControlsOut& o)
        {
            o.leftVehicle = veh_;
            o.leftInd = (uint8_t)(ind_ == kVcIndBoth ? kVcIndBoth : 0);   // hazards stay on a parked car, a single side goes out
            o.leftIndChanged = o.leftInd != ind_;
            o.setBelt = true;                                              // the game's own value goes back (flag 32 only matters in a car)
            o.beltFlag = beltBaseline_;
            ++stats_.left;
            o.Add(VcEv::Left, o.leftInd, belt_ ? 1 : 0);
            veh_ = 0;
            seat_ = -1;
            ind_ = 0;
            belt_ = false;
            exitSeen_ = false;
            acHave_ = false;
            guardLeft_ = 0;
        }

        void Press(unsigned bit, const VehControlsIn& in, VehControlsOut& o)
        {
            const int idx = VcKeyIndex(bit);
            if (bit == kVcKeyBelt)
            {
                belt_ = !belt_;
                o.setBelt = true;
                o.beltFlag = !belt_;
                ++(belt_ ? stats_.beltOn : stats_.beltOff);
                ++stats_.taken[idx];
                o.Add(VcEv::Belt, belt_ ? 1 : 0);
                return;
            }
            if (seat_ != 0)
            {
                ++stats_.rejNotDriver;
                o.Add(VcEv::Rejected, (uint8_t)idx, (uint8_t)VcWhy::NotDriver);
                return;
            }
            if (bit == kVcKeyEngine)
            {
                const bool fuelEmpty = FuelEmpty(in);
                const bool wantOn = !engineSeen_;
                if (wantOn && fuelEmpty)
                {
                    ++stats_.rejNoFuel;
                    o.Add(VcEv::Rejected, (uint8_t)idx, (uint8_t)VcWhy::NoFuel);
                    return;
                }
                engineSeen_ = wantOn;
                engineOffSince_ = wantOn ? -1.0 : in.nowMs - cfg_.engineOffHudMs;   // our own Y: the glyph follows at once
                guardLeft_ = 0;                                            // the player decides now: no more guard re-issues
                o.setEngine = true;
                o.engineOn = wantOn;
                if (wantOn) { Unmark(veh_); ++stats_.engineOn; }
                else        { Mark(veh_); ++stats_.engineOff; }
                ++stats_.taken[idx];
                o.Add(VcEv::EngineKey, wantOn ? 1 : 0);
                return;
            }
            uint8_t next = ind_;
            if (bit == kVcKeyLeft)        next = (ind_ == proto::kVeh2IndicatorL) ? 0 : proto::kVeh2IndicatorL;
            else if (bit == kVcKeyRight)  next = (ind_ == proto::kVeh2IndicatorR) ? 0 : proto::kVeh2IndicatorR;
            else if (bit == kVcKeyHazard) next = (ind_ == kVcIndBoth) ? 0 : kVcIndBoth;
            ++stats_.taken[idx];
            if (next != ind_)
            {
                ind_ = next;
                acHave_ = false;
                o.setIndicators = true;
                o.ind = ind_;
                ++stats_.indicatorChanges;
                o.Add(VcEv::Indicator, ind_);
            }
        }

        // After a completed turn (the net heading change reached cancelTurnDeg and the wheel has been centred for centreHoldMs) a single
        // indicator goes out by itself. Hazards never do; a lane change (the heading comes back) never reaches the threshold.
        void AutoCancel(const VehControlsIn& in, VehControlsOut& o)
        {
            const bool single = ind_ == proto::kVeh2IndicatorL || ind_ == proto::kVeh2IndicatorR;
            if (!cfg_.autoCancel || seat_ != 0 || !single)
            {
                acHave_ = false;
                return;
            }
            if (!acHave_)
            {
                acHave_ = true;
                acHeading_ = in.headingDeg;
                acMs_ = in.nowMs;
                acNet_ = 0.0f;
                acCentredMs_ = 0.0;
                return;
            }
            const double dt = in.nowMs - acMs_;
            const float d = VcWrapDeg(in.headingDeg - acHeading_);
            acHeading_ = in.headingDeg;
            acMs_ = in.nowMs;
            if (!(dt > 0.0) || dt > 1000.0 || in.speedMps < cfg_.minSpeedMps)
            {
                acCentredMs_ = 0.0;                                       // a hitch or a standing car counts for nothing
                return;
            }
            acNet_ += d;
            const bool centred = in.steerOk ? std::fabs(in.steer) <= cfg_.centreAxis
                                            : std::fabs(d) * 1000.0 / dt <= (double)cfg_.centreYawDegPerS;
            acCentredMs_ = centred ? acCentredMs_ + dt : 0.0;
            if (std::fabs(acNet_) >= cfg_.cancelTurnDeg && acCentredMs_ >= cfg_.centreHoldMs)
            {
                const float net = acNet_;
                ind_ = 0;
                acHave_ = false;
                o.setIndicators = true;
                o.ind = 0;
                ++stats_.autoCancels;
                ++stats_.indicatorChanges;
                o.Add(VcEv::AutoCancel, 0, 0, (int16_t)(net > 3000.0f ? 3000 : (net < -3000.0f ? -3000 : (int)net)));
            }
        }

        bool FuelEmpty(const VehControlsIn& in) const { return cfg_.fuelUse && in.fuelKnown && in.fuelPermille == 0; }

        // The engine of the car we drive: the guard for a car we switched off, and an empty tank (fuel_use).
        void EngineRules(const VehControlsIn& in, VehControlsOut& o)
        {
            if (seat_ != 0 || !in.engineOk)
                return;
            if (FuelEmpty(in))
            {
                if (in.engineRunning && !(o.setEngine && !o.engineOn) && in.nowMs - fuelCutAtMs_ >= cfg_.fuelCutGapMs)
                {
                    fuelCutAtMs_ = in.nowMs;
                    engineSeen_ = false;
                    o.setEngine = true;
                    o.engineOn = false;
                    ++stats_.fuelCuts;
                    o.Add(VcEv::FuelCut, 0);
                }
                return;
            }
            if (guardLeft_ > 0 && in.nowMs <= guardUntilMs_ && in.engineRunning && !o.setEngine)
            {
                --guardLeft_;
                engineSeen_ = false;
                o.setEngine = true;
                o.engineOn = false;
                ++stats_.engineGuards;
                o.Add(VcEv::EngineGuard, 0);
            }
        }

        // The cars we switched off (bounded, oldest dropped).
        bool IsOff(uint32_t v) const
        {
            for (int i = 0; i < offN_; ++i)
                if (off_[i] == v)
                    return true;
            return false;
        }
        void Mark(uint32_t v)
        {
            if (IsOff(v))
                return;
            if (offN_ == kOffCap)
            {
                for (int i = 1; i < kOffCap; ++i)
                    off_[i - 1] = off_[i];
                --offN_;
            }
            off_[offN_++] = v;
        }
        void Unmark(uint32_t v)
        {
            for (int i = 0; i < offN_; ++i)
                if (off_[i] == v)
                {
                    for (int j = i + 1; j < offN_; ++j)
                        off_[j - 1] = off_[j];
                    --offN_;
                    return;
                }
        }

        VehControlsConfig cfg_;
        uint32_t veh_ = 0;
        int      seat_ = -1;
        uint8_t  ind_ = 0;
        bool     belt_ = false, beltBaseline_ = true, exitSeen_ = false;
        bool     engineSeen_ = true;
        double   engineOffSince_ = -1.0;       // when the engine first read stopped (-1 = running / unknown)
        // auto-cancel
        bool     acHave_ = false;
        float    acHeading_ = 0.0f, acNet_ = 0.0f;
        double   acMs_ = 0.0, acCentredMs_ = 0.0;
        // engine
        uint32_t off_[kOffCap] = {};
        int      offN_ = 0;
        int      guardLeft_ = 0;
        double   guardUntilMs_ = 0.0, fuelCutAtMs_ = -1e9;
        VehControlsStats stats_;
    };

    // ---- the wire (sender / receiver) ---------------------------------------------------------------------------------
    // The flags2 a controller sends: what it read off the car (lights, doors, roof, pedals: sync::Flags2FromState) plus the indicator state its
    // own state machine holds. veh_indicator_sync=0: the bits go out clear (0.4.19: no getter, so they always did).
    inline uint8_t ControllerFlags2(uint8_t bodyFlags2, uint8_t indicators, bool indicatorSync)
    {
        const uint8_t keep = (uint8_t)(bodyFlags2 & ~kVcIndBoth);
        return indicatorSync ? (uint8_t)(keep | (indicators & kVcIndBoth)) : keep;
    }

    // The flags2 bits a copy applies from a body delta (sync::DiffBody's flags2): veh_indicator_sync=0 leaves the indicator bits alone.
    inline uint8_t CopyFlags2Delta(uint8_t deltaFlags2, bool indicatorSync)
    {
        return indicatorSync ? deltaFlags2 : (uint8_t)(deltaFlags2 & ~kVcIndBoth);
    }

    // ---- the HUD ---------------------------------------------------------------------------------------------------------
    // The indicator bits the strip draws: the driver's own state machine, or - in any other seat - what the server says the car shows.
    inline uint8_t HudIndicators(bool driver, uint8_t machineInd, uint8_t serverFlags2, bool indicatorSync)
    {
        if (driver)
            return (uint8_t)(machineInd & kVcIndBoth);
        return indicatorSync ? (uint8_t)(serverFlags2 & kVcIndBoth) : (uint8_t)0;
    }

    // 0 off, 1 low beam, 2 high beam (GET_VEHICLE_LIGHTS_STATE: a high beam with the lights off is not a state of its own).
    inline int HudLights(bool readOk, bool on, bool high)
    {
        if (!readOk || !on)
            return 0;
        return high ? 2 : 1;
    }

    // Unbuckled and moving: the belt icon turns to the warning colour.
    inline bool HudBeltWarn(bool belt, float speedMps, float thresholdMps) { return !belt && speedMps > thresholdMps; }

    // The fuel gauge on the speedometer plate: the server's level of the car we sit in. Hidden when we are not in a networked car (no number to show).
    struct FuelHud
    {
        bool  show = false;
        float frac = 0.0f;
    };
    inline FuelHud FuelHudFor(bool inVehicle, bool networked, bool haveState, unsigned permille)
    {
        FuelHud h;
        if (!inVehicle || !networked || !haveState)
            return h;
        h.show = true;
        h.frac = (float)(permille > 1000u ? 1000u : permille) / 1000.0f;
        return h;
    }

}
