// r3_t5_tests.h: desync overhaul round 3, T5 (car collisions and car position), included by tests/vehicle_drive_test.cpp (one include, one call).
// docs/sync-specs/vehicle-position.md has the measurements and the design; this file pins them.
//
// The live evidence (GI's gage.log, 0.4.7, protocol v6): "p2 hits me on his screen, for me he is about 2-3 meters away just staggering"; shared delay 84 ms
// with 58 delay rises per 10 s; T4's machinery fired 0 times on that machine ("copies whose response was adopted 0 (contacts left to the grace 2)").
//
//   A. POSITION. tests/vehsim.h (one car, one receiver) with the REAL SampleClock stamp (NetCfg::stampBias): round 2's present-time lead closes the render
//      DELAY but not the network latency the sender's stamp does not carry (its fastest uplink + our fastest downlink), so a 25 m/s copy is drawn ~2 m
//      from where its owner has it NOW. The near-car clock (NetClock::NearT + the link lag, NearLeadWantMs) closes it, and the shared delay can no longer
//      move a near car. The 58 delay rises are sub-millisecond ripple.
//   B. THE CLOCKS' PIECES (NearT, LinkLagMs, NearLeadWantMs, the lead ranges, the extrapolation cap).
//   C. THE TWO-MACHINE SIMULATION (tests/vehsim_2m.h) with the real stamp: contacts land at the same instant on both screens, the server's arbiter still
//      sees the touch when the two controllers' uplinks differ (the half-round-trip correction), the proximity contact path.
//   D. The proximity prediction (PredictOverlap) and the drive controller's use of it.
//   E. The diagnostics (PositionDiag, the rise statistics).
// Every case asserts BOTH sides where there is an old behaviour: the round-2 / unfixed numbers show the problem at least this large (so a regression of the
// harness fails here), the new ones are at most this large.
#pragma once

#include "sync/vehicle_impact.h"
#include "sync/vehicle_position.h"
#include "vehsim_2m.h"

