// r4_t2_tests.h: desync overhaul round 4, T2 (car to car collision), included by tests/vehicle_drive_test.cpp (one include, one call).
// docs/sync-specs/vehicle-collision-authority.md has the rule and the measurements.
//
// GI (0.4.8 log, one car-to-car approach): "car to car collision is not even close and very desync". Everything here is SIMULATED (tests/vehsim_2m.h with the
// real SampleClock stamp: sample time + the controller's fastest uplink, NetClock baseline = the fastest downlink); nothing ran in the game.
//
// The baseline / candidate tables of the report are printed by the same binary, without any check: R4T2_REPORT=1 [R4T2_SEEDS=8] wine vehicle_drive_test.exe (the four scenes under the four
// game models, 'base' = round 3 as shipped against 'new'); R4T2_REPORT=1 R4T2_EXPLORE=1 (variants: grace off, no proximity, no near clock, no server impacts ...); R4T2_TRACE=1 (the
// divergence of the two cars every 250 ms after the contact); R4T2_DBG=<ms> (positions on both machines from that time); R4T2_DBG2=<ms> (one copy: target, ideal, the owner's real car).
#pragma once

#include "sync/vehicle_contact.h"
#include "sync/vehicle_drive.h"
#include "sync/vehicle_impact.h"
#include "sync/vehicle_position.h"
#include "vehsim_2m.h"

#include <cstdlib>
#include <functional>

namespace r4t2 {

namespace imp = gage::sync::impact;
using namespace gage::sync;
using vehsim::kPi;
using vehsim2::Mode;
using vehsim2::Scene;
using vehsim2::CarDef;
namespace sc2 = vehsim2::scenes;

constexpr int kR4T2Checks = 14;

// ---- the four scenes of the brief --------------------------------------------------------------------------------------------------------
// Each is sized so the cars meet ~3 s after the start (the clocks are warm by then) and the run lasts 2+ s past the contact.
inline Scene HeadOn15() {          // 15 + 15 m/s, 0.4 m lateral offset (the fronts overlap by 1.6 m of their width), closing 30 m/s
    Scene s; s.name = "head-on 15+15";
    s.cars.push_back(sc2::C("A", 0, 0.0, -45, 0, 15));
    s.cars.push_back(sc2::C("B", 1, 0.4, 45, kPi, 15));
    s.durMs = 7000; return s;
}
inline Scene SideSwipe20() {       // parallel at 20 m/s, B (faster, from behind) drifts 3.4 degrees toward A and rubs along its side; closing along the side ~12 m/s
    Scene s; s.name = "side-swipe 20";
    s.cars.push_back(sc2::C("A", 0, 0.0, 0, 0, 20));
    s.cars.push_back(sc2::C("B", 1, 6.4, -30, 0.06, 32));
    s.durMs = 7000; return s;
}
inline Scene RearEnd25() {         // A 12 m/s ahead, B 25 m/s behind, closing 13 m/s
    Scene s; s.name = "rear-end 25";
    s.cars.push_back(sc2::C("A", 0, 0.0, 0, 0, 12));
    s.cars.push_back(sc2::C("B", 1, 0.2, -42, 0, 25));
    s.durMs = 7000; return s;
}
inline Scene IntoStationary15() {  // A 15 m/s into B parked (empty); B's last driver (machine 1) stands next to it: the lead fades with the distance to HIM
    Scene s = sc2::IntoParked(true);
    s.hasViewer[1] = true; s.viewerX[1] = 3.0; s.viewerY[1] = 1.0;
    return s;
}

inline Scene Cruise15() {          // two cars side by side at 15 m/s, 8 m apart, no contact: the pure position pipeline (a window is set by the caller)
    Scene s; s.name = "cruise 15";
    s.cars.push_back(sc2::C("A", 0, 0.0, 0, 0, 15));
    s.cars.push_back(sc2::C("B", 1, 8.0, 0, 0, 15));
    s.durMs = 7000; return s;
}

// ---- the link of the brief: RTT 92 ms (46 each way), 15 ms jitter, 30 samples/s ---------------------------------------------------------------
// Round 3's logic, as shipped in 0.4.8 / 0.4.9: constant-velocity extrapolation, every contact opens the grace.
inline void Baseline(vehsim2::Opt& o) { o.accelExtrap = false; o.ownNearM = 0.0; }
inline vehsim2::Opt BriefLink(Mode mode, uint64_t seed) {
    vehsim2::Opt o;
    o.mode = mode;
    o.seed = seed;
    for (int i = 0; i < 4; ++i) { o.upMs[i] = 46.0; o.downMs[i] = 46.0; }
    o.upJitMs = 15.0; o.downJitMs = 15.0; o.loss = 0.0;
    o.sendHz = 30.0; o.sendJitMs = 6.0;
    o.stampBias = true; o.arbCorrect = true; o.nearClock = true; o.proximity = true;    // round 3's shipped configuration ...
    o.accelExtrap = true;                                                              // ... plus R4 T2's shipped default
    o.ownNearM = 10.0;                                                                 // ... and the contact gate (kContactNearM)
    return o;
}

struct Avg {
    int n = 0;
    double view[2][vehsim2::Result::kViewT] = {}, viewMax[2][vehsim2::Result::kViewT] = {}, viewVel[2][vehsim2::Result::kViewT] = {};
    double overlap = 0, dv[2] = {}, dvMaxRun = 0, closing = 0, dvStep = 0, truthMean = 0, metricMean = 0, truthMax = 0, metricMax = 0;
    double impacts = 0, applied = 0, snaps = 0, pops = 0, popMax = 0, refContact = 0, spread = 0;
    uint32_t blowups = 0;
};
template <class Fn>
inline Avg RunSeeds(const Scene& sc, Mode mode, int seeds, Fn tweak) {
    Avg a;
    for (int s = 1; s <= seeds; ++s) {
        vehsim2::Opt o = BriefLink(mode, uint64_t(s));
        tweak(o);
        const vehsim2::Result r = vehsim2::Run(sc, o);
        ++a.n;
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < vehsim2::Result::kViewT; ++k) {
                a.view[c][k] += r.viewDiv[c][k];
                a.viewMax[c][k] = std::max(a.viewMax[c][k], r.viewDiv[c][k]);
                a.viewVel[c][k] += r.viewDivVel[c][k];
            }
            a.dv[c] += r.dvMaxCar[c];
            a.dvMaxRun = std::max(a.dvMaxRun, r.dvMaxCar[c]);
        }
        a.overlap = std::max(a.overlap, r.trueOverlapMax);
        a.closing += r.closingRef; a.dvStep = std::max(a.dvStep, r.dvStepMax);
        a.truthMean += r.truthApproachMean; a.metricMean += r.metricApproachMean;
        a.truthMax = std::max(a.truthMax, r.truthApproachMax); a.metricMax = std::max(a.metricMax, r.metricApproachMax);
        a.impacts += r.impacts; a.applied += r.applied; a.snaps += r.snaps; a.pops += r.pops; a.popMax = std::max(a.popMax, r.popMax);
        a.refContact += r.refContactMs; a.spread += std::max(0.0, r.contactSpreadMs); a.blowups += r.blowups;
    }
    const double n = double(a.n);
    for (int c = 0; c < 2; ++c) {
        for (int k = 0; k < vehsim2::Result::kViewT; ++k) { a.view[c][k] /= n; a.viewVel[c][k] /= n; }
        a.dv[c] /= n;
    }
    a.closing /= n; a.truthMean /= n; a.metricMean /= n; a.impacts /= n; a.applied /= n; a.snaps /= n; a.pops /= n; a.refContact /= n; a.spread /= n;
    return a;
}
inline Avg RunSeeds(const Scene& sc, Mode mode, int seeds) { return RunSeeds(sc, mode, seeds, [](vehsim2::Opt&) {}); }

