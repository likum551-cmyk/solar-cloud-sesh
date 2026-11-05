// 0.4.22 vehicle controls: the pure tests of libs/sync/vehicle_controls.h, included by gameplay_test.cpp after Check() and F() are defined.
//
// The state machine is driven the way the engine's TickVehicleControls drives it, against a model of the GAME (a car with indicators, an engine
// that GTA may start for a new driver, the seatbelt ped flag). Everything the model says about GTA is a hypothesis, live-unverified: it has
// switches for the alternatives (GTA honours the engine's disableAutoStart or not; GTA restarts an engine we switched off or not), and the
// checks run under each.
//
//   A. indicators: toggles, the other side, hazards, driver only, menu / chat / not-in-car gating, veh_keys off, leaving
//   B. auto-cancel after a completed turn (and what must NOT cancel it)
//   C. engine: Y off / on, the read-back, the car we switched off keeps its state, the guard is bounded, the player wins over the guard
//   D. seatbelt: any seat, the flag, entry / leave / exit, the flag re-assert
//   E. fuel: the gauge value, fuel_use (an empty tank cuts the engine and will not start)
//   F. the wire / HUD helpers and the disabled GTA controls
#pragma once

#include "sync/vehicle_body.h"
#include "sync/vehicle_controls.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace vehctl {

using namespace gage::sync;
namespace proto = gage::proto;

constexpr int kPureChecks = 39;  // asserted by gameplay_test's total

// The game, as far as the controls care.
struct SimGame
{
    // the car
    uint8_t ind = 0;                    // SET_VEHICLE_INDICATOR_LIGHTS (no getter exists: this is what the model shows)
    bool    engine = true;
    bool    disableAutoStart = false;   // the flag of the last SET_VEHICLE_ENGINE_ON
    // the ped
    bool    pcf32 = true;               // PED config flag 32 (WillFlyThroughWindscreen); the game's own value is true here
    // model switches
    bool    honourDisable = true;       // GTA does not start an engine whose last off call set disableAutoStart
    bool    restartsAlways = false;     // a hostile GTA: starts the engine 200 ms after every off call during the first 5 s of a seat
    bool    flipsBelt = false;          // GTA sets the belt flag back to true now and then (the re-assert test)
    // what the controls did
    int     indCalls = 0, engineCalls = 0, beltCalls = 0, offCalls = 0, onCalls = 0;
    double  seatedAtMs = -1e9;          // the model's entry time: GTA starts a stopped engine 500 ms after we sit down
    double  lastOffMs = -1e9;
    bool    seated = false;
    bool    entryStartDone = false;
};

struct Rig
{
    VehControls vc;
    SimGame     g;
    VehControlsConfig cfg;
    double nowMs = 1000.0;
    uint32_t veh = 0;
    int      seat = 0;
    bool     blocked = false, exiting = false;
    uint8_t  shownInd = 0;
    float    heading = 90.0f, speed = 0.0f, steer = 0.0f;
    bool     steerOk = true, engineOk = true, beltReadOk = false;
    bool     fuelKnown = false;
    unsigned fuel = 1000;
    VehControlsOut last;
    int      events[16] = {};           // count per VcEv
    int      rejected[8] = {};          // count per VcWhy

    explicit Rig(VehControlsConfig c = VehControlsConfig()) : vc(c), cfg(c) {}

    void Sit(uint32_t handle, int wireSeat, uint8_t shown = 0)
    {
        veh = handle;
        seat = wireSeat;
        shownInd = shown;
        g.seated = true;
        g.seatedAtMs = nowMs;
        g.entryStartDone = false;
    }
    void GetOut()
    {
        veh = 0;
        g.seated = false;
    }

    // One script tick of dtMs with the given presses.
    VehControlsOut Step(unsigned presses = 0, double dtMs = 1000.0 / 60.0)
    {
        nowMs += dtMs;
        // the game's own behaviour first
        if (g.seated && !g.entryStartDone && nowMs - g.seatedAtMs >= 500.0)
        {
            g.entryStartDone = true;                                           // once per seating
            if (!g.engine && !(g.honourDisable && g.disableAutoStart))
                g.engine = true;                                               // the engine starts for the new driver
        }
        if (g.seated && g.restartsAlways && !g.engine && nowMs - g.lastOffMs >= 200.0 && nowMs - g.seatedAtMs < 5000.0)
            g.engine = true;
        if (g.flipsBelt && g.seated)
            g.pcf32 = true;
        VehControlsIn in;
        in.nowMs = nowMs;
        in.presses = presses;
        in.vehicle = veh;
        in.seat = veh ? seat : -1;
        in.blocked = blocked;
        in.exiting = exiting;
        in.shownInd = shownInd;
        in.headingDeg = heading;
        in.speedMps = speed;
        in.steerOk = steerOk;
        in.steer = steer;
        in.engineOk = engineOk;
        in.engineRunning = g.engine;
        in.beltFlagOk = beltReadOk;
        in.beltFlag = g.pcf32;
        in.fuelKnown = fuelKnown;
        in.fuelPermille = fuel;
        const VehControlsOut o = vc.Tick(in);
        Apply(o);
        last = o;
        for (int i = 0; i < o.nev; ++i)
        {
            ++events[(int)o.ev[i].kind & 15];
            if (o.ev[i].kind == VcEv::Rejected)
                ++rejected[o.ev[i].b & 7];
        }
        return o;
    }

