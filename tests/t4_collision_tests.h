// t4_collision_tests.h: the vehicle-collision work of desync overhaul round 2 (T4), included by tests/vehicle_drive_test.cpp (one include, one call).
// docs/sync-specs/vehicle-collisions.md has the design and the numbers; this file pins them.
//
//   A. ContactArbiter (the server's one resolution per contact): momentum, time of contact, the controllers' own clocks, tracks that never
//      overlap (each controller's physics stops it at the other's copy), same controller / untrusted / near miss, masses, the cap, re-arming.
//   B. The client half: ImpactTracker (velocity jumps no driver produces), DecideImpactApply (authoritative minus local, the gates),
//      UnwrapStampMs, DecideAdopt, VehicleInterpolator::ReplaceFrom (the guard and its confirmation), VehicleDriveController::EndGrace.
//   C. The two-machine simulation (tests/vehsim_2m.h): the REAL controllers, interpolators, NetClock, PresentLead, arbiter, tracker and applier
//      behind a simulated relay, against planar rigid-body physics. Every case asserts BOTH sides: the CURRENT behaviour (Mode::Current) shows
//      the problem at least this large, the new behaviour is at most this large, so a regression of either the harness or the fix fails here.
#pragma once

#include "sync/vehicle_impact.h"
#include "vehsim_2m.h"

