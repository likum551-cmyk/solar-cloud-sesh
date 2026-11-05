// 0.4.22 vehicle controls: the WIRE path (libs/sync/vehicle_controls.h). Included by t3_vehvisual_test.cpp after vehsignals_tests.h (it uses that file's Check,
// F, Drive, StoreRig, VCmd, PlaceAt, kAdder and the netrig.h Rig / Probe) and called once from main.
//
// What is tested: the controller's side (a VehControls machine fed key presses, the flags2 it sends = ControllerFlags2, the engine bit read off its car),
// the REAL server (apps/server/vehicles.h relaying the controller's flags2 and driver flags) through the REAL ClientSession (netclient), and an observer's
// side (DiffBody + CopyFlags2Delta + the engine bit, as ApplyVehicleBody / ApplyVehicleFields do). What is not: the natives themselves (the engine glue in
// apps/engine/engine.cpp TickVehicleControls / ApplyVehicleBody cannot run offline; the model cars below stand in for the game).
#pragma once

#include "sync/vehicle_controls.h"

#include <chrono>
#include <functional>

constexpr int kVehControlsWireChecks = 6;   // the number of Check() calls RunVehControlsWireTests makes (a skipped section fails main's count)

namespace vcw {

// A's game: what SET_VEHICLE_INDICATOR_LIGHTS / SET_VEHICLE_ENGINE_ON did to the car.
struct ACar
{
    uint8_t ind = 0;
    bool    engine = true;
    bool    disableAutoStart = false;
    int     indCalls = 0, engineCalls = 0;
};

// An observer's copy of the car, brought to the server's state the way the engine does: only the bits that differ (DiffBody), the indicator bits
// through CopyFlags2Delta (veh_indicator_sync), the engine on a change of the engine bit.
struct Copy
{
    uint8_t ind = 0;
    bool    engine = true;
    int     indCalls = 0, engineCalls = 0;
    sync::BodyShown shown;
    int     appliedEngine = -1;
    void Apply(const proto::VehicleState& st, bool indSync)
    {
        sync::BodyDelta d = sync::DiffBody(shown, st.flags2, st.doors);
        d.flags2 = sync::CopyFlags2Delta(d.flags2, indSync);
        if (d.flags2 & proto::kVeh2IndicatorL)
        {
            ind = (uint8_t)((st.flags2 & proto::kVeh2IndicatorL) ? (ind | proto::kVeh2IndicatorL) : (ind & ~proto::kVeh2IndicatorL));
            ++indCalls;
        }
        if (d.flags2 & proto::kVeh2IndicatorR)
        {
            ind = (uint8_t)((st.flags2 & proto::kVeh2IndicatorR) ? (ind | proto::kVeh2IndicatorR) : (ind & ~proto::kVeh2IndicatorR));
            ++indCalls;
        }
        shown = sync::BodyShown{ st.flags2, st.doors, true };
        const int e = (st.flags & proto::kVehEngine) ? 1 : 0;
        if (e != appliedEngine)
        {
            engine = e != 0;
            appliedEngine = e;
            ++engineCalls;
        }
    }
};

// A's client in miniature: the key presses, the machine, the natives it names on the model car, and the VehicleUpdate the sender builds.
struct Controller
{
    sync::VehControls vc;
    ACar     car;
    bool     indSync = true;            // veh_indicator_sync
    unsigned pending = 0;               // key presses for the next tick
    uint32_t seq = 900;
    proto::VehicleUpdate Tick(double nowMs, uint16_t vid, uint8_t shownInd)
    {
        sync::VehControlsIn in;
        in.nowMs = nowMs;
        in.presses = pending;
        pending = 0;
        in.vehicle = 77;
        in.seat = 0;
        in.shownInd = shownInd;
        in.engineOk = true;
        in.engineRunning = car.engine;
        in.steerOk = true;
        const sync::VehControlsOut o = vc.Tick(in);
        if (o.setIndicators)
        {
            car.ind = o.ind;
            ++car.indCalls;
        }
        if (o.setEngine)
        {
            const sync::VcEngineCall call = sync::EngineCallFor(o.engineOn);
            car.engine = call.on;
            car.disableAutoStart = call.disableAutoStart;
            ++car.engineCalls;
        }
        proto::VehicleUpdate u = Drive(vid, 100.0f, 104.0f, 0.0f);
        u.flags = (uint8_t)(car.engine ? proto::kVehEngine : 0);
        u.flags2 = sync::ControllerFlags2(0, vc.Indicators(), indSync);
        u.seq = ++seq;
        return u;
    }
};

}   // namespace vcw

