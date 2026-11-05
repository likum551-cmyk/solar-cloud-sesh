// 0.4.19 radio off: the pure tests of libs/sync/radio_off.h, included by gameplay_test.cpp after Check() is defined.
//
// The unit tests pin each rule; the simulation runs the real RadioOffGate the way the engine's TickRadioOff drives it
// against a model of the GAME (all of it a hypothesis, live-unverified):
//   * the game starts a vehicle's radio when its engine starts (500 ms after we get in) unless that vehicle's radio is
//     disabled, and a "hostile" moment at t = 5 s turns the station on again whatever the radio's state (a script reset);
//   * the radio wheel opens while the radio-wheel control (85) is held and NOT disabled this frame, and while it is open
//     the game runs at 0.3 time scale (the single-player slow-mo GI reported).
// The OLD behaviour (no_radio=0: the gate issues nothing) must show both defects; the gate must show neither.
#pragma once

#include "sync/radio_off.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace radiooff {

using namespace gage::sync;

constexpr int kPureChecks = 15;   // asserted by gameplay_test's total

struct SimResult
{
    double radioOnMs = 0;        // wall ms the vehicle radio played while we sat in it
    double radioOnLongestMs = 0; // the longest single stretch
    int    slowFrames = 0;       // frames IsSlowFrame flags (game timer vs wall clock)
    int    wheelFrames = 0;      // frames the radio wheel was open
    int    vehApplies = 0, globals = 0, holdFrames = 0;
};

// A 20 s scripted drive at 60 fps: on foot until 1 s, getting in (USING only) 1.0..1.6 s, seated from 1.6 s, Q held
// 8..11 s, out at 15 s, on foot after.
inline SimResult RunDrive(bool enabled)
{
    RadioOffGate gate;
    SimResult r;
    const int32_t kCar = 77;
    bool radioEnabled = true;      // SET_VEHICLE_RADIO_ENABLED state of kCar
    bool stationOn = false;        // the car's radio is playing
    bool engineStarted = false;
    double runMs = 0;
    double gameMs = 0;             // the game timer
    double prevWall = -1, prevGame = -1;
    const double dt = 1000.0 / 60.0;
    for (int f = 0; f <= 20 * 60; ++f)
    {
        const double t = f * dt;
        const bool using_ = t >= 1000.0 && t < 15000.0;
        const bool seated = t >= 1600.0 && t < 15000.0;
        const bool qHeld = t >= 8000.0 && t < 11000.0;
        // the game: the engine starts 500 ms after we get in and starts the radio with it
        if (seated && !engineStarted && t >= 1500.0)
        {
            engineStarted = true;
            if (radioEnabled) stationOn = true;
        }
        if (f == 5 * 60) stationOn = true;   // the hostile reset at 5 s

        // the engine's TickRadioOff: read back, gate, apply, hold
        RadioOffIn in;
        in.nowMs = t;
        in.enabled = enabled;
        in.vehIn = seated ? kCar : 0;
        in.vehUsing = using_ ? kCar : 0;
        const int32_t target = RadioTarget(in.vehIn, in.vehUsing);
        in.radioReadsOn = target ? RadioReadsOn(stationOn, seated, stationOn ? 3 : kRadioStationOff) : false;
        const RadioOffOut out = gate.Tick(in);
        if (out.applyGlobal) ++r.globals;
        if (out.applyVehicle == kCar)
        {
            radioEnabled = false;
            stationOn = false;
            ++r.vehApplies;
        }
        bool wheelDisabled = false;
        if (out.holdControls)
        {
            ++r.holdFrames;
            for (const int c : kRadioControls)
                if (c == kControlVehRadioWheel) wheelDisabled = true;
        }

        // the game's frame: the wheel and its slow-mo, the radio playing
        const bool wheelOpen = seated && qHeld && !wheelDisabled;
        if (wheelOpen) ++r.wheelFrames;
        gameMs += dt * (wheelOpen ? 0.3 : 1.0);
        if (seated && stationOn)
        {
            r.radioOnMs += dt;
            runMs += dt;
            if (runMs > r.radioOnLongestMs) r.radioOnLongestMs = runMs;
        }
        else
            runMs = 0;
        if (prevWall >= 0 && seated && IsSlowFrame(gameMs - prevGame, t - prevWall))
            ++r.slowFrames;
        prevWall = t;
        prevGame = gameMs;
    }
    return r;
}