    void Apply(const VehControlsOut& o)
    {
        if (o.setIndicators)
        {
            g.ind = o.ind;
            ++g.indCalls;
        }
        if (o.setEngine)
        {
            const VcEngineCall call = EngineCallFor(o.engineOn);
            g.engine = call.on;
            g.disableAutoStart = call.disableAutoStart;
            ++g.engineCalls;
            if (!o.engineOn) { ++g.offCalls; g.lastOffMs = nowMs; }
            else ++g.onCalls;
        }
        if (o.setBelt)
        {
            g.pcf32 = o.beltFlag;
            ++g.beltCalls;
        }
        if (o.leftVehicle)
        {
            g.ind = o.leftInd;
            ++g.indCalls;
        }
    }

    void Run(double ms, unsigned firstPresses = 0)
    {
        const double end = nowMs + ms;
        Step(firstPresses);
        while (nowMs < end)
            Step();
    }

    // Heading rate (deg/s) and steering axis held for ms, car rolling at spd m/s.
    void Turn(double ms, float degPerS, float axis, float spd)
    {
        const double end = nowMs + ms;
        speed = spd;
        steer = axis;
        while (nowMs < end)
        {
            const double dt = 1000.0 / 60.0;
            heading = std::fmod(heading + degPerS * (float)(dt / 1000.0) + 720.0f, 360.0f);
            Step(0, dt);
        }
    }
    int Ev(VcEv k) const { return events[(int)k & 15]; }
    int Rej(VcWhy w) const { return rejected[(int)w & 7]; }
};

inline std::string Bits(uint8_t b)
{
    return std::string(b & proto::kVeh2IndicatorL ? "L" : "-") + (b & proto::kVeh2IndicatorR ? "R" : "-");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// A. indicators
// ---------------------------------------------------------------------------------------------------------------------------------
inline void Indicators()
{
    {
        Rig r;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyLeft);
        const uint8_t a = r.g.ind;
        r.Step(kVcKeyLeft);
        const uint8_t b = r.g.ind;
        r.Step(kVcKeyLeft);
        r.Step(kVcKeyRight);
        const uint8_t c = r.g.ind;
        r.Step(kVcKeyRight);
        const uint8_t d = r.g.ind;
        Check("controls: Left turns the left indicator on and off, Right while Left is on switches to the right one, Right again turns it off (each change is one SET_VEHICLE_INDICATOR_LIGHTS round)",
              a == proto::kVeh2IndicatorL && b == 0 && c == proto::kVeh2IndicatorR && d == 0 && r.g.indCalls == 5 && r.vc.Stats().indicatorChanges == 5,
              F("%s %s %s %s, %d calls", Bits(a).c_str(), Bits(b).c_str(), Bits(c).c_str(), Bits(d).c_str(), r.g.indCalls));
    }
    {
        Rig r;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyHazard);
        const uint8_t a = r.g.ind;
        r.Step(kVcKeyHazard);
        const uint8_t b = r.g.ind;
        r.Step(kVcKeyHazard);
        r.Step(kVcKeyLeft);
        const uint8_t c = r.g.ind;      // hazards, then Left: the left one only
        r.Step(kVcKeyHazard);
        const uint8_t d = r.g.ind;      // Left, then hazards: both
        Check("controls: Down arrow toggles both indicators (hazards); Left from the hazards leaves the left one only, hazards from Left make both",
              a == kVcIndBoth && b == 0 && c == proto::kVeh2IndicatorL && d == kVcIndBoth,
              F("%s %s %s %s", Bits(a).c_str(), Bits(b).c_str(), Bits(c).c_str(), Bits(d).c_str()));
    }
    {
        Rig r;
        r.Sit(77, 2);                                     // a passenger
        r.Step();
        r.Step(kVcKeyLeft | kVcKeyRight | kVcKeyHazard | kVcKeyEngine);
        const bool untouched = r.g.ind == 0 && r.g.engine && r.g.indCalls == 0 && r.g.engineCalls == 0;
        r.Step(kVcKeyBelt);
        Check("controls: in a passenger seat the indicators, hazards and engine keys do nothing (counted: not the driver) while the seatbelt key works",
              untouched && r.Rej(VcWhy::NotDriver) == 4 && r.vc.Stats().rejNotDriver == 4 && r.vc.Belt() && !r.g.pcf32);
    }
    {
        Rig r;
        r.Step(kVcKeyLeft | kVcKeyEngine | kVcKeyBelt);   // on foot
        const bool onFoot = r.g.ind == 0 && r.g.indCalls == 0 && r.g.engineCalls == 0 && r.g.beltCalls == 0 && r.Rej(VcWhy::NotInVehicle) == 3
                            && r.vc.Stats().rejNotInVehicle == 3 && r.vc.Vehicle() == 0;
        r.Sit(77, 0);
        r.Step();
        r.Step();
        Check("controls: with the player on foot no key is taken (counted: not in a vehicle) and nothing is queued for the moment the player sits down",
              onFoot && r.g.ind == 0 && !r.vc.Belt() && r.g.engineCalls == 0);
    }
    {
        Rig r;
        r.Sit(77, 0);
        r.Step();
        r.blocked = true;                                 // the chat or the Tab menu has the keyboard
        r.Step(kVcKeyLeft | kVcKeyHazard | kVcKeyEngine | kVcKeyBelt);
        r.blocked = false;
        r.Run(300.0);
        Check("controls: while the chat / Tab menu / pause menu has the keyboard the presses are dropped (counted: blocked) and do not fire when it closes",
              r.g.ind == 0 && r.g.engine && !r.vc.Belt() && r.Rej(VcWhy::Blocked) == 4 && r.vc.Stats().rejBlocked == 4 && r.g.indCalls == 0);
    }
    {
        VehControlsConfig c;
        c.keys = false;                                   // veh_keys=0
        Rig r(c);
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyLeft | kVcKeyHazard | kVcKeyEngine | kVcKeyBelt);
        r.Run(500.0);
        Check("controls: veh_keys=0 issues nothing at all (no indicator, engine or belt-flag call, not even the entry's) and counts the presses as off",
              r.g.indCalls == 0 && r.g.engineCalls == 0 && r.g.beltCalls == 0 && r.Rej(VcWhy::Off) == 4 && r.vc.Stats().rejOff == 4 && r.vc.Vehicle() == 0);
    }
    {
        Rig r;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyLeft);
        r.GetOut();
        const VehControlsOut left = r.Step();
        const bool single = left.leftVehicle == 77 && left.leftInd == 0 && left.leftIndChanged && r.g.ind == 0;
        Rig h;
        h.Sit(78, 0);
        h.Step();
        h.Step(kVcKeyHazard);
        h.GetOut();
        const VehControlsOut hz = h.Step();
        const bool hazardsStay = hz.leftVehicle == 78 && hz.leftInd == kVcIndBoth && !hz.leftIndChanged && h.g.ind == kVcIndBoth;
        // a new driver finds the hazards on (the server's last flags2) and can switch them off
        Rig n;
        n.Sit(78, 0, kVcIndBoth);
        n.Step();
        const bool inherits = n.vc.Indicators() == kVcIndBoth;
        n.Step(kVcKeyHazard);
        Check("controls: leaving a car puts a single indicator out and leaves the hazards on; the next driver starts from the state the car shows and Down switches the hazards off",
              single && hazardsStay && inherits && n.g.ind == 0, F("single %d hazards %d inherits %d off %d", (int)single, (int)hazardsStay, (int)inherits, (int)(n.g.ind == 0)));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// B. auto-cancel
