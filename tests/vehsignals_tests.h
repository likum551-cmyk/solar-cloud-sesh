// vehsignals_tests.h: 0.4.19 vehicle signals (libs/sync/vehicle_signals.h): the controller's horn and headlights / high beam on the other
// clients' copies. Included by tests/t3_vehvisual_test.cpp after r3_t4_tests.h (one line) and run from its main (one call); it uses that
// file's helpers (Check, F, Kin, Drive, StoreRig, VCmd, PlaceAt, IdOf, kAdder, Rig, Probe).
//
//   A. HornSync on a simulated 120 fps clock against fake cars that model START_VEHICLE_HORN two ways (a call REPLACES the remaining time,
//      or a call while it sounds is IGNORED): rise at once, renewals (not one call per frame or per state), no gap while held, the release
//      call, a car whose states stop is let go, veh_horn_stop=0, the IS_HORN_ACTIVE read-back tallies, an uncallable native is bounded.
//   B. LightsKeeper against a fake car: a copy that keeps its lights is only read, one the game dropped is restored after a confirmed
//      difference, a change is not judged at once, a far copy is not read, a native that does nothing is "stuck" and left alone.
//   C. VehicleStore: the horn bit rides the very next frame (motion-only entries too) and its clear the frame after; the lights are full.
//   D. The REAL server over loopback ENet: A's horn press and release and A's headlights / high beam reach B's ClientSession, and B's
//      HornSync / LightsKeeper (the engine's own step functions) make B's fake car sound / light accordingly.
//
// Evidence level: SIMULATED. What the game's natives do (does a renewal extend a sounding horn, does a 1 ms call cut it, does
// GET_VEHICLE_LIGHTS_STATE read a forced-on copy as on) is live-unverified; the two horn models bracket the first two questions.
#pragma once

#include "sync/vehicle_signals.h"

#include <chrono>
#include <functional>

constexpr int kVehSignalsChecks = 18;   // the number of Check() calls RunVehSignalsTests makes (a skipped section fails main's count)

// ---------------------------------------------------------------------------------------------------------------------------
// fakes
// ---------------------------------------------------------------------------------------------------------------------------

// START_VEHICLE_HORN / IS_HORN_ACTIVE of one car. Replace: a call sets the remaining time to its duration (a 1 ms call cuts a sounding horn).
// IgnoreWhileSounding: a call made while the horn sounds changes nothing (the 1 ms release call too).
struct FakeHorn
{
    enum class Model { Replace, IgnoreWhileSounding } model = Model::Replace;
    bool callable = true, readable = true;
    bool deaf = false;                  // the car never sounds (a native that is called and does nothing)
    double now = 0.0, until = -1.0;
    int longCalls = 0, shortCalls = 0, reads = 0;
    int frame = 0, lastCallFrame = -1, readsInCallFrame = 0;
    bool Sounding() const { return now < until; }
    bool StartHorn(int ms)
    {
        if (!callable) return false;
        ++(ms > 1 ? longCalls : shortCalls);
        lastCallFrame = frame;
        if (deaf) return true;
        if (model == Model::IgnoreWhileSounding && Sounding()) return true;
        until = now + ms;
        return true;
    }
    bool HornActive(bool& a)
    {
        ++reads;
        if (lastCallFrame == frame) ++readsInCallFrame;
        if (!readable) return false;
        a = Sounding();
        return true;
    }
};

// GET_VEHICLE_LIGHTS_STATE / SET_VEHICLE_LIGHTS / SET_VEHICLE_FULLBEAM of one car. inert: the setters are called and change nothing.
struct FakeLights
{
    bool on = false, high = false;
    bool readable = true, callable = true, inert = false;
    bool highReportsOff = false;   // 0.4.30 the live game's read: a lit high beam reads lightsOn 0 / highbeamsOn 1
    int reads = 0, setLights = 0, setHigh = 0;
    bool ReadLights(bool& o, bool& h)
    {
        ++reads;
        if (!readable) return false;
        o = on && !(highReportsOff && high);
        h = high;
        return true;
    }
    bool SetLights(int mode)
    {
        if (!callable) return false;
        ++setLights;
        if (!inert) on = mode == 2;
        return true;
    }
    bool SetFullbeam(bool h)
    {
        if (!callable) return false;
        ++setHigh;
        if (!inert) high = h;
        return true;
    }
};

// One horn run on a 120 fps clock: the controller's horn is held from riseMs to releaseMs, a fresh state every frame unless `statesUntil`
// stops them. Records what the car does.
struct HornRun
{
    double firstSound = -1.0;           // first frame the car sounds
    int gapFrames = 0;                  // frames between the rise and the release (as the copy sees them) with the car silent
    double maxGapMs = 0.0;              // the longest silent stretch inside the hold
    double quietAt = -1.0;              // first silent frame at or after the release
};
static constexpr double kFrameMs = 1000.0 / 120.0;

