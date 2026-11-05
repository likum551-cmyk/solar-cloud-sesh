// 0.4.22 "cars shake": tests for libs/sync/vehicle_shake.h. Included at the end of tests/vehicle_drive_test.cpp (needs Check, F, V, Q,
// Rng, kPi, Len from above).
//
// What is proven here, and from what:
//  - The angular velocity setter takes the BODY frame (the read-back lines of GI's live log reproduce to 3 decimals as R(q) * wrote at a
//    heading of ~238 degrees) while the getter returns the WORLD frame. Two writers of the engine, the hyper assists' anti-roll and the
//    drive controller of every remote copy, handed the setter a world vector: the old-path rows below do exactly that.
//  - What that does to a car is a SIMULATION (a two-axis pitch / roll oscillator driven by bump noise, the real HyperAssistStep and the real
//    VehicleDriveController): with the world vector the car's pitch / roll rates are turned by its heading every tick, the high-frequency
//    part of its pitch and roll grows with the heading (a buzz at tick rate), and the result depends on the direction the car faces; with
//    the body-frame vector the result is the same at every heading. The sim says nothing about how big the buzz is in the real game: the
//    live meters of the 10 s "engine: vehicle shake (0.4.22)" line measure that.
//  - The meters (LocalMeter, CopyMeter) see a 50 Hz buzz at full gain and a 2 Hz bounce at none.

#pragma once

#include "sync/vehicle_shake.h"