// ---------------------------------------------------------------------------------------------------------------------------------
inline void AutoCancelTests()
{
    {
        // 90 degrees left (heading rises), the wheel held for 3 s, then centred: still on at the apex, out within about a second of centring.
        Rig r;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyLeft);
        r.Turn(1000.0, 0.0f, 0.0f, 12.0f);                 // waiting at 43 km/h with the signal on: nothing happens
        const bool wait = r.g.ind == proto::kVeh2IndicatorL;
        r.Turn(3000.0, 30.0f, -0.6f, 8.0f);               // the turn: 90 degrees
        const bool apex = r.g.ind == proto::kVeh2IndicatorL;
        const double centredAt = r.nowMs;
        r.Turn(1500.0, 0.0f, 0.0f, 10.0f);
        const bool out = r.g.ind == 0 && r.vc.Stats().autoCancels == 1;
        // when exactly: the cancel call came centreHoldMs after the wheel centred (within a few frames)
        Check("auto-cancel: a left indicator waits through a straight stretch, stays on through a 90 degree left turn with the wheel held, and goes out by itself once the wheel has been centred for 0.4 s",
              wait && apex && out && r.Ev(VcEv::AutoCancel) == 1 && r.nowMs - centredAt > 0.0,
              F("wait %d apex %d out %d, cancels %u", (int)wait, (int)apex, (int)out, r.vc.Stats().autoCancels));
    }
    {
        Rig r;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyRight);
        r.Turn(2000.0, 0.0f, 0.0f, 12.0f);
        r.Turn(3000.0, 30.0f, -0.6f, 8.0f);               // the wrong way round: right signalled, a left turn made
        r.Turn(1500.0, 0.0f, 0.0f, 10.0f);
        Check("auto-cancel: a completed turn the other way than the signal also puts it out (the signal is stale)", r.g.ind == 0 && r.vc.Stats().autoCancels == 1);
    }
    {
        // A chicane / lane change: +30 degrees then -30 degrees (60 degrees of swing in all, more than the threshold, but a net of 0),
        // the wheel centred between; 20 s later the indicator is still on.
        Rig r;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyLeft);
        r.Turn(1000.0, 30.0f, -0.4f, 15.0f);
        r.Turn(1000.0, 0.0f, 0.0f, 15.0f);
        r.Turn(1000.0, -30.0f, 0.4f, 15.0f);
        r.Turn(20000.0, 0.0f, 0.0f, 15.0f);
        Check("auto-cancel: a lane change (the heading comes back) does not cancel the indicator, however long it stays on afterwards", r.g.ind == proto::kVeh2IndicatorL && r.vc.Stats().autoCancels == 0);
    }
    {
        // The threshold: 40 degrees of turn does not cancel, 50 does.
        Rig a;
        a.Sit(77, 0);
        a.Step();
        a.Step(kVcKeyLeft);
        a.Turn(2000.0, 20.0f, -0.5f, 8.0f);                // 40 degrees
        a.Turn(3000.0, 0.0f, 0.0f, 10.0f);
        Rig b;
        b.Sit(77, 0);
        b.Step();
        b.Step(kVcKeyLeft);
        b.Turn(2000.0, 25.0f, -0.5f, 8.0f);                // 50 degrees
        b.Turn(3000.0, 0.0f, 0.0f, 10.0f);
        Check("auto-cancel: a 40 degree turn leaves the indicator on, a 50 degree turn cancels it (the threshold is about 45)",
              a.g.ind == proto::kVeh2IndicatorL && b.g.ind == 0, F("40: %s, 50: %s", Bits(a.g.ind).c_str(), Bits(b.g.ind).c_str()));
    }
    {
        Rig r;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyHazard);
        r.Turn(3000.0, 30.0f, -0.6f, 8.0f);
        r.Turn(3000.0, 0.0f, 0.0f, 10.0f);
        Check("auto-cancel: the hazards never cancel, through a 90 degree turn and a straight run", r.g.ind == kVcIndBoth && r.vc.Stats().autoCancels == 0);
    }
    {
        // Switching the signal in the middle of a turn starts a fresh count: 30 degrees left under the left signal, Right pressed,
        // then 20 more degrees left (a net of 50 had the count carried over) leaves the right signal on.
        Rig r;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyLeft);
        r.Turn(1000.0, 30.0f, -0.5f, 8.0f);
        r.Step(kVcKeyRight);
        r.Turn(1000.0, 20.0f, -0.5f, 8.0f);
        r.Turn(3000.0, 0.0f, 0.0f, 10.0f);
        Check("auto-cancel: pressing the other side in the middle of a turn starts the count again (30 degrees under Left do not add to 20 under Right)",
              r.g.ind == proto::kVeh2IndicatorR && r.vc.Stats().autoCancels == 0, F("ind %s, cancels %u", Bits(r.g.ind).c_str(), r.vc.Stats().autoCancels));
    }
    {
        VehControlsConfig c;
        c.autoCancel = false;                              // veh_indicator_autocancel=0
        Rig r(c);
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyLeft);
        r.Turn(3000.0, 30.0f, -0.6f, 8.0f);
        r.Turn(3000.0, 0.0f, 0.0f, 10.0f);
        Check("auto-cancel: veh_indicator_autocancel=0 keeps the indicator on through a completed turn", r.g.ind == proto::kVeh2IndicatorL && r.vc.Stats().autoCancels == 0);
    }
    {
        // A car spinning on the spot (a push, a crash) or standing still does not count as a turn.
        Rig r;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyLeft);
        r.Turn(4000.0, 60.0f, -1.0f, 0.2f);                // 240 degrees at 0.2 m/s
        r.Turn(3000.0, 0.0f, 0.0f, 0.0f);
        Check("auto-cancel: heading changes while the car is (nearly) standing count for nothing", r.g.ind == proto::kVeh2IndicatorL && r.vc.Stats().autoCancels == 0);
    }
    {
        // The steering axis cannot be read: the heading rate decides "centred".
        Rig r;
        r.steerOk = false;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyLeft);
        r.Turn(3000.0, 30.0f, 0.0f, 8.0f);
        const bool apex = r.g.ind == proto::kVeh2IndicatorL;
        r.Turn(1500.0, 0.0f, 0.0f, 10.0f);
        Check("auto-cancel: with no steering read the end of the turn is the heading rate falling under 4 deg/s (still on at the apex, out afterwards)", apex && r.g.ind == 0);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// C. engine