inline void PrintAvg(const char* label, const Avg& a) {
    std::printf("%-26s | refT %5.0f closing %4.1f | view A-car [0,.5,1,2s] %5.2f %5.2f %5.2f %5.2f (worst %5.2f %5.2f %5.2f %5.2f) | B-car %5.2f %5.2f %5.2f %5.2f (worst %5.2f %5.2f %5.2f %5.2f) | "
                "dv A/B %4.1f/%4.1f max %4.1f step %4.1f | ovl %4.2f | imp %3.1f app %3.1f | snaps %3.1f pops %3.1f popMax %4.2f | approach truth %.2f/%.2f metric %.2f/%.2f | blowups %u\n",
                label, a.refContact, a.closing, a.view[0][0], a.view[0][1], a.view[0][2], a.view[0][3], a.viewMax[0][0], a.viewMax[0][1], a.viewMax[0][2], a.viewMax[0][3],
                a.view[1][0], a.view[1][1], a.view[1][2], a.view[1][3], a.viewMax[1][0], a.viewMax[1][1], a.viewMax[1][2], a.viewMax[1][3], a.dv[0], a.dv[1], a.dvMaxRun, a.dvStep,
                a.overlap, a.impacts, a.applied, a.snaps, a.pops, a.popMax, a.truthMean, a.truthMax, a.metricMean, a.metricMax, a.blowups);
}