static void VCW_Store()
{
    std::printf("-- 0.4.22 vehicle controls: the store (controller -> observer frames)\n");
    StoreRig R;
    proto::VehicleWorld w;
    for (int i = 0; i < 6; i++) { R.vs.Update(0, Drive(R.veh, 0, 1.0f + i, 10), 1000 + i * 50); R.Frame(1, 1030 + i * 50, w); }
    // The driver puts the left indicator on: the change is told in full entries (3 frames), the later frames are motion-only and the bit stays.
    int fullsWithLeft = 0;
    bool firstFull = false;
    for (int i = 0; i < 8; i++)
    {
        proto::VehicleUpdate u = Drive(R.veh, 0, 7.0f + i, 10);
        u.flags2 = sync::ControllerFlags2(0, proto::kVeh2IndicatorL, true);
        R.vs.Update(0, u, 1400 + i * 50);
        R.Frame(1, 1430 + i * 50, w);
        const proto::VehicleState* e = R.Entry(w);
        if (e && !e->motionOnly && (e->flags2 & sync::kVcIndBoth) == proto::kVeh2IndicatorL)
        {
            ++fullsWithLeft;
            if (i == 0)
                firstFull = true;
        }
    }
    // The hazards replace it, then everything is released: the very next frame carries the clear.
    proto::VehicleUpdate hz = Drive(R.veh, 0, 20.0f, 10);
    hz.flags2 = sync::ControllerFlags2(0, sync::kVcIndBoth, true);
    R.vs.Update(0, hz, 1850);
    R.Frame(1, 1880, w);
    const proto::VehicleState* eh = R.Entry(w);
    const bool hazardsNext = eh && !eh->motionOnly && (eh->flags2 & sync::kVcIndBoth) == sync::kVcIndBoth;
    proto::VehicleUpdate off = Drive(R.veh, 0, 21.0f, 10);
    off.flags2 = sync::ControllerFlags2(0, 0, true);
    R.vs.Update(0, off, 1900);
    R.Frame(1, 1930, w);
    const proto::VehicleState* ec = R.Entry(w);
    const bool clearNext = ec && (ec->flags2 & sync::kVcIndBoth) == 0;
    Check("controls wire: the store tells the controller's indicator bits to an observer in the very next frame (left, then the hazards), in full entries for at least 3 frames, and the clear in the frame after the release",
          firstFull && fullsWithLeft >= 3 && hazardsNext && clearNext, F("first %d, full entries with left %d, hazards next %d, clear next %d", (int)firstFull, fullsWithLeft, (int)hazardsNext, (int)clearNext));
}