namespace shaketests {

constexpr int kShakeChecks = 14;

using namespace gage::sync;
using namespace gage::sync::shake;

// The four lines of the live log, gage.log.1 08:06:52 (the car had just been spawned at the player's heading, 233; yaw rate 0).
struct LiveLine { double wrote[3], read[3]; };
static const LiveLine kLive0652[4] = {
    { { -0.019, 0.032, -0.002 }, { 0.037, -0.001, -0.001 } },
    { { 0.037, -0.001, -0.002 }, { -0.020, -0.030, -0.002 } },
    { { -0.023, -0.027, -0.002 }, { -0.010, 0.034, -0.003 } },
    { { -0.009, 0.040, -0.003 }, { 0.038, -0.014, -0.003 } },
};

static double LiveMisfit(double headingDeg, bool asBody) {
    const DriveQuat q = vq::FromHeadingDeg(headingDeg);
    double worst = 0;
    for (const LiveLine& l : kLive0652) {
        const DriveVec w = V(l.wrote[0], l.wrote[1], l.wrote[2]);
        const DriveVec r = V(l.read[0], l.read[1], l.read[2]);
        worst = std::max(worst, Dist3(asBody ? BodyToWorld(q, w) : w, r));
    }
    return worst;
}

// ---- the two-axis oscillator the sims share ----------------------------------------------------------------------------

struct Osc2 {
    double th[2] = { 0, 0 }, om[2] = { 0, 0 };   // pitch about the body x axis, roll about the body y axis; body rates
    void Step(Rng& rng, double wn, double zeta, double dt, double noise) {
        for (int a = 0; a < 2; ++a) {
            double n = -6.0;
            for (int i = 0; i < 12; ++i) n += rng.Uniform();   // ~ N(0, 1)
            om[a] += (-wn * wn * th[a] - 2.0 * zeta * wn * om[a] + n * noise) * dt;
            th[a] += om[a] * dt;
        }
    }
    // the orientation of a car that faces `headingDeg` with this pitch / roll
    DriveQuat Quat(double headingDeg) const { return vq::Mul(vq::FromHeadingDeg(headingDeg), vq::Exp(V(th[0], th[1], 0))); }
};

struct LoopResult { double thetaRmsDeg = 0, hfDeg = 0, errMaxDeg = 0; int ticks = 0, blends = 0; };

// ---- B. the hyper assists' anti-roll against a car with a suspension ----------------------------------------------------

// mode 0: no assist, 1: the pre-0.4.22 write (the world vector handed to the setter), 2: the body-frame write. The game's setter is the
// live log's: the vector written is a BODY vector.
static LoopResult AntiRollLoop(int mode, double headingDeg, double wn, double zeta, uint64_t seed) {
    Rng rng(seed);
    Osc2 car;
    const HyperAssist a = MakeHyperAssist();
    LoopResult r;
    double p1[2] = { 0, 0 }, p2[2] = { 0, 0 }, sumTh = 0, sumHf = 0;
    int n = 0, nHf = 0;
    const double tick = 0.01;
    const int sub = 4;
    for (int t = 0; t < 3000; ++t) {
        const DriveQuat q = car.Quat(headingDeg);
        if (mode != 0) {
            HyperAssistIn in;
            const DriveVec f = vq::Rotate(q, V(0, 1, 0)), rt = vq::Rotate(q, V(1, 0, 0)), up = vq::Rotate(q, V(0, 0, 1));
            const DriveVec w = BodyToWorld(q, V(car.om[0], car.om[1], 0));         // GET_ENTITY_ROTATION_VELOCITY
            in.vel = { f.x * 40.0f, f.y * 40.0f, f.z * 40.0f };
            in.fwd = { f.x, f.y, f.z };
            in.right = { rt.x, rt.y, rt.z };
            in.up = { up.x, up.y, up.z };
            in.angVel = { w.x, w.y, w.z };
            in.dt = float(tick);
            in.grounded = true;
            in.mask = kHyperAssistAll;
            const HyperAssistOut out = HyperAssistStep(a, in);
            const DriveVec world = V(out.angVel.x, out.angVel.y, out.angVel.z);
            const DriveVec wrote = mode == 2 ? WorldToBody(q, world) : world;       // what the engine hands SET_ENTITY_ANGULAR_VELOCITY
            const DriveVec took = WorldToBody(q, BodyToWorld(q, wrote));            // the game: wrote is a body vector
            car.om[0] = took.x;
            car.om[1] = took.y;
        }
        for (int s = 0; s < sub; ++s) car.Step(rng, wn, zeta, tick / sub, 6.0);
        const double a0 = car.th[0] * 180.0 / kPi, a1 = car.th[1] * 180.0 / kPi;
        if (t > 200) {
            sumTh += a0 * a0 + a1 * a1;
            ++n;
            if (t > 202) {
                const double s0 = a0 - 2 * p1[0] + p2[0], s1 = a1 - 2 * p1[1] + p2[1];
                sumHf += s0 * s0 + s1 * s1;
                ++nHf;
            }
        }
        p2[0] = p1[0]; p2[1] = p1[1];
        p1[0] = a0; p1[1] = a1;
    }
    r.thetaRmsDeg = std::sqrt(sumTh / n / 2);
    r.hfDeg = std::sqrt(sumHf / nHf / 2);
    r.ticks = n;
    return r;
}

// ---- C. a remote copy's pitch / roll under the drive controller --------------------------------------------------------

// The copy is parked on the spot its owner's pose says; the owner's own suspension moves its pitch / roll (a second oscillator), the
// controller commands omega = target omega + gain * rotation error every tick. old: o.angularVelocity (a world vector) goes to the body
// frame setter; new: o.angularVelocityBody. Stops at the first quaternion blend (the controller gave up on the angular command).
static LoopResult CopyLoop(bool body, double headingDeg, double wn, double zeta, uint64_t seed) {
    Rng rngT(seed), rngC(seed + 1);
    Osc2 tgt, car;
    VehicleDriveController ctl{ VehicleDriveConfig() };
    LoopResult r;
    double p1[2] = { 0, 0 }, p2[2] = { 0, 0 }, sumTh = 0, sumHf = 0;
    int n = 0, nHf = 0;
    const double tick = 0.01;
    const int sub = 4;
    for (int t = 0; t < 1500; ++t) {
        for (int s = 0; s < sub; ++s) tgt.Step(rngT, wn, zeta, tick / sub, 6.0);
        const DriveQuat qt = tgt.Quat(headingDeg), q = car.Quat(headingDeg);
        VehicleSample tg;
        tg.pos = V(10, 20, 5);
        tg.rot = qt;
        tg.mode = VehMode::Interpolating;
        const DriveVec wt = BodyToWorld(qt, V(tgt.om[0], tgt.om[1], 0));
        tg.angVel = V(wt.x, wt.y, wt.z);
        const VehicleDriveOutput o = ctl.Update(Input(tg, V(10, 20, 5), q, V(0, 0, 0), tick * 1000.0));
        if (t > 0) {                                                   // the first frame snaps onto the target
            if (o.setQuaternion) { ++r.blends; break; }
            if (t > 200) r.errMaxDeg = std::max(r.errMaxDeg, double(o.rotErrorDeg));
            if (o.setAngularVelocity) {
                const DriveVec wrote = body ? o.angularVelocityBody : o.angularVelocity;
                const DriveVec took = WorldToBody(q, BodyToWorld(q, wrote));   // the game takes a body vector
                car.om[0] = took.x;
                car.om[1] = took.y;
            }
        }
        for (int s = 0; s < sub; ++s) car.Step(rngC, wn, zeta, tick / sub, 6.0);
        const double a0 = car.th[0] * 180.0 / kPi, a1 = car.th[1] * 180.0 / kPi;
        if (t > 200) {
            sumTh += a0 * a0 + a1 * a1;
            ++n;
            if (t > 202) {
                const double s0 = a0 - 2 * p1[0] + p2[0], s1 = a1 - 2 * p1[1] + p2[1];
                sumHf += s0 * s0 + s1 * s1;
                ++nHf;
            }
        }
        p2[0] = p1[0]; p2[1] = p1[1];
        p1[0] = a0; p1[1] = a1;
        ++r.ticks;
    }
    if (n) r.thetaRmsDeg = std::sqrt(sumTh / n / 2);
    if (nHf) r.hfDeg = std::sqrt(sumHf / nHf / 2);
    return r;
}

// ---- D. the meters ----------------------------------------------------------------------------------------------------

static void FeedLocal(LocalMeter& m, double t0Ms, int ticks, double speed, const std::function<void(int, double&, double&, double&)>& sig) {
    for (int i = 0; i < ticks; ++i) {
        double pitch = 0, roll = 0, vz = 0;
        sig(i, pitch, roll, vz);
        const DriveQuat q = vq::Mul(vq::FromHeadingDeg(30), vq::Exp(V(pitch * kPi / 180.0, roll * kPi / 180.0, 0)));
        m.Add(t0Ms + i * 10.0, V(0, speed, vz), q);
    }
}

static void TestShake() {
    std::printf("-- 0.4.22 cars shake: the angular velocity frame, the anti-roll and copy loops, the meters\n");

    // A. the frame, from the live log
    double bestH = -1, bestE = 1e9;
    for (int h10 = 0; h10 < 3600; ++h10) {
        const double e = LiveMisfit(h10 / 10.0, true);
        if (e < bestE) { bestE = e; bestH = h10 / 10.0; }
    }
    Check("frame: the 4 read-backs of the live log are R(q) * wrote to 3 decimals at a heading near the player's (233): the setter takes the BODY frame",
          bestE < 0.0015 && bestH > 232.0 && bestH < 244.0, F("best heading %.1f, worst misfit %.4f rad/s", bestH, bestE));
    Check("frame: read as a WORLD vector none of the 4 lines fits (misfit over 0.05 rad/s): the old writes were not taken as world vectors",
          LiveMisfit(bestH, false) > 0.05, F("misfit %.4f", LiveMisfit(bestH, false)));
    {
        const DriveQuat q = vq::FromHeadingDeg(bestH);
        int body = 0, world = 0, both = 0, neither = 0;
        for (const LiveLine& l : kLive0652) {
            const AngFrame f = ClassifyAngWrite(q, V(l.wrote[0], l.wrote[1], l.wrote[2]), V(l.read[0], l.read[1], l.read[2]));
            body += f == AngFrame::Body; world += f == AngFrame::World; both += f == AngFrame::Both; neither += f == AngFrame::Neither;
        }
        const DriveVec w = V(0.2f, -0.1f, 0.3f);
        const bool world1 = ClassifyAngWrite(q, w, w) == AngFrame::World;
        const bool both1 = ClassifyAngWrite(vq::FromHeadingDeg(0), w, w) == AngFrame::Both;
        const bool neither1 = ClassifyAngWrite(q, w, V(5, 5, 5)) == AngFrame::Neither && ClassifyAngWrite(q, w, V(NAN, 0, 0)) == AngFrame::Neither;
        const bool bodyOnly = ClassifyAngWrite(q, V(0, 0, 0.4f), V(0, 0, 0.4f)) == AngFrame::Both;   // a pure yaw is no evidence
        Check("frame: ClassifyAngWrite calls the live lines Body, a read-back equal to the write World, a heading of 0 or a pure yaw Both, garbage / NaN Neither",
              body == 4 && world == 0 && both == 0 && neither == 0 && world1 && both1 && neither1 && bodyOnly,
              F("live lines body %d world %d both %d neither %d", body, world, both, neither));
    }
    {
        Rng rng(5);
        int n = 0, ok = 0;
        for (int i = 0; i < 300; ++i) {
            const DriveQuat q = vq::Mul(vq::FromHeadingDeg(rng.Range(0, 360)), vq::Exp(V(rng.Range(-0.5, 0.5), rng.Range(-0.5, 0.5), rng.Range(-1, 1))));
            const DriveVec v = V(rng.Range(-3, 3), rng.Range(-3, 3), rng.Range(-3, 3));
            ++n;
            if (Dist3(WorldToBody(q, BodyToWorld(q, v)), v) < 1e-4 && std::fabs(Len(BodyToWorld(q, v)) - Len(v)) < 1e-4) ++ok;
        }
        Check("frame: WorldToBody undoes BodyToWorld and both keep the length (300 random tilted cars)", n == 300 && ok == 300, F("%d/%d", ok, n));
    }
    {
        AngFrameJudge j;
        j.Configure(true);
        bool startsBody = j.UseBody();
        for (int i = 0; i < 7; ++i) j.Add(AngFrame::World);
        for (int i = 0; i < 40; ++i) { j.Add(AngFrame::Both); j.Add(AngFrame::Neither); }
        const bool still = j.UseBody() && j.Flips() == 0;                      // 7 world-only, no lead of 8, Both / Neither are no evidence
        j.Add(AngFrame::World);
        const bool flipped = !j.UseBody() && j.Flips() == 1;                   // the 8th world-only read-back
        for (int i = 0; i < 15; ++i) j.Add(AngFrame::Body);                    // evidence is cumulative: 8 world vs 15 body is a lead of 7
        const bool notYet = !j.UseBody();
        j.Add(AngFrame::Body);
        const bool back = j.UseBody() && j.Flips() == 2;
        AngFrameJudge off;
        off.Configure(false);
        for (int i = 0; i < 30; ++i) off.Add(AngFrame::Body);
        const bool locked = !off.UseBody() && off.Flips() == 0 && off.Total().body == 30 && off.Locked();
        Check("frame: the judge starts on the body frame, flips after a lead of 8 read-backs (not on Both / Neither), flips back, and stays on the world frame when the fix is off",
              startsBody && still && flipped && notYet && back && locked, F("%d %d %d %d %d %d", startsBody, still, flipped, notYet, back, locked));
    }

    // B. the anti-roll loop (the real HyperAssistStep, a simulated pitch / roll suspension)
    {
        const double kWn[2] = { 2.0 * kPi * 1.5, 2.0 * kPi * 2.5 };
        const double kZeta[2] = { 0.3, 0.7 };
        bool invariant = true, damps = true, buzzes = true, headingDep = true;
        int runs = 0, tickMin = 1 << 30;
        std::string got;
        for (int m = 0; m < 2; ++m) {
            const LoopResult free0 = AntiRollLoop(0, 0, kWn[m], kZeta[m], 7);
            const LoopResult ref = AntiRollLoop(2, 0, kWn[m], kZeta[m], 7);
            double oldHfMax = 0, oldHfMin = 1e9;
            for (int hd = 0; hd < 360; hd += 45) {
                const LoopResult nw = AntiRollLoop(2, hd, kWn[m], kZeta[m], 7);
                const LoopResult old = AntiRollLoop(1, hd, kWn[m], kZeta[m], 7);
                runs += 2;
                tickMin = std::min(tickMin, std::min(nw.ticks, old.ticks));
                if (std::fabs(nw.hfDeg - ref.hfDeg) > 2e-3 * ref.hfDeg || std::fabs(nw.thetaRmsDeg - ref.thetaRmsDeg) > 2e-3 * ref.thetaRmsDeg) invariant = false;
                if (!(nw.thetaRmsDeg < 0.97 * free0.thetaRmsDeg) || !(nw.hfDeg < 1.3 * free0.hfDeg)) damps = false;
                if (hd >= 90 && hd <= 270 && !(old.hfDeg > 2.0 * free0.hfDeg)) buzzes = false;
                oldHfMax = std::max(oldHfMax, old.hfDeg);
                oldHfMin = std::min(oldHfMin, old.hfDeg);
            }
            if (!(oldHfMax > 3.0 * oldHfMin)) headingDep = false;
            got += F("[%.1f Hz: free HF %.4f, body-frame %.4f, old %.4f..%.4f deg] ", kWn[m] / (2 * kPi), free0.hfDeg, ref.hfDeg, oldHfMin, oldHfMax);
        }
        Check("anti-roll loop: with the body-frame write the car's pitch / roll are the same at all 8 headings, damped (amplitude under 0.97x, buzz under 1.3x of an unassisted car)",
              invariant && damps && runs == 32 && tickMin > 2700, got);
        Check("anti-roll loop: the old world-vector write buzzes (pitch / roll high-frequency part over 2x an unassisted car at headings 90..270, over 3x between its best and worst heading)",
              buzzes && headingDep, got);
    }

    // C. a parked copy's pitch / roll under the drive controller
    {
        const double wn = 2.0 * kPi * 2.5, zeta = 0.7;
        const LoopResult ref = CopyLoop(true, 0, wn, zeta, 11);
        bool invariant = true, oldDiffers = false;
        int runs = 0;
        double oldWorst = 0;
        std::string got;
        for (int hd = 0; hd < 360; hd += 45) {
            const LoopResult nw = CopyLoop(true, hd, wn, zeta, 11);
            const LoopResult old = CopyLoop(false, hd, wn, zeta, 11);
            runs += 2;
            if (nw.blends != 0 || nw.ticks != ref.ticks || std::fabs(nw.errMaxDeg - ref.errMaxDeg) > 2e-3 * ref.errMaxDeg || std::fabs(nw.hfDeg - ref.hfDeg) > 2e-3 * ref.hfDeg) invariant = false;
            if (hd == 180 && (old.blends > 0 || old.errMaxDeg > 5.0 * ref.errMaxDeg)) oldDiffers = true;
            oldWorst = std::max(oldWorst, old.blends > 0 ? 99.0 : old.errMaxDeg);
        }
        got = F("body-frame: max rotation error %.3f deg, HF %.4f deg at every heading; old worst %.3f deg (99 = gave up to a quaternion blend)", ref.errMaxDeg, ref.hfDeg, oldWorst);
        Check("copy loop: with the controller's body-frame command (angularVelocityBody) a parked copy tracks its owner's pitch / roll identically at all 8 headings",
              invariant && runs == 16 && ref.ticks > 1400 && ref.errMaxDeg > 0.0, got);
        Check("copy loop: with the world vector handed to the body-frame setter the copy facing south (heading 180) is far worse than at heading 0",
              oldDiffers, got);
    }

    // D. the meters
    {
        LocalMeter smooth;
        FeedLocal(smooth, 0, 1000, 20.0, [](int i, double& p, double& r, double& vz) {
            const double t = i * 0.01;
            p = 1.0 * std::sin(2 * kPi * 2.0 * t);
            r = 1.5 * std::sin(2 * kPi * 1.5 * t + 1.0);
            vz = 0.4 * std::sin(2 * kPi * 2.0 * t);
        });
        const LocalMeter::Window ws = smooth.Take();
        LocalMeter buzz;
        FeedLocal(buzz, 0, 1000, 20.0, [](int i, double& p, double& r, double& vz) {
            p = 0.2 * ((i & 1) ? 1.0 : -1.0);        // 50 Hz at tick rate, 0.2 degree
            r = 0.1 * ((i & 1) ? -1.0 : 1.0);
            vz = 0.3 * ((i & 1) ? 1.0 : -1.0);
        });
        const LocalMeter::Window wb = buzz.Take();
        Check("local meter: a 2 Hz bounce of 1 degree reads as nothing (pitch / roll high-frequency part under 0.02 degree, no vertical sign flips), a 50 Hz buzz of 0.2 degree as 4x its amplitude",
              ws.ticks == 998 && ws.band[1].ticks == 998 && ws.band[1].PitchRms() < 0.02 && ws.band[1].RollRms() < 0.02 && ws.band[1].vzFlips == 0 &&
                  wb.ticks == 998 && std::fabs(wb.band[1].PitchRms() - 0.8) < 0.02 && std::fabs(wb.band[1].RollRms() - 0.4) < 0.02 &&
                  wb.band[1].vzFlips > 990 && std::fabs(wb.band[1].FlipsPerS() - 100.0) < 2.0,
              F("smooth ticks %u pitch %.4f roll %.4f flips %u | buzz ticks %u pitch %.3f roll %.3f flips %u (%.1f/s)", ws.ticks, ws.band[1].PitchRms(),
                ws.band[1].RollRms(), ws.band[1].vzFlips, wb.ticks, wb.band[1].PitchRms(), wb.band[1].RollRms(), wb.band[1].vzFlips,
                wb.band[1].FlipsPerS()));
        LocalMeter bands, gap;
        FeedLocal(bands, 0, 100, 5.0, [](int, double& p, double&, double&) { p = 0.0; });
        FeedLocal(bands, 1000, 100, 20.0, [](int, double& p, double&, double&) { p = 0.0; });
        FeedLocal(bands, 2000, 100, 45.0, [](int, double& p, double&, double&) { p = 0.0; });
        const LocalMeter::Window wband = bands.Take();
        // a 400 ms pause with a 3 degree jump across it must not read as a jolt
        FeedLocal(gap, 0, 50, 20.0, [](int, double& p, double&, double&) { p = 0.0; });
        FeedLocal(gap, 900, 50, 20.0, [](int, double& p, double&, double&) { p = 3.0; });
        const LocalMeter::Window wgap = gap.Take();
        Check("local meter: speed bands 0-10 / 10-30 / over 30 m/s fill separately, and a pause longer than 30 ms restarts the history (no jolt across it)",
              wband.band[0].ticks > 90 && wband.band[1].ticks > 90 && wband.band[2].ticks > 90 && wband.band[0].MeanSpeed() < 10 &&
                  wband.band[1].MeanSpeed() > 10 && wband.band[1].MeanSpeed() < 30 && wband.band[2].MeanSpeed() > 30 && wgap.restarts == 1 &&
                  wgap.band[1].pitchMax < 0.01,
              F("ticks %u/%u/%u, gap restarts %u, gap max %.4f", wband.band[0].ticks, wband.band[1].ticks, wband.band[2].ticks, wgap.restarts,
                wgap.band[1].pitchMax));
    }
    {
        CopyMeter steady, buzz, decay, snap;
        for (int i = 0; i < 400; ++i) {
            steady.Add(10.0, V(0.002f, 0, 0), V(0, 0, 0), 0.0, false);
            const float s = (i & 1) ? 0.03f : -0.03f;                   // 3 cm each way every frame, parked
            buzz.Add(10.0, V(s, 0, 0), V(0, float(0.5 * (s / 0.03) * kPi / 180.0), 0), 0.0, false);
            decay.Add(10.0, V(float(0.5 * std::exp(-0.05 * i)), 0, 0), V(0, 0, 0), 20.0, false);
            snap.Add(10.0, V(i == 200 ? 5.0f : 0.001f, 0, 0), V(0, 0, 0), 0.0, i == 200);
        }
        const CopyMeter::Window a = steady.Take(), b = buzz.Take(), c = decay.Take(), d = snap.Take();
        Check("copy meter: a steady error reads 0, a +-3 cm / +-0.5 degree flip-flop of a parked copy reads 4x its amplitude with a flip every frame, a convergent error has no flips",
              a.bucket[CopyMeter::Parked].frames == 398 && a.bucket[CopyMeter::Parked].PosHfRmsMm() < 0.01 && a.bucket[CopyMeter::Parked].posFlips == 0 &&
                  b.bucket[CopyMeter::Parked].frames == 398 && std::fabs(b.bucket[CopyMeter::Parked].PosHfRmsMm() - 120.0) < 2.0 &&
                  std::fabs(b.bucket[CopyMeter::Parked].RotHfRmsMdeg() - 2000.0) < 40.0 && b.bucket[CopyMeter::Parked].posFlips == 398 &&
                  b.bucket[CopyMeter::Parked].rotFlips == 398 && c.bucket[CopyMeter::Moving].frames == 398 && c.bucket[CopyMeter::Moving].posFlips == 0 &&
                  c.bucket[CopyMeter::Moving].PosHfRmsMm() < 1.0,
              F("steady %u frames %.4f mm | buzz %.1f mm %.0f mdeg flips %u/%u | decay flips %u %.3f mm", a.bucket[0].frames, a.bucket[0].PosHfRmsMm(),
                b.bucket[0].PosHfRmsMm(), b.bucket[0].RotHfRmsMdeg(), b.bucket[0].posFlips, b.bucket[0].rotFlips, c.bucket[2].posFlips, c.bucket[2].PosHfRmsMm()));
        Check("copy meter: a snapped frame restarts the history (the 5 m jump is no HF spike) and the target speed picks the bucket (0.5 / 3 m/s)",
              d.restarts == 1 && d.bucket[CopyMeter::Parked].posMax < 0.01 && CopyMeter::KindOf(0.49) == CopyMeter::Parked &&
                  CopyMeter::KindOf(0.5) == CopyMeter::Slow && CopyMeter::KindOf(2.99) == CopyMeter::Slow && CopyMeter::KindOf(3.0) == CopyMeter::Moving,
              F("restarts %u, max %.4f m", d.restarts, d.bucket[0].posMax));
    }
    {
        // UNEVEN ticks: a car rolling at a steady 20 deg/s / a copy whose error grows at a steady 0.5 m/s, sampled with the gaps alternating
        // 8 / 12 ms (local) and 8 / 16 ms (copy). Nothing buzzes. The plain second difference p - 2 p1 + p2 would read rate * (dt - dt1) =
        // 0.08 degree / 4 mm of buzz out of that alone, so the same series is also summed the plain way to prove the fixture binds.
        LocalMeter lm;
        CopyMeter cm;
        double t = 0, tc = 0, pPlain = 0, p1 = 0, p2 = 0, ePlain = 0, e1 = 0, e2 = 0, sumP = 0, sumE = 0;
        int nP = 0, nE = 0;
        for (int i = 0; i < 400; ++i) {
            const double dt = (i & 1) ? 12.0 : 8.0, dtc = (i & 1) ? 16.0 : 8.0;
            t += dt;
            tc += dtc;
            const double roll = 20.0 * t / 1000.0, err = 0.5 * tc / 1000.0;
            lm.Add(t, V(0, 20.0f, 0), vq::Mul(vq::FromHeadingDeg(30), vq::Exp(V(0, roll * kPi / 180.0, 0))));
            cm.Add(dtc, V(float(err), 0, 0), V(0, 0, 0), 0.0, false);
            pPlain = roll;
            ePlain = err;
            if (i >= 2) { const double d = pPlain - 2 * p1 + p2, de = ePlain - 2 * e1 + e2; sumP += d * d; sumE += de * de; ++nP; ++nE; }
            p2 = p1; p1 = pPlain;
            e2 = e1; e1 = ePlain;
        }
        const LocalMeter::Window wl = lm.Take();
        const CopyMeter::Window wc = cm.Take();
        const double plainRoll = std::sqrt(sumP / nP), plainMm = std::sqrt(sumE / nE) * 1000.0;
        Check("meters: uneven ticks are no buzz (a steady 20 deg/s roll reads under 0.005 degree, a steady 0.5 m/s copy error under 0.5 mm) where the plain second difference of the same series reads over 0.05 degree / 2 mm",
              wl.band[1].ticks == 398 && wl.band[1].RollRms() < 0.005 && wc.bucket[CopyMeter::Parked].frames == 398 &&
                  wc.bucket[CopyMeter::Parked].PosHfRmsMm() < 0.5 && plainRoll > 0.05 && plainMm > 2.0,
              F("meter roll %.5f deg, copy %.4f mm | plain roll %.4f deg, copy %.3f mm", wl.band[1].RollRms(), wc.bucket[CopyMeter::Parked].PosHfRmsMm(),
                plainRoll, plainMm));
    }
}

}   // namespace shaketests