// ---------------------------------------------------------------------------------------------------------------------------------
inline void Engine()
{
    {
        Rig r;
        r.Sit(77, 0);
        r.Step();
        const VehControlsOut off = r.Step(kVcKeyEngine);
        const bool stoppedOk = off.setEngine && !off.engineOn && !r.g.engine && r.g.disableAutoStart;
        r.Run(300.0);
        const bool stays = !r.g.engine;                    // throttle does not restart it: disableAutoStart was set
        const VehControlsOut on = r.Step(kVcKeyEngine);
        const bool startedOk = on.setEngine && on.engineOn && r.g.engine && !r.g.disableAutoStart;
        const bool counts = r.g.offCalls == 1 && r.g.onCalls == 1;
        // The game kills the engine by itself (a stall, a flooded or burnt-out car): the controls follow the read, so Y starts it again.
        r.g.engine = false;
        r.Step();
        const VehControlsOut again = r.Step(kVcKeyEngine);
        const bool follows = again.setEngine && again.engineOn && r.g.engine;
        Check("engine: Y switches a running engine off with disableAutoStart set (it stays off), Y again starts it with the flag clear, and after the game itself stops the engine Y starts it (the read wins)",
              stoppedOk && stays && startedOk && counts && follows, F("off %d stays %d on %d counts %d follows %d", (int)stoppedOk, (int)stays, (int)startedOk, (int)counts, (int)follows));
    }
    {
        Rig r;
        r.engineOk = false;                                // GET_IS_VEHICLE_ENGINE_RUNNING cannot be called
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyEngine);
        const bool first = !r.g.engine;
        r.Step(kVcKeyEngine);
        Check("engine: when the engine state cannot be read Y still alternates off / on from the last state the controls know", first && r.g.engine && r.g.engineCalls == 2);
    }
    for (int model = 0; model < 2; ++model)
    {
        // The car we switched off, left and re-entered. model 0: GTA honours disableAutoStart (the engine stays off, nothing to correct);
        // model 1: GTA starts the engine for the new driver anyway (the guard switches it off again, once).
        Rig r;
        r.g.honourDisable = model == 0;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyEngine);
        r.GetOut();
        r.Run(2000.0);
        r.Sit(77, 0);
        r.Run(3500.0);
        const bool off = !r.g.engine;
        const int guards = (int)r.vc.Stats().engineGuards;
        Check(model == 0 ? "engine: a car we switched off keeps its state when we sit in it again (GTA honours disableAutoStart: nothing needs correcting)"
                         : "engine: a car we switched off is switched off again when GTA starts its engine for the new driver anyway (the guard re-issues once)",
              off && guards == (model == 0 ? 0 : 1), F("engine %d, guard re-issues %d", (int)r.g.engine, guards));
    }
    {
        // The HUD's engine-off glyph. A stopped engine that the game starts for the new driver 0.5 s after entry is never shown (the glyph waits 0.6 s);
        // one that stays stopped is shown after that; our own Y shows it at once and Y again clears it.
        Rig a;
        a.g.engine = false;
        a.Sit(88, 0);
        a.Run(300.0);
        const bool waits = !a.g.engine && !a.vc.EngineOffHud(a.nowMs);      // stopped for 0.3 s: not shown yet
        bool everShown = false;
        for (int i = 0; i < 100; ++i)
        {
            a.Step();
            everShown = everShown || a.vc.EngineOffHud(a.nowMs);
        }
        const bool startedNeverShown = a.g.engine && !everShown;
        Rig b;
        b.g.engine = false;
        b.g.disableAutoStart = true;                       // the game leaves this engine stopped
        b.Sit(88, 0);
        b.Run(300.0);
        const bool notYet = !b.vc.EngineOffHud(b.nowMs);
        b.Run(500.0);
        const bool shown = b.vc.EngineOffHud(b.nowMs);
        Rig c;
        c.Sit(77, 0);
        c.Step();
        const bool runningHidden = !c.vc.EngineOffHud(c.nowMs);
        c.Step(kVcKeyEngine);
        c.Step();
        const bool offAtOnce = c.vc.EngineOffHud(c.nowMs);
        c.Step(kVcKeyEngine);
        c.Step();
        const bool clearedAgain = !c.vc.EngineOffHud(c.nowMs);
        c.Step(kVcKeyEngine);
        c.Step();
        const bool offOnceMore = c.vc.EngineOffHud(c.nowMs);
        c.GetOut();
        c.Step();
        const bool goneWithTheCar = offOnceMore && !c.vc.EngineOffHud(c.nowMs);          // out of the car there is no glyph to show
        Check("engine: the HUD's engine-off glyph waits 0.6 s (the game starts a new driver's engine about 0.5 s after entry), then shows a stopped engine; Y shows it at once and Y again clears it; out of the car it is gone",
              waits && startedNeverShown && notYet && shown && runningHidden && offAtOnce && clearedAgain && goneWithTheCar,
              F("waits %d started %d notYet %d shown %d running %d offAtOnce %d cleared %d gone %d", (int)waits, (int)startedNeverShown, (int)notYet, (int)shown,
                (int)runningHidden, (int)offAtOnce, (int)clearedAgain, (int)goneWithTheCar));
    }
    {
        // A car we never touched: entering it issues nothing, and its engine state is left to the game.
        Rig r;
        r.g.engine = false;
        r.g.honourDisable = true;
        r.Sit(88, 0);
        r.Run(3000.0);
        Check("engine: a car we did not switch off is left to the game on entry (no engine call at all, the game starts it for the new driver)",
              r.g.engineCalls == 0 && r.g.engine && r.vc.Stats().engineGuards == 0);
    }
    {
        // A hostile GTA that restarts the engine again and again: the guard stops after engineGuardMax re-issues.
        Rig r;
        r.g.restartsAlways = true;
        r.g.honourDisable = false;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyEngine);
        r.GetOut();
        r.Run(500.0);
        r.g.engine = false;                                // the car stands switched off, as we left it
        r.Sit(77, 0);
        r.Run(6000.0);
        Check("engine: the guard re-issues at most 3 times and then leaves the engine alone, whatever the game does",
              r.vc.Stats().engineGuards == 3 && r.g.offCalls == 1 + 3, F("%u guards, %d off calls", r.vc.Stats().engineGuards, r.g.offCalls));
    }
    {
        // The player starts the engine inside the guard window: the guard never fights that.
        Rig r;
        r.g.honourDisable = false;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyEngine);
        r.GetOut();
        r.Run(1000.0);
        r.Sit(77, 0);
        r.Run(200.0);
        r.g.engine = false;                                // off at the moment of the press
        const int guardsBefore = (int)r.vc.Stats().engineGuards;
        r.Step(kVcKeyEngine);                              // the player starts it
        r.Run(3000.0);
        Check("engine: a Y press inside the guard window starts the engine and the guard never switches it off again",
              r.g.engine && (int)r.vc.Stats().engineGuards == guardsBefore, F("engine %d, guards %u", (int)r.g.engine, r.vc.Stats().engineGuards));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// D. seatbelt