namespace t4 {

namespace imp = gage::sync::impact;
using gage::sync::DriveVec;

constexpr int kT4Checks = 24;

// ---- A. the arbiter -------------------------------------------------------------------------------------------------------------------

// What one controller reports: a track y(t) along a straight line with a heading, and the sender's clock offset (its u16 stamp is the server
// time + clk). The track is sampled at 30 Hz (phase-shifted per vehicle) and the arbiter is stepped once per 33.3 ms tick with the samples
// that exist by then, exactly as the server does.
struct Track {
    uint16_t id = 0, ctrl = 0;
    double x = 0, clk = 0, heading = 0;
    std::function<double(double)> y;      // position along y by server ms
    std::function<double(double)> vy;     // velocity along y
    double phase = 0;
    imp::MassClass mc = imp::MassClass::Car;
    bool trusted = true;
};
static std::vector<imp::ImpactOut> DriveArbiter(imp::ContactArbiter& a, const std::vector<Track>& tr, double t0, double t1) {
    std::vector<imp::ImpactOut> all;
    std::vector<imp::ContactArbiter::Live> live;
    for (const Track& k : tr) {
        imp::ContactArbiter::Live l; l.id = k.id; l.ctrl = k.ctrl; l.mc = k.mc; l.trusted = k.trusted;
        live.push_back(l);
    }
    for (double t = t0; t <= t1; t += 1000.0 / 30.0) {
        for (const Track& k : tr) {
            const double ts = t - k.phase;                 // this vehicle's newest sample is a little older than the tick
            imp::ArbSample s;
            s.tMs = ts;
            s.p = {k.x, k.y(ts)};
            s.v = {0, k.vy(ts)};
            s.h = k.heading;
            s.stamp = uint16_t(uint64_t(ts + k.clk) & 0xFFFFu);
            a.Note(k.id, k.ctrl, s);
        }
        a.Step(t, live, all);
    }
    return all;
}
// Car A drives +y at 20 m/s from y = -50, car B drives -y at 20 m/s from y = +50; each controller's physics halts its car at `stopMs` (where the
// other's copy is): from then on both stand TOUCHING, they never overlap. The geometric touch (true boxes) is at 2385 ms.
static std::vector<Track> HeadOnTracks(double lateral = 0.4, double stopMs = 2385.0, uint16_t ctrl1 = 1, imp::MassClass m0 = imp::MassClass::Car,
                                       imp::MassClass m1 = imp::MassClass::Car, bool trusted1 = true, double vA = 20.0, double vB = 20.0) {
    Track a, b;
    a.id = 0; a.ctrl = 0; a.x = 0.0; a.clk = 123456.0; a.heading = 0.0; a.phase = 0.0; a.mc = m0;
    b.id = 1; b.ctrl = ctrl1; b.x = lateral; b.clk = 7777.0; b.heading = imp::kPi; b.phase = 11.0; b.mc = m1; b.trusted = trusted1;
    // the start positions make the true boxes touch at 2385 ms whatever the speeds
    const double ya0 = -(vA * 2.385 + 2.3), yb0 = vB * 2.385 + 2.3;
    a.y = [=](double t) { return ya0 + vA * std::min(t, stopMs) / 1000.0; };
    a.vy = [=](double t) { return t < stopMs ? vA : 0.0; };
    b.y = [=](double t) { return yb0 - vB * std::min(t, stopMs) / 1000.0; };
    b.vy = [=](double t) { return t < stopMs ? -vB : 0.0; };
    return {a, b};
}

static void TestArbiter() {
    // 1. head-on: ONE impact, momentum conserved, the closing speed and the time right.
    {
        imp::ContactArbiter arb;
        const auto out = DriveArbiter(arb, HeadOnTracks(), 0, 5000);
        const bool one = out.size() == 1;
        double mom = 1e9, closing = 0, t = 0;
        if (one) {
            const imp::ImpactOut& o = out[0];
            mom = imp::Len(o.dv[0] + o.dv[1]) / std::max(1e-9, imp::Len(o.dv[0]));
            closing = o.closing;
            t = o.tMs;
        }
        // the boxes (with the arbiter's 0.3 m margin each) touch when the centres are 4.6 + 0.6 = 5.2 m apart: t = (100 - 5.2) / 40 = 2.37 s
        Check("T4 arbiter head-on 20+20: exactly one impact; both cars' changes cancel (equal masses, momentum conserved within 5%); closing 40 m/s; time of contact within 15 ms of the geometric touch; the pair was examined",
              one && mom < 0.05 && std::fabs(closing - 40.0) < 1.0 && std::fabs(t - 2370.0) < 15.0 && arb.stats().pairsExamined > 0,
              F("%zu impacts, momentum residue %.3f, closing %.2f, t %.1f ms, examined %llu", out.size(), mom, closing, t, (unsigned long long)arb.stats().pairsExamined));
    }
    // 2. each controller is told the contact on ITS OWN clock
    {
        imp::ContactArbiter arb;
        const auto out = DriveArbiter(arb, HeadOnTracks(), 0, 5000);
        bool ok = out.size() == 1 && out[0].stampOk[0] && out[0].stampOk[1];
        double e0 = 1e9, e1 = 1e9;
        if (ok) {
            const imp::ImpactOut& o = out[0];
            // the controllers' clocks are the server's + 123456 / + 7777 ms; they read 2.5 s later when the message arrives
            e0 = imp::UnwrapStampMs(o.stamp[0], o.tMs + 123456.0 + 150.0) - (o.tMs + 123456.0);
            e1 = imp::UnwrapStampMs(o.stamp[1], o.tMs + 7777.0 + 150.0) - (o.tMs + 7777.0);
        }
        Check("T4 arbiter: the contact time is delivered on each controller's own sample clock (u16, wrapping) -- both unwrap to the true contact instant within 2 ms",
              ok && std::fabs(e0) < 2.0 && std::fabs(e1) < 2.0, F("ok %d, error %.2f / %.2f ms", int(ok), e0, e1));
    }
    // 3. tracks that were already bounced when the server looks (the case every real contact is)
    {
        imp::ContactArbiter arb;
        const auto out = DriveArbiter(arb, HeadOnTracks(0.4, 2360.0), 0, 5000);   // both cars stop dead 10 ms BEFORE the arbiter's margin touches
        // they stop 5.4 m apart (centres): never touching with the margin either -- a stop short of the margin is no contact at all
        imp::ContactArbiter arb2;
        const auto out2 = DriveArbiter(arb2, HeadOnTracks(0.4, 2385.0), 0, 5000);  // they stop touching (true boxes): the margin saw it first
        Check("T4 arbiter: tracks that stop AT the touch (each controller's physics halts it at the other's copy, so the boxes never overlap) still give the impact, with the PRE-impact velocities; stopping 0.4 m short of it gives none",
              out2.size() == 1 && std::fabs(out2[0].closing - 40.0) < 1.0 && out.empty() && arb.stats().pairsExamined > 0,
              F("touching: %zu impacts, closing %.2f; short: %zu (examined %llu)", out2.size(), out2.empty() ? -1.0 : out2[0].closing, out.size(),
                (unsigned long long)arb.stats().pairsExamined));
    }
    // 4. the same controller never goes through the arbiter (its own physics resolves it)
    {
        imp::ContactArbiter same, diff;
        const auto o1 = DriveArbiter(same, HeadOnTracks(0.4, 2385.0, 0), 0, 5000);
        const auto o2 = DriveArbiter(diff, HeadOnTracks(0.4, 2385.0, 1), 0, 5000);
        Check("T4 arbiter: two cars with the SAME controller are not arbitrated (counted as sameCtrl); with different controllers the same tracks give one impact",
              o1.empty() && same.stats().sameCtrl > 0 && o2.size() == 1, F("same: %zu impacts, sameCtrl %llu; different: %zu", o1.size(), (unsigned long long)same.stats().sameCtrl, o2.size()));
    }
    // 5. near miss and graze: no impact, but they were looked at
    {
        imp::ContactArbiter miss, graze;
        const auto o1 = DriveArbiter(miss, HeadOnTracks(3.2, 1e9), 0, 5000);    // 3.2 m between the centre lines: a 1.2 m gap, driving through
        const auto o2 = DriveArbiter(graze, HeadOnTracks(2.4, 1e9), 0, 5000);   // 0.4 m gap: inside the 0.6 m touch margin, but no closing along the normal
        Check("T4 arbiter: a head-on pass 1.2 m apart and a 0.4 m graze (no closing speed along the contact normal) give no impact; both pairs were examined and the graze touched",
              o1.empty() && o2.empty() && miss.stats().pairsExamined > 0 && graze.stats().pairsExamined > 0 && (graze.stats().softTouches + graze.stats().overlaps) > 0,
              F("miss %zu (examined %llu), graze %zu (examined %llu, soft %llu, overlaps %llu)", o1.size(), (unsigned long long)miss.stats().pairsExamined, o2.size(),
                (unsigned long long)graze.stats().pairsExamined, (unsigned long long)graze.stats().softTouches, (unsigned long long)graze.stats().overlaps));
    }
    // 6. masses: a bike into a truck
    {
        imp::ContactArbiter arb;
        const auto out = DriveArbiter(arb, HeadOnTracks(0.0, 2385.0, 1, imp::MassClass::Light, imp::MassClass::Heavy, true, 10.0, 5.0), 0, 5000);
        double ratio = 0, small = 0, big = 0;
        if (!out.empty()) {
            const imp::ImpactOut& o = out[0];
            ratio = 250.0 * imp::Len(o.dv[0]) / std::max(1e-9, 7000.0 * imp::Len(o.dv[1]));
            small = imp::Len(o.dv[1]);
            big = imp::Len(o.dv[0]);
        }
        Check("T4 arbiter: a 250 kg bike (10 m/s) into a 7 t truck (5 m/s) -- momentum is conserved (m1 dv1 = m2 dv2 within 3%), the bike takes 20x+ the truck's change of velocity",
              out.size() == 1 && std::fabs(ratio - 1.0) < 0.03 && big > 20.0 * small, F("%zu impacts, momentum ratio %.3f, dv bike %.2f truck %.2f", out.size(), ratio, big, small));
    }
    // 7. untrusted controller
    {
        imp::ContactArbiter bad, good;
        const auto o1 = DriveArbiter(bad, HeadOnTracks(0.4, 2385.0, 1, imp::MassClass::Car, imp::MassClass::Car, false), 0, 5000);
        const auto o2 = DriveArbiter(good, HeadOnTracks(0.4, 2385.0, 1, imp::MassClass::Car, imp::MassClass::Car, true), 0, 5000);
        Check("T4 arbiter: no impact is resolved for a pair with an untrusted controller (counted); the same tracks with both trusted give one",
              o1.empty() && bad.stats().untrusted > 0 && o2.size() == 1, F("untrusted: %zu (counted %llu), trusted: %zu", o1.size(), (unsigned long long)bad.stats().untrusted, o2.size()));
    }
    // 8. the cap: a 140 m/s ram cannot move a car by more than kMaxDvMs
    {
        imp::ContactArbiter a;
        Track r, st;
        r.id = 0; r.ctrl = 0; r.x = 0; r.heading = 0; r.phase = 0;
        r.y = [](double t) { return -140.0 + 140.0 * std::min(t, 1000.0) / 1000.0; };
        r.vy = [](double t) { return t < 1000.0 ? 140.0 : 0.0; };
        st.id = 1; st.ctrl = 1; st.x = 0; st.heading = imp::kPi; st.phase = 5.0;
        st.y = [](double) { return 0.0; };
        st.vy = [](double) { return 0.0; };
        const auto o = DriveArbiter(a, {r, st}, 0, 2000);
        Check("T4 arbiter: a 140 m/s ram into a standing car is resolved but no car's change of velocity exceeds the cap (30 m/s); the cap is counted",
              o.size() == 1 && imp::Len(o[0].dv[0]) <= imp::kMaxDvMs + 1e-6 && imp::Len(o[0].dv[1]) <= imp::kMaxDvMs + 1e-6 && a.stats().capped > 0 && o[0].closing > 100.0,
              F("%zu impacts, dv %.1f / %.1f, closing %.1f, capped %llu", o.size(), o.empty() ? -1.0 : imp::Len(o[0].dv[0]), o.empty() ? -1.0 : imp::Len(o[0].dv[1]),
                o.empty() ? -1.0 : o[0].closing, (unsigned long long)a.stats().capped));
    }
    // 9. one impact per contact; a new contact after the pair parted gives a second
    {
        // B stands at y = 0. A drives in, touches (stops 200 ms), backs off at 10 m/s for 1 s, and drives in again.
        auto yA = [](double t) {
            if (t < 2270) return -50.0 + 20.0 * t / 1000.0;
            if (t < 2470) return -4.6;
            if (t < 3470) return -4.6 - 10.0 * (t - 2470) / 1000.0;
            return std::min(-4.6, -14.6 + 20.0 * (t - 3470) / 1000.0);
        };
        auto vA = [](double t) {
            if (t < 2270) return 20.0;
            if (t < 2470) return 0.0;
            if (t < 3470) return -10.0;
            return t < 3970 ? 20.0 : 0.0;
        };
        Track a, b;
        a.id = 0; a.ctrl = 0; a.x = 0; a.heading = 0; a.y = yA; a.vy = vA;
        b.id = 1; b.ctrl = 1; b.x = 0.2; b.heading = imp::kPi; b.phase = 7.0; b.y = [](double) { return 0.0; }; b.vy = [](double) { return 0.0; };
        imp::ContactArbiter arb, arbStay;
        const auto two = DriveArbiter(arb, {a, b}, 0, 5000);
        Track stay = a;
        stay.y = [](double t) { return t < 2270 ? -50.0 + 20.0 * t / 1000.0 : -4.6; };
        stay.vy = [](double t) { return t < 2270 ? 20.0 : 0.0; };
        const auto one = DriveArbiter(arbStay, {stay, b}, 0, 5000);
        Check("T4 arbiter: one contact is one impact however long the tracks keep touching (2.7 s of standing contact: one), and driving in again after backing off is a second",
              one.size() == 1 && two.size() == 2 && arbStay.stats().overlaps > 20, F("standing contact: %zu impacts (%llu overlap evaluations); back off and again: %zu", one.size(),
                                                                                          (unsigned long long)arbStay.stats().overlaps, two.size()));
    }
}

// ---- B. the client half ---------------------------------------------------------------------------------------------------------------

static void TestClient() {
    // 10. the tracker: ordinary driving is never an episode, a hit is, at any frame rate
    {
        int episodesDrive = 0, episodesHit[3] = {0, 0, 0};
        double hitDv[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
            const double fps = k == 0 ? 30.0 : k == 1 ? 60.0 : 144.0;
            imp::ImpactTracker t;
            double speed = 20.0, now = 1000.0;
            // hard braking at 10 m/s^2 for a second, then a launch at 12 m/s^2 (a hyper car launches harder), at this frame rate
            for (int i = 0; i < int(fps); ++i) { speed -= 10.0 / fps; now += 1000.0 / fps; t.Feed(now, {0, speed}, 0.0); }
            for (int i = 0; i < int(fps); ++i) { speed += 12.0 / fps; now += 1000.0 / fps; t.Feed(now, {0, speed}, 0.0); }
            if (k == 0) episodesDrive = int(t.Episodes());
            // a 14 m/s change in ONE frame, then three calm frames
            now += 1000.0 / fps; t.Feed(now, {0, speed - 14.0}, 1.5);
            for (int i = 0; i < 3; ++i) { now += 1000.0 / fps; t.Feed(now, {0, speed - 14.0}, 1.5); }
            imp::V2 dv; double dw = 0;
            const bool any = t.Sum(now - 1000.0, now, dv, dw);
            episodesHit[k] = any ? int(t.Episodes()) : 0;
            hitDv[k] = dv.y;
        }
        Check("T4 tracker: braking at 10 m/s^2 and launching at 12 m/s^2 are no episode; a 14 m/s change in one frame is exactly one, summing to -14 m/s, at 30 / 60 / 144 fps",
              episodesDrive == 0 && episodesHit[0] == 1 && episodesHit[1] == 1 && episodesHit[2] == 1 && std::fabs(hitDv[0] + 14.0) < 1e-6 && std::fabs(hitDv[1] + 14.0) < 1e-6 &&
                  std::fabs(hitDv[2] + 14.0) < 1e-6,
              F("drive episodes %d; hit episodes %d/%d/%d, dv %.2f/%.2f/%.2f", episodesDrive, episodesHit[0], episodesHit[1], episodesHit[2], hitDv[0], hitDv[1], hitDv[2]));
    }
    // 11-13. the apply decision
    {
        imp::ImpactTracker t;
        t.Feed(1000.0, {0, 20}, 0.0);
        t.Feed(1016.7, {0, 20}, 0.0);
        t.Feed(1033.3, {0, -20}, 0.0);   // the game's wall response: -40 m/s in one frame
        t.Feed(1050.0, {0, -20}, 0.0);
        const imp::ApplyDecision d = imp::DecideImpactApply(t, {0, -24.5}, -2.3, 1030.0, 1180.0, true, 0.30);
        Check("T4 apply: authoritative change -24.5 m/s, the game's physics had already given -40: the car is written the DIFFERENCE (+15.5 m/s), yaw likewise",
              d.why == imp::ApplyWhy::Applied && d.localContact && std::fabs(d.dv.y - 15.5) < 1e-6 && std::fabs(d.dw - (-2.3)) < 1e-6, F("why %d, dv %.2f, dw %.2f, local %.2f", int(d.why), d.dv.y, d.dw, d.dvLocal.y));
        const imp::ImpactTracker none;
        const imp::ApplyDecision a = imp::DecideImpactApply(none, {0, -24.5}, 0.0, 1030.0, 1180.0, false, 0.30);
        const imp::ApplyDecision b = imp::DecideImpactApply(none, {0, -24.5}, 0.0, 1030.0, 1180.0, true, 0.30);
        const imp::ApplyDecision c = imp::DecideImpactApply(none, {0, -24.5}, 0.0, 1030.0, 1180.0, true, 0.02);
        Check("T4 apply: no contact felt locally -- ignored unless the server's boxes truly overlapped (depth >= 0.10 m) AND our copy is near, then the full change is applied",
              a.why == imp::ApplyWhy::NoLocalContact && c.why == imp::ApplyWhy::NoLocalContact && b.why == imp::ApplyWhy::Applied && std::fabs(b.dv.y + 24.5) < 1e-6,
              F("far %d, near+deep %d (dv %.2f), near+shallow %d", int(a.why), int(b.why), b.dv.y, int(c.why)));
        // a local jump pointing the other way (a wall, a different hit) is not "corrected away"
        imp::ImpactTracker o;
        o.Feed(1000.0, {0, 0}, 0.0);
        o.Feed(1016.7, {0, 0}, 0.0);
        o.Feed(1033.3, {0, 12}, 0.0);   // thrown FORWARD by something else
        o.Feed(1050.0, {0, 12}, 0.0);
        const imp::ApplyDecision e = imp::DecideImpactApply(o, {0, -24.5}, 0.0, 1030.0, 1180.0, true, 0.3);
        const imp::ApplyDecision f = imp::DecideImpactApply(t, {0, -24.5}, 0.0, 1030.0, 5000.0, true, 0.3);    // 4 s old
        const imp::ApplyDecision g = imp::DecideImpactApply(t, {0.1, 0.1}, 0.0, 1030.0, 1180.0, true, 0.3);    // tiny
        const imp::ApplyDecision nan = imp::DecideImpactApply(t, {std::nan(""), 0}, 0.0, 1030.0, 1180.0, true, 0.3);
        Check("T4 apply: a local jump in the opposite direction is not subtracted; a message 4 s old is Stale; a 0.14 m/s change is Tiny; NaN is refused",
              e.why == imp::ApplyWhy::Applied && std::fabs(e.dv.y + 24.5) < 1e-6 && f.why == imp::ApplyWhy::Stale && g.why == imp::ApplyWhy::Tiny && nan.why == imp::ApplyWhy::Stale,
              F("opposite: %d dv %.2f; old %d; tiny %d; nan %d", int(e.why), e.dv.y, int(f.why), int(g.why), int(nan.why)));
    }
    // 14. the stamp domain
    {
        // the engine stamps whole ms of its clock & 0xFFFF; the message comes back as that u16
        const double now = 3.0 * 65536.0 + 40.0;                  // just after a wrap
        const uint16_t before = uint16_t(uint64_t(now - 100.0) & 0xFFFF);   // 100 ms ago: 65476
        const uint16_t after = uint16_t(uint64_t(now + 30.0) & 0xFFFF);
        Check("T4 stamp: a u16 contact time 100 ms before a wrap unwraps to 100 ms ago; 30 ms ahead stays ahead; a mid-range value unwraps exactly",
              std::fabs(imp::UnwrapStampMs(before, now) - (now - 100.0)) < 1.0 && std::fabs(imp::UnwrapStampMs(after, now) - (now + 30.0)) < 1.0 &&
                  std::fabs(imp::UnwrapStampMs(uint16_t(uint64_t(1234567.0) & 0xFFFF), 1234600.0) - 1234567.0) < 1.0,
              F("%.1f / %.1f", imp::UnwrapStampMs(before, now) - now, imp::UnwrapStampMs(after, now) - now));
    }
    // 15. adoption
    {
        imp::AdoptCfg cfg;
        const imp::AdoptOut small = imp::DecideAdopt(0, 20, 0.3, 0, 19, 0.0, 0.0, cfg);               // 1 m/s: tyre noise
        const imp::AdoptOut hit = imp::DecideAdopt(0, 20, 0.3, 1.5, -2.5, 3.0, 0.0, cfg);             // the copy was thrown back
        const imp::AdoptOut huge = imp::DecideAdopt(0, 20, 0.0, 60, -40, 40.0, 0.0, cfg);             // an interlocked-bodies explosion
        const imp::AdoptOut bad = imp::DecideAdopt(0, 20, 0, std::nan(""), 0, 0, 0.0, cfg);
        const double hugeLen = std::sqrt((huge.vx - 0) * (huge.vx - 0) + (huge.vy - 20) * (huge.vy - 20));
        Check("T4 adopt: 1 m/s is not adopted; a hit is (the stream's vertical velocity kept); an absurd response is scaled to 30 m/s and the yaw rate clamped to 8 rad/s; NaN refused",
              !small.adopt && hit.adopt && std::fabs(hit.vx - 1.5) < 1e-9 && std::fabs(hit.vy + 2.5) < 1e-9 && std::fabs(hit.vz - 0.3) < 1e-9 && huge.adopt &&
                  std::fabs(hugeLen - 30.0) < 1e-6 && std::fabs(huge.yawRate - 8.0) < 1e-9 && !bad.adopt,
              F("small %d, hit %d (%.2f,%.2f,%.2f), huge change %.2f yaw %.2f, nan %d", int(small.adopt), int(hit.adopt), hit.vx, hit.vy, hit.vz, hugeLen, huge.yawRate, int(bad.adopt)));
    }
    // 16. ReplaceFrom and its guard
    {
        VehicleInterpolator ip;
        auto snap = [](double t, double y, double vy) {
            VehicleSnapshot s; s.tMs = t; s.pos = V(0, y, 0); s.vel = V(0, vy, 0); s.rot = Q(0, 0, 0, 1); return s;
        };
        for (int i = 0; i <= 6; ++i) ip.Push(snap(i * 33.3, i * 33.3 * 0.020, 20.0));      // the stream: 20 m/s northward
        // the copy was hit at t = 200: it now moves at -3 m/s
        VehicleSnapshot adopted = snap(200.0, 4.0, -3.0);
        const DriveVec pre = V(0, 20, 0);
        ip.ReplaceFrom(adopted, 300.0, &pre);
        VehicleSample at; ip.Sample(300.0, at);     // 100 ms after the adoption: extrapolated along the adopted velocity
        // stale, pre-impact-looking samples after the adoption are refused; one that shows the impact is accepted and lifts the guard
        const VehPush p1 = ip.Push(snap(233.0, 4.66, 20.0));
        const VehPush p2 = ip.Push(snap(266.0, 5.32, 19.5));
        const VehPush p3 = ip.Push(snap(300.0, 3.5, -2.0));
        const VehPush p4 = ip.Push(snap(333.0, 3.45, -2.0));
        VehicleInterpolator noConfirm;
        for (int i = 0; i <= 6; ++i) noConfirm.Push(snap(i * 33.3, i * 33.3 * 0.020, 20.0));
        noConfirm.ReplaceFrom(adopted, 100.0, nullptr);
        const VehPush q1 = noConfirm.Push(snap(266.0, 5.32, 19.5));    // inside the 100 ms guard
        const VehPush q2 = noConfirm.Push(snap(310.0, 5.9, 19.5));     // after it
        Check("T4 ReplaceFrom: the adopted state replaces the stream from its instant (a car extrapolates along the adopted -3 m/s, 3.7 m not 6 m ahead); pre-impact-looking stream pushes are refused; the first one that shows the impact lifts the guard; without a confirmation velocity the guard is a plain time window",
              std::fabs(at.pos.y - 3.7) < 0.05 && std::fabs(at.vel.y + 3.0) < 1e-4 && p1 == VehPush::Stale && p2 == VehPush::Stale && p3 == VehPush::Accepted && p4 == VehPush::Accepted &&
                  ip.Stats().guarded == 2 && ip.Stats().injected == 1 && q1 == VehPush::Stale && q2 == VehPush::Accepted && noConfirm.Stats().guarded == 1,
              F("y %.2f vy %.2f; pushes %d %d %d %d (guarded %u); window %d %d (%u)", at.pos.y, at.vel.y, int(p1), int(p2), int(p3), int(p4), ip.Stats().guarded, int(q1), int(q2),
                noConfirm.Stats().guarded));
    }
    // 17. EndGrace
    {
        VehicleDriveController c;
        auto in = [](double y, double vy, double localVy) {
            VehicleDriveInput i;
            i.target.pos = V(0, y, 0); i.target.vel = V(0, vy, 0); i.target.rot = Q(0, 0, 0, 1); i.target.mode = VehMode::Interpolating;
            i.localPos = V(0, y, 0); i.localVel = V(0, localVy, 0); i.localRot = Q(0, 0, 0, 1); i.dtMs = 16.7;
            return i;
        };
        double y = 0;
        for (int k = 0; k < 5; ++k) { y += 0.33; c.Update(in(y, 20, 20)); }
        y += 0.33;
        const VehicleDriveOutput hit = c.Update(in(y, 20, 5.0));            // the copy's velocity is 15 m/s off the last command: a kick
        c.EndGrace(150.0);
        y += 0.33;
        const VehicleDriveOutput next = c.Update(in(y, 20, 5.0));           // would be the next grace frame
        Check("T4 EndGrace: a kick opens the grace (presence); EndGrace ends it at once -- the next frame is a full-weight command (no grace, no snap), and no new contact is looked for during the cooldown",
              hit.contactOpened >= 1 && hit.graceActive && !next.graceActive && next.graceWeight == 1.0f && !next.snap && next.contactOpened == 0 && c.Stats().contacts == 1,
              F("opened %d grace %d; next grace %d weight %.2f snap %d opened %d; contacts %u", int(hit.contactOpened), int(hit.graceActive), int(next.graceActive), next.graceWeight,
                int(next.snap), int(next.contactOpened), c.Stats().contacts));
    }
}

// ---- C. the two-machine simulation ------------------------------------------------------------------------------------------------------

struct Avg {
    double pos1 = 0, pos3 = 0, vel1 = 0, ovMax = 0, ovMs = 0, spread = 0, popMax = 0;
    uint32_t claims = 0, impacts = 0, applied = 0, adopted = 0;
};
static Avg RunAvg(const vehsim2::Scene& sc, vehsim2::Mode mode, const vehsim2::Opt& base, int seeds = 4) {  // NOLINT
    Avg a;
    for (int s = 1; s <= seeds; ++s) {
        vehsim2::Opt o = base;
        o.mode = mode;
        o.seed = uint64_t(s);
        const vehsim2::Result r = vehsim2::Run(sc, o);
        a.pos1 += r.posErr[1]; a.pos3 += r.posErr[2]; a.vel1 += r.velErr[1];
        a.ovMax = std::max(a.ovMax, r.trueOverlapMax); a.ovMs += r.trueOverlapMs; a.spread += std::max(0.0, r.contactSpreadMs);
        a.popMax = std::max(a.popMax, r.popMax);
        a.claims += r.claims; a.impacts += r.impacts; a.applied += r.applied; a.adopted += r.predictions;
    }
    a.pos1 /= seeds; a.pos3 /= seeds; a.vel1 /= seeds; a.ovMs /= seeds; a.spread /= seeds;
    return a;
}

static void TestSim() {
    namespace vs = vehsim2;
    using vs::Mode;
    vs::Opt sym;                                         // 25 ms each way, 30 ms downlink jitter, 3% loss: a good connection
    vs::Opt asym = sym;                                  // machine 1 on a 90 ms link, 5% loss
    asym.upMs[1] = asym.downMs[1] = 90.0; asym.upMs[2] = asym.downMs[2] = 60.0; asym.downJitMs = 40.0; asym.loss = 0.05;
    const Mode AS = Mode::AdoptServer, ASC = Mode::AdoptServerClaim, CUR = Mode::Current;

    {   // 18. head-on at 40 m/s closing
        const vs::Scene sc = vs::scenes::HeadOn();
        const Avg c = RunAvg(sc, CUR, sym), n = RunAvg(sc, AS, sym);
        Check("T4 sim head-on 20+20 (good link): today each car is thrown back by a wall coming at it -- the real cars' state 1 s after the reference contact is 8+ m off and 8+ m/s wrong; adopting the copy's response + the server's resolution brings it under 2 m and 0.5 m/s",
              c.pos1 >= 8.0 && c.vel1 >= 8.0 && n.pos1 <= 2.0 && n.vel1 <= 0.5 && n.impacts > 0 && n.adopted > 0,
              F("today pos %.2f vel %.2f; new pos %.2f vel %.2f (impacts %u, adopted %u)", c.pos1, c.vel1, n.pos1, n.vel1, n.impacts, n.adopted));
    }
    {   // 19. head-on on an asymmetric link
        const vs::Scene sc = vs::scenes::HeadOn();
        const Avg c = RunAvg(sc, CUR, asym), n = RunAvg(sc, AS, asym);
        Check("T4 sim head-on with machine 1 on a 90 ms link and 5% loss: today 15+ m / 15+ m/s off, new under 2.5 m / 0.5 m/s (the render delay and the latency no longer decide the outcome)",
              c.pos1 >= 12.0 && c.vel1 >= 12.0 && n.pos1 <= 2.5 && n.vel1 <= 0.5, F("today pos %.2f vel %.2f; new pos %.2f vel %.2f", c.pos1, c.vel1, n.pos1, n.vel1));
    }
    {   // 20. rear-end
        const vs::Scene sc = vs::scenes::RearEnd();
        const Avg c = RunAvg(sc, CUR, sym), n = RunAvg(sc, AS, sym);
        Check("T4 sim rear-end 12/24: today 4+ m off after 1 s (the struck car is a wall for the striker), new under 1 m",
              c.pos1 >= 4.0 && n.pos1 <= 1.0, F("today pos %.2f vel %.2f; new pos %.2f vel %.2f", c.pos1, c.vel1, n.pos1, n.vel1));
    }
    {   // 21. sustained push
        const vs::Scene sc = vs::scenes::SustainedPush();
        const Avg c = RunAvg(sc, CUR, sym), n = RunAvg(sc, AS, sym);
        Check("T4 sim sustained push (24 m/s into a car that brakes to 6): today 3+ m off after 1 s, new under 1 m",
              c.pos1 >= 3.0 && n.pos1 <= 1.0, F("today pos %.2f; new pos %.2f", c.pos1, n.pos1));
    }
    {   // 22. into a parked car: the claim makes it ONE physics instance
        const vs::Scene sc = vs::scenes::IntoParked();
        const Avg c = RunAvg(sc, CUR, sym), n = RunAvg(sc, ASC, sym), nc = RunAvg(sc, AS, sym);
        Check("T4 sim ramming a parked car (its controller is its last driver, on another machine): today the rammer hits a wall and the car moves only when the owner's screen hears about it (4+ m / 3+ m/s off); with the empty-car claim the rammer's machine simulates both cars: exactly the reference (under 5 cm), claimed once; without the claim (adopt + server only) still under 1 m",
              c.pos1 >= 3.0 && n.pos1 < 0.05 && n.pos3 < 0.05 && n.claims >= 4 && nc.pos1 <= 1.0 && n.ovMax < 0.05,
              F("today pos %.2f vel %.2f; claim pos %.3f/%.3f claims %u overlap %.3f; no claim pos %.2f", c.pos1, c.vel1, n.pos1, n.pos3, n.claims, n.ovMax, nc.pos1));
    }
    {   // 23. two cars into a third (three machines)
        const vs::Scene sc = vs::scenes::TwoIntoThird();
        const Avg c = RunAvg(sc, CUR, sym), n = RunAvg(sc, ASC, sym);
        Check("T4 sim two cars into a third (parked, controller on a third machine): today a car ends 25+ m off after 3 s (the parked car is pushed by one screen only), with the claim under 4 m",
              c.pos3 >= 20.0 && n.pos3 <= 4.0, F("today pos3 %.2f; new pos3 %.2f (claims %u)", c.pos3, n.pos3, n.claims));
    }
    {   // 24. do no harm: side-swipe and T-bone on a good link
        const Avg cs = RunAvg(vs::scenes::SideSwipe(), CUR, sym), ns = RunAvg(vs::scenes::SideSwipe(), AS, sym);
        const Avg ct = RunAvg(vs::scenes::TBone(), CUR, sym), nt = RunAvg(vs::scenes::TBone(), AS, sym);
        Check("T4 sim: a glancing side-swipe is untouched (within 0.1 m) and a T-bone on a good link stays within 0.5 m of what it is today (the planar model is least sure of side hits: docs list it)",
              std::fabs(ns.pos1 - cs.pos1) <= 0.1 && nt.pos1 <= ct.pos1 + 0.5, F("side-swipe %.2f -> %.2f; t-bone %.2f -> %.2f", cs.pos1, ns.pos1, ct.pos1, nt.pos1));
    }
}

static void TestT4() {
    TestArbiter();
    TestClient();
    TestSim();
}

}   // namespace t4