static HornRun RunHorn(sync::HornSync& hs, FakeHorn& car, double riseMs, double releaseMs, double endMs, double statesUntil = 1e18,
                       double holdWantUntil = -1.0)
{
    HornRun r;
    double stateMs = 0.0, gapStart = -1.0;
    for (int f = 0;; ++f)
    {
        const double t = f * kFrameMs;
        if (t > endMs) break;
        car.now = t;
        car.frame = f;
        if (t <= statesUntil) stateMs = t;
        const bool want = holdWantUntil >= 0.0 ? t < holdWantUntil : (t >= riseMs && t < releaseMs);
        sync::StepHorn(hs, car, want, stateMs, t);
        const bool s = car.Sounding();
        if (s && r.firstSound < 0.0) r.firstSound = t;
        if (t >= riseMs && t < releaseMs && r.firstSound >= 0.0)
        {
            if (!s)
            {
                ++r.gapFrames;
                if (gapStart < 0.0) gapStart = t;
            }
            else if (gapStart >= 0.0)
            {
                r.maxGapMs = std::max(r.maxGapMs, t - gapStart);
                gapStart = -1.0;
            }
        }
        if (t >= releaseMs && !s && r.quietAt < 0.0) r.quietAt = t;
    }
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------------
// A. HornSync
// ---------------------------------------------------------------------------------------------------------------------------

static void VS_Horn()
{
    std::printf("-- 0.4.19 A. horn (HornSync)\n");
    {
        sync::HornSync hs;
        FakeHorn car;
        const HornRun r = RunHorn(hs, car, 100.0, 2100.0, 2600.0);
        const sync::HornStats& s = hs.Stats();
        // 2 s held at 120 fps = 240 frames, 60 states at 30 Hz; renewals every 120 ms (125 on the frame grid) = 17 calls.
        Check("horn (a call replaces the remaining time): the rise sounds the copy in the same frame, a 2 s hold is renewed 14..18 times (not per frame, "
              "not per state) with no silent frame, the release is one 1 ms call and the copy is quiet in the next frame",
              r.firstSound >= 100.0 && r.firstSound < 100.0 + kFrameMs && r.gapFrames == 0 && car.longCalls >= 14 && car.longCalls <= 18 &&
                  car.shortCalls == 1 && r.quietAt >= 2100.0 && r.quietAt <= 2100.0 + 2 * kFrameMs && s.rises == 1 && s.releases == 1 &&
                  s.startsIssued == (uint32_t)car.longCalls && s.stopsIssued == 1,
              F("first sound %.1f, gaps %d, long calls %d, short calls %d, quiet at %.1f", r.firstSound, r.gapFrames, car.longCalls, car.shortCalls,
                r.quietAt));
    }
    {
        sync::HornSync hs;
        FakeHorn car;
        car.model = FakeHorn::Model::IgnoreWhileSounding;
        const HornRun r = RunHorn(hs, car, 100.0, 2100.0, 2600.0);
        const double tail = r.quietAt - 2100.0;
        Check("horn (a call while it sounds is ignored): the silent stretches inside a 2 s hold stay under one renewal (120 ms) and the tail after "
              "the release under the 300 ms hold",
              r.firstSound >= 100.0 && r.maxGapMs <= 120.0 + 2 * kFrameMs && r.quietAt >= 2100.0 && tail <= 300.0 + kFrameMs,
              F("longest gap %.1f ms, tail %.1f ms", r.maxGapMs, tail));
    }
    {
        // The controller's states stop at 1000 ms with the horn bit set (a lost release, the car left our range): let go 750 ms later.
        sync::HornSync hs;
        FakeHorn car;
        double stateMs = 0.0, quietAt = -1.0;
        bool soundedAgain = false;
        for (int f = 0; f * kFrameMs <= 3000.0; ++f)
        {
            const double t = f * kFrameMs;
            car.now = t;
            car.frame = f;
            if (t <= 1000.0 || t >= 2500.0) stateMs = t;            // a fresh state again at 2.5 s, still with the bit
            sync::StepHorn(hs, car, true, stateMs, t);
            if (t > 1000.0 && t < 2500.0 && !car.Sounding() && quietAt < 0.0) quietAt = t;
            if (t >= 2500.0 && car.Sounding()) soundedAgain = true;
        }
        const sync::HornStats& s = hs.Stats();
        Check("horn: a car whose states stop with the horn on is let go 750 ms after its last state (counted stale, one release call), and a fresh "
              "state with the bit starts it again",
              quietAt >= 1750.0 && quietAt <= 1750.0 + 3 * kFrameMs && s.staleReleases == 1 && s.releases == 1 && s.rises == 2 && soundedAgain &&
                  car.shortCalls == 1,
              F("quiet at %.1f, stale %u, rises %u, again %d", quietAt, s.staleReleases, s.rises, (int)soundedAgain));
    }
    {
        sync::HornConfig c;
        c.stopMs = 0;                                               // veh_horn_stop=0
        sync::HornSync hs(c);
        FakeHorn car;
        const HornRun r = RunHorn(hs, car, 100.0, 1100.0, 1600.0);
        const double tail = r.quietAt - 1100.0;
        Check("horn (veh_horn_stop=0): no release call; the last hold runs out by itself within 300 ms of the release",
              car.shortCalls == 0 && hs.Stats().stopsIssued == 0 && hs.Stats().releases == 1 && tail > kFrameMs && tail <= 300.0 + kFrameMs,
              F("short calls %d, tail %.1f ms", car.shortCalls, tail));
    }
    {
        // The read-back: what the game shows, tallied per phase; never read in the frame of a call.
        sync::HornSync good, deafSync, stuckSync;
        FakeHorn goodCar, deafCar, stuckCar;
        deafCar.deaf = true;
        stuckCar.model = FakeHorn::Model::IgnoreWhileSounding;      // ignores the 1 ms release call while it sounds
        RunHorn(good, goodCar, 100.0, 2100.0, 2600.0);
        RunHorn(deafSync, deafCar, 100.0, 2100.0, 2600.0);
        RunHorn(stuckSync, stuckCar, 100.0, 2100.0, 2600.0);
        // The rise's read-back falls due in the frame of the first renewal (both 120 ms after the rise): it must wait one frame.
        sync::HornConfig cc;
        cc.probeAfterMs = cc.renewMs;
        sync::HornSync collide(cc);
        FakeHorn collideCar;
        RunHorn(collide, collideCar, 100.0, 2100.0, 2600.0);
        const sync::HornStats& g = good.Stats();
        const sync::HornStats& d = deafSync.Stats();
        const sync::HornStats& k = stuckSync.Stats();
        Check("horn read-back (IS_HORN_ACTIVE): a working horn reads heard after the rise, sounding every 250 ms while held, quiet after the release; a "
              "deaf car reads silent in every phase; a car that ignores the release call reads still sounding; no read lands in a call's frame",
              g.riseHeard == 1 && g.riseSilent == 0 && g.heldHeard >= 6 && g.heldSilent == 0 && g.releaseQuiet == 1 && g.releaseSounding == 0 &&
                  d.riseHeard == 0 && d.riseSilent == 1 && d.heldSilent >= 6 && d.heldHeard == 0 && d.releaseQuiet == 1 && k.releaseSounding == 1 &&
                  goodCar.readsInCallFrame == 0 && deafCar.readsInCallFrame == 0 && stuckCar.readsInCallFrame == 0 && g.probes == (uint32_t)goodCar.reads &&
                  collideCar.readsInCallFrame == 0 && collide.Stats().riseHeard == 1,
              F("good: rise %u/%u held %u/%u release %u/%u; deaf: rise %u/%u held %u/%u; stuck release sounding %u; reads in call frames %d/%d/%d/%d",
                g.riseHeard, g.riseSilent, g.heldHeard, g.heldSilent, g.releaseQuiet, g.releaseSounding, d.riseHeard, d.riseSilent, d.heldHeard,
                d.heldSilent, k.releaseSounding, goodCar.readsInCallFrame, deafCar.readsInCallFrame, stuckCar.readsInCallFrame,
                collideCar.readsInCallFrame));
    }
    {
        // UNRESOLVED natives: START_VEHICLE_HORN and IS_HORN_ACTIVE cannot be called. 10 s of a held horn.
        sync::HornSync hs;
        FakeHorn car;
        car.callable = false;
        car.readable = false;
        int attempts = 0;
        for (int f = 0; f * kFrameMs <= 10000.0; ++f)
        {
            const double t = f * kFrameMs;
            car.now = t;
            car.frame = f;
            const sync::HornOut o = sync::StepHorn(hs, car, t >= 100.0, t, t);
            if (o.call != sync::HornCall::None) ++attempts;
        }
        const sync::HornStats& s = hs.Stats();
        Check("horn UNRESOLVED: a START_VEHICLE_HORN that cannot be called is tried 3 times, then left alone for 5 s (bounded: under 12 attempts in "
              "10 s, not one per renewal), and the unreadable read-back is counted, never taken as silence",
              attempts >= 3 && attempts <= 12 && s.suspends >= 1 && s.callFails == (uint32_t)attempts && s.startsIssued == 0 && s.probeFails >= 1 &&
                  s.riseSilent == 0 && s.heldSilent == 0,
              F("attempts %d, suspends %u, probe fails %u", attempts, s.suspends, s.probeFails));
    }
    {
        // 0.4.30: a police car with the siren on, E held: IS_HORN_ACTIVE reads false (the siren's second tone), the held key is the signal.
        using sync::HornBitToSend;
        const bool tone = HornBitToSend(false, true, true, true, 1.0f, true);
        const bool guards = !HornBitToSend(false, false, true, true, 1.0f, true) &&   // siren off: the read-back rules
                            !HornBitToSend(false, true, true, true, 0.0f, true) &&    // E not held
                            !HornBitToSend(false, true, false, true, 1.0f, true) &&   // a passenger
                            !HornBitToSend(false, true, true, false, 1.0f, true) &&   // the axis could not be read
                            !HornBitToSend(false, true, true, true, 1.0f, false) &&   // veh_siren_horn=0 (0.4.29)
                            HornBitToSend(true, false, false, false, 0.0f, false);    // a sounding horn is always sent
        // the bit through a copy's HornSync: E held 2 s => the copy's horn starts and is renewed while held; the 0.4.29 rule sends nothing
        auto copyCalls = [](bool sirenHornOn) {
            sync::HornSync hs;
            FakeHorn car;
            for (int f = 0; f * kFrameMs <= 3000.0; ++f)
            {
                const double t = f * kFrameMs;
                car.now = t;
                car.frame = f;
                const bool held = t >= 100.0 && t < 2100.0;
                sync::StepHorn(hs, car, HornBitToSend(false, true, true, true, held ? 1.0f : 0.0f, sirenHornOn), t, t);
            }
            return car.longCalls;
        };
        const int calls = copyCalls(true), oldCalls = copyCalls(false);
        {
            // a 100 ms tap (GTA's siren-mode switch) is never sent; a hold is, from 150 ms; a sounding horn is never delayed
            sync::SirenHornHold tap, hold, horn;
            bool tapSent = false, holdAt149 = true, holdAt150 = false;
            for (double t = 0.0; t <= 100.0; t += kFrameMs) tapSent |= tap.Step(false, true, t);
            tapSent |= tap.Step(false, false, 116.0);
            for (double t = 0.0; t < 149.0; t += 1.0) holdAt149 = hold.Step(false, true, t);
            holdAt150 = hold.Step(false, true, 150.0);
            const bool hornNow = horn.Step(true, false, 0.0);
            Check("horn (0.4.30): the siren-on key bit counts only after a 150 ms hold (a tap of E is the siren-mode switch), a sounding horn at once",
                  !tapSent && !holdAt149 && holdAt150 && hornNow, F("tap sent %d, held 148 ms %d, held 150 ms %d, horn %d", (int)tapSent, (int)holdAt149, (int)holdAt150, (int)hornNow));
        }
        Check("horn (0.4.30): siren on + E held + IS_HORN_ACTIVE false sends the horn bit (the siren tone), never with the siren off, E up, a "
              "passenger, an unreadable axis or veh_siren_horn=0; a copy fed that bit for 2 s starts its horn and renews it, the 0.4.29 rule sends none",
              tone && guards && calls >= 2 && oldCalls == 0, F("tone %d guards %d, copy calls %d (0.4.29 rule %d)", (int)tone, (int)guards, calls, oldCalls));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// B. LightsKeeper
// ---------------------------------------------------------------------------------------------------------------------------

// The engine's order per frame: ApplyVehicleFields on a change (here: the car is set at once), then the keeper.
static void LightsFrames(sync::LightsKeeper& lk, FakeLights& car, const sync::LightsWant& want, bool isNear, double fromMs, double toMs,
                         const std::function<void(double)>& each = nullptr)
{
    for (int f = 0;; ++f)
    {
        const double t = fromMs + f * kFrameMs;
        if (t > toMs) break;
        if (each) each(t);
        sync::StepLights(lk, car, want, isNear, t);
    }
}

static void VS_Lights()
{
    std::printf("-- 0.4.19 B. headlights / high beam (LightsKeeper)\n");
    const sync::LightsWant onHigh{ true, true }, off{ false, false };
    {
        sync::LightsKeeper lk;
        FakeLights car;
        car.on = car.high = true;                                   // ApplyVehicleFields set them and the game kept them
        LightsFrames(lk, car, onHigh, true, 0.0, 5000.0);
        const sync::LightsStats& s = lk.Stats();
        Check("lights: a near copy that keeps the server's headlights + high beam is read about every 500 ms and never touched",
              car.setLights == 0 && car.setHigh == 0 && s.reads >= 8 && s.reads <= 11 && s.match == s.reads && s.mismatch == 0 && s.reissued == 0,
              F("reads %u, set calls %d/%d", s.reads, car.setLights, car.setHigh));
    }
    {
        sync::LightsKeeper lk;
        FakeLights car;
        car.on = car.high = true;
        double restoredAt = -1.0;
        LightsFrames(lk, car, onHigh, true, 0.0, 4000.0, [&](double t) {
            if (t >= 1000.0 && t < 1000.0 + kFrameMs) car.on = car.high = false;   // the game drops the forced lights (an engine toggle, a re-seat)
            if (t > 1000.0 && car.on && car.high && restoredAt < 0.0) restoredAt = t;
        });
        const sync::LightsStats& s = lk.Stats();
        Check("lights: the game drops the copy's forced headlights + high beam: two reads in a row disagree, then ONE re-issue of SET_VEHICLE_LIGHTS 2 "
              "and SET_VEHICLE_FULLBEAM restores them (within about a second), counted restored",
              car.on && car.high && car.setLights == 1 && car.setHigh == 1 && s.reissued == 1 && s.restored == 1 && s.mismatch == 2 && restoredAt > 1000.0 &&
                  restoredAt < 2100.0,
              F("set calls %d/%d, mismatch %u, reissued %u, restored %u at %.0f", car.setLights, car.setHigh, s.mismatch, s.reissued, s.restored, restoredAt));
    }
    {
        // A change of the wanted state: ApplyVehicleFields' own commands ran this frame; the game takes a little time. Not judged at once.
        // A far copy is not read at all.
        sync::LightsKeeper lk;
        FakeLights car;
        LightsFrames(lk, car, off, true, 0.0, 1000.0);
        bool changeJudged = false;
        LightsFrames(lk, car, onHigh, true, 1000.0, 1100.0, [&](double t) {
            if (t >= 1060.0 && !car.on) car.on = car.high = true;     // the game shows the change 60 ms later
            changeJudged = changeJudged || lk.Stats().mismatch != 0;
        });
        sync::LightsKeeper farKeeper;
        FakeLights farCar;
        LightsFrames(farKeeper, farCar, onHigh, false, 0.0, 3000.0);
        Check("lights: a change of the server's state is not judged in the 150 ms its own commands need (no mismatch, no re-issue); a far copy is "
              "never read",
              !changeJudged && car.setLights == 0 && lk.Stats().mismatch == 0 && lk.Stats().match >= 1 && farCar.reads == 0 && farCar.setLights == 0,
              F("mismatch %u, set calls %d, far reads %d", lk.Stats().mismatch, car.setLights, farCar.reads));
    }
    {
        // A native that is called and does nothing (the game ignores SET_VEHICLE_LIGHTS on a copy), and one that cannot be read.
        sync::LightsKeeper lk;
        FakeLights car;
        car.inert = true;                                           // stays off whatever we say
        LightsFrames(lk, car, onHigh, true, 0.0, 20000.0);
        sync::LightsKeeper blind;
        FakeLights blindCar;
        blindCar.readable = false;
        LightsFrames(blind, blindCar, onHigh, true, 0.0, 5000.0);
        const sync::LightsStats& s = lk.Stats();
        Check("lights stuck: a setter that changes nothing is re-issued 3 times, then the car is left alone for 10 s (counted stuck; about 6 re-issues "
              "in 20 s, not a stream); an unreadable copy is never touched",
              s.reissued >= 3 && s.reissued <= 8 && s.stuck >= 1 && s.stuck <= 2 && s.restored == 0 && car.setLights == (int)s.reissued &&
                  blindCar.setLights == 0 && blindCar.setHigh == 0 && blind.Stats().readFails >= 8 && blind.Stats().reads == 0,
              F("reissued %u, stuck %u, set calls %d; blind read fails %u", s.reissued, s.stuck, car.setLights, blind.Stats().readFails));
    }
    {
        // 0.4.30: under the live read (highReportsOff) a copy forced on with the high beam is kept, never fought; the 0.4.29 rule (highIsLit false) fights it.
        sync::LightsKeeper lk;
        FakeLights car;
        car.highReportsOff = true;
        car.on = car.high = true;
        LightsFrames(lk, car, onHigh, true, 0.0, 10000.0);
        sync::LightsConfig oldCfg;
        oldCfg.highIsLit = false;
        sync::LightsKeeper oldKeeper(oldCfg);
        FakeLights oldCar;
        oldCar.highReportsOff = true;
        oldCar.on = oldCar.high = true;
        LightsFrames(oldKeeper, oldCar, onHigh, true, 0.0, 10000.0);
        const sync::LightsStats& s = lk.Stats();
        Check("lights (0.4.30): a copy whose game reads its lit high beam as lightsOn 0 / highbeamsOn 1 is only read (no mismatch, no re-issue in 10 s); "
              "the 0.4.29 rule (veh_lights_beam_lit=0) re-issues it and ends stuck",
              s.reads >= 16 && s.mismatch == 0 && s.reissued == 0 && car.setLights == 0 && car.setHigh == 0 && oldKeeper.Stats().reissued >= 3 &&
                  oldKeeper.Stats().stuck >= 1,
              F("new: reads %u mismatch %u reissued %u; old: reissued %u stuck %u", s.reads, s.mismatch, s.reissued, oldKeeper.Stats().reissued,
                oldKeeper.Stats().stuck));
    }
    {
        // 0.4.30 review M4: a copy that should be dark but still carries the full-beam flag reads "lit" under LightsLit; the keeper must clear
        // the flag too (SET_VEHICLE_LIGHTS(1) alone never changes the read, it would end stuck and loop every back-off)
        sync::LightsKeeper lk;
        FakeLights car;
        car.on = false;
        car.high = true;
        LightsFrames(lk, car, off, true, 0.0, 5000.0);
        const sync::LightsStats& s = lk.Stats();
        Check("lights (0.4.30): a copy wanted dark with its full-beam flag left set is put dark (the flag cleared in the first re-issue), never stuck",
              !car.on && !car.high && s.stuck == 0 && s.reissued == 1 && s.restored == 1 && car.setHigh == 1,
              F("on %d high %d, reissued %u restored %u stuck %u, fullbeam calls %d", (int)car.on, (int)car.high, s.reissued, s.restored, s.stuck, car.setHigh));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// C. VehicleStore
// ---------------------------------------------------------------------------------------------------------------------------

static void VS_Store()
{
    std::printf("-- 0.4.19 C. VehicleStore (horn, lights)\n");
    StoreRig R;
    proto::VehicleWorld w;
    for (int i = 0; i < 6; i++) { R.vs.Update(0, Drive(R.veh, 0, 1.0f + i, 10), 1000 + i * 50); R.Frame(1, 1030 + i * 50, w); }
    // The driver presses the horn: the very next frame carries it, and so does every later one (motion-only entries carry `flags`).
    bool hornNext = false, hornInMotionOnly = false, clearNext = false;
    int withHorn = 0;
    for (int i = 0; i < 8; i++)
    {
        proto::VehicleUpdate u = Drive(R.veh, 0, 7.0f + i, 10);
        u.flags |= proto::kVehHorn;
        R.vs.Update(0, u, 1400 + i * 50);
        R.Frame(1, 1430 + i * 50, w);
        const proto::VehicleState* e = R.Entry(w);
        const bool h = e && (e->flags & proto::kVehHorn);
        if (i == 0) hornNext = h;
        if (h) ++withHorn;
        if (h && e->motionOnly) hornInMotionOnly = true;
    }
    R.vs.Update(0, Drive(R.veh, 0, 20, 10), 1850);
    R.Frame(1, 1880, w);
    clearNext = R.Entry(w) && !(R.Entry(w)->flags & proto::kVehHorn);
    // Headlights + high beam: a change of flags2 is told in full for 3 frames.
    int fullsWithLights = 0;
    for (int i = 0; i < 5; i++)
    {
        proto::VehicleUpdate u = Drive(R.veh, 0, 21.0f + i, 10);
        u.flags2 = proto::kVeh2Headlights | proto::kVeh2HighBeam;
        R.vs.Update(0, u, 1900 + i * 50);
        R.Frame(1, 1930 + i * 50, w);
        const proto::VehicleState* e = R.Entry(w);
        if (e && !e->motionOnly && e->flags2 == (proto::kVeh2Headlights | proto::kVeh2HighBeam)) ++fullsWithLights;
    }
    Check("store: the controller's horn reaches an observer in the very next frame and in every frame while held (motion-only entries carry it), its "
          "clear in the frame after the release; headlights + high beam are told in full entries (3 frames)",
          hornNext && withHorn == 8 && hornInMotionOnly && clearNext && fullsWithLights >= 3,
          F("next %d, frames with horn %d/8, in motion-only %d, cleared %d, full entries with the lights %d", (int)hornNext, withHorn,
            (int)hornInMotionOnly, (int)clearNext, fullsWithLights));
}

// ---------------------------------------------------------------------------------------------------------------------------
// D. The real server
// ---------------------------------------------------------------------------------------------------------------------------

static void VS_RealServer()
{
    std::printf("-- 0.4.18 D. the real server (controller A, observer B)\n");
    Rig rig;
    if (!rig.Start())
    {
        Check("signals real server: starts", false);
        Check("signals real server: lights", false);
        Check("signals real server (0.4.30): high beam read as 0 / 1", false);
        return;
    }
    Probe alice(rig), bob(rig);
    const bool joined = alice.Join("Alice") && bob.Join("Bob");
    const bool placed = joined && PlaceAt(rig, alice, Kin(100, 100, 30)) && PlaceAt(rig, bob, Kin(104, 100, 30));
    const proto::VehicleResult spawn = placed ? VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder) : proto::VehicleResult();
    const uint16_t vid = spawn.vehId;
    const bool seen = placed && rig.Until([&] { return bob.Vehicle(vid) != nullptr; }, 2000);
    const proto::VehicleResult enter = seen ? VCmd(rig, alice, proto::VehicleOp::Enter, vid, 0) : proto::VehicleResult();
    if (!(seen && enter.code == proto::VehicleResultCode::Ok))
    {
        Check("signals real server: A drives a car B sees", false);
        Check("signals real server: lights", false);
        Check("signals real server (0.4.30): high beam read as 0 / 1", false);
        return;
    }

    using DClock = std::chrono::steady_clock;
    const auto t0 = DClock::now();
    auto ms = [&] { return std::chrono::duration<double, std::milli>(DClock::now() - t0).count(); };
    uint32_t seq = 500;
    bool hornDown = false;
    uint8_t lights2 = 0;
    // B's client in miniature: every ~8 ms a frame (StepHorn / StepLights on B's newest state, as TickVehicleSignals does), states counted.
    sync::HornSync bHorn;
    FakeHorn bCar;
    sync::LightsKeeper bKeep;
    FakeLights bLights;
    uint64_t bStatesSeen = 0;
    double bStateMs = -1e9;
    uint8_t bAppliedLights2 = 0xFF;
    bool dropLightsOnce = false;
    auto frameB = [&](double now) {
        const uint64_t states = bob.vehicleFullStates + bob.vehicleMotionOnly;
        const proto::VehicleState* st = bob.Vehicle(vid);
        if (!st) return;
        if (states != bStatesSeen)
        {
            bStatesSeen = states;
            bStateMs = now;
            if (st->flags2 != bAppliedLights2)                    // ApplyVehicleFields / ApplyVehicleBody: on a change only
            {
                bAppliedLights2 = st->flags2;
                bLights.SetLights((st->flags2 & proto::kVeh2Headlights) ? 2 : 1);
                bLights.SetFullbeam((st->flags2 & proto::kVeh2HighBeam) != 0);
            }
        }
        if (dropLightsOnce)
        {
            bLights.on = bLights.high = false;                      // B's game drops the forced lights once
            dropLightsOnce = false;
        }
        bCar.now = now;
        ++bCar.frame;
        sync::StepHorn(bHorn, bCar, (st->flags & proto::kVehHorn) != 0, bStateMs, now);
        sync::StepLights(bKeep, bLights, sync::LightsWant{ (st->flags2 & proto::kVeh2Headlights) != 0, (st->flags2 & proto::kVeh2HighBeam) != 0 },
                         true, now);
    };
    // Run until `pred` or `forMs`: A sends at 30 Hz, B runs a frame every ~8 ms, the rig is stepped in between.
    auto run = [&](double forMs, const std::function<bool(double)>& pred) {
        const double start = ms();
        double nextSend = start, nextFrame = start;
        while (ms() - start < forMs)
        {
            const double now = ms();
            if (now >= nextSend)
            {
                proto::VehicleUpdate u = Drive(vid, 100.0f, 104.0f, 0.0f);
                if (hornDown) u.flags |= proto::kVehHorn;
                u.flags2 = lights2;
                u.seq = ++seq;
                alice.s.SendVehicleState(u);
                nextSend += 33.0;
            }
            rig.Step();
            if (now >= nextFrame)
            {
                frameB(now);
                nextFrame += kFrameMs;
                if (pred && pred(now)) return true;
            }
        }
        return false;
    };
    run(600.0, nullptr);                                            // settle: B holds A's car, no horn

    // ---- the horn: 1.5 s held, then released ----
    const int callsBefore = bCar.longCalls;
    const uint64_t statesBefore = bStatesSeen;
    hornDown = true;
    const double press = ms();
    double heard = -1.0;
    int gapFrames = 0;
    run(1500.0, [&](double now) {
        if (bCar.Sounding() && heard < 0.0) heard = now;
        else if (heard >= 0.0 && !bCar.Sounding()) ++gapFrames;
        return false;
    });
    const int holdCalls = bCar.longCalls - callsBefore;
    const uint64_t holdStates = bStatesSeen - statesBefore;
    hornDown = false;
    const double release = ms();
    double quiet = -1.0;
    run(800.0, [&](double now) {
        if (!bCar.Sounding() && quiet < 0.0) quiet = now;
        return false;
    });
    Check("signals real server: A's 1.5 s horn press reaches B's copy through the server and ClientSession: sounding within 250 ms of the press, no "
          "silent frame while held, quiet within 250 ms of the release, and far fewer START_VEHICLE_HORN calls than states received",
          heard >= 0.0 && heard - press < 250.0 && gapFrames == 0 && quiet >= 0.0 && quiet - release < 250.0 && holdCalls >= 8 && holdCalls <= 16 &&
              holdStates >= 20 && (uint64_t)holdCalls * 2 <= holdStates && bHorn.Stats().releases == 1 && bCar.shortCalls == 1,
          F("heard after %.0f ms, gaps %d, quiet after %.0f ms, %d calls for %llu states, releases %u", heard - press, gapFrames, quiet - release,
            holdCalls, (unsigned long long)holdStates, bHorn.Stats().releases));

    // ---- the lights: on with the high beam; B's game drops them once; then off ----
    lights2 = proto::kVeh2Headlights | proto::kVeh2HighBeam;
    const double lightsOn = ms();
    double bSeesOn = -1.0;
    run(1000.0, [&](double now) {
        const proto::VehicleState* st = bob.Vehicle(vid);
        if (st && st->flags2 == lights2 && bSeesOn < 0.0) bSeesOn = now;
        return bSeesOn >= 0.0 && bLights.on && bLights.high;
    });
    run(300.0, nullptr);
    dropLightsOnce = true;
    const double dropped = ms();
    double restored = -1.0;
    run(2500.0, [&](double now) {
        if (now > dropped + 1.0 && bLights.on && bLights.high) { restored = now; return true; }
        return false;
    });
    run(400.0, nullptr);                                            // the keeper's next read sees the re-issue took (counted restored)
    lights2 = 0;
    const double lightsOff = ms();
    double bSeesOff = -1.0;
    run(1500.0, [&](double now) {
        const proto::VehicleState* st = bob.Vehicle(vid);
        if (st && st->flags2 == 0 && bSeesOff < 0.0) bSeesOff = now;
        return false;
    });
    const sync::LightsStats& ls = bKeep.Stats();
    Check("signals real server: A's headlights + high beam reach B within 300 ms each way; B's keeper restores them after B's game dropped them (one "
          "re-issue round, within about a second) and leaves B's copy off after A switched them off",
          bSeesOn >= 0.0 && bSeesOn - lightsOn < 300.0 && restored > 0.0 && restored - dropped < 1500.0 && ls.reissued == 1 && ls.restored == 1 &&
              bSeesOff >= 0.0 && bSeesOff - lightsOff < 300.0 && !bLights.on && !bLights.high,
          F("on after %.0f ms, restored after %.0f ms (reissued %u, restored %u), off after %.0f ms, B car on %d high %d", bSeesOn - lightsOn,
            restored - dropped, ls.reissued, ls.restored, bSeesOff - lightsOff, (int)bLights.on, (int)bLights.high));

    // ---- 0.4.30: A's game reads its high beam as lightsOn 0 / highbeamsOn 1; A's flags2 comes from ReadVehicleBody's rule ----
    FakeLights aCar;
    aCar.highReportsOff = true;
    aCar.on = aCar.high = true;
    bool rOn = true, rHigh = false;
    aCar.ReadLights(rOn, rHigh);
    lights2 = sync::Flags2FromState(sync::LightsLit(rOn, rHigh), rHigh, false, false, false, false);
    bLights.highReportsOff = true;                                  // B's game reads its copy the same way
    const sync::LightsStats before = bKeep.Stats();
    const double beamOn = ms();
    double bLit = -1.0;
    run(2500.0, [&](double now) {
        if (bLights.on && bLights.high && bLit < 0.0) bLit = now;
        return false;
    });
    const sync::LightsStats& after = bKeep.Stats();
    Check("signals real server (0.4.30): A's high beam, read by A's game as lightsOn 0 / highbeamsOn 1, reaches B as headlights + high beam within 300 ms "
          "and B's copy (read the same way) stays lit, never fought (no mismatch, no re-issue in 2.5 s)",
          !rOn && rHigh && (lights2 & proto::kVeh2Headlights) && bLit >= 0.0 && bLit - beamOn < 300.0 && bLights.on && bLights.high &&
              after.mismatch == before.mismatch && after.reissued == before.reissued && after.reads > before.reads,
          F("read %d/%d, flags2 %02x, lit after %.0f ms, B on %d high %d, mismatch +%u reissued +%u", (int)rOn, (int)rHigh, lights2, bLit - beamOn,
            (int)bLights.on, (int)bLights.high, after.mismatch - before.mismatch, after.reissued - before.reissued));
    lights2 = 0;
}

static void RunVehSignalsTests()
{
    VS_Horn();
    VS_Lights();
    VS_Store();
    VS_RealServer();
}