namespace r3t5 {

namespace imp = gage::sync::impact;
namespace vs = vehsim;
using namespace gage::sync;

constexpr int kR3T5Checks = 26;

// ---- A. position (one car) ------------------------------------------------------------------------------------------------------------

// The live rhythm: a 30 Hz sender into the 30 Hz tick, WorldState and VehicleWorld of one tick 0.5 ms apart (601 frames per 10 s), a clean link (arrival gap
// max 36 ms), the SampleClock's stamp. `fix` = the near-car clock with the link lag the two RTTs give.
static vs::Metrics RunOne(const vs::Script& sc, double v, bool fix, double upMs = 40, double downMs = 40, double jit = 3.0, double loss = 0.0, bool bias = true,
                          double lagOverrideMs = -1.0, double holeFromMs = -1.0, double holeToMs = -1.0, uint64_t seed = 1, double distM = 10.0) {
    (void)v;
    vs::Truth tr(sc);
    vs::RunCfg rc;
    rc.net.stampBias = bias;
    rc.net.secondStream = true;
    rc.net.upMs = upMs; rc.net.downMs = downMs;
    rc.net.upJitterMs = 3.0; rc.net.downJitterMs = jit; rc.net.loss = loss;
    rc.net.stallFromMs = holeFromMs; rc.net.stallToMs = holeToMs;
    rc.net.seed = seed;
    rc.lead = true; rc.leadDistM = distM;
    rc.nearClock = fix;
    rc.linkLagMs = fix ? (lagOverrideMs >= 0.0 ? lagOverrideMs : LinkLagMs(upMs + downMs, upMs + downMs)) : 0.0;
    if (fix) rc.leadCfg = NearLeadConfig();
    return vs::Run(tr, rc);
}

static void TestPosition() {
    {   // 1. the reproduction: 2-3 m at 25 m/s is the network latency the stamp lacks
        const vs::Metrics real = RunOne(vs::scn::Straight(25, 30000), 25, false);
        const vs::Metrics ideal = RunOne(vs::scn::Straight(25, 30000), 25, false, 40, 40, 3.0, 0.0, false);
        const double msEq = real.nowMean / 25.0 * 1000.0;
        Check("R3 T5 repro: with the real SampleClock stamp round 2's lead draws a 25 m/s copy 1.8+ m from where its owner has it NOW (about up + down = 80 ms of travel; GI saw 2-3 m); the old simulation's ideal stamp hid more than 0.8 m of it",
              real.nowMean >= 1.8 && real.nowP95 <= real.nowMean + 0.15 && msEq >= 65.0 && msEq <= 95.0 && real.nowFrames > 1000 && ideal.nowMean <= real.nowMean - 0.8,
              F("real stamp: mean %.2f p95 %.2f max %.2f m (= %.0f ms of travel, %d frames); ideal stamp: mean %.2f m", real.nowMean, real.nowP95, real.nowMax, msEq, real.nowFrames, ideal.nowMean));
    }
    {   // 2. the near-car clock at every speed band
        bool ok = true;
        std::string got;
        for (double v : {3.0, 10.0, 25.0, 40.0}) {
            const vs::Metrics o = RunOne(vs::scn::Straight(v, 24000), v, false), n = RunOne(vs::scn::Straight(v, 24000), v, true);
            ok = ok && n.nowP95 <= 0.2 && n.nowMax <= 0.3 && o.nowMean >= 0.055 * v && n.nowFrames > 800;
            got += F("%s%.0f m/s: %.2f -> %.2f m (p95 %.2f)", got.empty() ? "" : "; ", v, o.nowMean, n.nowMean, n.nowP95);
        }
        Check("R3 T5 fix: the near-car clock + link lag draws the copy within 0.2 m (p95) of the owner's position now at 3 / 10 / 25 / 40 m/s (round 2: 55 ms of travel or more, 3.3 m at 40 m/s)", ok, got);
    }
    {   // 3. an asymmetric path: the RTT-derived lag is exact when both ends see the same link; the owner-unknown fallback uses our round trip for both halves
        const vs::Metrics o = RunOne(vs::scn::Straight(25, 24000), 25, false, 20, 70), n = RunOne(vs::scn::Straight(25, 24000), 25, true, 20, 70);
        const vs::Metrics wrong = RunOne(vs::scn::Straight(25, 24000), 25, true, 20, 70, 3.0, 0.0, true, 40.0);   // half the lag: what a missing LinkInfo could cost
        Check("R3 T5 fix: on a 20 up / 70 down path the lag the two round trips give (90 ms) leaves under 0.15 m where round 2 left 2+ m; a lag that is 50 ms short (a wrong owner round trip) leaves a proportional 1.2+ m: the owner's number matters, which is why the server tells it (LinkInfo)",
              o.nowMean >= 2.0 && n.nowMean <= 0.15 && wrong.nowMean >= 1.0 && wrong.nowMean < o.nowMean && LinkLagMs(90.0, 0.0) == 90.0,
              F("round 2 %.2f m; RTT-derived lag %.2f m; 40 ms instead of 90: %.2f m", o.nowMean, n.nowMean, wrong.nowMean));
    }
    {   // 4. a manoeuvre: lane change and hard brake
        const vs::Metrics lo = RunOne(vs::scn::LaneChange(25), 25, false), ln = RunOne(vs::scn::LaneChange(25), 25, true);
        const vs::Metrics bo = RunOne(vs::scn::HardBrake(25), 25, false), bn = RunOne(vs::scn::HardBrake(25), 25, true);
        Check("R3 T5 fix: through a lane change and a hard brake (9 m/s^2) the extrapolated copy stays within 0.6 m (p95) of the owner now and its mean error is a fraction of round 2's (the price: dead reckoning is wrong for a moment at a steering or braking onset)",
              ln.nowP95 <= 0.6 && bn.nowP95 <= 0.6 && ln.nowMean <= lo.nowMean * 0.25 && bn.nowMean <= bo.nowMean * 0.4 && lo.nowP95 >= 1.8,
              F("lane change mean %.2f -> %.2f (p95 %.2f -> %.2f); brake mean %.2f -> %.2f (p95 %.2f -> %.2f)", lo.nowMean, ln.nowMean, lo.nowP95, ln.nowP95, bo.nowMean, bn.nowMean, bo.nowP95, bn.nowP95));
    }
    {   // 5. the shared delay (jitter 30 ms, 5% loss, a 400 ms hole at 12 s) cannot move a near copy
        const vs::Metrics o = RunOne(vs::scn::Straight(25, 30000), 25, false, 40, 40, 30.0, 0.05, true, -1.0, 12000, 12400);
        const vs::Metrics n = RunOne(vs::scn::Straight(25, 30000), 25, true, 40, 40, 30.0, 0.05, true, -1.0, 12000, 12400);
        Check("R3 T5 fix: with 30 ms downlink jitter, 5% loss and a 400 ms hole the delay rises (110 ms and a 37 ms jump) and the near copy stays within 0.2 m of the owner (max), no pop over 0.2 m; round 2 sat 2 m off",
              o.delayRisesBig >= 1 && o.delayMaxMs >= 105.0 && n.nowMax <= 0.2 && n.popMax <= 0.2 && o.nowMean >= 1.9,
              F("delay %.0f..%.0f ms (%u rises over 5 ms, largest %.0f ms); round 2 mean %.2f; near mean %.2f max %.2f pop %.2f", n.delayMinMs, n.delayMaxMs, n.delayRisesBig, n.riseMaxMs, o.nowMean, n.nowMean, n.nowMax, n.popMax));
    }
    {   // 6. the stagger is not the delay: 58 rises per 10 s are ripple
        const vs::Metrics m = RunOne(vs::scn::Straight(25, 40000), 25, false);
        const double per10 = m.delayRises / m.runSec * 10.0;
        Check("R3 T5 stagger: at the live rhythm the shared delay makes 30+ 'rises' per 10 s (GI: 58) but their mean is under 1 ms and the delay never leaves a 3 ms band after warm-up: the counter was ripple, not a flapping target; the near copy's frame-to-frame jitter is not worse than round 2's",
              per10 >= 30.0 && m.riseMeanMs < 1.0 && m.delayMaxMs - m.delayMinMs <= 3.0 && m.delayMeanMs >= 80.0 && m.delayMeanMs <= 90.0,
              F("%.1f rises per 10 s, mean %.2f ms, delay %.0f..%.0f (mean %.1f) ms, the one start-up rise %.1f ms", per10, m.riseMeanMs, m.delayMinMs, m.delayMaxMs, m.delayMeanMs, m.riseMaxMs));
        const vs::Metrics n = RunOne(vs::scn::Straight(25, 40000), 25, true);
        Check("R3 T5 stagger: the near copy's frame-to-frame error change is no worse than round 2's (p95 within 1 cm, max within 3 cm): the new clock adds no jitter of its own",
              n.stepP95 <= m.stepP95 + 0.01 && n.stepMax <= m.stepMax + 0.03 && n.nowFrames > 1500, F("step p95 %.3f -> %.3f m, max %.3f -> %.3f m", m.stepP95, n.stepP95, m.stepMax, n.stepMax));
    }
}

// ---- B. the pieces ----------------------------------------------------------------------------------------------------------------------

static void TestPieces() {
    {   // 7. LinkLagMs / NearLeadWantMs / ranges
        const PresentLeadConfig nc = NearLeadConfig(), oc;
        Check("R3 T5 pieces: LinkLagMs = half our round trip + half the owner's (80 ms for 80 / 80, 50 for 20 / 80); the owner's unknown (0) stands for ours; each half capped at 150 ms; non-finite or negative reads as 0",
              LinkLagMs(80, 80) == 80.0 && LinkLagMs(20, 80) == 50.0 && LinkLagMs(80, 0) == 80.0 && LinkLagMs(1000, 1000) == 300.0 && LinkLagMs(std::nan(""), 80) == 40.0 &&
                  LinkLagMs(-5, 0) == 0.0 && LinkLagMs(0, 0) == 0.0);
        Check("R3 T5 pieces: NearLeadWantMs = (nearT + lag) - (renderT - extra), never negative: the delay and the class extra cancel (100 ms delay + 33 ms extra + 80 ms lag -> 213 ms), a clock ahead of the near time wants 0",
              std::fabs(NearLeadWantMs(1000.0, 80.0, 900.0, 33.0) - 213.0) < 1e-9 && NearLeadWantMs(900.0, 0.0, 1000.0, 0.0) == 0.0 && NearLeadWantMs(std::nan(""), 80.0, 900.0, 0.0) == 0.0 &&
                  std::fabs(NearLeadWantMs(1000.0, -50.0, 900.0, 0.0) - 100.0) < 1e-9);
        Check("R3 T5 pieces: the near lead ranges are full within 40 m and none beyond 70 m, capped at 450 ms; round 2's stay 30 / 45 m / 250 ms (the switch back); SetConfig changes the ranges without touching the lead",
              nc.fullWithinM == 40.0f && nc.zeroBeyondM == 70.0f && nc.maxLeadMs == 450.0 && oc.fullWithinM == 30.0f && oc.zeroBeyondM == 45.0f && oc.maxLeadMs == 250.0 &&
                  PresentLead::Weight(nc, 55.0) == 0.5);
        PresentLead pl(oc);
        pl.Update(10.0, 200.0, 0.0);
        const double capped = pl.LeadMs();
        pl.SetConfig(nc);
        const double kept = pl.LeadMs();
        pl.Update(10.0, 400.0, 1000.0);
        Check("R3 T5 pieces: PresentLead::SetConfig keeps the lead it has (200 -> 200 ms capped by the old config's 250) and the new cap lets it reach 400 ms",
              capped == 200.0 && kept == 200.0 && std::fabs(pl.LeadMs() - 400.0) < 1e-9, F("%.0f %.0f %.0f", capped, kept, pl.LeadMs()));
    }
    {   // 8. NearT
        NetClock c(NetClockConfig::ForTickHz(30.0));
        double now = 1000.0;
        for (int i = 0; i < 90; ++i, now += 33.3) c.OnFrame(uint32_t(i), now);   // frames exactly on the tick: the offset is constant
        const double t0 = c.NearT(now), again = c.NearT(now);
        double prev = t0;
        bool mono = true;
        for (int i = 0; i < 30; ++i) { now += 16.7; const double t = c.NearT(now); mono = mono && t >= prev; prev = t; }
        Check("R3 T5 NearT: idempotent for one nowMs, never runs backwards, advances at the local clock's rate (500 ms of local time -> 500 ms +- 10%)",
              t0 == again && mono && std::fabs((prev - t0) - 501.0) <= 55.0, F("%.1f -> %.1f over 501 ms", t0, prev));
        // a delay rise moves RenderT but not NearT
        NetClock d(NetClockConfig::ForTickHz(30.0));
        double tn = 0;
        for (int i = 0; i < 200; ++i, tn += 33.3) d.OnFrame(uint32_t(i), tn + 40.0);
        const double dl0 = d.DelayMs();
        const double r0 = d.RenderT(tn), n0 = d.NearT(tn);
        for (int i = 200; i < 230; ++i, tn += 33.3) d.OnFrame(uint32_t(i), tn + 40.0 + ((i % 3) ? 60.0 : 0.0));   // jitter: the delay rises
        const double dl1 = d.DelayMs();
        const double n1 = d.NearT(tn);
        Check("R3 T5 NearT: a jitter burst raises the shared delay (render clock aims lower) while the near clock keeps following the local clock (its offset to now - baseline stays within 12 ms)",
              dl1 > dl0 + 10.0 && std::fabs((n1 - (tn - d.BaselineMs()))) < 12.0 && d.Stats().nearResyncs == 0 && r0 > 0.0, F("delay %.0f -> %.0f; near - (now - baseline) = %.1f ms", dl0, dl1, n1 - (tn - d.BaselineMs())));
    }
    {   // 9. the extrapolation cap grows with the lead
        VehicleInterpolator ip;
        VehicleSnapshot a;
        a.pos = V(0, 0, 0); a.vel = V(25, 0, 0); a.tMs = 1000; a.rot = DriveQuat{0, 0, 0, 1};
        ip.Push(a);
        VehicleSample s0, s1, s2;
        ip.Sample(1000 + 600, s0);
        ip.SetExtrapolateMaxMs(700.0);
        ip.Sample(1000 + 600, s1);
        ip.SetExtrapolateMaxMs(std::nan(""));
        ip.Sample(1000 + 600, s2);
        Check("R3 T5 pieces: a copy drawn 600 ms past its newest snapshot is Holding (stopped at 500 ms of travel) under the default cap and Extrapolating (15 m) once the cap grew with the lead (700 ms); a non-finite value is ignored",
              s0.mode == VehMode::Holding && std::fabs(s0.pos.x - 12.5) < 0.01 && s1.mode == VehMode::Extrapolating && std::fabs(s1.pos.x - 15.0) < 0.01 && s2.mode == VehMode::Extrapolating,
              F("%d %.2f / %d %.2f / %d", (int)s0.mode, s0.pos.x, (int)s1.mode, s1.pos.x, (int)s2.mode));
    }
}

// ---- C. two machines, the real stamp ----------------------------------------------------------------------------------------------------

struct Avg2 { double pos1 = 0, pos3 = 0, ovMax = 0, spread = 0; uint32_t impacts = 0, applied = 0, noLocal = 0, prox = 0; int n = 0; };
static Avg2 Run2(const vehsim2::Scene& sc, vehsim2::Mode mode, bool fix, bool prox, double up0, double down0, double up1, double down1, int seeds = 4, double upJ = 3.0,
                 double downJ = 3.0, double loss = 0.0) {
    Avg2 a;
    for (int s = 1; s <= seeds; ++s) {
        vehsim2::Opt o;
        o.mode = mode;
        o.seed = uint64_t(s);
        o.upMs[0] = up0; o.downMs[0] = down0; o.upMs[1] = up1; o.downMs[1] = down1; o.upMs[2] = up1; o.downMs[2] = down1;
        o.upJitMs = upJ; o.downJitMs = downJ; o.loss = loss;
        o.stampBias = true;
        o.arbCorrect = fix;
        o.nearClock = fix;
        o.proximity = prox;
        const vehsim2::Result r = vehsim2::Run(sc, o);
        a.pos1 += r.posErr[1]; a.pos3 += r.posErr[2]; a.ovMax = std::max(a.ovMax, r.trueOverlapMax); a.spread += std::max(0.0, r.contactSpreadMs);
        a.impacts += r.impacts; a.applied += r.applied; a.noLocal += r.noLocal; a.prox += r.graces; ++a.n;
    }
    a.pos1 /= seeds; a.pos3 /= seeds; a.spread /= seeds;
    return a;
}

static void TestSim2() {
    using vehsim2::Mode;
    namespace sc = vehsim2::scenes;
    {   // 10. head-on and rear-end on 40 ms links
        const Avg2 ho = Run2(sc::HeadOn(), Mode::AdoptServer, false, false, 40, 40, 40, 40), hn = Run2(sc::HeadOn(), Mode::AdoptServer, true, false, 40, 40, 40, 40);
        const Avg2 ro = Run2(sc::RearEnd(), Mode::AdoptServer, false, false, 40, 40, 40, 40), rn = Run2(sc::RearEnd(), Mode::AdoptServer, true, false, 40, 40, 40, 40);
        Check("R3 T5 sim (real stamp, 40 ms each way): the cars overlap 1.5+ m at the same instant today (head-on) and the rear-end ends 5+ m off with the touch 300+ ms apart on the two screens; with the near-car clock + the arbiter correction the overlap is under 0.15 m, the contacts within 10 ms, the rear-end under 1.5 m",
              ho.ovMax >= 1.5 && hn.ovMax <= 0.15 && ro.pos1 >= 5.0 && ro.spread >= 300.0 && rn.spread <= 10.0 && rn.pos1 <= 1.5 && hn.pos1 <= ho.pos1,
              F("head-on overlap %.2f -> %.2f m, pos %.2f -> %.2f; rear-end pos %.2f -> %.2f m, contact spread %.0f -> %.0f ms", ho.ovMax, hn.ovMax, ho.pos1, hn.pos1, ro.pos1, rn.pos1, ro.spread, rn.spread));
    }
    {   // 10b. the hostile link of the usual matrix: machines 1 and 2 on 90 ms each way with 20 / 40 ms jitter and 5% loss, machine 0 on 25 ms
        const Avg2 ho = Run2(sc::HeadOn(), Mode::AdoptServer, false, false, 25, 25, 90, 90, 6, 20, 40, 0.05), hn = Run2(sc::HeadOn(), Mode::AdoptServer, true, true, 25, 25, 90, 90, 6, 20, 40, 0.05);
        const Avg2 ro = Run2(sc::RearEnd(), Mode::AdoptServer, false, false, 25, 25, 90, 90, 6, 20, 40, 0.05), rn = Run2(sc::RearEnd(), Mode::AdoptServer, true, true, 25, 25, 90, 90, 6, 20, 40, 0.05);
        const Avg2 so = Run2(sc::SustainedPush(), Mode::AdoptServer, false, false, 25, 25, 90, 90, 6, 20, 40, 0.05), sn = Run2(sc::SustainedPush(), Mode::AdoptServer, true, true, 25, 25, 90, 90, 6, 20, 40, 0.05);
        Check("R3 T5 sim (hostile link: 90 ms each way, 20 / 40 ms jitter, 5% loss, 6 seeds): head-on 1.5+ m -> under 1.0 m, rear-end 4+ m -> under 2.0 m with the touches 500+ ms apart -> under 20 ms, a sustained push over 3 s 8+ m -> under 7.5 m; the price: the sustained push's real cars overlap more (1.0 m today -> under 1.3 m) because the arbiter now sees the touches and fires again",
              ho.pos1 >= 1.4 && hn.pos1 <= 1.0 && ro.pos1 >= 4.0 && rn.pos1 <= 2.0 && ro.spread >= 400.0 && rn.spread <= 20.0 && so.pos3 >= 8.0 && sn.pos3 <= 7.5 && sn.ovMax <= 1.3 && sn.ovMax >= so.ovMax,
              F("head-on %.2f -> %.2f; rear-end %.2f -> %.2f (spread %.0f -> %.0f ms); sustained 3 s %.2f -> %.2f, overlap %.2f -> %.2f", ho.pos1, hn.pos1, ro.pos1, rn.pos1, ro.spread, rn.spread, so.pos3, sn.pos3, so.ovMax, sn.ovMax));
    }
    {   // 11. different uplinks: the arbiter sees the touch only with the correction
        const Avg2 to = Run2(sc::TBone(), Mode::AdoptServer, false, false, 15, 15, 60, 60), tn = Run2(sc::TBone(), Mode::AdoptServer, true, false, 15, 15, 60, 60);
        Check("R3 T5 sim: controllers on 15 ms and 60 ms links (the SampleClock places their samples 45 ms apart): the server's arbiter MISSES a 15 / 12 m/s T-bone entirely today (0 impacts in 4 runs, the T4 machinery silent, the cars end 8+ m off); with the half-round-trip correction and the near clock it resolves every one and the cars end under 4 m",
              to.impacts == 0 && to.pos1 >= 8.0 && tn.impacts >= 4 && tn.applied >= 8 && tn.pos1 <= 4.0, F("impacts %u -> %u, applied %u -> %u, pos %.2f -> %.2f m", to.impacts, tn.impacts, to.applied, tn.applied, to.pos1, tn.pos1));
    }
    {   // 12. the correction alone, no near clock: the server finds the touch
        vehsim2::Opt o;
        o.mode = Mode::AdoptServer; o.seed = 1;
        o.upMs[0] = 15; o.downMs[0] = 15; o.upMs[1] = 60; o.downMs[1] = 60;
        o.upJitMs = 3; o.downJitMs = 3; o.loss = 0.0; o.stampBias = true;
        const vehsim2::Result off = vehsim2::Run(sc::TBone(), o);
        o.arbCorrect = true;
        const vehsim2::Result on = vehsim2::Run(sc::TBone(), o);
        Check("R3 T5 sim: the arbiter's correction by itself (copies still lag) turns 0 impacts into 1 for the 15 ms / 60 ms pair -- it is the half round trip that lines the two tracks up",
              off.impacts == 0 && on.impacts == 1, F("impacts %u -> %u", off.impacts, on.impacts));
    }
    {   // 13. the slew keeps the arbiter's track increasing
        double cur = 0.0, t = 0.0, lastT = -1e9;
        bool inc = true;
        const double targets[] = {10, 10, 10, 60, 60, 5, 5, 5, 80, 80};
        for (int i = 0; i < 10; ++i) {
            for (int k = 0; k < 6; ++k) {                       // six samples (33 ms apart, arriving 2..60 ms apart) per target
                const double dt = (k % 2) ? 2.0 : 60.0;
                cur = imp::SlewArbCorrMs(cur, targets[i] * 0.5, dt);
                t += 33.3;
                const double placed = t - cur;
                inc = inc && placed > lastT;
                lastT = placed;
            }
        }
        const double first = imp::SlewArbCorrMs(0.0, 40.0, 1e9), none = imp::SlewArbCorrMs(5.0, -3.0, 100.0);
        Check("R3 T5 server: the correction follows the round trip at most 0.25 ms per ms (successive placed times keep increasing through 10 -> 60 -> 5 -> 80 ms round-trip swings), the first call takes the target, a negative or non-finite target reads as 0",
              inc && first == 40.0 && none == 0.0 && imp::SlewArbCorrMs(0.0, 100.0, 40.0) == 10.0 && imp::SlewArbCorrMs(7.0, std::nan(""), 100.0) == 0.0, F("%.1f %.1f", first, none));
    }
}

// ---- D. the proximity contact -----------------------------------------------------------------------------------------------------------

static imp::Pose P(double x, double y, double heading, double vx, double vy, double w = 0.0) {
    imp::Pose p;
    p.p = {x, y}; p.v = {vx, vy}; p.h = heading; p.w = w;
    return p;
}

static void TestProximity() {
    const imp::ProximityCfg cfg;
    using imp::MassClass;
    {   // 14. PredictOverlap
        // head-on 20 + 20 m/s, 10 m between the origins: the boxes (4.6 m long + the margins) meet within 120 ms
        const imp::ProximityOut a = imp::PredictOverlap(P(0, 0, 0, 0, 20), MassClass::Car, P(0, 10, vs::kPi, 0, -20), MassClass::Car, cfg);
        const imp::ProximityOut far = imp::PredictOverlap(P(0, 0, 0, 0, 20), MassClass::Car, P(0, 30, vs::kPi, 0, -20), MassClass::Car, cfg);
        const imp::ProximityOut lane = imp::PredictOverlap(P(0, 0, 0, 0, 20), MassClass::Car, P(3.5, 10, vs::kPi, 0, -20), MassClass::Car, cfg);
        const imp::ProximityOut apart = imp::PredictOverlap(P(0, 0, 0, 0, -20), MassClass::Car, P(0, 10, kPi, 0, 20), MassClass::Car, cfg);
        const imp::ProximityOut slow = imp::PredictOverlap(P(0, 0, 0, 0, 0.5), MassClass::Car, P(0, 6.2, kPi, 0, -0.5), MassClass::Car, cfg);
        const imp::ProximityOut touching = imp::PredictOverlap(P(0, 0, 0, 0, 20), MassClass::Car, P(0, 4.4, vs::kPi, 0, -20), MassClass::Car, cfg);
        Check("R3 T5 proximity: a head-on pair 10 m apart at 40 m/s closing is predicted to touch within the horizon (closing ~40 m/s); the same pair 30 m apart, in adjacent lanes (3.5 m), moving apart, closing at 1 m/s, or already touching is not",
              a.hit && a.tHitMs > 0.0 && a.tHitMs <= cfg.horizonMs && a.closing > 39.0 && !far.hit && !lane.hit && !apart.hit && !slow.hit && !touching.hit,
              F("hit %d at %.0f ms closing %.1f; far %d lane %d apart %d slow %d touching %d", (int)a.hit, a.tHitMs, a.closing, (int)far.hit, (int)lane.hit, (int)apart.hit, (int)slow.hit, (int)touching.hit));
        // a bike (narrow) passes a car 1.5 m to the side; a car in the same place would touch it
        const imp::ProximityOut bike = imp::PredictOverlap(P(0, 0, 0, 0, 20), MassClass::Light, P(1.75, 10, vs::kPi, 0, -20), MassClass::Car, cfg);
        const imp::ProximityOut carr = imp::PredictOverlap(P(0, 0, 0, 0, 20), MassClass::Car, P(1.75, 10, vs::kPi, 0, -20), MassClass::Car, cfg);
        const imp::ProximityOut rear = imp::PredictOverlap(P(0, 0, 0, 0, 24), MassClass::Car, P(0.2, 6.5, 0, 0, 12), MassClass::Car, cfg);
        const imp::ProximityOut nan = imp::PredictOverlap(P(0, 0, 0, std::nan(""), 20), MassClass::Car, P(0, 10, vs::kPi, 0, -20), MassClass::Car, cfg);
        // the boxes would meet within the horizon, but the copy drifts toward us at only 1 m/s: a slow lane drift is not a collision (the closing floor is 1.5 m/s)
        const imp::ProximityOut drift = imp::PredictOverlap(P(0, 0, 0, 0, 20), MassClass::Car, P(2.75, 0, 0, -1.0, 20), MassClass::Car, cfg);
        const imp::ProximityOut driftFast = imp::PredictOverlap(P(0, 0, 0, 0, 20), MassClass::Car, P(2.75, 0, 0, -3.0, 20), MassClass::Car, cfg);
        Check("R3 T5 proximity: the boxes are the arbiter's (a bike is narrow: 1.75 m to the side of a car is a miss for a bike, a hit for a car); a rear-end at 12 m/s closing is predicted; a non-finite input is no hit; a copy drifting into our lane at 1 m/s is not a collision, at 3 m/s it is",
              !bike.hit && carr.hit && rear.hit && rear.closing > 11.0 && !nan.hit && !drift.hit && driftFast.hit,
              F("bike %d car %d rear %d (%.1f) nan %d drift 1 m/s %d, 3 m/s %d", (int)bike.hit, (int)carr.hit, (int)rear.hit, rear.closing, (int)nan.hit, (int)drift.hit, (int)driftFast.hit));
    }
    {   // 15. the controller: a proximity grace, upgraded by a real contact
        VehicleDriveController c;
        auto frame = [&](VehicleDriveController& ctl, double t, bool prox, bool contact, DriveVec localVel) {
            VehicleDriveInput in;
            in.target.pos = V(0, 20.0 * t / 1000.0, 0); in.target.vel = V(0, 20, 0); in.target.rot = DriveQuat{0, 0, 0, 1};
            in.target.mode = VehMode::Extrapolating;
            in.localPos = in.target.pos; in.localVel = localVel; in.localRot = DriveQuat{0, 0, 0, 1};
            in.dtMs = t > 0 ? 16.7 : 0;
            in.proximity = prox;
            in.contact = contact;
            return ctl.Update(in);
        };
        double t = 0;
        for (int i = 0; i < 20; ++i, t += 16.7) frame(c, t, false, false, V(0, 20, 0));
        const VehicleDriveOutput none = frame(c, t, false, false, V(0, 20, 0));
        t += 16.7;
        const VehicleDriveOutput open = frame(c, t, true, false, V(0, 20, 0));
        t += 16.7;
        const VehicleDriveOutput again = frame(c, t, true, false, V(0, 20, 0));
        t += 16.7;
        const VehicleDriveOutput up = frame(c, t, false, true, V(0, 12, 0));    // the flag in the proximity grace
        t += 16.7;
        const VehicleDriveOutput after = frame(c, t, false, true, V(0, 12, 0));
        Check("R3 T5 proximity: a controller with no overlap prediction opens no grace; the first prediction opens one (contactOpened 4, soft weight below 1); a second prediction inside it opens nothing; the first real contact (the collision flag) inside it is reported as a contact (contactOpened 1) and counted as an upgrade, with the grace restarted",
              none.contactOpened == 0 && !none.graceActive && open.contactOpened == 4 && open.graceActive && open.graceWeight < 1.0f && again.contactOpened == 0 && again.graceActive &&
                  up.contactOpened == 1 && up.graceActive && after.contactOpened == 0 && c.Stats().contactsByProximity == 1 && c.Stats().proximityUpgrades == 1 && c.Stats().contacts == 2,
              F("none %u open %u (w %.2f) again %u up %u; stats prox %u upgrades %u contacts %u", none.contactOpened, open.contactOpened, open.graceWeight, again.contactOpened, up.contactOpened,
                c.Stats().contactsByProximity, c.Stats().proximityUpgrades, c.Stats().contacts));
        // after a grace ends the cooldown holds a new prediction off; EndGrace also clears the proximity state
        VehicleDriveController d;
        t = 0;
        for (int i = 0; i < 20; ++i, t += 16.7) frame(d, t, false, false, V(0, 20, 0));
        frame(d, t, true, false, V(0, 20, 0)); t += 16.7;
        d.EndGrace(150.0);
        const VehicleDriveOutput cool = frame(d, t, true, false, V(0, 20, 0)); t += 16.7;
        const VehicleDriveOutput flagAfterEnd = frame(d, t, false, true, V(0, 12, 0));
        Check("R3 T5 proximity: EndGrace (the adoption path) ends a proximity grace: the cooldown holds the next prediction off, and a flag right after it is not an 'upgrade' of a grace that no longer exists",
              cool.contactOpened == 0 && !cool.graceActive && flagAfterEnd.contactOpened == 0 && d.Stats().proximityUpgrades == 0, F("cool %u flag %u", cool.contactOpened, flagAfterEnd.contactOpened));
    }
    {   // 16. the two-machine simulation with the proximity path on top of the near clock
        namespace sc = vehsim2::scenes;
        using vehsim2::Mode;
        double sumNear = 0, sumProx = 0, tb = 0, tbP = 0;
        std::string got;
        const struct { const char* n; vehsim2::Scene s; } cases[] = {{"head-on", sc::HeadOn()}, {"rear-end", sc::RearEnd()}, {"t-bone", sc::TBone()}, {"sustained", sc::SustainedPush()}};
        for (const auto& cs : cases) {
            const Avg2 n = Run2(cs.s, Mode::AdoptServer, true, false, 40, 40, 40, 40), p = Run2(cs.s, Mode::AdoptServer, true, true, 40, 40, 40, 40);
            sumNear += n.pos1; sumProx += p.pos1;
            if (!std::strcmp(cs.n, "t-bone")) { tb = n.pos1; tbP = p.pos1; }
            got += F("%s%s %.2f -> %.2f", got.empty() ? "" : "; ", cs.n, n.pos1, p.pos1);
            if (p.pos1 > n.pos1 + 0.25) got += " WORSE";
        }
        Check("R3 T5 sim: the proximity path on top of the near clock never costs more than 0.25 m at 1 s in any of head-on / rear-end / T-bone / sustained push and gains 1+ m on the T-bone at 40 ms each way (the copy yields to the hit instead of being a wall)",
              got.find("WORSE") == std::string::npos && sumProx <= sumNear + 0.1 && tbP <= tb - 1.0, got);
    }
}

// ---- E. diagnostics ---------------------------------------------------------------------------------------------------------------------

static void TestDiag() {
    {   // 17. PositionDiag
        PositionDiag d;
        for (int i = 0; i < 100; ++i) {
            PositionDiag::Frame f;
            f.speedMps = 25.0; f.distM = 10.0; f.totalM = 0.1 + 0.01 * i; f.targetM = 0.05; f.trackM = 0.08; f.unledMs = 84.0; f.leadMs = 160.0; f.snapped = (i == 7);
            d.Add(f);
        }
        PositionDiag::Frame lo; lo.speedMps = 2.0; lo.distM = 60.0; lo.totalM = 0.3;
        d.Add(lo);
        PositionDiag::Frame mid; mid.speedMps = 12.0; mid.distM = 30.0; mid.totalM = 0.2;     // 5-15 m/s: a band edge at 15 (not 10) matters
        d.Add(mid);
        PositionDiag::Frame bad; bad.speedMps = 25.0; bad.distM = 10.0; bad.totalM = std::nan("");
        d.Add(bad);
        const PositionDiag::Window w = d.Take();
        const std::string line = PositionDiag::Format(w);
        const PositionDiag::Window w2 = d.Take();
        Check("R3 T5 diag: PositionDiag bands by speed (0-5 / 5-15 / 15-30 / over 30 m/s) and distance (under 15 / 15-45 / over 45 m), ignores non-finite frames, reports mean / p95 / max with the aim, tracking, lead and snaps, and Take() starts a fresh window",
              w.frames == 102 && w.total[2].n == 100 && w.total[0].n == 1 && w.total[1].n == 1 && w.totalByDist[0].n == 100 && w.totalByDist[1].n == 1 && w.totalByDist[2].n == 1 && std::fabs(w.total[2].Mean() - 0.595) < 0.01 &&
                  std::fabs(w.total[2].P95() - 1.05) <= 0.06 && std::fabs(w.total[2].max - 1.09) < 1e-9 && w.snaps == 1 && w2.frames == 0 &&
                  line.find("speed 15-30 m/s: 100 frames, drawn vs the owner's report now mean 0.59 p95 1.05 max 1.09 m (aim 0.05, tracking 0.08), aim 84 ms behind, lead 160 ms") != std::string::npos &&
                  line.find("speed 0-5 m/s: 1 frames") != std::string::npos && line.find("over 45 m: mean 0.30") != std::string::npos && line.find("drive snaps 1") != std::string::npos &&
                  PositionDiag::Format(w2) == "no remote copy within range",
              line.substr(0, 200));
    }
    {   // 18. the rise statistics
        NetClock c(NetClockConfig::ForTickHz(30.0));
        double now = 0;
        for (int i = 0; i < 100; ++i, now += 33.3) c.OnFrame(uint32_t(i), now + 40.0);
        const NetClockStats before = c.Stats();
        c.TakeDelayRiseMaxMs();
        for (int i = 100; i < 160; ++i, now += 33.3) c.OnFrame(uint32_t(i), now + 40.0 + ((i % 2) ? 45.0 : 0.0));    // a burst of 45 ms lateness
        const NetClockStats after = c.Stats();
        const double win = c.TakeDelayRiseMaxMs(), win2 = c.TakeDelayRiseMaxMs();
        Check("R3 T5 diag: the clock reports how BIG its delay rises were (sum, largest, the ones over 5 ms) so 'N rises' can be told from N steps of a millisecond; the window maximum resets on Take",
              after.delayRises > before.delayRises && after.delayRisesBig >= 1 && after.delayRiseMaxMs >= 30.0 && win >= 30.0 && win2 == 0.0 && after.delayRiseSumMs >= after.delayRiseMaxMs,
              F("rises %u -> %u (over 5 ms %u), largest %.1f, window %.1f then %.1f", before.delayRises, after.delayRises, after.delayRisesBig, after.delayRiseMaxMs, win, win2));
    }
}

static void TestR3T5() {
    std::printf("-- R3 T5: A. car position (the real stamp, the near-car clock)\n");
    TestPosition();
    std::printf("-- R3 T5: B. the pieces\n");
    TestPieces();
    std::printf("-- R3 T5: C. two machines with the real stamp\n");
    TestSim2();
    std::printf("-- R3 T5: D. proximity contact\n");
    TestProximity();
    std::printf("-- R3 T5: E. diagnostics\n");
    TestDiag();
}

}   // namespace r3t5