// R4T2_REPORT=1: print the baseline / candidate table (not a check)
inline void Trace(const char* label, const Scene& sc, Mode mode, uint64_t seed) {
    vehsim2::Opt o = BriefLink(mode, seed);
    std::printf("%s trace (view divergence of car 0 / car 1 after the reference contact; pos m / vel m/s):", label);
    int last = -1;
    o.viewTrace = [&](double ms, int car, double d, double dv) { if (ms > 3100) return; if (last != int(ms)) { last = int(ms); std::printf("\n  +%4.0f ms:", ms); } std::printf("  car%d %5.2f m %5.2f m/s", car, d, dv); };
    vehsim2::Run(sc, o);
    std::printf("\n");
}
inline void Dbg(const Scene& sc, Mode mode, uint64_t seed, double fromMs, double toMs, double stepMs) {
    vehsim2::Opt o = BriefLink(mode, seed);
    struct Row { double x[2][2], y[2][2], vx[2][2], vy[2][2], h[2][2]; bool real[2][2]; };
    std::vector<std::pair<double, Row>> rows;
    Row cur{};
    double lastT = -1;
    o.trace = [&](double t, int m, int c, bool real, const vehsim2::gp::Body& b) {
        if (t < fromMs || t > toMs || m > 1 || c > 1) return;
        if (std::fmod(t - fromMs, stepMs) >= 1.0) return;
        if (t != lastT) { if (lastT >= 0) rows.push_back({lastT, cur}); lastT = t; cur = Row{}; }
        cur.x[m][c] = b.p.x; cur.y[m][c] = b.p.y; cur.vx[m][c] = b.v.x; cur.vy[m][c] = b.v.y; cur.h[m][c] = b.h; cur.real[m][c] = real;
    };
    const vehsim2::Result r = vehsim2::Run(sc, o);
    std::printf("dbg: ref contact %.0f ms; columns per machine m: car0(x,y,vy)[R=real] car1(x,y,vy)\n", r.refContactMs);
    for (auto& pr : rows) {
        std::printf("t=%5.0f", pr.first);
        for (int m = 0; m < 2; ++m)
            for (int c = 0; c < 2; ++c)
                std::printf(" | m%d c%d%s (%6.2f,%7.2f) v(%5.1f,%5.1f) h%5.2f", m, c, pr.second.real[m][c] ? "R" : "c", pr.second.x[m][c], pr.second.y[m][c], pr.second.vx[m][c], pr.second.vy[m][c], pr.second.h[m][c]);
        std::printf("\n");
    }
}
inline void Explore() {
    const int seeds = 4;
    struct V { const char* n; std::function<void(vehsim2::Opt&)> f; Mode m; };
    const V vs[] = {
        {"shipped (AS) accel extrap ON", [](vehsim2::Opt&) {}, Mode::AdoptServer},
        {"constant-velocity extrap (R3)", [](vehsim2::Opt& o) { o.accelExtrap = false; }, Mode::AdoptServer},
        {"grace off", [](vehsim2::Opt& o) { o.grace = false; }, Mode::AdoptServer},
        {"no proximity", [](vehsim2::Opt& o) { o.proximity = false; }, Mode::AdoptServer},
        {"no near clock", [](vehsim2::Opt& o) { o.nearClock = false; o.arbCorrect = false; o.proximity = false; }, Mode::AdoptServer},
        {"no server impacts (A only)", [](vehsim2::Opt&) {}, Mode::Adopt},
        {"current", [](vehsim2::Opt&) {}, Mode::Current},
        {"no jitter no loss", [](vehsim2::Opt& o) { o.upJitMs = 0; o.downJitMs = 0; }, Mode::AdoptServer},
        {"tiny rtt 20", [](vehsim2::Opt& o) { for (int i = 0; i < 4; ++i) { o.upMs[i] = 10; o.downMs[i] = 10; } }, Mode::AdoptServer},
    };
    for (const Scene& sc : {RearEnd25(), SideSwipe20()}) {
        std::printf("== %s\n", sc.name.c_str());
        for (const V& v : vs) PrintAvg(v.n, RunSeeds(sc, v.m, seeds, v.f));
    }
}
inline void Dbg2(const Scene& sc, Mode mode, uint64_t seed, double fromMs, double toMs, double stepMs, int machine, int car) {
    vehsim2::Opt o = BriefLink(mode, seed);
    o.winFromMs = fromMs; o.winToMs = toMs;
    double last = -1e9;
    o.copyTrace = [&](double t, int m, int c, const VehicleSample& tg, const VehicleSample& idl, const vehsim2::gp::Body& P, const vehsim2::gp::Body& rb) {
        if (m != machine || c != car || t - last < stepMs) return;
        last = t;
        std::printf("t=%5.0f copy y %7.2f vy %5.1f | target y %7.2f vy %5.1f mode %d | ideal y %7.2f vy %5.1f | owner real y %7.2f vy %5.1f | copy-real %5.2f target-real %5.2f ideal-real %5.2f\n",
                    t, P.p.y, P.v.y, tg.pos.y, tg.vel.y, int(tg.mode), idl.pos.y, idl.vel.y, rb.p.y, rb.v.y, P.p.y - rb.p.y, tg.pos.y - rb.p.y, idl.pos.y - rb.p.y);
    };
    vehsim2::Run(sc, o);
}
inline void Report() {
    if (std::getenv("R4T2_DBG2")) { const double t0 = std::atof(std::getenv("R4T2_DBG2")); Dbg2(RearEnd25(), Mode::AdoptServer, 1, t0, t0 + 2500, 100, 1, 0); return; }
    if (std::getenv("R4T2_EXPLORE")) { Explore(); return; }
    if (std::getenv("R4T2_DBG")) {
        const double t0 = std::atof(std::getenv("R4T2_DBG"));
        Dbg(RearEnd25(), Mode::AdoptServer, 1, t0, t0 + 2500, 250);
        return;
    }
    if (std::getenv("R4T2_TRACE")) {
        Trace("rear-end 25", RearEnd25(), Mode::AdoptServer, 1);
        Trace("side-swipe 20", SideSwipe20(), Mode::AdoptServer, 1);
        return;
    }
    const int seeds = std::getenv("R4T2_SEEDS") ? std::atoi(std::getenv("R4T2_SEEDS")) : 6;
    std::printf("-- R4 T2 report: RTT 92 ms (46/46), jitter 15 ms, 30 Hz, real stamp; %d seeds. 'base' = round 3 as shipped, 'new' = accel extrapolation + contact gate\n", seeds);
    struct Row { const char* name; Scene sc; Mode mode; };
    auto brake = [](Scene s) { for (auto& c : s.cars) if (!c.empty) c.reactM = 18.0; s.name += " +brake"; return s; };
    const Row rows[] = {
        {"head-on 15+15", HeadOn15(), Mode::AdoptServer},
        {"side-swipe 20", SideSwipe20(), Mode::AdoptServer},
        {"rear-end 25", RearEnd25(), Mode::AdoptServer},
        {"stationary hit 15", IntoStationary15(), Mode::AdoptServerClaim},
        {"head-on +brake", brake(HeadOn15()), Mode::AdoptServer},
        {"rear-end +brake", brake(RearEnd25()), Mode::AdoptServer},
    };
    struct Model { const char* name; double alpha, flagHz, bump; };
    const Model models[] = {{"model 0: velocity honoured, no world contacts", 1.0, 0.0, 0.0}, {"model 1: body takes 50% of a command per frame", 0.5, 0.0, 0.0},
                            {"model 2: world flag 0.5 Hz, flag only", 1.0, 0.5, 0.0}, {"model 3: world flag 0.5 Hz + 3 m/s bump", 1.0, 0.5, 3.0}};
    for (const Model& m : models) {
        std::printf("== %s\n", m.name);
        for (const Row& r : rows) {
            for (int variant = 0; variant < 2; ++variant) {
                auto tw = [&](vehsim2::Opt& o) { o.respAlpha = m.alpha; o.worldFlagHz = m.flagHz; o.worldBumpMs = m.bump; if (variant == 0) Baseline(o); };
                char lab[64];
                std::snprintf(lab, sizeof(lab), "%s %s", variant == 0 ? "base" : "new ", r.name);
                PrintAvg(lab, RunSeeds(r.sc, r.mode, seeds, tw));
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the checks
// ---------------------------------------------------------------------------------------------------------------------------------------
static double WorstView(const Avg& a, int k) { return std::max(a.viewMax[0][k], a.viewMax[1][k]); }
static double MeanView(const Avg& a, int k) { return std::max(a.view[0][k], a.view[1][k]); }

// A: what 'drawn vs the owner's report now' can and cannot see
static void TestMetric() {
    {   // 1. the metric is blind to a wrong link lag (it uses the same lag the aim does); the truth is not
        auto run = [](double err) {
            return RunSeeds(Cruise15(), Mode::Current, 4, [&](vehsim2::Opt& o) { o.lagErrMs = err; o.winFromMs = 2500; o.winToMs = 6000; o.proximity = false; });
        };
        const Avg ok = run(0.0), bad = run(30.0), badNeg = run(-30.0);
        Check("R4 T2 metric: 'drawn vs the owner's report now' is LAG-COMPENSATED WITH THE LAG THE AIM USES, so a link lag that is 30 ms too long (ENet's RTT includes up to a tick of service delay) leaves the live number unchanged while the copy really is 0.4+ m from where its owner is (15 m/s); the simulation's ground truth sees it",
              ok.truthMean <= 0.15 && ok.metricMean <= 0.15 && bad.truthMean >= ok.truthMean + 0.35 && std::fabs(bad.metricMean - ok.metricMean) <= 0.1 &&
                  badNeg.truthMean >= ok.truthMean + 0.30 && std::fabs(badNeg.metricMean - ok.metricMean) <= 0.1,
              F("lag exact: truth %.2f metric %.2f m; +30 ms: truth %.2f metric %.2f; -30 ms: truth %.2f metric %.2f", ok.truthMean, ok.metricMean, bad.truthMean, bad.metricMean, badNeg.truthMean, badNeg.metricMean));
    }
    {   // 2. the speed bands mix distances: 'lead 104 ms against 176' is the fade, not a cap
        PositionDiag d;
        PresentLead lead(NearLeadConfig());
        const double want = 180.0;
        for (double dist = 120.0; dist > 0.5; dist -= 35.0 / 60.0) {     // a 35 m/s car closing from 120 m at 60 fps
            const double l = lead.Update(dist, want, 1000.0 / 60.0);
            PositionDiag::Frame f; f.speedMps = 35.0; f.distM = dist; f.totalM = 0.2; f.unledMs = want - l; f.leadMs = l;
            d.Add(f);
        }
        const PositionDiag::Window w = d.Take();
        const double allLead = w.leadSum[3] / w.speedFrames[3], nearLead = w.leadNearSum[3] / std::max<uint32_t>(1, w.nearFrames[3]);
        const double allAim = w.unledSum[3] / w.speedFrames[3], nearAim = w.unledNearSum[3] / std::max<uint32_t>(1, w.nearFrames[3]);
        const std::string line = PositionDiag::Format(w);
        Check("R4 T2 metric: for a 35 m/s car the all-distance 'lead' average falls to ~half of the lead it really has within 40 m (the ring beyond 70 m has none by design: GI's 104 ms against 176 ms is that arithmetic) and the line now prints the in-range numbers too",
              allLead > 70.0 && allLead < 130.0 && nearLead >= 170.0 && allAim >= 50.0 && nearAim <= 15.0 && line.find("within 40 m") != std::string::npos,
              F("all distances: lead %.0f ms, aim %.0f ms behind; within 40 m: lead %.0f, aim %.0f; %s", allLead, allAim, nearLead, nearAim, line.substr(0, 170).c_str()));
    }
}

// B: acceleration-aware extrapolation (the interpolator)
static void TestAccelExtrap() {
    auto push = [](VehicleInterpolator& ip, double p0, double v0, double a, double fromMs, double toMs, double stepMs = 33.3) {
        for (double t = fromMs; t <= toMs + 1e-6; t += stepMs) {
            VehicleSnapshot s;
            const double ts = t / 1000.0;
            double vv = v0 + a * ts;
            double pp = p0 + v0 * ts + 0.5 * a * ts * ts;
            if (vv < 0) { vv = 0; pp = p0 + v0 * v0 / (-2 * a); }
            s.tMs = t; s.pos = V(0, pp, 0); s.vel = V(0, vv, 0); s.rot = DriveQuat{0, 0, 0, 1};
            ip.Push(s);
        }
    };
    VehicleInterpConfig off; off.accelExtrapolation = false;
    {   // 3. hard braking 20 m/s at -8 m/s^2, drawn 250 ms past the newest snapshot
        VehicleInterpolator on, old(off);
        push(on, 0, 20, -8, 0, 300); push(old, 0, 20, -8, 0, 300);
        VehicleSample a, b;
        on.Sample(550, a); old.Sample(550, b);
        const double ts = 0.55, truthP = 20 * ts - 4 * ts * ts, truthV = 20 - 8 * ts;
        Check("R4 T2 accel: a car braking at 8 m/s^2 drawn 250 ms past its newest snapshot is within 0.05 m / 0.3 m/s of where it is with the acceleration carried (constant velocity: 0.25+ m and 1.8+ m/s off)",
              std::fabs(a.pos.y - truthP) <= 0.05 && std::fabs(a.vel.y - truthV) <= 0.3 && std::fabs(b.pos.y - truthP) >= 0.2 && std::fabs(b.vel.y - truthV) >= 1.5 && on.TangentialAccel() < -6.5 && old.TangentialAccel() == 0.0,
              F("with: %.3f m (truth %.3f) %.2f m/s (truth %.2f), accel %.1f; constant velocity: %.3f m, %.2f m/s", a.pos.y, truthP, a.vel.y, truthV, on.TangentialAccel(), b.pos.y, b.vel.y));
    }
    {   // 4. a stop never reverses and never overshoots the stopping point (the car is 175 ms from standing still when its newest snapshot is taken)
        VehicleInterpolator on, old(off);
        push(on, 0, 3, -8, 0, 200); push(old, 0, 3, -8, 0, 200);
        VehicleSample a, b, m;
        on.Sample(600, a); old.Sample(600, b); on.Sample(300, m);     // 400 ms past the newest snapshot (stopped 225 ms ago); and 100 ms past it (still rolling)
        const double stopP = 3.0 * 3.0 / 16.0;
        Check("R4 T2 accel: braking from 3 m/s at 8 m/s^2 stops at 0.56 m and the copy stops there (velocity 0, never negative) when it is drawn 400 ms past the newest snapshot, and still rolls (positive, slower) at 100 ms; constant velocity runs 0.4+ m past the stop",
              a.pos.y <= stopP + 0.03 && a.pos.y >= stopP - 0.03 && a.vel.y >= 0.0 && a.vel.y <= 0.02 && b.pos.y >= stopP + 0.4 && m.vel.y > 0.1 && m.vel.y < 1.4 && m.pos.y < stopP,
              F("with %.3f m %.3f m/s (stop at %.3f); 100 ms past: %.3f m %.2f m/s; constant velocity %.3f m", a.pos.y, a.vel.y, stopP, m.pos.y, m.vel.y, b.pos.y));
    }
    {   // 5. a collision is not an acceleration; a steady turn is not one either; a steady pedal is estimated
        VehicleInterpolator hit, turn, gas, one;
        push(hit, 0, 10, 0, 0, 100);
        {   // +8 m/s inside one sample interval (240 m/s^2)
            VehicleSnapshot s; s.tMs = 133.3; s.pos = V(0, 1.4, 0); s.vel = V(0, 18, 0); s.rot = DriveQuat{0, 0, 0, 1}; hit.Push(s);
        }
        for (int i = 0; i <= 9; ++i) {         // 15 m/s on a circle of 30 m: yaw rate 0.5 rad/s
            const double t = 33.3 * i, th = 0.5 * t / 1000.0;
            VehicleSnapshot s; s.tMs = t; s.pos = V(30 * (std::cos(th) - 1), 30 * std::sin(th), 0); s.vel = V(-15 * std::sin(th), 15 * std::cos(th), 0);
            s.rot = vq::FromHeadingDeg(th * 180.0 / kPi); s.hasAngVel = true; s.angVel = V(0, 0, 0.5); turn.Push(s);
        }
        push(gas, 0, 5, 4, 0, 300);
        VehicleSnapshot s1; s1.tMs = 0; s1.rot = DriveQuat{0, 0, 0, 1}; one.Push(s1);
        VehicleInterpolator turnOld(off);
        for (int i = 0; i <= 9; ++i) {
            const double t = 33.3 * i, th = 0.5 * t / 1000.0;
            VehicleSnapshot s; s.tMs = t; s.pos = V(30 * (std::cos(th) - 1), 30 * std::sin(th), 0); s.vel = V(-15 * std::sin(th), 15 * std::cos(th), 0);
            s.rot = vq::FromHeadingDeg(th * 180.0 / kPi); s.hasAngVel = true; s.angVel = V(0, 0, 0.5); turnOld.Push(s);
        }
        VehicleSample t1, t2;
        turn.Sample(450, t1); turnOld.Sample(450, t2);
        Check("R4 T2 accel: a velocity step of 8 m/s in one sample interval (a hit) is rejected (acceleration 0); a steady 15 m/s turn at 0.5 rad/s estimates ~0 and extrapolates exactly as before (under 1 cm); a steady +4 m/s^2 is estimated within 20%; one snapshot estimates 0",
              hit.TangentialAccel() == 0.0 && std::fabs(turn.TangentialAccel()) <= 0.3 && Dist(t1.pos, t2.pos) <= 0.01 && gas.TangentialAccel() >= 3.2 && gas.TangentialAccel() <= 4.8 && one.TangentialAccel() == 0.0,
              F("hit %.1f, turn %.2f (differs %.4f m), gas %.2f, one %.1f", hit.TangentialAccel(), turn.TangentialAccel(), Dist(t1.pos, t2.pos), gas.TangentialAccel(), one.TangentialAccel()));
    }
}

// C: the two-machine scenes of the brief
static void TestScenes() {
    struct Row { const char* name; Scene sc; Mode mode; };
    const Row rows[] = {{"head-on 15+15", HeadOn15(), Mode::AdoptServer}, {"side-swipe 20", SideSwipe20(), Mode::AdoptServer}, {"rear-end 25", RearEnd25(), Mode::AdoptServer},
                        {"stationary hit 15", IntoStationary15(), Mode::AdoptServerClaim}};
    constexpr int kSeeds = 6;
    {   // 6. the targets: divergence between A's view and B's view of each car <= 1.0 m at +1 s, <= 0.5 m at +2 s (worst seed, both cars); the baseline fails them in two scenes
        bool newOk = true;
        double baseWorst2 = 0, baseMean1 = 0, baseWorstSide = 0;
        std::string got;
        for (const Row& r : rows) {
            const Avg nw = RunSeeds(r.sc, r.mode, kSeeds), base = RunSeeds(r.sc, r.mode, kSeeds, [](vehsim2::Opt& o) { Baseline(o); });
            newOk = newOk && WorstView(nw, 2) <= 1.0 && WorstView(nw, 3) <= 0.5 && nw.snaps <= 0.01 && nw.refContact > 0;
            if (std::string(r.name) == "rear-end 25" || std::string(r.name) == "side-swipe 20") {
                baseWorst2 = (baseWorst2 == 0) ? MeanView(base, 3) : std::min(baseWorst2, MeanView(base, 3));
                baseMean1 = (baseMean1 == 0) ? MeanView(base, 2) : std::min(baseMean1, MeanView(base, 2));
            }
            got += F("%s%s: +1 s %.2f -> %.2f, +2 s %.2f -> %.2f (worst new %.2f / %.2f)", got.empty() ? "" : "; ", r.name, MeanView(base, 2), MeanView(nw, 2), MeanView(base, 3), MeanView(nw, 3), WorstView(nw, 2), WorstView(nw, 3));
            (void)baseWorstSide;
        }
        Check("R4 T2 scenes (RTT 92 ms, 15 ms jitter, 30 Hz, real stamp, 6 seeds): the two machines' views of each car end within 1.0 m at +1 s and 0.5 m at +2 s in the head-on, side-swipe, rear-end and stationary hits, no snaps; round 3's logic leaves 0.5+ m at +1 s and 0.55+ m at +2 s in the side-swipe and the rear-end (braking copies run a*L too fast)",
              newOk && baseWorst2 >= 0.55 && baseMean1 >= 0.5, got + F(" | base min over the two scenes: +1 s %.2f, +2 s %.2f", baseMean1, baseWorst2));
    }
    {   // 7. no velocity change larger than the physical closing speed allows (one impulse per car, restitution 0.25 + a margin)
        bool ok = true;
        std::string got;
        for (const Row& r : rows) {
            const Avg nw = RunSeeds(r.sc, r.mode, kSeeds);
            const imp::Shape sa = imp::ShapeOf(imp::MassClass::Car);
            const double bound = 1.25 * nw.closing * (sa.mass / (sa.mass + sa.mass)) * 1.15 + 1.0;   // equal cars
            ok = ok && nw.dvStep <= bound && nw.refContact > 0 && nw.closing > 5.0;
            got += F("%s%s: largest one-step change %.1f m/s (closing %.1f, bound %.1f)", got.empty() ? "" : "; ", r.name, nw.dvStep, nw.closing, bound);
        }
        Check("R4 T2 scenes: the largest velocity change of any real car between two consecutive milliseconds stays under (1 + 0.25) x closing x m_other/(m + m_other) x 1.15 + 1 m/s in all four scenes (the server residual never doubles a hit)", ok, got);
    }
    {   // 8. the divergence check runs after EVERY contact, not only after an adopted response
        const Avg side = RunSeeds(SideSwipe20(), Mode::AdoptServer, kSeeds);
        vehsim2::Opt o = BriefLink(Mode::AdoptServer, 1);
        const vehsim2::Result sr = vehsim2::Run(SideSwipe20(), o);
        const vehsim2::Result hr = vehsim2::Run(HeadOn15(), o);
        Check("R4 T2 watch: in the side-swipe nothing is adopted (adoptions 0: T4's +500 ms check compared 0 times, as in every window of GI's log) yet the watch opens on the copy's contact and compares it with its owner at +0.5, +1 and +2 s; the head-on is watched on both machines",
              sr.adoptions == 0 && sr.watchN[0] >= 1 && sr.watchN[1] >= 1 && sr.watchN[2] >= 1 && sr.watchSum[0] / sr.watchN[0] <= 1.0 && hr.watchN[0] >= 2 && hr.watchN[2] >= 2 && side.refContact > 0,
              F("side-swipe: adopted %u, compared %u/%u/%u (mean %.2f m at +0.5 s, opened by flag %u kick %u proximity %u world %u); head-on: adopted %u, compared %u/%u/%u", sr.adoptions, sr.watchN[0], sr.watchN[1], sr.watchN[2],
                sr.watchN[0] ? sr.watchSum[0] / sr.watchN[0] : 0.0, sr.watchOpened[1], sr.watchOpened[3], sr.watchOpened[4], sr.watchOpened[7], hr.adoptions, hr.watchN[0], hr.watchN[1], hr.watchN[2]));
    }
    {   // 9. every scene under every game-behaviour model (response lag, world contacts with and without a bump): the means stay inside the targets
        struct Model { const char* name; double alpha, flagHz, bump; };
        const Model models[] = {{"honoured", 1.0, 0.0, 0.0}, {"50% per frame", 0.5, 0.0, 0.0}, {"world flag 0.5 Hz", 1.0, 0.5, 0.0}, {"world flag + 3 m/s bump", 1.0, 0.5, 3.0}};
        bool ok = true;
        std::string got;
        for (const Model& m : models) {
            double w1 = 0, w2 = 0;
            for (const Row& r : rows) {
                const Avg nw = RunSeeds(r.sc, r.mode, kSeeds, [&](vehsim2::Opt& o) { o.respAlpha = m.alpha; o.worldFlagHz = m.flagHz; o.worldBumpMs = m.bump; });
                w1 = std::max(w1, MeanView(nw, 2));
                w2 = std::max(w2, MeanView(nw, 3));
            }
            ok = ok && w1 <= 1.0 && w2 <= 0.5;
            got += F("%s%s: +1 s %.2f, +2 s %.2f", got.empty() ? "" : "; ", m.name, w1, w2);
        }
        Check("R4 T2 models: under every game model (the copy takes all / half of a velocity command per frame; the world touches it 0.5 times a second with and without a 3 m/s bump) the mean divergence of every scene is within 1.0 m at +1 s and 0.5 m at +2 s", ok, got);
    }
}

// D: the contact gate
static void TestGate() {
    {   // 10. the drive controller
        auto frame = [](VehicleDriveController& ctl, double t, bool contact, bool own, bool prox, DriveVec localVel) {
            VehicleDriveInput in;
            in.target.pos = V(0, 20.0 * t / 1000.0, 0); in.target.vel = V(0, 20, 0); in.target.rot = DriveQuat{0, 0, 0, 1};
            in.target.mode = VehMode::Extrapolating;
            in.localPos = in.target.pos; in.localVel = localVel; in.localRot = DriveQuat{0, 0, 0, 1};
            in.dtMs = t > 0 ? 16.7 : 0;
            in.contact = contact; in.ownNear = own; in.proximity = prox;
            return ctl.Update(in);
        };
        auto warm = [&](VehicleDriveController& c, double& t) { for (int i = 0; i < 20; ++i, t += 16.7) frame(c, t, false, true, false, V(0, 20, 0)); };
        double t = 0;
        VehicleDriveController w, o2;
        warm(w, t);
        const VehicleDriveOutput wf = frame(w, t, true, false, false, V(0, 20, 0));      // a flag edge, nothing of ours near
        t = 0; warm(o2, t);
        const VehicleDriveOutput of = frame(o2, t, true, true, false, V(0, 20, 0));      // the same with our car near
        VehicleDriveController l, k, p;
        t = 0; warm(l, t);
        const VehicleDriveOutput lo = frame(l, t, false, false, false, V(0, 11, 0));    // a 9 m/s speed loss, nothing near
        t = 0; warm(k, t);
        const VehicleDriveOutput ki = frame(k, t, false, false, false, V(4, 20, 0));    // a 4 m/s sideways kick, nothing near
        t = 0; warm(p, t);
        const VehicleDriveOutput pr = frame(p, t, false, false, true, V(0, 20, 0));     // a predicted overlap: the proximity trigger is not gated
        const float full = wf.velocity.y;
        Check("R4 T2 gate: a collision-flag edge, a 9 m/s speed loss or a 4 m/s kick on a copy with NO vehicle of ours near opens no grace (contactOpened 0, contactIgnored 1/2/3, the velocity written is the full command, counted); with our car near the flag opens one as before; the proximity trigger (which only fires with our car near) is not gated",
              wf.contactOpened == 0 && wf.contactIgnored == 1 && !wf.graceActive && wf.graceWeight == 1.0f && std::fabs(full - 20.0f) < 0.5f && w.Stats().contactsIgnored == 1 && w.Stats().ignoredByFlag == 1 &&
                  of.contactOpened == 1 && of.graceActive && of.contactIgnored == 0 && lo.contactOpened == 0 && lo.contactIgnored == 2 && !lo.graceActive && ki.contactOpened == 0 && ki.contactIgnored == 3 && !ki.graceActive &&
                  pr.contactOpened == 4 && pr.graceActive,
              F("no car near: opened %u ignored %u grace %d vy %.1f; car near: opened %u grace %d; loss: opened %u ignored %u; kick: opened %u ignored %u; proximity: opened %u", wf.contactOpened, wf.contactIgnored, (int)wf.graceActive, full,
                of.contactOpened, (int)of.graceActive, lo.contactOpened, lo.contactIgnored, ki.contactOpened, ki.contactIgnored, pr.contactOpened));
    }
    {   // 11. the two-machine simulation: the world touches a copy 0.5 times a second; the grace lets it drift off its owner
        auto run = [](Mode m, const Scene& sc, bool gate) {
            return RunSeeds(sc, m, 8, [&](vehsim2::Opt& o) { o.worldFlagHz = 0.5; o.ownNearM = gate ? 10.0 : 0.0; });
        };
        double offWorst = 0, onWorst = 0;
        uint32_t onIgnored = 0, offIgnored = 0, onWatchFlag = 0;
        for (uint64_t seed = 1; seed <= 8; ++seed) {
            for (int g = 0; g < 2; ++g) {
                vehsim2::Opt o = BriefLink(Mode::AdoptServer, seed);
                o.worldFlagHz = 0.5; o.ownNearM = g ? 10.0 : 0.0;
                for (const Scene& sc : {HeadOn15(), RearEnd25(), SideSwipe20()}) {
                    const vehsim2::Result r = vehsim2::Run(sc, o);
                    double worst = 0;
                    for (int c = 0; c < 2; ++c) worst = std::max(worst, std::max(r.viewDiv[c][2], r.viewDiv[c][3]));
                    if (g) { onWorst = std::max(onWorst, worst); onIgnored += r.gatedIgnored; onWatchFlag += r.watchOpened[1] + r.watchOpened[4] + r.watchOpened[3]; }
                    else { offWorst = std::max(offWorst, worst); offIgnored += r.gatedIgnored; }
                }
                const vehsim2::Result rs = vehsim2::Run(IntoStationary15(), [&] { vehsim2::Opt q = BriefLink(Mode::AdoptServerClaim, seed); q.worldFlagHz = 0.5; q.ownNearM = g ? 10.0 : 0.0; return q; }());
                double worst = 0;
                for (int c = 0; c < 2; ++c) worst = std::max(worst, std::max(rs.viewDiv[c][2], rs.viewDiv[c][3]));
                if (g) onWorst = std::max(onWorst, worst); else offWorst = std::max(offWorst, worst);
            }
        }
        (void)run;
        Check("R4 T2 gate (simulation, the world flags a copy 0.5 times a second, 8 seeds, 4 scenes): without the gate a world touch opens a soft grace and a copy is up to 1.5+ m off its owner at +1 / +2 s after the hit; with it (the shipped rule) the worst is under 0.4 m, the ignored touches are counted, and the real car-to-car contacts still open their graces",
              offWorst >= 1.5 && onWorst <= 0.4 && onIgnored > 0 && offIgnored == 0 && onWatchFlag > 0, F("worst divergence at +1/+2 s: gate off %.2f m, on %.2f m; touches ignored: on %u, off %u; contact graces still opened with the gate: %u", offWorst, onWorst, onIgnored, offIgnored, onWatchFlag));
    }
}

// E: the diagnostics
static void TestDiag() {
    {   // 12. ContactWatch
        ContactWatch w;
        const bool o1 = w.Open(7, ContactWatch::kFlag, 1000.0);
        const bool o2 = w.Open(7, ContactWatch::kServerImpact, 1100.0);          // merged into the running watch
        w.Sample(7, 1400.0, 0.5, 0.4);                                              // too early: nothing
        int c1 = w.Sample(7, 1510.0, 0.50, 0.30);                                   // +0.5 s
        int c2 = w.Sample(7, 2005.0, 0.80, 0.60);                                   // +1 s
        int c3 = w.Sample(7, 3020.0, 0.20, 0.10);                                   // +2 s
        const bool again = w.Open(7, ContactWatch::kProximity, 3100.0);             // finished: a new one may start
        w.Open(9, ContactWatch::kWorld, 3100.0);
        const int late = w.Sample(9, 3100.0 + 1200.0, 3.0, 3.0);                    // read 1200 ms after it opened: +0.5 s was due 700 ms ago (skipped, counted as missed), +1 s only 200 ms ago (compared)
        const ContactWatch::Window win = w.Take();
        const std::string line = ContactWatch::Format(win);
        ContactWatch full;
        int accepted = 0;
        for (uint16_t id = 0; id < 12; ++id) accepted += full.Open(id, ContactWatch::kKick, 0.0) ? 1 : 0;
        Check("R4 T2 watch: a watch compares a copy with its owner at +0.5, +1 and +2 s (once each), a second event on a watched copy is merged, a read more than 300 ms after a check was due is skipped and counted, a finished watch can be reopened, the table holds 8, and the line says all of it",
              o1 && !o2 && c1 == 1 && c2 == 1 && c3 == 1 && again && late == 1 && win.merged == 1 && win.opened[ContactWatch::kFlag] == 1 && win.opened[ContactWatch::kProximity] == 1 && win.opened[ContactWatch::kWorld] == 1 &&
                  win.check[0].n == 1 && std::fabs(win.check[1].sum - 3.80) < 1e-9 && win.check[2].max == 0.20 && win.check[0].missed == 1 && accepted == 8 && full.Take().dropped == 4 &&
                  line.find("flag 1") != std::string::npos && line.find("+2 s: 1 compared") != std::string::npos && ContactWatch::Format(ContactWatch().Take()) == "no contact watched",
              line.substr(0, 260));
    }
    {   // 13. what a flag touched
        TouchCfg c;
        TouchEvidence own; own.ownDistM = 6.0; own.copyDistM = 3.0; own.heightM = 0.5;
        TouchEvidence copy; copy.ownDistM = 14.0; copy.copyDistM = 4.0; copy.heightM = 0.5; copy.speedLossMs = 9.0;
        TouchEvidence air; air.ownDistM = -1.0; air.copyDistM = -1.0; air.heightM = 2.4; air.speedLossMs = 6.0;
        TouchEvidence solid; solid.heightM = 0.6; solid.speedLossMs = 5.0;
        TouchEvidence ground; ground.heightM = 0.55; ground.speedLossMs = 0.4; ground.ownDistM = 10.5; ground.copyDistM = 7.5;
        TouchEvidence ownAir; ownAir.ownDistM = 4.0; ownAir.heightM = 2.4; ownAir.speedLossMs = 8.0;     // a car of ours close wins over everything else
        TouchTally t;
        t.Add(ClassifyTouch(own), false, 0.5, 0); t.Add(ClassifyTouch(ground), true, 0.55, 0.4); t.Add(ClassifyTouch(solid), true, 0.6, 5.0); t.Add(ClassifyTouch(air), true, 2.4, 6.0);
        const TouchTally::Window w = t.Take();
        Check("R4 T2 touch: a flag with a car of ours within 10 m is that car; with another copy within 7 m it is the copy; above 1.6 m it is in the air; else 3 m/s of speed lost says something solid and less says the ground / a kerb (the evidence we can read: no native names the other entity); the tally counts the gated ones and the extremes",
              ClassifyTouch(own) == TouchKind::OwnCar && ClassifyTouch(ownAir) == TouchKind::OwnCar && ClassifyTouch(copy) == TouchKind::OtherCopy && ClassifyTouch(air) == TouchKind::Airborne && ClassifyTouch(solid) == TouchKind::Solid && ClassifyTouch(ground) == TouchKind::Ground &&
                  w.byKind[0] == 1 && w.byKind[2] == 1 && w.byKind[3] == 1 && w.byKind[4] == 1 && w.gated == 3 && w.heightMax == 2.4 && w.lossMax == 6.0 && std::string(TouchName(TouchKind::Solid)).find("solid") != std::string::npos,
              F("%d %d %d %d %d gated %u", (int)ClassifyTouch(own), (int)ClassifyTouch(copy), (int)ClassifyTouch(air), (int)ClassifyTouch(solid), (int)ClassifyTouch(ground), w.gated));
    }
}

// F: the arbiter's arithmetic (GI: "a 15.6 m/s change from a 1.4 m approach is huge")
static void TestArbiterArithmetic() {
    using namespace gage::sync::impact;
    auto hit = [](MassClass ma, MassClass mb, double va, double vb, double gap) {
        const Shape sa = ShapeOf(ma), sb = ShapeOf(mb);
        Pose a, b;
        a.p = {0, 0}; a.h = 0; a.v = {0, va};
        b.p = {0, sa.hl + sb.hl + gap}; b.h = kPi; b.v = {0, -vb};
        const Manifold m = Collide(a, sa, b, sb);
        return std::make_pair(Resolve(a, sa, b, sb, m, SolverCfg()), std::make_pair(sa, sb));
    };
    const auto eq = hit(MassClass::Car, MassClass::Car, 12.5, 12.5, -0.1), deep = hit(MassClass::Car, MassClass::Car, 12.5, 12.5, -1.4);
    const auto bike = hit(MassClass::Light, MassClass::Heavy, 6.5, 6.5, -0.1);
    const double dvEq = Len(eq.first.dv[0]), dvEqB = Len(eq.first.dv[1]), dvDeep = Len(deep.first.dv[0]);
    const double momentum = eq.second.first.mass * eq.first.dv[0].y + eq.second.second.mass * eq.first.dv[1].y;
    const double dvBike = Len(bike.first.dv[0]);
    Check("R4 T2 arbiter: the change of velocity is a function of the CLOSING SPEED and the masses, not of how close the cars came: two equal cars at 25 m/s closing get 15.6 m/s each (restitution 0.25; the 15.6 in GI's log needs 25 m/s of closing, or 13 m/s for a bike into a truck), the same hit 1.4 m deeper gives the same change, and the two halves cancel in momentum",
          eq.first.valid && std::fabs(dvEq - 15.625) < 0.2 && std::fabs(dvEqB - dvEq) < 1e-6 && std::fabs(dvDeep - dvEq) < 1e-6 && std::fabs(momentum) < 1e-6 && eq.first.closing > 24.9 && eq.first.closing < 25.1 && std::fabs(dvBike - 15.6) < 1.0,
          F("equal cars %.2f / %.2f m/s (closing %.1f), 1.4 m deeper %.2f, momentum sum %.1e; bike into a truck at 13 m/s closing: bike %.2f m/s", dvEq, dvEqB, eq.first.closing, dvDeep, momentum, dvBike));
}

inline void TestR4T2() {
    std::printf("-- R4 T2: A. what the position metric measures\n");
    TestMetric();
    std::printf("-- R4 T2: B. acceleration-aware extrapolation\n");
    TestAccelExtrap();
    std::printf("-- R4 T2: C. the car-to-car scenes of the brief\n");
    TestScenes();
    std::printf("-- R4 T2: D. the contact gate\n");
    TestGate();
    std::printf("-- R4 T2: E. diagnostics\n");
    TestDiag();
    std::printf("-- R4 T2: F. the arbiter's arithmetic\n");
    TestArbiterArithmetic();
}

}   // namespace r4t2