static void VCW_RealServer()
{
    std::printf("-- 0.4.22 vehicle controls: the real server (controller A, observers B and D)\n");
    constexpr int kChecks = 5;
    auto failAll = [&](const char* why) {
        for (int i = 0; i < kChecks; ++i)
            Check(F("controls wire real server: %s", why).c_str(), false);
    };
    Rig rig;
    if (!rig.Start()) { failAll("the server starts"); return; }
    Probe alice(rig), bob(rig);
    const bool joined = alice.Join("Alice") && bob.Join("Bob");
    const bool placed = joined && PlaceAt(rig, alice, Kin(100, 100, 30)) && PlaceAt(rig, bob, Kin(104, 100, 30));
    const proto::VehicleResult spawn = placed ? VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder) : proto::VehicleResult();
    const uint16_t vid = spawn.vehId;
    const bool seen = placed && rig.Until([&] { return bob.Vehicle(vid) != nullptr; }, 2000);
    const proto::VehicleResult enter = seen ? VCmd(rig, alice, proto::VehicleOp::Enter, vid, 0) : proto::VehicleResult();
    if (!(seen && enter.code == proto::VehicleResultCode::Ok))
    {
        failAll("A drives a car B sees");
        return;
    }

    using DClock = std::chrono::steady_clock;
    const auto t0 = DClock::now();
    auto ms = [&] { return std::chrono::duration<double, std::milli>(DClock::now() - t0).count(); };
    vcw::Controller A;
    vcw::Copy bOn, bOff;                                         // B with veh_indicator_sync on, and the same state seen with it off
    // A sends at 30 Hz, the rig runs in between, B's copies follow the newest state every pass; `pred` ends the run early.
    auto run = [&](double forMs, const std::function<bool()>& pred) {
        const double start = ms();
        double nextSend = start;
        while (ms() - start < forMs)
        {
            const double now = ms();
            if (now >= nextSend)
            {
                const proto::VehicleState* st = alice.Vehicle(vid);
                alice.s.SendVehicleState(A.Tick(now, vid, st ? (uint8_t)(st->flags2 & sync::kVcIndBoth) : (uint8_t)0));
                nextSend += 33.0;
            }
            rig.Step();
            if (const proto::VehicleState* st = bob.Vehicle(vid))
            {
                bOn.Apply(*st, true);
                bOff.Apply(*st, false);
            }
            if (pred && pred())
                return true;
        }
        return false;
    };
    // A key press at the next A tick, and the time until `pred` holds on the observer (-1 = never within 1.5 s).
    auto pressAndWait = [&](unsigned key, const std::function<bool()>& pred) {
        const double at = ms();
        A.pending = key;
        return run(1500.0, pred) ? ms() - at : -1.0;
    };
    run(600.0, nullptr);                                         // settle: B holds A's car, nothing pressed

    // ---- 1. the indicators: Left, Left, Down, Down ----
    const double tL = pressAndWait(sync::kVcKeyLeft, [&] { return bOn.ind == proto::kVeh2IndicatorL; });
    const uint8_t serverL = rig.srv.FindVehicle(vid)->s.flags2 & sync::kVcIndBoth;
    const double tL0 = pressAndWait(sync::kVcKeyLeft, [&] { return bOn.ind == 0; });
    const double tH = pressAndWait(sync::kVcKeyHazard, [&] { return bOn.ind == sync::kVcIndBoth; });
    const uint8_t serverH = rig.srv.FindVehicle(vid)->s.flags2 & sync::kVcIndBoth;
    const double tH0 = pressAndWait(sync::kVcKeyHazard, [&] { return bOn.ind == 0; });
    Check("controls wire real server: A's Left / Left / Down / Down reach B's copy as left, none, both, none (each within 500 ms, the server holds the same bits)",
          tL > 0 && tL < 500 && tL0 > 0 && tL0 < 500 && tH > 0 && tH < 500 && tH0 > 0 && tH0 < 500 && serverL == proto::kVeh2IndicatorL && serverH == sync::kVcIndBoth &&
              (rig.srv.FindVehicle(vid)->s.flags2 & sync::kVcIndBoth) == 0 && A.car.indCalls == 4,
          F("left %.0f ms, off %.0f, hazards %.0f, off %.0f; server %02x / %02x; A's calls %d", tL, tL0, tH, tH0, serverL, serverH, A.car.indCalls));

    // ---- 2. the engine: Y off (stays off), Y on ----
    const double tE = pressAndWait(sync::kVcKeyEngine, [&] { return !bOn.engine; });
    const bool offSide = !A.car.engine && A.car.disableAutoStart && (rig.srv.FindVehicle(vid)->s.flags & proto::kVehEngine) == 0;
    run(400.0, nullptr);
    const bool stays = !A.car.engine && !bOn.engine;
    const double tE1 = pressAndWait(sync::kVcKeyEngine, [&] { return bOn.engine; });
    Check("controls wire real server: A's Y puts the engine bit out for B within 500 ms (A's car is off with disableAutoStart and the server holds the bit clear), it stays off, and Y again brings it back",
          tE > 0 && tE < 500 && offSide && stays && tE1 > 0 && tE1 < 500 && A.car.engine && !A.car.disableAutoStart && A.car.engineCalls == 2,
          F("off after %.0f ms (A off with the flag and the server bit clear: %d, stays %d), on after %.0f ms, A's calls %d", tE, (int)offSide, (int)stays, tE1, A.car.engineCalls));

    // ---- 3. veh_indicator_sync=0 on the controller: the machine blinks, nothing goes out; the engine key is not affected ----
    A.indSync = false;
    A.pending = sync::kVcKeyRight;
    bool leaked = false;
    run(800.0, [&] {
        if ((rig.srv.FindVehicle(vid)->s.flags2 & sync::kVcIndBoth) != 0 || bOn.ind != 0)
            leaked = true;
        return false;
    });
    const bool machineHas = A.vc.Indicators() == proto::kVeh2IndicatorR && A.car.ind == proto::kVeh2IndicatorR;
    const double tEs = pressAndWait(sync::kVcKeyEngine, [&] { return !bOn.engine; });
    pressAndWait(sync::kVcKeyEngine, [&] { return bOn.engine; });
    A.pending = sync::kVcKeyRight;                               // the machine's indicator off again
    run(300.0, nullptr);
    Check("controls wire real server: veh_indicator_sync=0 on the controller keeps the indicator bits off the wire (A's own car blinks, the server and B never see a bit) while Y still reaches B",
          machineHas && !leaked && tEs > 0 && tEs < 500 && A.vc.Indicators() == 0, F("machine %d, leaked %d, engine key after %.0f ms", (int)machineHas, (int)leaked, tEs));
    A.indSync = true;

    // ---- 4. veh_indicator_sync=0 on the observer: the bits are on the wire, the copy ignores them (and the hazards stay on for D below) ----
    const double tHz = pressAndWait(sync::kVcKeyHazard, [&] { return bOn.ind == sync::kVcIndBoth; });
    run(300.0, nullptr);
    const proto::VehicleState* bst = bob.Vehicle(vid);
    Check("controls wire real server: with veh_indicator_sync=0 on the observer the indicator bits arrive on the wire but its copy never applies them (no call at all), while a copy with the switch on shows the hazards",
          tHz > 0 && bst && (bst->flags2 & sync::kVcIndBoth) == sync::kVcIndBoth && bOff.ind == 0 && bOff.indCalls == 0 && bOn.ind == sync::kVcIndBoth,
          F("hazards for the sync copy after %.0f ms, copy with sync off: ind %02x calls %d", tHz, bOff.ind, bOff.indCalls));

    // ---- 5. a late joiner arrives while the hazards are on: its first sight of the car shows them ----
    Probe dave(rig);
    const bool daveJoined = dave.Join("Dave");
    const bool daveAt = daveJoined && PlaceAt(rig, dave, Kin(108, 100, 30));
    vcw::Copy dCopy;
    bool daveSees = false;
    if (daveAt)
        daveSees = run(3000.0, [&] {
            if (const proto::VehicleState* st = dave.Vehicle(vid))
                dCopy.Apply(*st, true);
            return dCopy.ind == sync::kVcIndBoth;
        });
    const double tOff = pressAndWait(sync::kVcKeyHazard, [&] {
        if (const proto::VehicleState* st = dave.Vehicle(vid))
            dCopy.Apply(*st, true);
        return dCopy.ind == 0;
    });
    Check("controls wire real server: a late joiner D, placed in range, gets the hazards in the car's first full entry (stream-in carries the indicator bits) and sees them go out when A switches them off",
          daveJoined && daveAt && daveSees && tOff > 0 && tOff < 500, F("joined %d, placed %d, sees %d, off after %.0f ms", (int)daveJoined, (int)daveAt, (int)daveSees, tOff));
}

static void RunVehControlsWireTests()
{
    VCW_Store();
    VCW_RealServer();
}