// ---------------------------------------------------------------------------------------------------------------------------------
inline void Belt()
{
    {
        Rig r;
        r.g.pcf32 = false;                                 // this ped's own value is "no fly-through"
        r.beltReadOk = true;
        r.Sit(77, 3);                                      // a rear passenger
        r.Step();
        const bool entry = r.g.pcf32 && !r.vc.Belt() && r.Ev(VcEv::Entered) == 1;          // unbuckled = the game's fly-through, said explicitly
        r.Step(kVcKeyBelt);
        const bool on = r.vc.Belt() && !r.g.pcf32;
        r.Step(kVcKeyBelt);
        const bool off = !r.vc.Belt() && r.g.pcf32;
        // with no read-back of the flag the entry still says it (the re-assert path cannot do it for the entry)
        Rig u;
        u.g.pcf32 = false;
        u.beltReadOk = false;
        u.Sit(77, 0);
        u.Step();
        const bool blind = u.g.pcf32 && u.g.beltCalls == 1;
        Check("belt: sitting down says the fly-through state explicitly (flag 32 true, also with no read-back), B buckles (flag false) and unbuckles (flag true) in any seat",
              entry && on && off && blind && r.vc.Stats().beltOn == 1 && r.vc.Stats().beltOff == 1, F("entry %d on %d off %d blind %d", (int)entry, (int)on, (int)off, (int)blind));
    }
    {
        Rig r;
        r.g.pcf32 = false;
        r.beltReadOk = true;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyBelt);
        r.exiting = true;                                  // F: the exit task runs
        const VehControlsOut ex = r.Step();
        const bool auto_ = !r.vc.Belt() && ex.setBelt && ex.beltFlag && r.Ev(VcEv::BeltAuto) == 1;
        r.GetOut();
        r.exiting = false;
        const VehControlsOut out = r.Step();
        const bool restored = out.setBelt && out.beltFlag == false && !r.g.pcf32;           // the ped's own value goes back
        r.Sit(77, 0);
        r.Step();
        // leaving buckled without the exit task (thrown out, the car destroyed, teleported): the belt is off the controls and the flag is back
        Rig h;
        h.g.pcf32 = false;
        h.beltReadOk = true;
        h.Sit(77, 0);
        h.Step();
        h.Step(kVcKeyBelt);
        const bool buckled = h.vc.Belt();
        h.GetOut();
        h.Step();
        const bool thrown = buckled && !h.vc.Belt() && !h.g.pcf32 && h.vc.BeltFlagWanted();
        Check("belt: F (the exit task) unbuckles at once, leaving puts the game's own flag value back (also when leaving buckled with no exit task), and the next ride starts unbuckled",
              auto_ && restored && thrown && !r.vc.Belt() && r.g.pcf32, F("auto %d restored %d thrown %d", (int)auto_, (int)restored, (int)thrown));
    }
    {
        Rig r;
        r.beltReadOk = true;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyBelt);
        r.Run(200.0);
        const int callsMatching = r.g.beltCalls;
        r.Run(2000.0);                                     // the game keeps the flag we set: nothing is re-issued
        const bool quiet = r.g.beltCalls == callsMatching && r.vc.Stats().beltReasserts == 0;
        r.g.flipsBelt = true;                              // the game puts the flag back to true
        r.Run(200.0);
        r.g.flipsBelt = false;
        r.Run(200.0);
        Check("belt: when the game moves the flag under a buckled belt the controls set it again (and never touch it while it matches)",
              quiet && r.vc.Stats().beltReasserts >= 1 && !r.g.pcf32 && r.vc.Belt(), F("quiet %d, reasserts %u, flag %d", (int)quiet, r.vc.Stats().beltReasserts, (int)r.g.pcf32));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// E. fuel