inline std::string Fmt(const SimResult& r)
{
    char b[256];
    std::snprintf(b, sizeof b, "radio on %.0f ms (longest %.0f), wheel frames %d, slow frames %d, vehicle applies %d, globals %d, hold frames %d",
                  r.radioOnMs, r.radioOnLongestMs, r.wheelFrames, r.slowFrames, r.vehApplies, r.globals, r.holdFrames);
    return b;
}

inline void RunPure()
{
    std::printf("-- 0.4.19 radio off (libs/sync/radio_off.h): rules, gate, a 20 s drive against a game model\n");

    // 1. the controls held: exactly the five radio controls, never the weapon wheel (37), cover (44), horn (86)
    {
        bool five = kRadioControlCount == 5;
        int sum = 0;
        bool foreign = false;
        for (const int c : kRadioControls)
        {
            sum += c;
            if (c < 81 || c > 85) foreign = true;
        }
        Check("radio: controls held = 81..85 (next / prev station, next / prev track, the radio wheel), nothing else",
              five && !foreign && sum == 81 + 82 + 83 + 84 + 85 && kControlVehRadioWheel == 85);
    }
    // 2. the target: the car we sit in wins over the one we are getting into; entering alone counts; on foot = none
    Check("radio: target = the vehicle we sit in, else the one we are entering, else none",
          RadioTarget(5, 7) == 5 && RadioTarget(0, 7) == 7 && RadioTarget(0, 0) == 0);
    // 3. the read-back
    Check("radio: reads on = the vehicle's radio on, or (seated) a real station; 255 / unreadable / not seated = off",
          RadioReadsOn(true, false, kRadioStationOff) && RadioReadsOn(false, true, 3) && !RadioReadsOn(false, true, kRadioStationOff) &&
          !RadioReadsOn(false, false, 3) && !RadioReadsOn(false, true, -1));
    // 4. the slow-mo ground truth
    Check("radio: slow frame = game timer under 60% of a 5..100 ms wall frame; hitches and an unreadable timer say nothing",
          IsSlowFrame(5.0, 16.7) && IsSlowFrame(9.9, 16.7) && !IsSlowFrame(16.0, 16.7) && !IsSlowFrame(10.5, 16.7) && !IsSlowFrame(10.0, 250.0) &&
          !IsSlowFrame(-1.0, 16.7) && !IsSlowFrame(0.5, 3.0));

    // 5. no_radio=0: nothing is ever issued
    {
        RadioOffGate g;
        bool any = false;
        for (int i = 0; i < 100; ++i)
        {
            RadioOffIn in;
            in.nowMs = i * 250.0;
            in.enabled = false;
            in.vehIn = i > 10 ? 9 : 0;
            in.vehUsing = i > 5 ? 9 : 0;
            in.radioReadsOn = true;
            const RadioOffOut o = g.Tick(in);
            any = any || o.applyGlobal || o.applyVehicle || o.holdControls;
        }
        Check("radio: no_radio=0 issues nothing (no globals, no vehicle off, no control hold)", !any);
    }
    // 6. first tick on foot: the globals, nothing else
    {
        RadioOffGate g;
        RadioOffIn in;
        in.nowMs = 100;
        const RadioOffOut o = g.Tick(in);
        Check("radio: first tick on foot = the globals only (no vehicle, no control hold)", o.applyGlobal && !o.applyVehicle && !o.holdControls);
    }
    // 7..11: one gate through a scripted sequence
    {
        RadioOffGate g;
        RadioOffIn in;
        in.nowMs = 0;
        g.Tick(in);                                   // globals at 0
        in.nowMs = 1000; in.vehUsing = 10;            // getting in
        const RadioOffOut enter = g.Tick(in);
        Check("radio: getting in = the vehicle off at once (before the engine starts it), the globals again, controls held",
              enter.applyVehicle == 10 && enter.why == RadioApplyWhy::NewVehicle && enter.applyGlobal && enter.holdControls);
        in.nowMs = 1100; in.vehIn = 10;               // seated, same car, radio reads on 100 ms after the apply
        in.radioReadsOn = true;
        const RadioOffOut early = g.Tick(in);
        in.nowMs = 1260;
        const RadioOffOut late = g.Tick(in);
        Check("radio: a read-back 'on' re-applies, but not within 250 ms of the last apply",
              early.applyVehicle == 0 && early.holdControls && late.applyVehicle == 10 && late.why == RadioApplyWhy::ReadBackOn);
        in.radioReadsOn = false;
        in.nowMs = 3200;
        const RadioOffOut quiet = g.Tick(in);
        in.nowMs = 3270;
        const RadioOffOut periodic = g.Tick(in);
        Check("radio: the car we stay in is re-asserted every 2 s with no read-back (not before)",
              quiet.applyVehicle == 0 && periodic.applyVehicle == 10 && periodic.why == RadioApplyWhy::Periodic);
        in.nowMs = 4000; in.vehIn = 11; in.vehUsing = 11;   // straight into another car (a seat swap / warp)
        const RadioOffOut swap = g.Tick(in);
        in.nowMs = 4100; in.vehIn = 0; in.vehUsing = 0;     // out
        const RadioOffOut out = g.Tick(in);
        in.nowMs = 4200; in.vehIn = 11; in.vehUsing = 11;   // back into the same car
        const RadioOffOut back = g.Tick(in);
        Check("radio: another car is new, and leaving then re-entering the SAME car is new again; on foot holds nothing",
              swap.applyVehicle == 11 && swap.why == RadioApplyWhy::NewVehicle && !out.holdControls && !out.applyVehicle &&
              back.applyVehicle == 11 && back.why == RadioApplyWhy::NewVehicle);
        in.nowMs = 14199;
        const RadioOffOut g1 = g.Tick(in);
        in.nowMs = 14200;
        const RadioOffOut g2 = g.Tick(in);
        Check("radio: the globals are re-asserted every 10 s (not before)", !g1.applyGlobal && g2.applyGlobal);
    }
    // 12. switching no_radio off and on again in the same car starts fresh
    {
        RadioOffGate g;
        RadioOffIn in;
        in.nowMs = 0; in.vehIn = 20;
        g.Tick(in);
        in.nowMs = 100; in.enabled = false;
        g.Tick(in);
        in.nowMs = 200; in.enabled = true;
        const RadioOffOut o = g.Tick(in);
        Check("radio: no_radio off then on in the same car = applied again at once (the gate forgot it)",
              o.applyVehicle == 20 && o.why == RadioApplyWhy::NewVehicle && o.applyGlobal);
    }
    // 13..15: the drive against the game model, old behaviour first (it must show both defects)
    {
        const SimResult old = RunDrive(false);
        const SimResult neu = RunDrive(true);
        Check("radio: model control: no_radio=0 shows GI's defects (the radio plays for seconds, the wheel opens and slows time)",
              old.radioOnMs > 5000.0 && old.wheelFrames > 100 && old.slowFrames > 100 && old.vehApplies == 0, Fmt(old));
        Check("radio: no_radio=1: the radio never plays (the engine start finds it disabled; the hostile reset at 5 s is off within 300 ms)",
              neu.radioOnMs <= 300.0 && neu.radioOnLongestMs <= 300.0, Fmt(neu));
        Check("radio: no_radio=1: the radio wheel never opens and no frame is slowed while Q is held 3 s",
              neu.wheelFrames == 0 && neu.slowFrames == 0 && neu.holdFrames > 0, Fmt(neu));
    }
}

}  // namespace radiooff