// ---------------------------------------------------------------------------------------------------------------------------------
inline void Fuel()
{
    {
        const FuelHud none = FuelHudFor(false, true, true, 800), notNet = FuelHudFor(true, false, true, 800), noState = FuelHudFor(true, true, false, 800);
        const FuelHud full = FuelHudFor(true, true, true, 1000), quarter = FuelHudFor(true, true, true, 250), empty = FuelHudFor(true, true, true, 0),
                      over = FuelHudFor(true, true, true, 5000);
        Check("fuel gauge: hidden on foot, in a car the server does not know and before its first state; otherwise the server's permille as a 0..1 fraction (0 is shown empty, past 1000 is clamped)",
              !none.show && !notNet.show && !noState.show && full.show && full.frac == 1.0f && quarter.show && std::fabs(quarter.frac - 0.25f) < 1e-6f &&
                  empty.show && empty.frac == 0.0f && over.show && over.frac == 1.0f);
    }
    {
        // fuel_use on: an empty tank cuts the engine of our own car, is not restarted by Y, and is allowed again once there is fuel.
        VehControlsConfig c;
        c.fuelUse = true;
        Rig r(c);
        r.fuelKnown = true;
        r.fuel = 400;
        r.Sit(77, 0);
        r.Step();
        r.Run(1000.0);
        const bool fine = r.g.engine && r.g.engineCalls == 0;
        r.fuel = 0;
        r.Run(2000.0);
        const bool cut = !r.g.engine && r.vc.Stats().fuelCuts == 1 && r.g.disableAutoStart;
        r.Step(kVcKeyEngine);
        r.Run(500.0);
        const bool refused = !r.g.engine && r.Rej(VcWhy::NoFuel) == 1;
        r.fuel = 300;
        r.Step(kVcKeyEngine);
        const bool started = r.g.engine;
        // A game that keeps restarting the engine: the cut is rate-limited (500 ms), not issued every frame.
        Rig h(c);
        h.fuelKnown = true;
        h.fuel = 0;
        h.g.restartsAlways = true;
        h.g.honourDisable = false;
        h.Sit(77, 0);
        h.Run(4000.0);
        const bool limited = h.vc.Stats().fuelCuts >= 2 && h.vc.Stats().fuelCuts <= 10;
        Check("fuel_use: with fuel the engine is left alone; at 0 it is cut (once, with disableAutoStart), Y does not start it (counted: no fuel), Y starts it again once there is fuel, and a game that keeps restarting it is cut at most every 500 ms",
              fine && cut && refused && started && limited, F("fine %d cut %d refused %d started %d limited %d (%u cuts)", (int)fine, (int)cut, (int)refused, (int)started, (int)limited, h.vc.Stats().fuelCuts));
    }
    {
        // fuel_use off (the default): the same empty tank changes nothing about the car.
        Rig r;
        r.fuelKnown = true;
        r.fuel = 0;
        r.Sit(77, 0);
        r.Step();
        r.Run(2000.0);
        r.Step(kVcKeyEngine);
        r.Step(kVcKeyEngine);
        Check("fuel_use off (default): an empty tank cuts nothing and refuses nothing (the gauge only shows the level)",
              r.vc.Stats().fuelCuts == 0 && r.Rej(VcWhy::NoFuel) == 0 && r.g.engine && r.g.engineCalls == 2);
    }
    {
        // fuel_use on, passenger seat: only the driver's car is cut (the passenger does not own the engine).
        VehControlsConfig c;
        c.fuelUse = true;
        Rig r(c);
        r.fuelKnown = true;
        r.fuel = 0;
        r.Sit(77, 1);
        r.Run(2000.0);
        Check("fuel_use: a passenger's controls never cut the engine of the car they ride in", r.g.engine && r.vc.Stats().fuelCuts == 0);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// F. wire / HUD helpers
// ---------------------------------------------------------------------------------------------------------------------------------
inline void Helpers()
{
    {
        const uint8_t body = Flags2FromState(true, true, false, false, false, true, true, false);   // lights, high beam, roof down, brake lights
        const uint8_t on = ControllerFlags2(body, proto::kVeh2IndicatorL, true);
        const uint8_t off = ControllerFlags2(body, kVcIndBoth, false);
        const uint8_t both = ControllerFlags2(body, kVcIndBoth, true);
        const uint8_t stale = ControllerFlags2((uint8_t)(body | proto::kVeh2IndicatorR), 0, true);      // bits from a read must not outlive the machine's state
        Check("wire: a controller's flags2 = the body read plus the indicator state of its controls; veh_indicator_sync=0 sends them clear; the other bits (lights, roof, brake) are never touched",
              on == (body | proto::kVeh2IndicatorL) && off == body && both == (body | kVcIndBoth) && stale == body, F("%02x %02x %02x %02x", on, off, both, stale));
    }
    {
        const uint8_t all = (uint8_t)(proto::kVeh2Headlights | proto::kVeh2IndicatorL | proto::kVeh2IndicatorR | proto::kVeh2Brake);
        Check("wire: a copy applies the indicator bits of a body delta, and veh_indicator_sync=0 strips exactly those two bits",
              CopyFlags2Delta(all, true) == all && CopyFlags2Delta(all, false) == (proto::kVeh2Headlights | proto::kVeh2Brake) && CopyFlags2Delta(kVcIndBoth, false) == 0);
    }
    {
        Check("HUD: the driver sees its own indicator state, any other seat the server's flags2 bits, and veh_indicator_sync=0 shows a passenger nothing",
              HudIndicators(true, proto::kVeh2IndicatorL, kVcIndBoth, true) == proto::kVeh2IndicatorL &&
                  HudIndicators(false, proto::kVeh2IndicatorL, proto::kVeh2IndicatorR, true) == proto::kVeh2IndicatorR &&
                  HudIndicators(false, 0, kVcIndBoth, false) == 0 && HudIndicators(true, 0, kVcIndBoth, true) == 0);
    }
    {
        Check("HUD: headlights read 0 off / 1 low / 2 high (a high beam read as lightsOn 0 / highbeamsOn 1 is the high beam, veh_lights_beam_lit=0 "
              "shows it off; an unreadable state is off); the belt warns only unbuckled above 20 km/h",
              HudLights(true, false, true) == 2 && HudLights(true, false, true, false) == 0 && HudLights(true, true, false) == 1 &&
                  HudLights(true, true, true) == 2 && HudLights(false, true, true) == 0 && HudLights(true, false, false) == 0 &&
                  HudBeltWarn(false, 8.0f, 5.56f) && !HudBeltWarn(false, 3.0f, 5.56f) && !HudBeltWarn(true, 30.0f, 5.56f));
    }
    {
        // 0.4.30: the 0.4.29 live log: GET_VEHICLE_LIGHTS_STATE reads a high beam as lightsOn 0 / highbeamsOn 1.
        const uint8_t f2 = Flags2FromState(LightsLit(false, true), true, false, false, false, false);
        Check("lights (0.4.30): a high beam read as lightsOn 0 / highbeamsOn 1 is lit: the controller sends kVeh2Headlights with kVeh2HighBeam and a copy "
              "shows it; veh_lights_beam_lit=0 keeps 0.4.29 (lightsOn alone); off reads off",
              (f2 & proto::kVeh2Headlights) && (f2 & proto::kVeh2HighBeam) && HeadlightsShown(false, (f2 & proto::kVeh2Headlights) != 0) &&
                  !LightsLit(false, true, false) && LightsLit(true, false) && !LightsLit(false, false),
              F("flags2 %02x", f2));
    }
    {
        bool has[8] = {};
        for (int i = 0; i < kVcDisabledControlCount; ++i)
        {
            const int c = kVcDisabledControls[i];
            has[0] |= c == 172; has[1] |= c == 173; has[2] |= c == 174; has[3] |= c == 175; has[4] |= c == 29; has[5] |= c == 246;
            has[6] |= c == 74 || c == 75 || c == 23;       // H (headlight), F (exit), INPUT_ENTER: GTA's own, never taken away
        }
        Check("controls: the held GTA controls are the four arrow-key cellphone controls (172..175), B's special ability (29) and Y's team chat (246); never H (74), the exit (75) or enter (23)",
              has[0] && has[1] && has[2] && has[3] && has[4] && has[5] && !has[6] && kVcDisabledControlCount == 6);
    }
    {
        // Several presses in one tick are taken in order and the last state wins; only one native round is issued for the indicators.
        Rig r;
        r.Sit(77, 0);
        r.Step();
        const VehControlsOut o = r.Step(kVcKeyLeft | kVcKeyRight);
        Check("controls: Left and Right in the same tick are taken in order (the right one wins) and the indicators are set once from the final state",
              o.setIndicators && o.ind == proto::kVeh2IndicatorR && r.g.ind == proto::kVeh2IndicatorR && r.g.indCalls == 1);
    }
    {
        // A different car in the next tick closes out the old one: its single indicator goes out, the new one is entered afresh.
        Rig r;
        r.Sit(77, 0);
        r.Step();
        r.Step(kVcKeyLeft | kVcKeyBelt);
        r.Sit(99, 1);
        const VehControlsOut o = r.Step();
        Check("controls: switching cars between two ticks closes out the first (left indicator out, belt reset) and enters the second unbuckled with its own state",
              o.leftVehicle == 77 && o.leftInd == 0 && r.vc.Vehicle() == 99 && !r.vc.Belt() && r.vc.Indicators() == 0 && r.vc.Seat() == 1);
    }
}

inline void RunPure()
{
    std::printf("-- 0.4.22 vehicle controls (libs/sync/vehicle_controls.h): indicators, auto-cancel, engine, belt, fuel gauge, wire helpers\n");
    Indicators();
    AutoCancelTests();
    Engine();
    Belt();
    Fuel();
    Helpers();
}

}   // namespace vehctl
