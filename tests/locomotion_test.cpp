// locomotion_test: the remote-ped locomotion controller (libs/sync/locomotion.h)
// against scripted remote poses and a simulated ped. Build target: locomotion_test
// (selftest/, never shipped). Pure computation, reproducible (seeded generator,
// simulated time).
//
// The simulated ped: SET_ENTITY_COORDS moves it at once; its velocity follows the
// commanded SET_ENTITY_VELOCITY with a first-order lag (pedLagMs, 80 ms: animation
// and capsule physics do not follow a velocity instantly); it stands on flat ground
// (z is physics-owned); SET_ENTITY_HEADING applies at once; its IS_PED_* gait comes
// from its real speed (no hysteresis, as the game reports it).
//
//   1. Gait thresholds: single jumps from still, the exact up/down crossing speeds
//      on a slow ramp, and no flapping under noise around every band at 30/60/144 fps.
//   2. Task re-issue: a 10 s straight walk / run / sprint issues a handful of tasks
//      (not per frame, nearly the same count at every frame rate), >= 500 ms apart
//      unless the gait changed; a target that turns 90 degrees every 600 ms is
//      re-issued per turn but rate-limited; a 40 degree turn is re-issued, a 10 degree
//      one is not; going still issues one stand-still.
//   3. Snaps: first frame, teleported, ResetHard (Reset does not snap), 5 + 5|v|; the persistent-error snap
//      fires after the same TIME at 30, 60 and 144 fps, and never below 3 m.
//   4. Velocity: correction gain and cap, vertical deadband and cap, none while jumping.
//   5. Heading: shortest arc across 0/360 both ways, same approach at every frame rate.
//   6. Jump and ragdoll edges (v3 inputs), inert with v2 defaults.
//   7. Closed loop: the ped follows a remote walking / running / sprinting a circle and
//      stopping, at 30/60/144 fps: converges, stays within a bound, settles at the stop;
//      a 60 s circle at 4.5 m/s (no persistent snap, p95 error, go-straight rate).
//   8. W1 (docs/sync-specs/onfoot.md 1-4, 6A, 7): the sender's gait is a hint the speed can
//      override (moveState 0 while moving walks; one stray 0 does not stand the ped still; a
//      stop needs 150 ms), a gait class change is debounced, the move rate matches animation
//      speed (and its quantised blend-ratio fallback), Reset() re-issues instead of snapping,
//      Pause() (leaving a vehicle), the velocity low-pass / dead band / ragdoll follow, and the
//      aim-task gate.
//
// Every loop-shaped check also asserts how many frames it measured; the run asserts
// the total number of checks.

#include "sync/locomotion.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace gage::sync;

static int g_run = 0, g_failed = 0;

static void Check(const char* name, bool ok, const std::string& got = "") {
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, got.empty() ? "" : "  got: ", got.c_str());
}

static std::string F(const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static const double kPi = 3.14159265358979323846;
static const double kFps[3] = {30, 60, 144};

static double WrapDeg(double d) {
    d = std::fmod(d, 360.0);
    if (d > 180.0) d -= 360.0;
    else if (d <= -180.0) d += 360.0;
    return d;
}

static LocoVec V(double x, double y, double z) {
    LocoVec v;
    v.x = float(x); v.y = float(y); v.z = float(z);
    return v;
}

static double HDist(const LocoVec& a, const LocoVec& b) { return std::hypot(double(a.x) - b.x, double(a.y) - b.y); }

struct Rng { // splitmix64
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t Next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double Uniform() { return double(Next() >> 11) / double(1ull << 53); }
};

// ---- the simulated ped -------------------------------------------------------

struct SimPed {
    LocoVec pos = V(0, 0, 30), vel;
    float heading = 0;
    double lagMs = 80;
    Gait GameGait() const { // what IS_PED_WALKING / RUNNING / SPRINTING would say
        const double s = std::hypot(vel.x, vel.y);
        return s < 0.5 ? Gait::Still : s < 2.65 ? Gait::Walk : s < 5.75 ? Gait::Run : Gait::Sprint;
    }
    void Apply(const LocoOutput& o, double dtMs) {
        if (o.snap) { pos = o.snapPos; vel = o.velocity; }
        if (o.setHeading) heading = o.heading;
        if (o.setVelocity && !o.snap) {
            const double k = 1.0 - std::exp(-dtMs / lagMs);
            vel.x = float(vel.x + (o.velocity.x - vel.x) * k);
            vel.y = float(vel.y + (o.velocity.y - vel.y) * k);
        }
        pos.x = float(pos.x + vel.x * dtMs / 1000.0);
        pos.y = float(pos.y + vel.y * dtMs / 1000.0);
        pos.z = 30; // flat ground owns z
    }
};

static LocoInput In(const LocoVec& pos, const LocoVec& vel, double heading, const SimPed& ped, double dtMs) {
    LocoInput in;
    in.pos = pos; in.vel = vel; in.heading = float(heading);
    in.pedPos = ped.pos; in.pedHeading = ped.heading; in.pedGait = ped.GameGait();
    in.dtMs = dtMs;
    return in;
}

// ---- 1: gait -----------------------------------------------------------------

static void TestGait() {
    const LocoConfig c;
    const bool single = LocomotionController::InferGait(Gait::Still, 0.4f, c) == Gait::Still &&
                        LocomotionController::InferGait(Gait::Still, 1.5f, c) == Gait::Walk &&
                        LocomotionController::InferGait(Gait::Still, 4.5f, c) == Gait::Run &&
                        LocomotionController::InferGait(Gait::Still, 7.0f, c) == Gait::Sprint &&
                        LocomotionController::InferGait(Gait::Sprint, 0.0f, c) == Gait::Still &&
                        LocomotionController::InferGait(Gait::Sprint, 1.5f, c) == Gait::Walk;
    Check("gait: GTA speeds map from any gait in one step (0.4 still, 1.5 walk, 4.5 run, 7 sprint; 0 and 1.5 from sprint)", single);

    // Slow ramp 0 -> 8 -> 0 in 1 cm/s steps: record where each change happened.
    std::vector<double> ups, downs;
    Gait g = Gait::Still;
    for (int i = 0; i <= 800; ++i) {
        const Gait n = LocomotionController::InferGait(g, float(i * 0.01), c);
        if (n != g) ups.push_back(i * 0.01);
        g = n;
    }
    for (int i = 800; i >= 0; --i) {
        const Gait n = LocomotionController::InferGait(g, float(i * 0.01), c);
        if (n != g) downs.push_back(i * 0.01);
        g = n;
    }
    const bool ramp = ups.size() == 3 && downs.size() == 3 &&
                      std::fabs(ups[0] - 0.50) < 0.006 && std::fabs(ups[1] - 3.10) < 0.006 && std::fabs(ups[2] - 6.10) < 0.006 &&
                      std::fabs(downs[0] - 5.29) < 0.006 && std::fabs(downs[1] - 2.09) < 0.006 && std::fabs(downs[2] - 0.24) < 0.006;
    Check("gait: ramp up changes at 0.50 / 3.10 / 6.10, ramp down below 5.30 / 2.10 / 0.25 (hysteresis bands; W11: was 2.90 / 5.90 and 2.40 / 5.60)", ramp,
          F("ups %zu [%.2f %.2f %.2f] downs %zu [%.2f %.2f %.2f]", ups.size(), ups.size() > 0 ? ups[0] : -1, ups.size() > 1 ? ups[1] : -1,
            ups.size() > 2 ? ups[2] : -1, downs.size(), downs.size() > 0 ? downs[0] : -1, downs.size() > 1 ? downs[1] : -1, downs.size() > 2 ? downs[2] : -1));

    // Noise straddling the middle of each band: one change at most (the entry), at any fps.
    struct Band { double mid, noise; };
    const Band bands[3] = {{0.375, 0.11}, {2.60, 0.22}, {5.70, 0.13}};
    int worst = 0;
    size_t frames = 0;
    for (const Band& b : bands)
        for (double fps : kFps) {
            LocomotionController lc;
            SimPed ped;
            Rng rng(uint64_t(b.mid * 1000 + fps));
            const double dt = 1000.0 / fps;
            int changes = 0;
            Gait last = Gait::Still;
            for (int f = 0; f * dt < 10000; ++f) {
                const double s = b.mid + (rng.Uniform() * 2 - 1) * b.noise;
                const LocoOutput o = lc.Update(In(V(0, 0, 30), V(s, 0, 0), 90, ped, dt));
                if (f > 0 && o.gait != last) ++changes;
                last = o.gait;
                ++frames;
            }
            worst = std::max(worst, changes);
        }
    Check("gait: speed noise across the middle of each band (+-0.11 / 0.22 / 0.13 m/s) never flaps (<= 1 change in 10 s, 30/60/144 fps)",
          frames > 6000 && worst <= 1, F("worst %d changes over %zu frames", worst, frames));
}

// ---- 2: task re-issue ----------------------------------------------------------

struct TaskRun {
    uint32_t goStraight = 0, standStill = 0, snaps = 0;
    double minGapSameGaitMs = 1e9; // smallest spacing between two issues of the same gait
    size_t frames = 0;
    double maxErr = 0;
};

// A remote moving along +x at `speed` (or zigzagging: its direction flips by 90 degrees every
// 600 ms; flips faster than the controller's 120 ms velocity low-pass are smoothed away)
// for 10 s, followed by the simulated ped.
static TaskRun RunStraight(double speed, double fps, bool zigzag) {
    LocomotionController lc;
    SimPed ped;
    TaskRun r;
    const double dt = 1000.0 / fps;
    double t = 0, x = 0, y = 0, lastIssueT = -1;
    Gait lastIssueGait = Gait::Still;
    for (int f = 0; t < 10000; ++f, t += dt) {
        double vx = speed, vy = 0;
        if (zigzag) { const bool left = int(t / 600) % 2 == 0; vx = speed * 0.7071; vy = (left ? 1 : -1) * speed * 0.7071; }
        const LocoOutput o = lc.Update(In(V(x, y, 30), V(vx, vy, 0), -90, ped, dt));
        if (o.task == LocoTask::GoStraight) {
            if (lastIssueT >= 0 && o.gait == lastIssueGait) r.minGapSameGaitMs = std::min(r.minGapSameGaitMs, t - lastIssueT);
            lastIssueT = t;
            lastIssueGait = o.gait;
        }
        ped.Apply(o, dt);
        x += vx * dt / 1000.0;
        y += vy * dt / 1000.0;
        if (t > 2000) r.maxErr = std::max(r.maxErr, HDist(ped.pos, V(x, y, 30)));
        ++r.frames;
    }
    r.goStraight = lc.Stats().goStraight;
    r.standStill = lc.Stats().standStill;
    r.snaps = lc.Stats().snaps;
    return r;
}

static void TestTaskReissue() {
    const double speeds[3] = {1.5, 4.5, 7.0};
    const char* names[3] = {"walk 1.5", "run 4.5", "sprint 7"};
    for (int i = 0; i < 3; ++i) {
        TaskRun rs[3];
        for (int j = 0; j < 3; ++j) rs[j] = RunStraight(speeds[i], kFps[j], false);
        std::printf("  [%s m/s, 10 s] go-straight issues %u / %u / %u at 30 / 60 / 144 fps (frames %zu / %zu / %zu), min gap %.0f ms, max err %.3f m\n",
                    names[i], rs[0].goStraight, rs[1].goStraight, rs[2].goStraight, rs[0].frames, rs[1].frames, rs[2].frames,
                    std::min({rs[0].minGapSameGaitMs, rs[1].minGapSameGaitMs, rs[2].minGapSameGaitMs}), rs[2].maxErr);
        // W1 (docs/sync-specs/onfoot.md item 2) + W8 item 3: the target sits 3 s (3..16 m) ahead and is re-issued
        // only when the ped is about to reach it (within max(1 m, 0.12 * lead)), so the count follows the speed:
        // 5 / 4 / 5 per 10 s measured (W1: 6 / 6 / 8 with a 2.5 s lead and 0.25, 20 before that).
        const uint32_t maxIssues[3] = {5, 5, 6};
        bool ok = true;
        uint32_t lo = 1000, hi = 0;
        for (const TaskRun& r : rs) {
            ok = ok && r.frames >= 299 && r.goStraight >= 2 && r.goStraight <= maxIssues[i] && r.minGapSameGaitMs >= 500 - 1e-6 && r.snaps == 1;
            lo = std::min(lo, r.goStraight);
            hi = std::max(hi, r.goStraight);
        }
        Check((std::string("task re-issue, straight ") + names[i] + " m/s for 10 s: 2.." + std::to_string(maxIssues[i]) +
               " issues (20 before W1), >= 500 ms apart, within +-2 across 30/60/144 fps, one snap (the first frame)").c_str(),
              ok && hi - lo <= 2, F("issues %u / %u / %u, min gap %.0f / %.0f / %.0f ms", rs[0].goStraight, rs[1].goStraight, rs[2].goStraight,
                                    rs[0].minGapSameGaitMs, rs[1].minGapSameGaitMs, rs[2].minGapSameGaitMs));
    }
    // A target whose direction flips by 90 degrees every 600 ms: each flip is re-issued, but never faster than minReissueMs.
    const TaskRun z = RunStraight(4.5, 144, true);
    std::printf("  [zigzag 4.5 m/s @144fps] issues %u over %zu frames, min gap %.0f ms\n", z.goStraight, z.frames, z.minGapSameGaitMs);
    Check("task re-issue: a target that turns 90 degrees every 600 ms is re-issued on each turn, rate-limited (<= 10 s / 500 ms + 1 issues, >= 500 ms apart)",
          z.frames > 1400 && z.goStraight >= 12 && z.goStraight <= 21 && z.minGapSameGaitMs >= 500 - 1e-6,
          F("issues %u, min gap %.0f ms", z.goStraight, z.minGapSameGaitMs));

    // A direction change: running 4.5 m/s along +x, turning by `deg` at 3 s. The last task was issued at
    // ~1.9 s (arrival) and the next arrival is due at ~3.75 s, so any issue in 3.0 .. 3.7 s is the turn's.
    // 40 degrees (over the 25 degree rule) is re-issued within ~300 ms (the low-pass turns the velocity
    // vector first); 10 degrees is left to the running task.
    auto turnIssueAt = [](double deg) {
        LocomotionController lc;
        SimPed ped;
        const double dt = 1000.0 / 60, a = deg * kPi / 180.0;
        double x = 0, y = 0, at = -1;
        uint32_t before = 0;
        for (int f = 0; f * dt < 3700; ++f) {
            const double t = f * dt, ang = t < 3000 ? 0.0 : a;
            const double vx = 4.5 * std::cos(ang), vy = 4.5 * std::sin(ang);
            const LocoOutput o = lc.Update(In(V(x, y, 30), V(vx, vy, 0), -90, ped, dt));
            if (t < 3000) before = lc.Stats().goStraight;
            else if (o.task == LocoTask::GoStraight && at < 0) at = t;
            ped.Apply(o, dt);
            x += vx * dt / 1000.0;
            y += vy * dt / 1000.0;
        }
        return std::make_pair(before, at);
    };
    const auto big = turnIssueAt(40), small = turnIssueAt(10);
    Check("task re-issue: a 40 degree turn is re-issued within 300 ms (rate limit permitting); a 10 degree turn is not (no issue in the next 700 ms)",
          big.first == 2 && big.second >= 3000 && big.second < 3300 && small.first == 2 && small.second < 0,
          F("40 deg: %u issues before, next at %.0f ms; 10 deg: next at %.0f ms", big.first, big.second, small.second));

    // Walk, then stop for 5 s: exactly one stand-still, no go-straight while still.
    LocomotionController lc;
    SimPed ped;
    const double dt = 1000.0 / 60;
    double x = 0;
    uint32_t goAfterStop = 0, standStill = 0;
    for (int f = 0; f * dt < 8000; ++f) {
        const double t = f * dt;
        const double v = t < 3000 ? 1.5 : 0.0;
        const LocoOutput o = lc.Update(In(V(x, 0, 30), V(v, 0, 0), -90, ped, dt));
        if (t > 3100 && o.task == LocoTask::GoStraight) ++goAfterStop;
        if (o.task == LocoTask::StandStill) { ++standStill; if (o.standStillMs != 2000) standStill += 100; }
        ped.Apply(o, dt);
        x += v * dt / 1000.0;
    }
    Check("task: a remote that stops gets exactly one TASK_STAND_STILL(2000) and no go-straight while still",
          standStill == 1 && goAfterStop == 0 && lc.CurrentGait() == Gait::Still, F("stand-still %u, go-straight after stop %u", standStill, goAfterStop));
}

// ---- 3: snaps ------------------------------------------------------------------

static void TestSnap() {
    {
        LocomotionController lc;
        SimPed ped;
        const LocoOutput a = lc.Update(In(V(10, 0, 30), V(0, 0, 0), 0, ped, 16));
        ped.Apply(a, 16);
        const LocoOutput b = lc.Update(In(V(10, 0, 30), V(0, 0, 0), 0, ped, 16));
        LocoInput tin = In(V(10.5, 0, 30), V(0, 0, 0), 0, ped, 16);
        tin.teleported = true;
        const LocoOutput c = lc.Update(tin);
        ped.Apply(c, 16);
        // W1 (item 3): Reset() only re-issues (the ped is where it is, 0.6 m behind a pose that runs at
        // 4.5 m/s: the delay of the smoother); ResetHard() (a new ped) snaps, as Reset() used to.
        lc.Reset();
        const LocoOutput d = lc.Update(In(V(11.1, 0, 30), V(4.5, 0, 0), 0, ped, 16));
        lc.ResetHard();
        const LocoOutput e = lc.Update(In(V(11.1, 0, 30), V(4.5, 0, 0), 0, ped, 16));
        Check("snap: on the first frame (to the pose), not on the next; on Pose::teleported (even 0.5 m); not after Reset() "
              "(pose 0.6 m ahead: gait task re-issued instead); after ResetHard()",
              a.snap && a.snapPos.x == 10.0f && !b.snap && c.snap && c.snapPos.x == 10.5f && !d.snap &&
                  d.task == LocoTask::GoStraight && e.snap && lc.Stats().snaps == 3,
              F("first %d next %d teleported %d reset snap %d task %d, hard reset %d, snaps %u", int(a.snap), int(b.snap), int(c.snap),
                int(d.snap), int(d.task), int(e.snap), lc.Stats().snaps));
    }
    {
        // 5 + 5|v|: standing remote 5.5 m away snaps at once; moving at 2 m/s (threshold 15 m) 8 m does not.
        LocomotionController a, b;
        SimPed pa, pb;
        a.Update(In(V(0, 0, 30), V(0, 0, 0), 0, pa, 16)); // first-frame snap: ped is at 0
        b.Update(In(V(0, 0, 30), V(0, 0, 0), 0, pb, 16));
        pa.pos = V(0, 0, 30); pb.pos = V(0, 0, 30);
        const LocoOutput oa = a.Update(In(V(5.5, 0, 30), V(0, 0, 0), 0, pa, 16));
        const LocoOutput ob = b.Update(In(V(8, 0, 30), V(2, 0, 0), 0, pb, 16));
        const LocoOutput oc = b.Update(In(V(4.9, 0, 30), V(0, 0, 0), 0, pb, 0)); // dt 0: no accumulation, under 5 m
        Check("snap: error > 5 + 5|v| snaps at once (5.5 m standing), 8 m at 2 m/s (limit 15 m) and 4.9 m standing do not",
              oa.snap && !ob.snap && !oc.snap, F("5.5 m %d, 8 m @2 m/s %d, 4.9 m %d", int(oa.snap), int(ob.snap), int(oc.snap)));
    }
    // Persistent error: the ped is held 4 m from a standing remote (something blocks it).
    // The excess over 3 m integrates to 0.5 m*s: 500 ms at every frame rate.
    double tSnap[3] = {-1, -1, -1};
    bool neverUnderTol = true;
    for (int j = 0; j < 3; ++j) {
        const double dt = 1000.0 / kFps[j];
        LocomotionController lc;
        SimPed ped;
        lc.Update(In(V(0, 0, 30), V(0, 0, 0), 0, ped, dt)); // initial snap
        for (int f = 1; f * dt <= 2000; ++f) {
            ped.pos = V(4, 0, 30); // blocked
            const LocoOutput o = lc.Update(In(V(0, 0, 30), V(0, 0, 0), 0, ped, dt));
            if (o.snap) { tSnap[j] = f * dt; break; }
        }
        LocomotionController under;
        under.Update(In(V(0, 0, 30), V(0, 0, 0), 0, ped, dt));
        for (int f = 1; f * dt <= 10000; ++f) {
            ped.pos = V(2.9, 0, 30);
            if (under.Update(In(V(0, 0, 30), V(0, 0, 0), 0, ped, dt)).snap) neverUnderTol = false;
        }
    }
    std::printf("  [persistent error 4 m] snap after %.1f / %.1f / %.1f ms at 30 / 60 / 144 fps (per-frame RAGECOOP: 1000 / 500 / 208 ms)\n",
                tSnap[0], tSnap[1], tSnap[2]);
    bool fr = true;
    for (int j = 0; j < 3; ++j) fr = fr && tSnap[j] > 0 && std::fabs(tSnap[j] - 500.0) <= 1000.0 / kFps[j] + 1e-6;
    Check("snap: a persistent 4 m error snaps after 500 ms (+- one frame) at 30, 60 and 144 fps (time-based accumulator)",
          fr, F("%.1f / %.1f / %.1f ms", tSnap[0], tSnap[1], tSnap[2]));
    Check("snap: a persistent 2.9 m error (under the 3 m tolerance) never snaps in 10 s at any frame rate", neverUnderTol);
}

// ---- 4: velocity ---------------------------------------------------------------

static void TestVelocity() {
    // The first frame snaps and seeds the velocity low-pass with the remote velocity `v`, so the
    // second frame sees a settled filter (W1 item 4: the output follows a 120 ms low-pass of the pose velocity).
    auto first = [](LocomotionController& lc, SimPed& ped, double vx = 0) {
        lc.Update(In(V(0, 0, 30), V(vx, 0, 0), 0, ped, 16));
        ped.pos = V(0, 0, 30);
    };
    LocomotionController a, b, c;
    SimPed pa, pb, pc;
    first(a, pa, 1); first(b, pb, 1); first(c, pc);
    const LocoOutput far = a.Update(In(V(4, 0, 30), V(1, 0, 0), 90, pa, 16)); // 4 m: (2 + 20) * 4 = 88 -> cap 3
    const LocoOutput near = b.Update(In(V(0.1, 0, 30), V(1, 0, 0), 90, pb, 16)); // (2 + 0.5) * 0.1 = 0.25
    const LocoOutput diag = c.Update(In(V(3, 3, 30), V(0, 0, 0), 90, pc, 16));
    const double corrFar = std::hypot(far.velocity.x - 1.0, far.velocity.y);
    const double corrDiag = std::hypot(diag.velocity.x, diag.velocity.y);
    Check("velocity: remote v + (2 + 5|e|) e: 0.1 m error adds 0.25 m/s; a 4 m and a diagonal 4.2 m error are capped at 3 m/s (no fling)",
          far.setVelocity && std::fabs(corrFar - 3.0) < 1e-4 && std::fabs(near.velocity.x - 1.25f) < 1e-4 && std::fabs(corrDiag - 3.0) < 1e-4 &&
              std::fabs(diag.velocity.x - diag.velocity.y) < 1e-5,
          F("far corr %.4f, near vx %.4f, diag corr %.4f", corrFar, near.velocity.x, corrDiag));

    LocomotionController z1, z2, z3;
    SimPed q1, q2, q3;
    first(z1, q1); first(z2, q2); first(z3, q3);
    const LocoOutput o1 = z1.Update(In(V(0, 0, 30.3), V(0, 0, -0.5), 0, q1, 16)); // inside the deadband: vz passes through
    const LocoOutput o2 = z2.Update(In(V(0, 0, 31.2), V(0, 0, 0), 0, q2, 16));    // 2 * (1.2 - 0.5) = 1.4
    const LocoOutput o3 = z3.Update(In(V(0, 0, 26.0), V(0, 0, 0), 0, q3, 16));    // 2 * (-4 + 0.5) = -7 -> -2
    Check("velocity z: |dz| <= 0.5 m passes the remote vz through; above it adds 2 * (|dz| - 0.5) capped at 2 m/s",
          std::fabs(o1.velocity.z + 0.5f) < 1e-5 && std::fabs(o2.velocity.z - 1.4f) < 1e-4 && std::fabs(o3.velocity.z + 2.0f) < 1e-5 &&
              !o1.velocityZFromPhysics,
          F("vz %.4f / %.4f / %.4f", o1.velocity.z, o2.velocity.z, o3.velocity.z));

    // Jumping: no vertical correction, z from physics.
    LocomotionController j;
    SimPed pj;
    first(j, pj);
    LocoInput in = In(V(0, 0, 31.5), V(0, 0, 3), 0, pj, 16);
    in.jumpSeq = 4;
    j.Update(in);
    in.jumpSeq = 5;
    const LocoOutput oj = j.Update(in);
    Check("velocity z while jumping: no vertical correction (vz = remote vz) and velocityZFromPhysics",
          oj.jumpStart && oj.velocityZFromPhysics && std::fabs(oj.velocity.z - 3.0f) < 1e-5, F("jump %d phys %d vz %.3f", int(oj.jumpStart), int(oj.velocityZFromPhysics), oj.velocity.z));

    // ---- W1 item 4: dead band, low-pass, ragdoll follow ----
    // Dead band: an error under 0.08 m is not steered (the output is the remote velocity); 0.1 m is (0.25 m/s, above).
    LocomotionController d1, d2;
    SimPed e1, e2;
    first(d1, e1, 1); first(d2, e2, 1);
    const LocoOutput under = d1.Update(In(V(0.07, 0, 30), V(1, 0, 0), 90, e1, 16));
    const LocoOutput over = d2.Update(In(V(0.09, 0, 30), V(1, 0, 0), 90, e2, 16));
    Check("velocity: no steering for an error under 0.08 m (0.07 m: output = remote velocity); 0.09 m is steered ((2 + 5 * 0.09) * 0.09 = 0.22 m/s)",
          std::fabs(under.velocity.x - 1.0f) < 1e-4 && std::fabs(over.velocity.x - 1.2205f) < 1e-3,
          F("0.07 m: vx %.4f, 0.09 m: vx %.4f", under.velocity.x, over.velocity.x));

    // Low-pass (tau 120 ms): a +-1 m/s stride ripple at 2 Hz on a 4.5 m/s run. Input RMS about the mean is
    // 0.71 m/s (0.77 unfiltered, the position correction adds to it); a first-order filter at 120 ms passes
    // 0.55 of a 2 Hz sine (0.39 RMS) and the position correction at the dead band's edge puts back some:
    // 0.47 measured. The spec's "< 0.35" is below what tau 120 ms can do (the filter alone is 0.39); the
    // bound is 0.55 (a third below the unfiltered output), and the input RMS is asserted too so the test
    // cannot pass on a silent input.
    {
        LocomotionController lc;
        SimPed ped;
        const double dt = 1000.0 / 60;
        double x = 0, sum = 0, sumSq = 0, inSum = 0, inSq = 0;
        size_t n = 0;
        for (int f = 0; f * dt < 6000; ++f) {
            const double t = f * dt / 1000.0;
            const double vx = 4.5 + std::sin(2 * kPi * 2.0 * t);
            const LocoOutput o = lc.Update(In(V(x, 0, 30), V(vx, 0, 0), -90, ped, dt));
            ped.Apply(o, dt);
            x += vx * dt / 1000.0;
            if (t >= 1.0) { sum += o.velocity.x; sumSq += double(o.velocity.x) * o.velocity.x; inSum += vx; inSq += vx * vx; ++n; }
        }
        const double mean = sum / double(n), rms = std::sqrt(std::max(0.0, sumSq / double(n) - mean * mean));
        const double inMean = inSum / double(n), inRms = std::sqrt(std::max(0.0, inSq / double(n) - inMean * inMean));
        std::printf("  [ripple +-1 m/s @2 Hz on 4.5 m/s] RMS about the mean: input %.3f, output %.3f m/s\n", inRms, rms);
        Check("velocity: a +-1 m/s stride ripple at 2 Hz is low-passed (output RMS about its mean < 0.55 m/s; input 0.71, unfiltered output 0.77)",
              n > 250 && std::fabs(inRms - 0.707) < 0.02 && rms < 0.55 && std::fabs(mean - 4.5) < 0.1,
              F("input rms %.3f, output rms %.3f, mean %.3f, frames %zu", inRms, rms, mean, n));
    }

    // Ragdoll follow: the pose velocity (and the position correction, capped at 8 m/s in total) is set while
    // ragdolling; still no task and no heading (NaturalMotion owns the pose), z from physics.
    {
        LocomotionController lc;
        SimPed ped;
        first(lc, ped, 6);
        LocoInput in = In(V(0, 0, 30), V(6, 0, 0), 0, ped, 16);
        in.ragdoll = true;
        const LocoOutput on = lc.Update(in);
        LocoInput far = In(V(3, 0, 30), V(6, 0, 0), 0, ped, 16);   // 3 m behind the pose: (2 + 15) * 3 = 51 -> capped
        far.ragdoll = true;
        const LocoOutput capped = lc.Update(far);
        Check("ragdoll follow: while ragdolling the pose velocity is set (vx 6 on the pose), the error correction keeps the total <= 8 m/s, "
              "z from physics, no task, no heading",
              on.ragdollStart && on.setVelocity && std::fabs(on.velocity.x - 6.0f) < 1e-3 && on.velocityZFromPhysics &&
                  on.task == LocoTask::None && !on.setHeading && capped.setVelocity &&
                  std::fabs(std::hypot(capped.velocity.x, capped.velocity.y) - 8.0) < 1e-3 && capped.task == LocoTask::None && !capped.setHeading,
              F("on pose: vx %.3f; 3 m off: |v| %.3f", on.velocity.x, std::hypot(capped.velocity.x, capped.velocity.y)));
    }
}

// ---- 5: heading ----------------------------------------------------------------

static void TestHeading() {
    struct Case { double from, to; };
    const Case cases[2] = {{350, 10}, {10, -10}};
    bool shortWay = true, inRange = true;
    double remain[2][3];
    size_t frames = 0;
    for (int c = 0; c < 2; ++c)
        for (int j = 0; j < 3; ++j) {
            const double dt = 1000.0 / kFps[j];
            LocomotionController lc;
            SimPed ped;
            ped.heading = float(cases[c].from);
            lc.Update(In(V(0, 0, 30), V(0, 0, 0), cases[c].from, ped, dt)); // first frame snaps to the pose heading = from
            ped.pos = V(0, 0, 30);
            ped.heading = float(cases[c].from);
            const int n = int(std::lround(100.0 / dt * 1.0)); // frames in ~100 ms
            double t = 0;
            for (int f = 0; f < 1000; ++f) {
                const LocoOutput o = lc.Update(In(V(0, 0, 30), V(0, 0, 0), cases[c].to, ped, dt));
                ped.heading = o.heading;
                t += dt;
                ++frames;
                if (o.heading < 0.0f || o.heading >= 360.0f) inRange = false;
                if (std::fabs(WrapDeg(o.heading - cases[c].from)) > 20.001 || std::fabs(WrapDeg(o.heading - cases[c].to)) > 20.001) shortWay = false;
                if (f + 1 == n) remain[c][j] = std::fabs(WrapDeg(o.heading - cases[c].to)) * std::exp(-(100.0 - t) / 50.0); // normalise to exactly 100 ms
            }
        }
    const double expect = 20.0 * std::exp(-100.0 / 50.0); // 2.71 deg
    std::printf("  [heading] remaining after 100 ms: 350->10: %.3f / %.3f / %.3f, 10->350: %.3f / %.3f / %.3f deg (expected %.3f)\n",
                remain[0][0], remain[0][1], remain[0][2], remain[1][0], remain[1][1], remain[1][2], expect);
    bool fr = true;
    for (int c = 0; c < 2; ++c)
        for (int j = 0; j < 3; ++j) fr = fr && std::fabs(remain[c][j] - expect) < 0.05;
    Check("heading: 350 -> 10 and 10 -> 350 go the short way across 0/360, output always in [0, 360)",
          frames == 6000 && shortWay && inRange, F("short %d range %d", int(shortWay), int(inRange)));
    Check("heading: the approach is framerate independent (remaining after 100 ms within 0.05 deg of 20 * e^-2 at 30/60/144 fps)", fr);
}

// ---- 6: jump and ragdoll ---------------------------------------------------------

static void TestJumpRagdoll() {
    LocomotionController lc;
    SimPed ped;
    const double dt = 1000.0 / 60;
    double x = 0;
    int jumps = 0, tasksDuringJump = 0, starts = 0, ends = 0, setsDuringRagdoll = 0, velocityDuringRagdoll = 0, taskAfterRagdoll = -1, taskAfterJump = -1;
    for (int f = 0; f * dt < 6000; ++f) {
        const double t = f * dt;
        LocoInput in = In(V(x, 0, 30), V(4.5, 0, 0), -90, ped, dt);
        in.jumpSeq = t < 1000 ? 7 : 8;          // first value 7 is a baseline, 8 at 1 s is one jump
        in.ragdoll = t >= 3000 && t < 4000;
        const LocoOutput o = lc.Update(in);
        if (o.jumpStart) ++jumps;
        if (t >= 1000 && t < 1650 && o.task != LocoTask::None) ++tasksDuringJump;
        if (t >= 1700 && t < 1800 && o.task == LocoTask::GoStraight && taskAfterJump < 0) taskAfterJump = int(t);
        if (o.ragdollStart) ++starts;
        if (o.ragdollEnd) ++ends;
        // W1 item 4: the pose velocity IS set while ragdolling now (the ragdoll follows the sender); tasks and heading are not.
        if (in.ragdoll && !o.ragdollStart && (o.setHeading || o.task != LocoTask::None)) ++setsDuringRagdoll;
        if (in.ragdoll && o.setVelocity) ++velocityDuringRagdoll;
        if (t >= 4000 && taskAfterRagdoll < 0 && o.task == LocoTask::GoStraight) taskAfterRagdoll = int(t);
        ped.Apply(o, dt);
        x += 4.5 * dt / 1000.0;
    }
    Check("jump: one jump per jumpSeq change (the first value is a baseline); no task for 700 ms of it, re-issued on landing",
          jumps == 1 && tasksDuringJump == 0 && taskAfterJump >= 1700, F("jumps %d tasks in jump %d re-issue at %d ms", jumps, tasksDuringJump, taskAfterJump));
    Check("ragdoll: one start and one end edge; no task and no heading while ragdolling, the pose velocity is followed on every frame (60); "
          "the walk task comes back on the frame it ends",
          starts == 1 && ends == 1 && setsDuringRagdoll == 0 && velocityDuringRagdoll >= 59 && taskAfterRagdoll >= 4000 && taskAfterRagdoll < 4017,
          F("starts %d ends %d task/heading sets %d velocity frames %d task back at %d ms", starts, ends, setsDuringRagdoll,
            velocityDuringRagdoll, taskAfterRagdoll));
    // v2 defaults (jumpSeq -1, flags false): inert.
    const TaskRun r = RunStraight(4.5, 60, false);
    LocomotionController v2;
    SimPed p2;
    bool inert = true;
    for (int f = 0; f < 600; ++f) {
        const LocoOutput o = v2.Update(In(V(0, 0, 30), V(0, 0, 0), 0, p2, dt));
        if (o.jumpStart || o.ragdollStart || o.ragdollEnd || o.velocityZFromPhysics) inert = false;
        p2.Apply(o, dt);
    }
    Check("v2 input (no jumpSeq, no flags): never a jump, ragdoll edge or physics-owned z", inert && r.frames > 500 && v2.Stats().jumps == 0);

    // A respawned ped (ResetForNewPed) starts with fresh edges: the jumpSeq that moved on while
    // there was no ped is a baseline (no jump on its first frame), and a ragdoll still running
    // remotely is started on it. Reset() (same ped, e.g. an aim task ended) keeps the edges.
    {
        auto run = [&](bool newPed, int32_t seqBefore, int32_t seqAfter, bool ragBefore, bool ragAfter) {
            LocomotionController c;
            SimPed sp;
            for (int f = 0; f < 30; ++f) {
                LocoInput in = In(V(0, 0, 30), V(0, 0, 0), 0, sp, dt);
                in.jumpSeq = seqBefore;
                in.ragdoll = ragBefore;
                sp.Apply(c.Update(in), dt);
            }
            if (newPed) c.ResetForNewPed(); else c.Reset();
            SimPed fresh;
            LocoInput in = In(V(0, 0, 30), V(0, 0, 0), 0, fresh, dt);
            in.jumpSeq = seqAfter;
            in.ragdoll = ragAfter;
            return c.Update(in);
        };
        const LocoOutput jNew = run(true, 1, 2, false, false);   // remote jumped while despawned
        const LocoOutput rNew = run(true, 0, 0, true, true);     // respawned mid-ragdoll
        const LocoOutput rSame = run(false, 0, 0, true, true);   // same ped, Reset mid-ragdoll
        const LocoOutput jSame = run(false, 1, 2, false, false); // same ped: a real jump
        Check("ResetForNewPed: no jump on a new ped's first frame, a running ragdoll is started on it; Reset keeps the edges",
              jNew.snap && !jNew.jumpStart && rNew.ragdollStart && !rSame.ragdollStart && jSame.jumpStart,
              F("new: jump %d ragdollStart %d; same ped: ragdollStart %d jump %d", int(jNew.jumpStart),
                int(rNew.ragdollStart), int(rSame.ragdollStart), int(jSame.jumpStart)));
    }
}

// ---- 7: closed loop ----------------------------------------------------------------

struct LoopResult {
    double maxErr = 0, settleErr = 0, maxStepSpeed = 0, stopExcursion = 0; // farthest the ped gets from the stop point in the 2 s after the stop
    uint32_t snaps = 0, issues = 0;
    size_t n = 0;
    Gait gaitMid = Gait::Still;
};

// The remote goes round a 15 m circle at `speed` for 10 s, then stops dead for 3 s.
static LoopResult RunCircle(double speed, double fps) {
    LocomotionController lc;
    SimPed ped;
    LoopResult r;
    const double dt = 1000.0 / fps, radius = 15;
    double th = 0;
    LocoVec stopPos;
    for (int f = 0; f * dt < 13000; ++f) {
        const double t = f * dt;
        const bool moving = t < 10000;
        const LocoVec pos = V(radius * std::cos(th), radius * std::sin(th), 30);
        const LocoVec vel = moving ? V(-speed * std::sin(th), speed * std::cos(th), 0) : V(0, 0, 0);
        const double hdg = WrapDeg(std::atan2(-double(vel.x), double(vel.y)) * 180.0 / kPi);
        const LocoOutput o = lc.Update(In(pos, vel, moving ? hdg : 0, ped, dt));
        const LocoVec before = ped.pos;
        ped.Apply(o, dt);
        if (!o.snap) r.maxStepSpeed = std::max(r.maxStepSpeed, HDist(ped.pos, before) / (dt / 1000.0));
        if (moving) th += speed / radius * dt / 1000.0;
        else stopPos = pos;
        const LocoVec now = V(radius * std::cos(th), radius * std::sin(th), 30);
        if (t >= 2000 && moving) { r.maxErr = std::max(r.maxErr, HDist(ped.pos, now)); ++r.n; }
        if (t >= 12000) r.settleErr = std::max(r.settleErr, HDist(ped.pos, stopPos));
        if (t >= 10000 && t < 12000) r.stopExcursion = std::max(r.stopExcursion, HDist(ped.pos, stopPos));
        if (t >= 5000 && t < 5000 + dt) r.gaitMid = o.gait;
    }
    r.snaps = lc.Stats().snaps;
    r.issues = lc.Stats().goStraight;
    return r;
}

static void TestClosedLoop() {
    const double speeds[3] = {1.5, 4.5, 7.0};
    const Gait gaits[3] = {Gait::Walk, Gait::Run, Gait::Sprint};
    // W1 item 4: the 0.08 m steering dead band lets the ped lag the pose by up to ~0.08 m before it
    // corrects, and the 120 ms velocity low-pass adds its lag on a curve (a 7 m/s circle pulls 3.3 m/s^2):
    // the bounds were 0.02 / 0.12 / 0.25 and the settle bound 5 cm; the spec asks p95 <= 0.5 m, max <= 1.0 m.
    const double bound[3] = {0.12, 0.20, 0.40}; // ~1.5-2x the measured 0.081 / 0.105 / 0.209 m
    const double settleBound = 0.09;            // the dead band: the ped rests up to 0.08 m from the stop point
    for (int i = 0; i < 3; ++i) {
        LoopResult rs[3];
        bool ok = true;
        for (int j = 0; j < 3; ++j) {
            rs[j] = RunCircle(speeds[i], kFps[j]);
            ok = ok && rs[j].n > 200 && rs[j].maxErr < bound[i] && rs[j].settleErr < settleBound && rs[j].snaps == 1 &&
                 rs[j].gaitMid == gaits[i] && rs[j].maxStepSpeed < speeds[i] + 3.0 + 0.5 && rs[j].issues <= 20 &&
                 rs[j].stopExcursion < 0.45; // 0.10 / 0.26 / 0.39 m measured; 0.18 / 0.50 / 0.75 m if the low-pass also smoothed a stop
        }
        std::printf("  [circle %.1f m/s r=15 then stop] err max %.3f / %.3f / %.3f m, settled %.4f / %.4f / %.4f m, stop excursion %.3f m, "
                    "ped speed max %.2f m/s, snaps %u/%u/%u, issues %u/%u/%u (30/60/144 fps)\n",
                    speeds[i], rs[0].maxErr, rs[1].maxErr, rs[2].maxErr, rs[0].settleErr, rs[1].settleErr, rs[2].settleErr,
                    std::max({rs[0].stopExcursion, rs[1].stopExcursion, rs[2].stopExcursion}),
                    std::max({rs[0].maxStepSpeed, rs[1].maxStepSpeed, rs[2].maxStepSpeed}),
                    rs[0].snaps, rs[1].snaps, rs[2].snaps, rs[0].issues, rs[1].issues, rs[2].issues);
        Check(F("closed loop, circle at %.1f m/s then stop: error < %.2f m after 2 s, settles < 9 cm (the steering dead band), no overshoot past the stop "
                "(< 45 cm), one snap, right gait, ped speed <= v + cap, <= 20 tasks in 13 s (30/60/144 fps)", speeds[i], bound[i]).c_str(),
              ok, F("err %.3f/%.3f/%.3f settle %.4f/%.4f/%.4f excursion %.3f/%.3f/%.3f snaps %u/%u/%u gait %d", rs[0].maxErr, rs[1].maxErr, rs[2].maxErr,
                    rs[0].settleErr, rs[1].settleErr, rs[2].settleErr, rs[0].stopExcursion, rs[1].stopExcursion, rs[2].stopExcursion,
                    rs[0].snaps, rs[1].snaps, rs[2].snaps, int(rs[1].gaitMid)));
    }
}

// ---- 8: W1 gait authority, debounce, move rate, pause, aim gate --------------------------

// One scripted run: a remote moving along +x at speedAt(t) m/s for `ms`, followed by the simulated ped.
struct ScriptRun {
    uint32_t goStraight = 0, standStill = 0;
    double firstStandStillMs = -1, firstSprintMs = -1;
    size_t frames = 0;
    Gait finalGait = Gait::Still;
};

template <class SpeedFn, class StateFn>
static ScriptRun RunScript(double ms, double fps, SpeedFn speedAt, StateFn stateAt) {
    LocomotionController lc;
    SimPed ped;
    ScriptRun r;
    const double dt = 1000.0 / fps;
    double x = 0;
    for (int f = 0; f * dt < ms; ++f) {
        const double t = f * dt, v = speedAt(t);
        LocoInput in = In(V(x, 0, 30), V(v, 0, 0), -90, ped, dt);
        in.moveState = int8_t(stateAt(f, t));
        const LocoOutput o = lc.Update(in);
        if (o.task == LocoTask::StandStill && r.firstStandStillMs < 0) r.firstStandStillMs = t;
        if (o.gait == Gait::Sprint && r.firstSprintMs < 0) r.firstSprintMs = t;
        ped.Apply(o, dt);
        x += v * dt / 1000.0;
        ++r.frames;
    }
    r.goStraight = lc.Stats().goStraight;
    r.standStill = lc.Stats().standStill;
    r.finalGait = lc.CurrentGait();
    return r;
}

static void TestAuthoritativeGait() {
    // (a) The sender reports move 0 while the pose moves (swim / climb / cover / strafe report 0): the speed wins.
    const ScriptRun a = RunScript(2000, 60, [](double) { return 1.5; }, [](int, double) { return 0; });
    Check("authoritative gait: moveState 0 while the pose moves at 1.5 m/s for 2 s: walks (go-straight issued, gait Walk), no stand-still",
          a.frames >= 119 && a.standStill == 0 && a.goStraight >= 1 && a.finalGait == Gait::Walk,
          F("stand-still %u go-straight %u gait %d over %zu frames", a.standStill, a.goStraight, int(a.finalGait), a.frames));

    // (b) One stray 0 among 2s (a one-sample flap of the sender's gait) at 4.5 m/s: no stand-still, stays Run.
    {
        LocomotionController lc;
        SimPed ped;
        const double dt = 1000.0 / 60;
        double x = 0;
        bool allRun = true;
        size_t frames = 0;
        for (int f = 0; f * dt < 1000; ++f) {
            LocoInput in = In(V(x, 0, 30), V(4.5, 0, 0), -90, ped, dt);
            in.moveState = f == 30 ? 0 : 2;
            const LocoOutput o = lc.Update(in);
            if (o.gait != Gait::Run) allRun = false;
            ped.Apply(o, dt);
            x += 4.5 * dt / 1000.0;
            ++frames;
        }
        Check("authoritative gait: moveState 2 at 4.5 m/s with one 0 frame: no stand-still, the gait stays Run",
              frames >= 59 && lc.Stats().standStill == 0 && allRun, F("stand-still %u all run %d frames %zu", lc.Stats().standStill, int(allRun), frames));
    }

    // (c) The sender's gait may be at most one above what the speed shows.
    {
        LocomotionController lo, hi;
        SimPed po, ph;
        LocoInput slow = In(V(0, 0, 30), V(1.5, 0, 0), 0, po, 16);
        slow.moveState = 3;
        LocoInput fast = In(V(0, 0, 30), V(4.5, 0, 0), 0, ph, 16);
        fast.moveState = 3;
        const LocoOutput os = lo.Update(slow), of = hi.Update(fast);
        Check("authoritative gait: capped at one above the speed's gait (moveState 3 at 1.5 m/s shows Run, at 4.5 m/s Sprint)",
              os.gait == Gait::Run && of.gait == Gait::Sprint, F("1.5 m/s -> %d, 4.5 m/s -> %d", int(os.gait), int(of.gait)));
    }

    // (d) Stopping: Still only after the speed stayed under idleDown for 150 ms. Inferred (no moveState).
    auto stopRun = [](double stopMs) {
        return RunScript(3000, 60, [stopMs](double t) { return t >= 1000 && t < 1000 + stopMs ? 0.0 : 4.5; },
                         [](int, double) { return -1; });
    };
    const ScriptRun blip = stopRun(83);
    const ScriptRun stop = stopRun(600);
    const double delay = stop.firstStandStillMs - 1000.0;
    std::printf("  [stop at 1000 ms] stand-still issued %.0f ms after the speed fell (83 ms blip: %u stand-stills)\n", delay, blip.standStill);
    Check("authoritative gait: a speed dip of 83 ms (under the 150 ms hold) does not stand the ped still",
          blip.frames >= 179 && blip.standStill == 0 && blip.finalGait == Gait::Run, F("stand-still %u gait %d", blip.standStill, int(blip.finalGait)));
    Check("authoritative gait: a real stop issues exactly one stand-still, 150 ms (+- 2 frames) after the speed fell",
          stop.standStill == 1 && delay >= 120 && delay <= 190, F("stand-still %u after %.0f ms", stop.standStill, delay));

    // Debounce: the sender's gait flapping Run / Sprint every frame (speed 5 m/s) must not re-issue the task
    // per frame; a sustained Sprint is taken after gaitDebounceMs (400 ms).
    const ScriptRun flap = RunScript(3000, 60, [](double) { return 5.0; }, [](int f, double) { return f % 2 ? 3 : 2; });
    Check("gait class change is debounced: Run <-> Sprint flapping every frame for 3 s issues <= 3 tasks (was one per frame)",
          flap.frames >= 179 && flap.goStraight <= 3 && flap.firstSprintMs < 0, F("go-straight %u, first sprint at %.0f ms", flap.goStraight, flap.firstSprintMs));
    const ScriptRun keep = RunScript(3000, 60, [](double) { return 5.0; }, [](int, double t) { return t < 1000 ? 2 : 3; });
    Check("gait class change is debounced: a sustained Sprint from 1000 ms is shown after 400 ms (+- 2 frames), not at once (W11: was 200)",
          keep.firstSprintMs >= 1360 && keep.firstSprintMs <= 1460 && keep.finalGait == Gait::Sprint,
          F("sprint first shown at %.0f ms, final gait %d", keep.firstSprintMs, int(keep.finalGait)));
}

static void TestMoveRate() {
    // Steady speeds: rate = speed / nominal(gait) clamped 0.6..1.6 (nominal 1.5 / 4.5 / 7.0 m/s; W8 item 3: was 0.8..1.25).
    struct Case { double speed, expect; Gait gait; };
    const Case cases[] = {{1.5, 1.0, Gait::Walk},   {2.0, 2.0 / 1.5, Gait::Walk}, {1.2, 0.8, Gait::Walk},  {4.5, 1.0, Gait::Run},
                          {5.4, 1.2, Gait::Run},    {3.3, 3.3 / 4.5, Gait::Run},   {7.0, 1.0, Gait::Sprint}, {8.4, 1.2, Gait::Sprint},
                          {6.3, 6.3 / 7.0, Gait::Sprint}, {9.9, 9.9 / 7.0, Gait::Sprint}, {12.0, 1.6, Gait::Sprint}, {0.8, 0.6, Gait::Walk}};
    bool ok = true;
    std::string bad;
    for (const Case& c : cases) {
        LocomotionController lc;
        SimPed ped;
        LocoOutput o;
        const double dt = 1000.0 / 60;
        double x = 0;
        for (int f = 0; f < 60; ++f) {
            o = lc.Update(In(V(x, 0, 30), V(c.speed, 0, 0), -90, ped, dt));
            ped.Apply(o, dt);
            x += c.speed * dt / 1000.0;
        }
        if (!(o.setMoveRate && o.gait == c.gait && std::fabs(o.moveRate - c.expect) < 0.01)) {
            ok = false;
            bad += F(" [%.1f m/s: set %d gait %d rate %.3f want %.3f]", c.speed, int(o.setMoveRate), int(o.gait), o.moveRate, c.expect);
        }
    }
    Check("move rate: a moving gait sets SET_PED_MOVE_RATE_OVERRIDE = speed / nominal(gait) clamped 0.6..1.6 (12 speeds, 3 gaits, both clamps hit)", ok, bad);
    {
        LocomotionController lc;
        SimPed ped;
        const LocoOutput o = lc.Update(In(V(0, 0, 30), V(0, 0, 0), 0, ped, 16));
        Check("move rate: none while still", !o.setMoveRate && o.gait == Gait::Still);
    }

    // Fallback (moveRateAvailable = false): a Run ramp 4.0 -> 5.5 m/s over 3 s gives quantised blend ratios
    // (0.25 steps, within half a step of Run's 2) re-issued at most every 400 ms; with the override the blend
    // ratio is always the plain 2.
    auto ramp = [](bool available, std::vector<double>& times, std::vector<float>& ratios) {
        LocomotionController lc;
        SimPed ped;
        const double dt = 1000.0 / 60;
        double x = 0;
        for (int f = 0; f * dt < 3000; ++f) {
            const double t = f * dt, v = 4.0 + 1.5 * t / 3000.0;
            LocoInput in = In(V(x, 0, 30), V(v, 0, 0), -90, ped, dt);
            in.moveRateAvailable = available;
            const LocoOutput o = lc.Update(in);
            if (o.task == LocoTask::GoStraight) { times.push_back(t); ratios.push_back(o.moveBlendRatio); }
            ped.Apply(o, dt);
            x += v * dt / 1000.0;
        }
    };
    std::vector<double> tF, tR;
    std::vector<float> rF, rR;
    ramp(false, tF, rF);
    ramp(true, tR, rR);
    bool quant = !rF.empty(), integral = !rR.empty();
    double minGap = 1e9;
    std::vector<float> distinct;
    for (size_t i = 0; i < rF.size(); ++i) {
        quant = quant && std::fabs(rF[i] * 4.0 - std::round(rF[i] * 4.0)) < 1e-4 && rF[i] >= 1.5f && rF[i] <= 2.5f;
        if (std::find(distinct.begin(), distinct.end(), rF[i]) == distinct.end()) distinct.push_back(rF[i]);
        if (i) minGap = std::min(minGap, tF[i] - tF[i - 1]);
    }
    for (float r : rR) integral = integral && r == 2.0f;
    std::printf("  [fallback blend ramp] %zu issues, %zu distinct ratios, min gap %.0f ms; with the override %zu issues\n", rF.size(), distinct.size(), minGap, rR.size());
    Check("move rate fallback: without the override the blend ratio is quantised (0.25 steps, 1.5..2.5 for Run), varies with the speed and is re-issued >= 400 ms apart; "
          "with it the ratio stays the plain 2",
          quant && integral && distinct.size() >= 3 && rF.size() >= 3 && minGap >= 400 - 1e-6,
          F("fallback: %zu issues, %zu ratios, min gap %.0f; override: integral %d", rF.size(), distinct.size(), minGap, int(integral)));
}

static void TestPause() {
    LocomotionController lc;
    SimPed ped;
    const double dt = 1000.0 / 60;
    double x = 0;
    int pausedFrames = 0, actions = 0, resumed = -1;
    bool snapAfter = false;
    for (int f = 0; f < 60 + 120; ++f) {
        if (f == 60) lc.Pause(1500);
        const LocoOutput o = lc.Update(In(V(x, 0, 30), V(4.5, 0, 0), -90, ped, dt));
        if (f >= 60) {
            if (o.paused) {
                ++pausedFrames;
                if (o.snap || o.task != LocoTask::None || o.setVelocity || o.setHeading || o.setMoveRate || o.jumpStart || o.ragdollStart)
                    ++actions;
            } else {
                if (resumed < 0 && o.task == LocoTask::GoStraight) resumed = int((f - 60) * dt);
                if (o.snap) snapAfter = true;
            }
        }
        ped.Apply(o, dt);
        x += 4.5 * dt / 1000.0;
    }
    Check("pause: Pause(1500) sets nothing for 1.5 s (no snap, task, velocity, heading, move rate), then re-issues the gait task without a snap",
          pausedFrames >= 88 && pausedFrames <= 91 && actions == 0 && resumed >= 1475 && resumed <= 1520 && !snapAfter,
          F("paused frames %d, actions while paused %d, task back at %d ms, snap after %d", pausedFrames, actions, resumed, int(snapAfter)));
}

static void TestAimGate() {
    const LocoVec ped = V(0, 0, 30);
    const LocoVec aim = V(10, 20, 31); // 22.4 m away: tolerance max(1, 0.05 * 22.4) = 1.12 m
    AimGate g;
    const bool first = g.ShouldIssue(aim, ped, Gait::Run, false, false, false);
    const bool same = g.ShouldIssue(aim, ped, Gait::Run, true, false, false);
    const bool small = g.ShouldIssue(V(10.8, 20, 31), ped, Gait::Run, true, false, false);
    const bool moved = g.ShouldIssue(V(11.5, 20, 31), ped, Gait::Run, true, false, false);
    Check("aim gate: issued when no aim task runs; not again for the same point or a 0.8 m move; issued after a 1.5 m move",
          first && !same && !small && moved, F("first %d same %d 0.8 m %d 1.5 m %d", int(first), int(same), int(small), int(moved)));
    const bool gaitSame = g.ShouldIssue(V(11.5, 20, 31), ped, Gait::Run, true, false, false);
    const bool gaitChanged = g.ShouldIssue(V(11.5, 20, 31), ped, Gait::Walk, true, false, false);
    const bool locoTask = g.ShouldIssue(V(11.5, 20, 31), ped, Gait::Walk, true, true, false);
    const bool lost = g.ShouldIssue(V(11.5, 20, 31), ped, Gait::Walk, false, false, false);
    Check("aim gate: issued when the shown gait changes, when the controller re-issued its go-straight, and when the task was lost; not otherwise",
          !gaitSame && gaitChanged && locoTask && lost, F("same gait %d changed %d loco issued %d task lost %d", int(gaitSame), int(gaitChanged), int(locoTask), int(lost)));
    AimGate h;
    h.ShouldIssue(aim, ped, Gait::Run, false, false, false);
    const bool air = h.ShouldIssue(V(30, 20, 31), ped, Gait::Walk, true, true, true);       // 20 m move, gait change, loco task: still no
    const bool airNoTask = h.ShouldIssue(V(30, 20, 31), ped, Gait::Walk, false, false, true);
    const bool landed = h.ShouldIssue(V(30, 20, 31), ped, Gait::Walk, true, false, false);  // the move is still pending
    AimGate far;
    far.ShouldIssue(V(0, 200, 31), ped, Gait::Run, false, false, false);
    const bool farSmall = far.ShouldIssue(V(5, 200, 31), ped, Gait::Run, true, false, false);  // 5 m of 200 m: tolerance 10 m
    const bool farMoved = far.ShouldIssue(V(12, 200, 31), ped, Gait::Run, true, false, false);
    Check("aim gate: never while airborne (jump or fall), and the pending change is issued on landing; a 200 m aim point needs a 10 m move, not 1 m",
          !air && !airNoTask && landed && !farSmall && farMoved,
          F("airborne %d / %d, landed %d; far 5 m %d, 12 m %d", int(air), int(airNoTask), int(landed), int(farSmall), int(farMoved)));
}

// 60 s round a 15 m circle at 4.5 m/s (the spec's loco closed-loop run).
static void TestLongCircle() {
    LocomotionController lc;
    SimPed ped;
    const double dt = 1000.0 / 60, radius = 15, speed = 4.5;
    double th = 0;
    std::vector<double> errs;
    for (int f = 0; f * dt < 60000; ++f) {
        const double t = f * dt;
        const LocoVec pos = V(radius * std::cos(th), radius * std::sin(th), 30);
        const LocoVec vel = V(-speed * std::sin(th), speed * std::cos(th), 0);
        const double hdg = WrapDeg(std::atan2(-double(vel.x), double(vel.y)) * 180.0 / kPi);
        const LocoOutput o = lc.Update(In(pos, vel, hdg, ped, dt));
        ped.Apply(o, dt);
        th += speed / radius * dt / 1000.0;
        const LocoVec now = V(radius * std::cos(th), radius * std::sin(th), 30);
        if (t >= 2000) errs.push_back(HDist(ped.pos, now));
    }
    std::sort(errs.begin(), errs.end());
    const double p95 = errs[size_t(double(errs.size()) * 0.95)], mx = errs.back();
    const LocoStats& s = lc.Stats();
    std::printf("  [circle 4.5 m/s r=15, 60 s] err p95 %.3f max %.3f m, snaps %u (persistent %u), go-straight %u, stand-still %u\n", p95, mx,
                s.snaps, s.persistentSnaps, s.goStraight, s.standStill);
    Check("closed loop, 60 s on a 4.5 m/s circle: p95 error <= 0.5 m, max <= 1.0 m, no persistent snap, 1 snap in all, <= 1 go-straight per s, no stand-still",
          errs.size() > 3400 && p95 <= 0.5 && mx <= 1.0 && s.persistentSnaps == 0 && s.snaps == 1 && s.goStraight <= 60 && s.standStill == 0,
          F("p95 %.3f max %.3f persistent %u snaps %u go-straight %u stand-still %u", p95, mx, s.persistentSnaps, s.snaps, s.goStraight, s.standStill));
}

// Sync plan W6: a swimming ped (protocol v5 kPfSwimming). The game reports no land gait for a swimmer
// (IS_PED_WALKING / RUNNING / SPRINTING are all false), which the stall rule reads as "the task ended":
// it re-issued the go-straight every minReissueMs and re-scaled a land animation that is not playing.
static void TestSwimming() {
    struct Run { uint32_t goStraight = 0, stall = 0; int moveRateFrames = 0; int frames = 0; double maxErr = 0; };
    auto run = [](bool swimming) {
        LocomotionController lc;
        SimPed ped;
        Run r;
        const double dt = 1000.0 / 60, speed = 0.7;   // slow: the 3 m target is reached after 2.9 s, so the stall rule is the one that re-issues
        for (int f = 0; f * dt < 6000; ++f) {
            const double t = f * dt / 1000.0;
            const LocoVec pos = V(0, speed * t, 30), vel = V(0, speed, 0);
            LocoInput in = In(pos, vel, 0, ped, dt);
            in.pedGait = Gait::Still;          // the game shows no land gait for a swimmer
            in.swimming = swimming;
            const LocoOutput o = lc.Update(in);
            ped.Apply(o, dt);
            if (o.setMoveRate) ++r.moveRateFrames;
            ++r.frames;
            if (f * dt > 1500) r.maxErr = std::max(r.maxErr, HDist(ped.pos, V(0, speed * (f + 1) * dt / 1000.0, 30)));
        }
        r.goStraight = lc.Stats().goStraight;
        r.stall = lc.Stats().whyStall;
        return r;
    };
    const Run land = run(false), swim = run(true);
    // A land ped that reads Still while a moving gait is commanded stalls: the first re-issue stallFirstMs (Sync A: 0.9 s, was
    // 2.5 s) after the task, then one per stallRetryMs (2.5 s: W8 item 3, it was one per 0.5 s = 11 in 6 s); a swimmer reads
    // Still all the time and must not stall at all.
    Check("swimming: a Still-reading swimmer is not a stall (a land ped is: exactly 3 stall re-issues in 6 s, at ~0.9 / 3.4 / 5.9 s, not 11; the swimmer 0), no land move-rate override, still tracked",
          land.frames == 360 && swim.frames == 360 && land.stall == 3 && swim.stall == 0 && swim.goStraight <= 3 &&
              land.moveRateFrames > 300 && swim.moveRateFrames == 0 && swim.maxErr < 1.0,
          F("land: go-straight %u stall %u move-rate frames %d; swim: go-straight %u stall %u move-rate frames %d max err %.3f", land.goStraight, land.stall,
            land.moveRateFrames, swim.goStraight, swim.stall, swim.moveRateFrames, swim.maxErr));
}


// ---- W8 item 3 / 4 (docs/sync-specs/live-0.4.2-fixes.md): churn, soft reset, swim correction ---------------

// A straight run at constant speed seen the way the controller sees it live: 33 ms samples whose arrival
// jitters by +-20 ms (the pose the smoother hands over moves by up to speed * 20 ms along the line) and the
// sender's stride ripple on the velocity, 60 fps frames.
struct ChurnRun {
    uint32_t goStraight = 0, whyForced = 0, whyReset = 0, whyOther = 0, whySum = 0;
    double minGapMs = 1e9;
    int frames = 0;
    double maxErr = 0;
};

static ChurnRun RunChurn(double speed, double durMs, uint64_t seed) {
    LocomotionController lc;
    SimPed ped;
    Rng rng(seed);
    ChurnRun r;
    const double dt = 1000.0 / 60, sampleMs = 33.3;
    double jitterA = 0, jitterB = 0, x = 0, lastIssue = -1;
    int sampleIdx = -1;
    for (int f = 0; f * dt < durMs; ++f) {
        const double t = f * dt;
        const int idx = int(t / sampleMs);
        while (sampleIdx < idx) { // a new sample: draw its arrival jitter (+-20 ms)
            ++sampleIdx;
            jitterA = jitterB;
            jitterB = (rng.Uniform() * 2.0 - 1.0) * 20.0;
        }
        const double frac = (t - idx * sampleMs) / sampleMs;
        const double jit = jitterA + (jitterB - jitterA) * frac;
        const double px = speed * (t + jit) / 1000.0;
        const double ripple = 0.5 * std::sin(2.0 * kPi * 2.0 * t / 1000.0) + (rng.Uniform() - 0.5) * 0.3;
        const double vy = (rng.Uniform() - 0.5) * 0.2;
        const LocoOutput o = lc.Update(In(V(px, 0, 30), V(speed + ripple, vy, 0), -90, ped, dt));
        if (o.task == LocoTask::GoStraight) {
            if (lastIssue >= 0) r.minGapMs = std::min(r.minGapMs, t - lastIssue);
            lastIssue = t;
        }
        ped.Apply(o, dt);
        x = px;
        if (t > 1500) r.maxErr = std::max(r.maxErr, HDist(ped.pos, V(px, 0, 30)));
        ++r.frames;
    }
    (void)x;
    const LocoStats& s = lc.Stats();
    r.goStraight = s.goStraight;
    r.whyForced = s.whyForced;   // the first frame's snap
    r.whyReset = s.whyReset;
    r.whyOther = s.whyGait + s.whyBlend + s.whyTurn + s.whyDrift + s.whyStall;
    r.whySum = s.whyGait + s.whyReset + s.whyForced + s.whyBlend + s.whyTurn + s.whyDrift + s.whyArrive + s.whyStall;
    return r;
}

static void TestChurn() {
    // 60 s at 4.5 m/s: the arrival rule alone (lead 3 s = 13.5 m, re-issued 1.62 m before the ped reaches it =
    // every 2.6 s) is the only re-issue; before W8 it was a lead of 2.5 s and 0.25 (every 1.9 s = 6 per 10 s).
    const ChurnRun r = RunChurn(4.5, 60000, 0xC0FFEE);
    const uint32_t allowed = 1 + uint32_t(60000 / 2500);
    std::printf("  [straight run 4.5 m/s, 60 s, 33 ms samples +-20 ms] go-straight %u (first-frame snap %u, arrival %u, reset %u, other %u), min gap %.0f ms, max err %.3f m\n",
                r.goStraight, r.whyForced, r.goStraight - r.whyForced - r.whyReset - r.whyOther, r.whyReset, r.whyOther, r.minGapMs, r.maxErr);
    Check("churn: a 60 s straight run (33 ms samples, 20 ms jitter, stride ripple) issues <= 1 task per 2.5 s (25 in all), none caused by Reset / turn / drift / stall, >= 2 s apart, ped within 0.5 m",
          r.frames >= 3599 && r.goStraight >= 15 && r.goStraight <= allowed && r.whyForced == 1 && r.whyReset == 0 && r.whyOther == 0 && r.minGapMs >= 2000 &&
              r.maxErr < 0.5,
          F("go-straight %u (limit %u) over %d frames, first-frame snap %u reset %u other %u, min gap %.0f ms, max err %.3f", r.goStraight, allowed, r.frames, r.whyForced,
            r.whyReset, r.whyOther, r.minGapMs, r.maxErr));
    Check("churn counters: the per-cause counters add up to every go-straight issued",
          r.whySum == r.goStraight && r.whySum > 0, F("sum %u, issued %u", r.whySum, r.goStraight));
}

// SoftReset: the running task is kept unless a position rule would re-issue it; Reset() always re-issues.
static void TestSoftReset() {
    struct Out { uint32_t issuedAtReset = 0; LocoWhy why = LocoWhy::None; LocoStats stats; };
    // A 4.5 m/s straight run; at 1000 ms (0.5 s after the first issue is long past) the engine "touches" the ped.
    auto run = [](int mode) { // 0 plain, 1 Reset, 2 SoftReset
        LocomotionController lc;
        SimPed ped;
        const double dt = 1000.0 / 60;
        double x = 0;
        Out out;
        for (int f = 0; f < 90; ++f) {
            if (f == 60) {
                if (mode == 1) lc.Reset();
                else if (mode == 2) lc.SoftReset();
            }
            const LocoOutput o = lc.Update(In(V(x, 0, 30), V(4.5, 0, 0), -90, ped, dt));
            if (f == 60 && o.task == LocoTask::GoStraight) { out.issuedAtReset = 1; out.why = o.why; }
            ped.Apply(o, dt);
            x += 4.5 * dt / 1000.0;
        }
        out.stats = lc.Stats();
        return out;
    };
    const Out plain = run(0), hard = run(1), soft = run(2);
    Check("soft reset: Reset() re-issues the gait task at once (caused by Reset); SoftReset() on a straight run keeps the running task (counted kept); no touch issues none",
          hard.issuedAtReset == 1 && hard.why == LocoWhy::Reset && hard.stats.whyReset == 1 && hard.stats.resets == 1 && soft.issuedAtReset == 0 &&
              soft.stats.softResets == 1 && soft.stats.softKept == 1 && soft.stats.softReissued == 0 && plain.issuedAtReset == 0 &&
              plain.stats.goStraight == 1 && soft.stats.goStraight == 1 && hard.stats.goStraight == 2,
          F("reset: issued %u why %d (whyReset %u); soft: issued %u kept %u reissued %u go-straight %u; plain issued %u go-straight %u; reset go-straight %u",
            hard.issuedAtReset, int(hard.why), hard.stats.whyReset, soft.issuedAtReset, soft.stats.softKept, soft.stats.softReissued, soft.stats.goStraight,
            plain.issuedAtReset, plain.stats.goStraight, hard.stats.goStraight));

    // The direction turns 90 degrees at 300 ms; at 400 ms (before minReissueMs = 500) a SoftReset re-evaluates
    // and the turn rule fires at once; the same frame without it issues nothing.
    auto turn = [](bool soft) {
        LocomotionController lc;
        SimPed ped;
        const double dt = 1000.0 / 60;
        double x = 0, y = 0;
        double issuedAt = -1;
        LocoWhy why = LocoWhy::None;
        for (int f = 0; f < 33; ++f) { // 0 .. 533 ms
            const double t = f * dt;
            if (f == 24 && soft) lc.SoftReset(); // 400 ms
            const double ang = t < 300 ? 0.0 : kPi / 2;
            const double vx = 4.5 * std::cos(ang), vy = 4.5 * std::sin(ang);
            const LocoOutput o = lc.Update(In(V(x, y, 30), V(vx, vy, 0), -90, ped, dt));
            if (f > 0 && o.task == LocoTask::GoStraight && issuedAt < 0) { issuedAt = t; why = o.why; }
            ped.Apply(o, dt);
            x += vx * dt / 1000.0;
            y += vy * dt / 1000.0;
        }
        return std::make_pair(issuedAt, why);
    };
    const auto withSoft = turn(true), without = turn(false);
    Check("soft reset: after a 90 degree turn a SoftReset at 400 ms re-issues at once for the turn (before the 500 ms rate limit); without it nothing is issued before 500 ms",
          withSoft.first >= 399 && withSoft.first <= 420 && withSoft.second == LocoWhy::Turn && (without.first < 0 || without.first >= 490),
          F("soft: issued at %.0f ms why %d; plain: issued at %.0f ms", withSoft.first, int(withSoft.second), without.first));
}

// W8 item 4: the water-tuned correction (LocoInput::swimming).
static void TestSwimCorrection() {
    struct R { double dvx = 0, dvz = 0; };
    auto run = [](bool swimming, double dzBelow) {
        LocomotionController lc;
        SimPed ped;
        LocoInput in = In(V(10, 0, 30), V(1.6, 0, 0), -90, ped, 16);
        in.swimming = swimming;
        in.pedGait = Gait::Still;
        lc.Update(in); // first frame: snap, started
        ped.pos = V(9.0, 0, 30 - dzBelow); // the ped is 1 m behind and dzBelow under the pose
        LocoInput in2 = In(V(10, 0, 30), V(1.6, 0, 0), -90, ped, 16);
        in2.swimming = swimming;
        in2.pedGait = Gait::Still;
        const LocoOutput o = lc.Update(in2);
        R r;
        r.dvx = o.velocity.x - 1.6;
        r.dvz = o.velocity.z;
        return r;
    };
    const R land = run(false, 1.4), swim = run(true, 1.4), landDiver = run(false, 0.8), swimDiver = run(true, 0.8);
    Check("swim correction: the horizontal pull is capped at 1.5 m/s (land 3.0) and the vertical band is 0.9 m (land 0.5): a diver 0.8 m under the pose is not pushed up, a land ped is",
          std::fabs(land.dvx - 3.0) < 0.05 && std::fabs(swim.dvx - 1.5) < 0.05 && std::fabs(land.dvz - 1.8) < 0.05 && std::fabs(swim.dvz - 0.5) < 0.05 &&
              std::fabs(landDiver.dvz - 0.6) < 0.05 && std::fabs(swimDiver.dvz) < 0.001,
          F("pull land %.2f swim %.2f; up 1.4 m: land %.2f swim %.2f; up 0.8 m: land %.2f swim %.2f", land.dvx, swim.dvx, land.dvz, swim.dvz, landDiver.dvz, swimDiver.dvz));
}


// ---- 9: W11 (docs/CLOUD-HANDOFF.md item 0b): no re-issue on a gait change, anchor steering, residual push -----------------

// A ped that moves by its animation: while a task runs it walks toward the task's target (the coordinate or the anchor
// entity) at nominal(blend) * moveRate; a SET_ENTITY_VELOCITY from the controller replaces that velocity for the frame
// (so a push of pedVel + residual adds to the animation, the pose-velocity push of the old logic replaces it).
struct AnimPed {
    LocoVec pos = V(0, 0, 30), anchor;
    float heading = 0;
    int task = 0;            // 0 none / stand-still, 1 go-straight coordinate, 2 go-to-entity (anchor)
    LocoVec target;
    float blend = 0, rate = 1;
    bool lost = false;       // the task is gone (the ped stands, reading Still)
    double scale = 1.0;      // animation speed error (0.9 = 10% slower than commanded)
    static double Nominal(double b) {
        if (b <= 0) return 0;
        if (b <= 1) return 1.5 * b;
        if (b <= 2) return 1.5 + 3.0 * (b - 1);
        return 4.5 + 2.5 * (std::min(b, 3.0) - 2);
    }
    LocoVec AnimVel() const {
        if (task == 0 || lost) return V(0, 0, 0);
        const LocoVec& t = task == 2 ? anchor : target;
        const double dx = double(t.x) - pos.x, dy = double(t.y) - pos.y, d = std::hypot(dx, dy);
        if (d < 0.25) return V(0, 0, 0);
        const double sp = Nominal(blend) * rate * scale;
        return V(dx / d * sp, dy / d * sp, 0);
    }
};

struct Pose { LocoVec pos, vel; float heading; };

// The pose of a remote that travels at speedAt(t) along the direction dirAt(t) (degrees, math convention: 0 = +x).
struct Path {
    std::function<double(double)> speed, dirDeg;
    double x = 0, y = 0;
    Pose At(double t) const {
        const double a = dirDeg(t) * kPi / 180.0, s = speed(t);
        Pose p;
        p.pos = V(x, y, 30);
        p.vel = V(s * std::cos(a), s * std::sin(a), 0);
        p.heading = float(WrapDeg(std::atan2(-std::cos(a), std::sin(a)) * 180.0 / kPi)); // GTA heading 0 = +y, counter-clockwise: h = atan2(-dx, dy)
        return p;
    }
    void Advance(double t, double dtMs) {
        const Pose p = At(t);
        x += p.vel.x * dtMs / 1000.0;
        y += p.vel.y * dtMs / 1000.0;
    }
};

static LocoOutput StepAnim(LocomotionController& lc, AnimPed& p, const Pose& pose, double dtMs, bool noAnchor = false,
                           bool forceStill = false) {
    const LocoVec av = p.AnimVel();
    LocoInput in;
    in.pos = pose.pos; in.vel = pose.vel; in.heading = pose.heading;
    in.pedPos = p.pos; in.pedHeading = p.heading; in.dtMs = dtMs;
    in.pedVel = av; in.havePedVel = true;
    const double as = std::hypot(av.x, av.y);
    in.pedGait = forceStill ? Gait::Still : as < 0.5 ? Gait::Still : as < 2.65 ? Gait::Walk : as < 5.75 ? Gait::Run : Gait::Sprint;
    in.noAnchor = noAnchor;
    const LocoOutput o = lc.Update(in);
    if (o.snap) p.pos = o.snapPos;
    if (o.setHeading) p.heading = o.heading;
    if (o.task == LocoTask::GoStraight) { p.task = 1; p.target = o.target; p.blend = o.moveBlendRatio; p.lost = false; }
    else if (o.task == LocoTask::GoToAnchor) { p.task = 2; p.blend = o.moveBlendRatio; p.lost = false; }
    else if (o.task == LocoTask::StandStill) p.task = 0;
    if (o.setBlend) p.blend = o.moveBlendRatio;
    if (o.moveAnchor) p.anchor = o.anchorPos;
    if (o.setMoveRate) p.rate = o.moveRate;
    LocoVec v = p.AnimVel();
    if (o.setVelocity) v = o.velocity;
    p.pos.x = float(p.pos.x + v.x * dtMs / 1000.0);
    p.pos.y = float(p.pos.y + v.y * dtMs / 1000.0);
    p.pos.z = 30;
    return o;
}

struct W11Run {
    LocoStats st;
    size_t frames = 0, setBlendFrames = 0, anchorFrames = 0, taskFrames = 0;
    double maxErr = 0, errSum = 0, maxPush = 0;
    int gaitChanges = 0;
    double firstIssueAfterMs = -1, lastIssueMs = -1;
    double anchorLeadMin = 1e9, anchorLeadMax = 0;
};

// Runs `path` for durMs on a 60 fps clock against an AnimPed. `cfgMode`: the controller's mode.
static W11Run RunW11(Path path, double durMs, LocoMode mode, double fps = 60.0, double errFromMs = 1500) {
    LocoConfig cfg;
    cfg.mode = mode;
    LocomotionController lc(cfg);
    AnimPed ped;
    W11Run r;
    const double dt = 1000.0 / fps;
    Gait last = Gait::Still;
    for (double t = 0; t < durMs; t += dt, ++r.frames) {
        const Pose pose = path.At(t / 1000.0);
        const LocoOutput o = StepAnim(lc, ped, pose, dt);
        if (o.task == LocoTask::GoStraight || o.task == LocoTask::GoToAnchor) { ++r.taskFrames; r.lastIssueMs = t; }
        if (o.setBlend) ++r.setBlendFrames;
        if (o.moveAnchor) {
            ++r.anchorFrames;
            const double lead = std::hypot(double(o.anchorPos.x) - pose.pos.x, double(o.anchorPos.y) - pose.pos.y);
            r.anchorLeadMin = std::min(r.anchorLeadMin, lead);
            r.anchorLeadMax = std::max(r.anchorLeadMax, lead);
        }
        if (r.frames > 0 && o.gait != last) ++r.gaitChanges;
        last = o.gait;
        const double err = HDist(ped.pos, pose.pos);
        if (t >= errFromMs) { r.maxErr = std::max(r.maxErr, err); r.errSum += err; }
        path.Advance(t / 1000.0, dt);
    }
    r.st = lc.Stats();
    return r;
}

static void TestW11() {
    // ---- item 1: a gait change never re-issues the task ----
    // 10 s straight along +x, 4.5 <-> 7 m/s every 2 s (Run <-> Sprint), Straight mode (the fallback path).
    {
        Path p;
        p.speed = [](double t) { return int(t / 2) % 2 == 0 ? 4.5 : 7.0; };
        p.dirDeg = [](double) { return 0.0; };
        const W11Run r = RunW11(p, 10000, LocoMode::Straight);
        std::printf("  [item 1, Run<->Sprint every 2 s, straight] go-straight %u (gait %u, kept %u, arrive %u, turn %u, drift %u), setBlend frames %zu, gait changes %d\n",
                    r.st.goStraight, r.st.whyGait, r.st.gaitKept, r.st.whyArrive, r.st.whyTurn, r.st.whyDrift, r.setBlendFrames, r.gaitChanges);
        Check("W11 item 1: Run <-> Sprint every 2 s for 10 s: the gait changes (>= 4) are carried by setBlend on the running task, none is a re-issue (no 'first of a gait', drift or turn re-issue; only arrivals)",
              r.frames >= 599 && r.gaitChanges >= 4 && r.st.gaitKept >= 4 && r.st.whyGait == 0 && r.st.whyDrift == 0 && r.st.whyTurn == 0 && r.setBlendFrames >= 4,
              F("gait changes %d, kept %u, first-of-a-gait issues %u, drift %u, turn %u, setBlend frames %zu", r.gaitChanges, r.st.gaitKept, r.st.whyGait, r.st.whyDrift, r.st.whyTurn, r.setBlendFrames));
    }
    {
        // The blend ratio asked for is the commanded gait's, and it is asked for until it matches the task's (the test ped's task
        // speed follows setBlend: after the Sprint is shown the ped runs 7 m/s again, error small).
        LocoConfig cfg;
        LocomotionController lc(cfg);
        AnimPed ped;
        Path p;
        p.speed = [](double t) { return t < 1.0 ? 4.5 : 7.0; };
        p.dirDeg = [](double) { return 0.0; };
        float blendAt3s = -1;
        bool setBlendWhileRun = false;
        for (double t = 0; t < 2000; t += 1000.0 / 60) {
            const Pose pose = p.At(t / 1000.0);
            const LocoOutput o = StepAnim(lc, ped, pose, 1000.0 / 60);
            if (o.setBlend && o.gait == Gait::Run) setBlendWhileRun = true;
            if (o.setBlend && o.gait == Gait::Sprint) blendAt3s = o.moveBlendRatio;
            p.Advance(t / 1000.0, 1000.0 / 60);
        }
        Check("W11 item 1: after Run -> Sprint the running task gets SET_PED_DESIRED_MOVE_BLEND_RATIO 3 (setBlend, ratio = the gait), and none while the gait still matches the task",
              std::fabs(blendAt3s - 3.0f) < 1e-4 && !setBlendWhileRun && lc.Stats().goStraight == 1, F("blend %.2f, run setBlend %d, issues %u", blendAt3s, int(setBlendWhileRun), lc.Stats().goStraight));
    }
    // Dead zone: 2.6 +- 0.4 m/s at 0.7 Hz (inside the 2.1 .. 3.1 band, over the old 2.4 / 2.9 edges for 0.32 s each), 10 s.
    {
        Path p;
        p.speed = [](double t) { return 2.6 + 0.4 * std::sin(2 * kPi * 0.7 * t); };
        p.dirDeg = [](double) { return 0.0; };
        const W11Run r = RunW11(p, 10000, LocoMode::Straight);
        Path q;
        q.speed = [](double t) { return 5.7 + 0.35 * std::sin(2 * kPi * 0.7 * t); };
        q.dirDeg = [](double) { return 0.0; };
        const W11Run r2 = RunW11(q, 10000, LocoMode::Straight);
        Check("W11 item 1: 2.6 +- 0.4 m/s and 5.7 +- 0.35 m/s at 0.7 Hz (inside the new walk/run and run/sprint dead zones) change gait at most once in 10 s (old edges flipped)",
              r.gaitChanges <= 1 && r2.gaitChanges <= 1 && r.frames >= 599, F("walk/run %d changes, run/sprint %d changes", r.gaitChanges, r2.gaitChanges));
    }
    // Hold: 300 ms dips of a run to walking speed never change the shown gait; 1 s dips do.
    {
        Path p;
        p.speed = [](double t) { return std::fmod(t, 2.0) < 0.3 && t > 1.0 ? 1.5 : 4.5; };
        p.dirDeg = [](double) { return 0.0; };
        const W11Run shortDip = RunW11(p, 10000, LocoMode::Straight);
        Path q;
        q.speed = [](double t) { return std::fmod(t, 3.0) < 1.0 && t > 1.0 ? 1.5 : 4.5; };
        q.dirDeg = [](double) { return 0.0; };
        const W11Run longDip = RunW11(q, 10000, LocoMode::Straight);
        Check("W11 item 1: a gait must hold ~400 ms: 300 ms dips of a run to 1.5 m/s (5 of them) change nothing, 1 s dips (2 of them) change the gait both ways",
              shortDip.gaitChanges == 0 && longDip.gaitChanges >= 4, F("300 ms dips: %d changes, 1 s dips: %d", shortDip.gaitChanges, longDip.gaitChanges));
    }

    // ---- item 2: the anchor ----
    // 12 s run along a path with gait changes (Run <-> Sprint every 3 s) and gentle turns (40 degrees over 4 s, then back).
    auto mixed = []() {
        Path p;
        p.speed = [](double t) { return int(t / 3) % 2 == 0 ? 4.5 : 7.0; };
        p.dirDeg = [](double t) { return t < 1 ? 0.0 : t < 5 ? (t - 1) * 10.0 : t < 7 ? 40.0 : t < 11 ? 40.0 - (t - 7) * 10.0 : 0.0; };
        return p;
    };
    {
        const W11Run a = RunW11(mixed(), 12000, LocoMode::Anchor);
        const W11Run s = RunW11(mixed(), 12000, LocoMode::Straight);
        std::printf("  [item 2, mixed run 12 s] anchor mode: %u tasks (anchor %u), max err %.2f m, lead %.1f..%.1f m; straight mode: %u tasks (turn %u, arrive %u, drift %u, gait %u)\n",
                    a.st.goStraight, a.st.anchorIssues, a.maxErr, a.anchorLeadMin, a.anchorLeadMax, s.st.goStraight, s.st.whyTurn, s.st.whyArrive, s.st.whyDrift, s.st.whyGait);
        Check("W11 item 2/4: a run/sprint with gait changes and 40 degree turns for 12 s issues exactly 1 task (one TASK_GO_TO_ENTITY) in anchor mode; the straight path needs >= 4",
              a.frames >= 719 && a.st.goStraight == 1 && a.st.anchorIssues == 1 && s.st.goStraight >= 4, F("anchor %u tasks, straight %u tasks", a.st.goStraight, s.st.goStraight));
        Check("W11 item 2: the anchor is moved every frame, 1.5..5 m ahead of the pose (0.8 s of travel), and the ped that follows it stays within 1 m of the pose after 1.5 s",
              a.anchorFrames == a.frames && a.anchorLeadMin >= 1.5 - 1e-3 && a.anchorLeadMax <= 5.0 + 1e-3 && a.maxErr <= 1.0 && a.st.snaps == 1,
              F("anchor frames %zu of %zu, lead %.2f..%.2f, max err %.3f, snaps %u", a.anchorFrames, a.frames, a.anchorLeadMin, a.anchorLeadMax, a.maxErr, a.st.snaps));
    }
    {
        // Straight mode with the anchor config absent behaves as before: GoStraight tasks, no anchor output.
        LocomotionController lc;
        AnimPed ped;
        Path p = mixed();
        bool anyAnchor = false, anyStraight = false;
        for (double t = 0; t < 3000; t += 1000.0 / 60) {
            const LocoOutput o = StepAnim(lc, ped, p.At(t / 1000.0), 1000.0 / 60);
            anyAnchor = anyAnchor || o.moveAnchor || o.anchorMode || o.task == LocoTask::GoToAnchor;
            anyStraight = anyStraight || o.task == LocoTask::GoStraight;
            p.Advance(t / 1000.0, 1000.0 / 60);
        }
        Check("W11 item 2: the default mode is Straight (loco_mode=straight): TASK_GO_STRAIGHT_TO_COORD only, no anchor output", !anyAnchor && anyStraight);
    }
    {
        // noAnchor (entity missing, aim task): that frame acts Straight, the task kind changes (one issue), and back to the anchor (one issue).
        LocoConfig cfg;
        cfg.mode = LocoMode::Anchor;
        LocomotionController lc(cfg);
        AnimPed ped;
        Path p;
        p.speed = [](double) { return 4.5; };
        p.dirDeg = [](double) { return 0.0; };
        std::vector<LocoTask> issued;
        for (double t = 0; t < 4000; t += 1000.0 / 60) {
            const LocoOutput o = StepAnim(lc, ped, p.At(t / 1000.0), 1000.0 / 60, t >= 1500 && t < 2500);
            if (o.task != LocoTask::None) issued.push_back(o.task);
            p.Advance(t / 1000.0, 1000.0 / 60);
        }
        Check("W11 item 2: noAnchor for 1 s of a run: anchor task, then one go-straight, then one anchor task again (3 issues)",
              issued.size() == 3 && issued[0] == LocoTask::GoToAnchor && issued[1] == LocoTask::GoStraight && issued[2] == LocoTask::GoToAnchor,
              F("%zu issues", issued.size()));
    }
    {
        // Still: one stand-still, the anchor parked 2 m ahead of the pose along its heading; moving again: one anchor task.
        LocoConfig cfg;
        cfg.mode = LocoMode::Anchor;
        LocomotionController lc(cfg);
        AnimPed ped;
        std::vector<LocoTask> issued;
        bool parkedNear = true;
        for (double t = 0; t < 4000; t += 1000.0 / 60) {
            const bool moving = t < 1000 || t >= 3000;
            Pose pose;
            pose.pos = V(t < 1000 ? t / 1000.0 * 4.5 : t < 3000 ? 4.5 : 4.5 + (t - 3000) / 1000.0 * 4.5, 0, 30);
            pose.vel = V(moving ? 4.5 : 0, 0, 0);
            pose.heading = 270; // GTA heading 270 = +x
            const LocoOutput o = StepAnim(lc, ped, pose, 1000.0 / 60);
            if (o.task != LocoTask::None) issued.push_back(o.task);
            if (!moving && t > 1500) parkedNear = parkedNear && o.moveAnchor && std::hypot(o.anchorPos.x - pose.pos.x, o.anchorPos.y - pose.pos.y) <= 1.5 + 1e-3;
        }
        Check("W11 item 2: a stop is one stand-still (the anchor parks within 1.5 m of the pose), moving again is one anchor task: anchor, stand-still, anchor",
              issued.size() == 3 && issued[0] == LocoTask::GoToAnchor && issued[1] == LocoTask::StandStill && issued[2] == LocoTask::GoToAnchor && parkedNear,
              F("%zu issues, parked near %d", issued.size(), int(parkedNear)));
    }
    {
        // A lost task (the ped reads Still for 500 ms+ while a gait is commanded): anchor mode re-issues ONCE as the straight task
        // (stall, stallFirstMs = 0.9 s after the issue), stays Straight for anchorFallbackMs (20 s), then goes back to the anchor task.
        LocoConfig cfg;
        cfg.mode = LocoMode::Anchor;
        LocomotionController lc(cfg);
        AnimPed ped;
        Path p;
        p.speed = [](double) { return 4.5; };
        p.dirDeg = [](double) { return 0.0; };
        struct Is { double t; LocoTask k; LocoWhy why; };
        std::vector<Is> is;
        double fallbackOffMs = -1;
        bool wasMode = false;
        for (double t = 0; t < 30000; t += 1000.0 / 60) {
            const LocoOutput o = StepAnim(lc, ped, p.At(t / 1000.0), 1000.0 / 60, false, t < 2800);
            if (o.task != LocoTask::None) is.push_back({t, o.task, o.why});
            if (!o.anchorMode && wasMode && fallbackOffMs < 0) fallbackOffMs = t;
            wasMode = o.anchorMode;
            p.Advance(t / 1000.0, 1000.0 / 60);
        }
        // the first anchor task after the fallback, and no anchor task in between (the straight path's own arrival re-issues are fine)
        double backT = -1;
        bool onlyStraightBetween = true;
        for (size_t i = 2; i < is.size() && backT < 0; ++i) {
            if (is[i].k == LocoTask::GoToAnchor) backT = is[i].t;
        }
        for (size_t i = 1; i < is.size(); ++i)
            if (is[i].k == LocoTask::GoToAnchor && is[i].t != backT) onlyStraightBetween = false;
        Check("W11 item 2: a lost anchor task (ped reads Still) is re-issued once as go-straight after ~0.9 s (cause stall; Sync A: was 2.5 s), 20 s of straight, then one anchor task again",
              is.size() >= 3 && is[0].k == LocoTask::GoToAnchor && is[1].k == LocoTask::GoStraight && is[1].why == LocoWhy::Stall &&
                  is[1].t >= 800 && is[1].t <= 1300 && backT >= is[1].t + 19900 && backT <= is[1].t + 20400 && onlyStraightBetween &&
                  lc.Stats().anchorFallbacks == 1,
              F("%zu issues: %.0f ms kind %d, %.0f ms kind %d why %d, back to anchor at %.0f ms (%d); fallbacks %u", is.size(), is.size() > 0 ? is[0].t : -1,
                is.size() > 0 ? int(is[0].k) : -1, is.size() > 1 ? is[1].t : -1, is.size() > 1 ? int(is[1].k) : -1, is.size() > 1 ? int(is[1].why) : -1,
                backT, 0, lc.Stats().anchorFallbacks));
        (void)fallbackOffMs;
    }

    // ---- item 3: residual velocity push ----
    {
        // A ped exactly on the pose with its animation at the pose speed: nothing is set at all (old logic: the pose velocity every frame).
        LocomotionController lc;
        AnimPed ped;
        Path p;
        p.speed = [](double) { return 4.5; };
        p.dirDeg = [](double) { return 0.0; };
        size_t setFrames = 0, frames = 0;
        for (double t = 0; t < 4000; t += 1000.0 / 60, ++frames) {
            const Pose pose = p.At(t / 1000.0);
            if (frames == 0) { ped.pos = pose.pos; }
            const LocoOutput o = StepAnim(lc, ped, pose, 1000.0 / 60);
            // The sim's anim speed is exactly 4.5 after the first task; ignore the first 0.5 s (task start)
            if (t > 500 && o.setVelocity) ++setFrames;
            p.Advance(t / 1000.0, 1000.0 / 60);
        }
        Check("W11 item 3: a ped whose animation already moves it at the pose speed is not pushed at all (no SET_ENTITY_VELOCITY inside the dead band)",
              setFrames == 0 && frames > 200, F("%zu velocity sets in %zu frames", setFrames, frames));
    }
    {
        // The push on top of pedVel: gain, low-pass and cap. Ped 0.5 m / 3 m / 10 m behind a pose running at 4.5 m/s, its own velocity 4.5.
        struct C { double behind; };
        const C cs[] = {{0.4}, {3.0}};
        double firstFrame[2] = {}, settled[2] = {};
        bool snapped[2] = {};
        for (int i = 0; i < 2; ++i) {
            LocoConfig cfg;
            cfg.residualDeadM = 0; cfg.startGraceMs = 0;   // Sync A added a dead band and a start grace: this checks the plain gain law, low-pass and cap
            LocomotionController lc(cfg);
            LocoInput in;
            in.vel = V(4.5f, 0, 0); in.heading = 270;
            in.pedVel = V(4.5f, 0, 0); in.havePedVel = true;
            in.pedGait = Gait::Run;
            in.dtMs = 16.667;
            in.pos = V(100, 0, 30); in.pedPos = V(100, 0, 30);
            lc.Update(in); // first frame snaps (baseline)
            in.pedPos = V(float(100 - cs[i].behind), 0, 30);
            for (int f = 0; f < 90; ++f) {
                const LocoOutput o = lc.Update(in);
                if (o.snap) snapped[i] = true;
                const double push = o.setVelocity ? o.velocity.x - 4.5 : 0.0;
                if (f == 0) firstFrame[i] = push;
                settled[i] = push;
            }
        }
        Check("W11 item 3: behind the pose by 0.4 m the push on top of the ped's own velocity is low-passed (first frame < 40% of the 1.6 m/s target) and reaches it; at 3 m it is capped at 2 m/s; no snap",
              firstFrame[0] > 0 && firstFrame[0] < 0.4 * 1.6 && std::fabs(settled[0] - 1.6) < 0.05 && std::fabs(settled[1] - 2.0) < 0.02 && !snapped[0] && !snapped[1],
              F("0.4 m: first %.3f settled %.3f; 3 m: settled %.3f", firstFrame[0], settled[0], settled[1]));
    }
    {
        // A hard snap only beyond the teleport threshold (5 + 5|v| = 27.5 m at 4.5 m/s): 20 m behind is pushed (capped), 30 m snaps.
        int snaps[2] = {};
        const double behind[2] = {20.0, 30.0};
        for (int i = 0; i < 2; ++i) {
            LocomotionController lc;
            LocoInput in;
            in.vel = V(4.5f, 0, 0); in.heading = 270; in.pedVel = V(4.5f, 0, 0); in.havePedVel = true; in.pedGait = Gait::Run; in.dtMs = 16.667;
            in.pos = V(100, 0, 30); in.pedPos = V(100, 0, 30);
            lc.Update(in);
            in.pedPos = V(float(100 - behind[i]), 0, 30);
            const LocoOutput o = lc.Update(in);
            snaps[i] = o.snap ? 1 : 0;
        }
        Check("W11 item 3: the hard snap stays at the teleport threshold (27.5 m at 4.5 m/s): 20 m behind is pushed, 30 m snaps", snaps[0] == 0 && snaps[1] == 1);
    }
    {
        // Closed loop: an animation 10% slower than the pose (4.05 vs 4.5 m/s: rate clamp / blend rounding) for 30 s. The residual push
        // carries the difference: bounded error, no snap.
        LocomotionController lc;
        AnimPed ped;
        Path p;
        p.speed = [](double) { return 4.5; };
        p.dirDeg = [](double) { return 0.0; };
        ped.scale = 0.9;
        double maxErr = 0;
        for (double t = 0; t < 30000; t += 1000.0 / 60) {
            const Pose pose = p.At(t / 1000.0);
            StepAnim(lc, ped, pose, 1000.0 / 60);
            if (t > 3000) maxErr = std::max(maxErr, HDist(ped.pos, pose.pos));
            p.Advance(t / 1000.0, 1000.0 / 60);
        }
        Check("W11 item 3: an animation 10% slower than the pose for 30 s is carried by the residual push: error <= 0.15 m ((2 + 5e) e = 0.45 m/s gives 0.086), one snap (the first frame), no persistent-error snap",
              maxErr <= 0.15 && maxErr >= 0.05 && lc.Stats().snaps == 1 && lc.Stats().persistentSnaps == 0,
              F("max err %.3f m, snaps %u, tasks %u", maxErr, lc.Stats().snaps, lc.Stats().goStraight));
    }
}

// ---- 10: Sync A (docs/sync-specs/onfoot.md "Sync A"): the slide metric against a more realistic simulated ped ---------------
//
// RealPed is the AnimPed's richer sibling. What it models: an animation speed that follows the commanded one (blend ratio x
// move rate x the ped's TRUE nominal speeds, which the controller only guesses) with a first-order lag (a start takes ~0.25 s
// to get going, a stop ~0.18 s), a stride ripple on the displacement, the ped moving along its FACING with a limited turn rate
// (so a turn bends the path late), go-to arrival (it stops within 0.1 m of the anchor / 0.25 m of a coordinate), a move rate
// that only holds while it is set every frame, IS_PED_* gait readings from the real speed, ground height (a ramp and a curb) and
// two readings of GET_ENTITY_VELOCITY after we set it: the animation's own velocity (the W11 assumption) or the velocity we set
// (it persists: the push would then be fed back). Not modelled: the real GTA animation set (foot plants, start / stop / turn
// transition clips), navmesh pathing, collision, how SET_ENTITY_VELOCITY really combines with the animation, the strafing and
// aim blends, and the real smoother (PoseFeed below is a plain 30 Hz sample interpolator with a fixed delay, jitter and loss).
struct RealPed {
    LocoVec pos = V(0, 0, 30);
    float facing = 0;                 // GTA heading, degrees (0 = +y, counter-clockwise)
    double speed = 0;                 // the animation's current speed (scalar, before the ripple)
    int task = 0;                     // 0 none / stand-still, 1 go-straight coordinate, 2 go-to-entity
    LocoVec target, anchor;
    float blend = 0, rate = 1;
    bool lost = false;
    // the TRUE nominal speeds at blend 1, 2, 3 and rate 1 (the controller assumes 1.5 / 4.5 / 7)
    double nomWalk = 1.5, nomRun = 4.5, nomSprint = 7.0;
    double accelTauMs = 250, decelTauMs = 180, ripple = 0.10, rippleHz = 1.9, turnDps = 300;
    bool persist = false;             // GET_ENTITY_VELOCITY returns the velocity we last set (else the animation's own)
    bool anchorDead = false;          // TASK_GO_TO_ENTITY never runs here: the ped stands (reads Still) while it is the task
    bool additive = false;            // SET_ENTITY_VELOCITY ADDS to the animation (displacement = animation + set) instead of replacing it
    LocoVec lastSet; bool hasLastSet = false;
    LocoVec animVel;                  // the animation's velocity this frame (what a read returns unless persist)
    double t = 0;

    double AnimSpeed(double b) const {
        if (b <= 0) return 0;
        if (b <= 1) return nomWalk * b;
        if (b <= 2) return nomWalk + (nomRun - nomWalk) * (b - 1);
        return nomRun + (nomSprint - nomRun) * (std::min(b, 3.0) - 2);
    }
    Gait GameGait() const {
        const double s = std::hypot(animVel.x, animVel.y);
        return s < 0.5 ? Gait::Still : s < 2.65 ? Gait::Walk : s < 5.75 ? Gait::Run : Gait::Sprint;
    }
    LocoVec Readback() const { return persist && hasLastSet ? lastSet : animVel; }
    // advance the animation state by dtMs; fills animVel (the displacement the animation produces this frame)
    void Anim(double dtMs) {
        double vd = 0;
        double arriveR = task == 2 ? 0.1 : 0.25;
        const LocoVec& tg = task == 2 ? anchor : target;
        const double dx = double(tg.x) - pos.x, dy = double(tg.y) - pos.y, d = std::hypot(dx, dy);
        if (task != 0 && !lost) {
            if (d < arriveR) { if (task == 1) lost = true; }
            else vd = (task == 2 && anchorDead) ? 0.0 : AnimSpeed(blend) * rate;
            // turn toward the bearing, limited rate
            const double want = std::atan2(-dx, dy) * 180.0 / kPi; // GTA heading of the bearing
            const double diff = WrapDeg(want - facing);
            const double maxTurn = turnDps * dtMs / 1000.0;
            facing = float(WrapDeg(facing + std::max(-maxTurn, std::min(maxTurn, diff))));
        }
        const double k = 1.0 - std::exp(-dtMs / (vd > speed ? accelTauMs : decelTauMs));
        speed += (vd - speed) * k;
        if (speed < 0.02) speed = 0;
        t += dtMs / 1000.0;
        const double rip = 1.0 + ripple * std::min(1.0, speed / 2.0) * std::sin(2 * kPi * rippleHz * t);
        const double a = double(facing) * kPi / 180.0;
        animVel = V(-std::sin(a) * speed * rip, std::cos(a) * speed * rip, 0);
    }
};

// A scripted remote: position integrated from speedAt / dirAt at 1 ms, sampled at 30 Hz (+- 3 ms), uplink U(0, 10) ms, loss.
struct SlideScenario {
    const char* name;
    std::function<double(double)> speed, dirDeg;   // m/s, degrees (math convention 0 = +x)
    double durMs = 20000;
    double teleportAtMs = -1;                      // a 30 m jump along +y here
    bool slopes = false;                           // ground = 0.25 * x, plus a 0.7 m curb at x = 25
    bool reverse = false;                          // nothing: reserved
};

struct SlideOpts {
    double scaleWalk = 1.0, scaleRun = 1.0, scaleSprint = 1.0;   // the ped's TRUE nominal speeds against the controller's guess
    bool persist = false, anchorDead = false, additive = false;
    double loss = 0.02, jitterMs = 10.0, delayMs = 130.0, fps = 60.0;
    LocoMode mode = LocoMode::Anchor;
    uint64_t seed = 7;
    LocoConfig cfg;
    double traceFromMs = -1, traceToMs = -1; // debugging: print one line per frame in this window
};

struct SlideResult {
    LocoStats st;
    LocoDiagSummary d;
    double p95Err = 0, maxErr = 0, tasksPer10s = 0, speedRatio = 0, zPushFrames = 0;
    uint32_t snaps = 0;
    float learned[3] = {1, 1, 1};   // the controller's learned nominal factors: walk, run, sprint
};

static double Ground(const SlideScenario& sc, double x) {
    return !sc.slopes ? 30.0 : 30.0 + 0.25 * x + (x > 25 ? 0.7 : 0.0);
}

static SlideResult RunSlide(const SlideScenario& sc, const SlideOpts& op) {
    Rng rng(op.seed);
    LocoConfig cfg = op.cfg;
    cfg.mode = op.mode;
    LocomotionController lc(cfg);
    LocoDiag diag;
    RealPed ped;
    ped.nomWalk = 1.5 * op.scaleWalk; ped.nomRun = 4.5 * op.scaleRun; ped.nomSprint = 7.0 * op.scaleSprint;
    ped.persist = op.persist;
    ped.anchorDead = op.anchorDead;
    ped.additive = op.additive;
    struct Smp { double t, arr; LocoVec pos, vel; float heading; bool tele; };
    std::vector<Smp> smp;
    double tx = 0, ty = 0, yOff = 0, nextSmp = 0, tele = 0;
    double tSub = 0;
    SlideResult res;
    std::vector<double> errs;
    double speedRatioSum = 0; int speedRatioN = 0;
    bool teleDone = false;
    const double dtBase = 1000.0 / op.fps;
    bool first = true;
    double now = 0, lastNow = 0;
    while (now < sc.durMs) {
        const double dt = first ? dtBase : now - lastNow;
        // truth + sampling up to `now`
        while (tSub < now) {
            const double a = sc.dirDeg(tSub / 1000.0) * kPi / 180.0, sp = sc.speed(tSub / 1000.0);
            tx += std::cos(a) * sp * 0.001; ty += std::sin(a) * sp * 0.001;
            if (!teleDone && sc.teleportAtMs >= 0 && tSub >= sc.teleportAtMs) { yOff += 30.0; teleDone = true; tele = 1; }
            tSub += 1.0;
            if (tSub >= nextSmp) {
                Smp s;
                s.t = tSub;
                s.arr = tSub + rng.Uniform() * op.jitterMs + 5.0;
                const double rip = 1.0 + 0.10 * std::min(1.0, sp / 2.0) * std::sin(2 * kPi * 1.9 * tSub / 1000.0 + 1.0);
                s.pos = V(tx, ty + yOff, Ground(sc, tx));
                s.vel = V(std::cos(a) * sp * rip, std::sin(a) * sp * rip, 0);
                s.heading = float(WrapDeg(std::atan2(-std::cos(a), std::sin(a)) * 180.0 / kPi));
                s.tele = tele > 0; tele = 0;
                if (rng.Uniform() >= op.loss || s.tele) smp.push_back(s);
                nextSmp += 1000.0 / 30.0 + (rng.Uniform() - 0.5) * 6.0;
            }
        }
        // pose at renderT from the samples that have arrived
        const double renderT = now - op.delayMs;
        LocoVec ppos, pvel; float phead = 0; bool ptele = false;
        {
            int a = -1;
            for (int i = int(smp.size()) - 1; i >= 0; --i) if (smp[i].arr <= now) { a = i; break; }
            if (a < 0) { now += dtBase; lastNow = now - dtBase; first = false; continue; }
            // newest arrived sample at or before renderT, and the next one if it has arrived
            int lo = -1;
            for (int i = a; i >= 0; --i) if (smp[i].t <= renderT) { lo = i; break; }
            if (lo < 0) lo = 0;
            const Smp& A = smp[lo];
            if (lo + 1 <= a && smp[lo + 1].t > A.t && !smp[lo + 1].tele) {
                const Smp& B = smp[lo + 1];
                const double f = std::max(0.0, std::min(1.0, (renderT - A.t) / (B.t - A.t)));
                ppos = V(A.pos.x + (B.pos.x - A.pos.x) * f, A.pos.y + (B.pos.y - A.pos.y) * f, A.pos.z + (B.pos.z - A.pos.z) * f);
                pvel = V(A.vel.x + (B.vel.x - A.vel.x) * f, A.vel.y + (B.vel.y - A.vel.y) * f, 0);
                phead = float(WrapDeg(A.heading + WrapDeg(B.heading - A.heading) * f));
            } else {
                const double ex = std::min(250.0, std::max(0.0, renderT - A.t)) / 1000.0; // extrapolate <= 250 ms, then hold
                ppos = V(A.pos.x + A.vel.x * ex, A.pos.y + A.vel.y * ex, A.pos.z);
                pvel = A.vel; phead = A.heading;
            }
            ptele = A.tele && (renderT - A.t) < dtBase * 1.5 && renderT >= A.t;
        }
        const double poseSp = std::hypot(pvel.x, pvel.y);
        // controller input (as DrivePeer builds it)
        LocoInput in;
        in.pos = ppos; in.vel = pvel; in.heading = phead; in.teleported = ptele;
        ped.pos.z = float(Ground(sc, ped.pos.x));
        in.pedPos = ped.pos; in.pedHeading = ped.facing; in.pedGait = ped.GameGait();
        in.dtMs = dt;
        in.moveState = int8_t(poseSp < 0.5 ? 0 : poseSp < 3.1 ? 1 : poseSp < 6.1 ? 2 : 3);
        const LocoVec rb = ped.Readback();
        in.pedVel = rb; in.havePedVel = true;
        in.noAnchor = false;
        const LocoOutput o = lc.Update(in);
        // apply as the engine does
        if (o.snap) { ped.pos = o.snapPos; ped.facing = o.heading; ped.speed = 0; ped.lost = false; }
        if (o.setHeading && !o.snap && o.gait == Gait::Still && ped.task == 0) ped.facing = o.heading;
        if (o.moveAnchor) ped.anchor = o.anchorPos;
        if (o.task == LocoTask::GoStraight) { ped.task = 1; ped.target = o.target; ped.blend = o.moveBlendRatio; ped.lost = false; }
        else if (o.task == LocoTask::GoToAnchor) { ped.task = 2; ped.blend = o.moveBlendRatio; ped.lost = false; }
        else if (o.task == LocoTask::StandStill) { ped.task = 0; }
        if (o.setBlend) ped.blend = o.moveBlendRatio;
        ped.rate = o.setMoveRate ? o.moveRate : 1.0f;
        ped.Anim(dt);
        LocoVec v = ped.animVel;
        if (o.setVelocity) {
            if (ped.additive) { v.x += o.velocity.x; v.y += o.velocity.y; }
            else { v.x = o.velocity.x; v.y = o.velocity.y; }
            ped.lastSet = o.velocity; ped.hasLastSet = true;
        }
        else ped.hasLastSet = false;
        // diagnostics from the same inputs the engine has: positions as read at the top of the frame
        LocoDiagFrame df;
        df.dtMs = dt; df.nowMs = now; df.pose = ppos; df.pedPos = in.pedPos; df.poseSpeed = float(poseSp); df.gait = o.gait;
        df.pushed = o.setVelocity; df.push = o.velocity; df.pedVel = rb; df.havePedVel = true;
        df.anchorMode = o.anchorMode; df.anchor = o.anchorPos;
        diag.Observe(df);
        if (o.setVelocity && std::fabs(double(ppos.z) - in.pedPos.z) > 0.5) res.zPushFrames += 1;
        if (op.traceFromMs >= 0 && now >= op.traceFromMs && now < op.traceToMs)
            std::printf("    t=%6.0f poseSp %.2f pedSp %.2f anim %.2f gait %d task %d why %d push %d (%.2f,%.2f) err %.2f rate %.2f blend %.1f\n", now, poseSp,
                        std::hypot(v.x, v.y), std::hypot(ped.animVel.x, ped.animVel.y), int(o.gait), int(o.task), int(o.why), int(o.setVelocity),
                        o.velocity.x - ped.animVel.x, o.velocity.y - ped.animVel.y, std::hypot(double(ppos.x) - in.pedPos.x, double(ppos.y) - in.pedPos.y),
                        ped.rate, ped.blend);
        ped.pos.x = float(ped.pos.x + v.x * dt / 1000.0);
        ped.pos.y = float(ped.pos.y + v.y * dt / 1000.0);
        if (now >= 2000) {
            errs.push_back(std::hypot(double(ppos.x) - in.pedPos.x, double(ppos.y) - in.pedPos.y));
            if (poseSp > 1.0) { speedRatioSum += std::hypot(v.x, v.y) / poseSp; ++speedRatioN; }
        }
        first = false;
        lastNow = now;
        now += dtBase * (0.8 + 0.4 * rng.Uniform()); // render frame time jitter +-20%
    }
    res.st = lc.Stats();
    res.d = diag.Summary();
    std::sort(errs.begin(), errs.end());
    if (!errs.empty()) { res.p95Err = errs[size_t(errs.size() * 0.95)]; res.maxErr = errs.back(); }
    res.tasksPer10s = double(res.st.goStraight + res.st.standStill) * 10000.0 / sc.durMs;
    res.speedRatio = speedRatioN ? speedRatioSum / speedRatioN : 0;
    res.snaps = res.st.snaps;
    res.learned[0] = lc.NominalScale(Gait::Walk); res.learned[1] = lc.NominalScale(Gait::Run); res.learned[2] = lc.NominalScale(Gait::Sprint);
    return res;
}

static std::vector<SlideScenario> SlideScenarios() {
    std::vector<SlideScenario> v;
    auto C = [](double s) { return [s](double) { return s; }; };
    auto D0 = [](double) { return 0.0; };
    v.push_back({"walk 1.5 straight", C(1.5), D0});
    v.push_back({"run 4.5 straight", C(4.5), D0});
    v.push_back({"sprint 7 straight", C(7.0), D0});
    v.push_back({"gait changes walk-run-sprint-run-walk", [](double t) {
        const double k = std::fmod(t, 15.0);
        return k < 3 ? 1.5 : k < 4 ? 1.5 + (4.5 - 1.5) * (k - 3) : k < 7 ? 4.5 : k < 8 ? 4.5 + 2.5 * (k - 7) : k < 10 ? 7.0 : k < 11 ? 7.0 - 2.5 * (k - 10) : k < 13 ? 4.5 : k < 14 ? 4.5 - 3.0 * (k - 13) : 1.5;
    }, D0});
    v.push_back({"circle R=15 at 4.5", C(4.5), [](double t) { return t * (4.5 / 15.0) * 180.0 / kPi; }});
    v.push_back({"gentle S-curve at 4.5", C(4.5), [](double t) { return 25.0 * std::sin(2 * kPi * t / 8.0); }});
    v.push_back({"run 3 s / stop 2 s", [](double t) { return std::fmod(t, 5.0) < 3.0 ? 4.5 : 0.0; }, D0});
    v.push_back({"walk 2 s / stop 2 s", [](double t) { return std::fmod(t, 4.0) < 2.0 ? 1.5 : 0.0; }, D0});
    v.push_back({"reversal: 180 deg turn every 4 s (0.4 m/s at the turn)", [](double t) {
        const double d = std::fabs(t - 4.0 * std::floor(t / 4.0 + 0.5));   // distance to the nearest turn
        return t > 2.0 && d < 0.6 ? 0.4 + 4.1 * d / 0.6 : 4.5;
    }, [](double t) { return int((t + 2.0) / 4) % 2 ? 180.0 : 0.0; }});
    v.push_back({"run up a ramp and a curb", C(4.5), D0, 20000, -1, true});
    SlideScenario tp{"teleport 30 m mid-run", C(4.5), D0, 20000, 8000};
    v.push_back(tp);
    return v;
}

static void PrintSlideTable(const char* title, const SlideOpts& op) {
    std::printf("  [%s]\n  %-40s %6s %6s %6s %6s %6s %6s %6s %5s\n", title, "scenario", "tasks", "skate", "idle m", "stops", "p95err", "maxerr", "spd/ps", "snaps");
    for (const SlideScenario& sc : SlideScenarios()) {
        const SlideResult r = RunSlide(sc, op);
        std::printf("  %-40s %6.1f %6.3f %6.2f %6u %6.2f %6.2f %6.2f %5u\n", sc.name, r.tasksPer10s, r.d.skate, r.d.idleSlideM, r.d.stops, r.p95Err, r.maxErr, r.speedRatio, r.snaps);
    }
}

// A one-frame unit input: a run at 4.5 m/s along +x, the ped `behind` m behind the pose with its own velocity `pedSpeed`.
static LocoInput UnitIn(double behind, double pedSpeed, double dtMs = 1000.0 / 60) {
    LocoInput in;
    in.vel = V(4.5, 0, 0); in.heading = 270;
    in.pos = V(100, 0, 30); in.pedPos = V(100 - behind, 0, 30);
    in.pedVel = V(pedSpeed, 0, 0); in.havePedVel = true;
    in.pedHeading = 270;
    in.pedGait = Gait::Run;
    in.dtMs = dtMs;
    return in;
}

static void TestSyncA() {
    if (std::getenv("SLIDE_TRACE")) {
        SlideOpts t; t.traceFromMs = std::atof(std::getenv("SLIDE_TRACE")); t.traceToMs = t.traceFromMs + 1500;
        const std::vector<SlideScenario> sc = SlideScenarios();
        RunSlide(sc[std::getenv("SLIDE_SC") ? std::atoi(std::getenv("SLIDE_SC")) : 6], t);
        return;
    }
    SlideOpts o;
    PrintSlideTable("anchor, true nominals = assumed", o);
    SlideOpts o2; o2.scaleWalk = 0.85; o2.scaleRun = 0.85; o2.scaleSprint = 0.85;
    PrintSlideTable("anchor, ped 15% slower than assumed", o2);
    SlideOpts o3; o3.scaleWalk = 1.15; o3.scaleRun = 1.15; o3.scaleSprint = 1.15;
    PrintSlideTable("anchor, ped 15% faster than assumed", o3);
    SlideOpts o4 = o; o4.persist = true;
    PrintSlideTable("anchor, readback = last set velocity", o4);
    SlideOpts o5 = o; o5.mode = LocoMode::Straight;
    PrintSlideTable("straight, true nominals = assumed", o5);
    SlideOpts o7 = o; o7.additive = true;
    PrintSlideTable("anchor, SET_ENTITY_VELOCITY adds to the animation", o7);
    SlideOpts o6 = o; o6.anchorDead = true;
    PrintSlideTable("anchor task never runs (go-to-entity dead on this machine)", o6);

    auto sc = [](const char* name) {
        for (const SlideScenario& c : SlideScenarios())
            if (std::string(c.name) == name) return c;
        return SlideScenarios()[0];
    };
    // ---- the simulated ped: the numbers the next live log's "engine: loco slide" line carries ----
    {
        // A ped whose real run animation is 15% slower / faster than the controller's guess: the animation rate is a closed loop
        // with a learned nominal, so the stride carries the ped (old: the velocity push carried the difference, skate 14-16%).
        SlideOpts a = o; a.scaleRun = 0.85;
        SlideOpts b = o; b.scaleRun = 1.15;
        const SlideResult ra = RunSlide(sc("run 4.5 straight"), a), rb = RunSlide(sc("run 4.5 straight"), b);
        Check("Sync A sim: a run animation 15% slower / faster than guessed is carried by the stride (skate <= 2%, old 14-16%), the learned nominal converges (+-0.06), error p95 <= 0.35 m",
              ra.d.skate <= 0.02 && rb.d.skate <= 0.02 && std::fabs(ra.learned[1] - 0.85) <= 0.06 && std::fabs(rb.learned[1] - 1.15) <= 0.06 && ra.p95Err <= 0.35 && rb.p95Err <= 0.35,
              F("slower: skate %.3f learned %.2f p95 %.2f; faster: skate %.3f learned %.2f p95 %.2f", ra.d.skate, ra.learned[1], ra.p95Err, rb.d.skate, rb.learned[1], rb.p95Err));
    }
    {
        // Run 3 s, stand 2 s, four times: the ped is not dragged in an idle pose after each stop (old: 4.6-7.4 m of idle slide in 20 s, the
        // 1.5 m anchor lead ran the ped through the pose and the push then pulled it back ~1 s).
        const char* nm = "run 3 s / stop 2 s";
        double worst = 0, worstP95 = 0;
        for (double scale : {1.0, 0.85, 1.15}) {
            SlideOpts a = o; a.scaleRun = a.scaleWalk = a.scaleSprint = scale;
            const SlideResult r = RunSlide(sc(nm), a);
            worst = std::max(worst, r.d.idleSlideM);
            worstP95 = std::max(worstP95, r.p95Err);
        }
        Check("Sync A sim: 4 stops of a run (nominal x1, x0.85, x1.15): at most 1.5 m of idle slide in 20 s (old 4.6-7.4 m), error p95 <= 1.2 m",
              worst <= 1.5 && worstP95 <= 1.2, F("worst idle slide %.2f m, worst p95 %.2f m", worst, worstP95));
    }
    {
        // GET_ENTITY_VELOCITY returning the velocity we set last frame (it persists): the push must not be fed back. Old: the ped ran at
        // 2.0-2.7x the pose speed, 6 m off, a snap every second.
        SlideOpts a = o; a.persist = true;
        const SlideResult r1 = RunSlide(sc("run 4.5 straight"), a), r2 = RunSlide(sc("gait changes walk-run-sprint-run-walk"), a);
        Check("Sync A sim: with a velocity read that returns our own last push, the ped stays on the pose (speed ratio <= 1.1, <= 1 snap, error p95 <= 0.8 m; old 2.0-2.7x, 12-17 snaps)",
              r1.speedRatio <= 1.1 && r2.speedRatio <= 1.1 && r1.snaps <= 1 && r2.snaps <= 1 && r1.p95Err <= 0.8 && r2.p95Err <= 0.8,
              F("run: ratio %.2f snaps %u p95 %.2f; gait changes: ratio %.2f snaps %u p95 %.2f", r1.speedRatio, r1.snaps, r1.p95Err, r2.speedRatio, r2.snaps, r2.p95Err));
    }
    {
        // TASK_GO_TO_ENTITY never moving the ped (frozen invisible anchor unusable): found after ~0.9 s, no dragging in the meantime.
        SlideOpts a = o; a.anchorDead = true;
        const SlideResult r = RunSlide(sc("run 4.5 straight"), a);
        Check("Sync A sim: a dead go-to-entity task (the ped stands) costs at most 1 stop-start, 2 snaps, error p95 <= 1 m and skate <= 10% (old: p95 4.4 m, 12 snaps, skate 41%)",
              r.d.stops <= 1 && r.snaps <= 2 && r.p95Err <= 1.0 && r.d.skate <= 0.10,
              F("stops %u snaps %u p95 %.2f skate %.3f", r.d.stops, r.snaps, r.p95Err, r.d.skate));
    }
    {
        // Scoreboard over every scenario but the two that move the pose discontinuously (an instant reversal, a teleport): at the assumed
        // and +-15% animation speeds, skate <= 7%, error p95 <= 1.2 m, tasks <= 5.5 per 10 s.
        double worstSkate = 0, worstP95 = 0, worstTasks = 0;
        std::string worstAt;
        int runs = 0;
        for (double scale : {1.0, 0.85, 1.15}) {
            SlideOpts a = o; a.scaleRun = a.scaleWalk = a.scaleSprint = scale;
            for (const SlideScenario& c : SlideScenarios()) {
                if (std::string(c.name).find("reversal") != std::string::npos || std::string(c.name).find("teleport") != std::string::npos) continue;
                const SlideResult r = RunSlide(c, a);
                ++runs;
                if (r.d.skate > worstSkate) { worstSkate = r.d.skate; worstAt = c.name; }
                worstP95 = std::max(worstP95, r.p95Err);
                worstTasks = std::max(worstTasks, r.tasksPer10s);
            }
        }
        Check("Sync A sim: 9 scenarios x nominal x1 / x0.85 / x1.15 (27 runs): worst skate <= 7%, worst error p95 <= 1.2 m, at most 5.5 tasks per 10 s",
              runs == 27 && worstSkate <= 0.07 && worstP95 <= 1.2 && worstTasks <= 5.5,
              F("%d runs, worst skate %.3f (%s), worst p95 %.2f m, worst tasks %.1f / 10 s", runs, worstSkate, worstAt.c_str(), worstP95, worstTasks));
    }

    // ---- pure units ----
    {
        // Leaving Still: the pose speed ramps 0 -> 1 -> 4.5 m/s over two frames (the interpolation across the first moving sample): the
        // shown gait is Run within 100 ms, not Walk for 400 ms (old).
        LocomotionController lc;
        LocoInput in = UnitIn(0, 0);
        in.vel = V(0, 0, 0); in.pedGait = Gait::Still;
        lc.Update(in);
        for (int i = 0; i < 20; ++i) lc.Update(in);
        double runAt = -1, t = 0;
        const double speeds[] = {1.0, 1.0, 4.5};
        for (int i = 0; i < 40; ++i, t += 1000.0 / 60) {
            in.vel = V(i < 2 ? speeds[i] : 4.5, 0, 0);
            const LocoOutput o = lc.Update(in);
            if (o.gait == Gait::Run && runAt < 0) runAt = t;
        }
        Check("Sync A: leaving Still with a one-frame speed ramp, the shown gait is Run within 100 ms (old: stuck at Walk for the 400 ms debounce)",
              runAt >= 0 && runAt <= 100, F("Run shown at %.0f ms", runAt));
    }
    {
        // A sender-hint (moveState) lifted gait still holds the full debounce; only the SPEED's own gait is followed fast.
        LocomotionController lc;
        LocoInput in = UnitIn(0, 0);
        in.vel = V(1.5, 0, 0); in.pedGait = Gait::Walk; in.moveState = 1;
        for (int i = 0; i < 60; ++i) lc.Update(in);           // walking for 1 s
        in.moveState = 3;                                      // the hint says sprint at 1.5 m/s: capped to Run, and held 400 ms
        double firstRun = -1, t = 0;
        for (int i = 0; i < 60; ++i, t += 1000.0 / 60) {
            const LocoOutput o = lc.Update(in);
            if (o.gait == Gait::Run && firstRun < 0) firstRun = t;
        }
        Check("Sync A: a gait the sender's hint lifted above the speed's still holds the 400 ms debounce (Run shown at ~400 ms, not at once)",
              firstRun >= 350 && firstRun <= 480, F("Run shown at %.0f ms", firstRun));
    }
    {
        // Start grace: the ped is 1 m behind a pose that has just started to run; for 1 s no velocity push (the start animation is
        // still getting up to speed), after 1.2 s the persistent error is pushed again. Old: pushed from the first frame.
        LocomotionController lc;
        LocoInput in = UnitIn(0, 0);
        in.vel = V(0, 0, 0); in.pedGait = Gait::Still;
        lc.Update(in);
        for (int i = 0; i < 20; ++i) lc.Update(in);
        size_t early = 0, late = 0;
        for (int i = 0; i < 120; ++i) {
            in = UnitIn(1.0, 4.5);
            const LocoOutput o = lc.Update(in);
            if (i >= 3 && i < 60 && o.setVelocity) ++early;
            if (i >= 90 && o.setVelocity) ++late;
        }
        Check("Sync A: no velocity push in the first second after leaving Still while the ped is under 2.5 m behind; pushed again after 1.2 s (old: from the first frame)",
              early == 0 && late > 20 && lc.Stats().graceFrames > 40, F("push frames early %zu, late %zu, grace frames %u", early, late, lc.Stats().graceFrames));
    }
    {
        // Dead band: a ped 0.25 m behind a running pose (animation at the pose speed) is not pushed; 0.6 m behind: the push ramps up from the
        // band's edge, (2 + 5 x 0.3) x 0.3 = 1.05 m/s on top of its own velocity (old band 0.08 m: 0.25 m gave 0.8 m/s).
        double push[2] = {};
        size_t frames0 = 0;
        const double behind[2] = {0.25, 0.6};
        for (int k = 0; k < 2; ++k) {
            LocomotionController lc;
            lc.Update(UnitIn(0, 4.5));
            for (int i = 0; i < 150; ++i) lc.Update(UnitIn(0, 4.5));     // 2.5 s of running on the pose: grace over
            for (int i = 0; i < 90; ++i) {
                const LocoOutput o = lc.Update(UnitIn(behind[k], 4.5));
                if (k == 0 && o.setVelocity) ++frames0;
                if (o.setVelocity) push[k] = o.velocity.x - 4.5;
            }
        }
        Check("Sync A: a running ped 0.25 m behind the pose is not pushed (dead band 0.30 m); 0.6 m behind it is pushed by about 1.05 m/s on top of its own velocity",
              frames0 == 0 && push[0] == 0.0 && std::fabs(push[1] - 1.05) < 0.1, F("0.25 m: %zu push frames; 0.6 m: %.3f m/s", frames0, push[1]));
    }
    {
        // A velocity read that is our own last push (the game keeps what we set) is not the animation: the push must not integrate.
        // The ped is 0.6 m behind, every read returns the last push. Old: the push grew by the correction EVERY frame.
        LocomotionController lc;
        LocoVec last = V(4.5, 0, 0);
        bool pushedBefore = false;
        double maxV = 0;
        for (int i = 0; i < 240; ++i) {
            LocoInput in = UnitIn(i < 150 ? 0.0 : 0.6, 4.5);
            if (pushedBefore) in.pedVel = last;
            const LocoOutput o = lc.Update(in);
            if (o.setVelocity) { last = o.velocity; pushedBefore = true; maxV = std::max(maxV, double(o.velocity.x)); }
            else pushedBefore = false;
        }
        Check("Sync A: a velocity read that returns our last push is not fed back: the push stays under 4.5 + 2 m/s after 90 frames of a 0.6 m error (old: grows every frame)",
              maxV <= 6.6 && lc.Stats().contaminated > 10, F("max pushed vx %.2f m/s, %u contaminated reads", maxV, lc.Stats().contaminated));
    }
    {
        // Settle: the pose has stopped, the ped is 1.0 m behind it (and stays there: it does not walk in this unit): no stand-still for
        // 1.5 s, the controller keeps a Walk toward the pose with the anchor ON the pose, then it stands. 0.1 m behind: stands after 150 ms.
        auto run = [](double behind, double* standAtMs, bool* anchorOnPose, Gait* gaitAt1s) {
            LocoConfig cfg; cfg.mode = LocoMode::Anchor;
            LocomotionController lc(cfg);
            for (int i = 0; i < 120; ++i) lc.Update(UnitIn(0, 4.5));
            *standAtMs = -1; *anchorOnPose = true;
            double t = 0;
            for (int i = 0; i < 150; ++i, t += 1000.0 / 60) {
                LocoInput in = UnitIn(behind, 0);
                in.vel = V(0, 0, 0); in.pedGait = Gait::Walk;
                const LocoOutput o = lc.Update(in);
                if (o.task == LocoTask::StandStill && *standAtMs < 0) *standAtMs = t;
                if (o.moveAnchor && std::hypot(double(o.anchorPos.x) - 100.0, double(o.anchorPos.y)) > 0.01) *anchorOnPose = false;
                if (i == 60) *gaitAt1s = o.gait;
            }
            return lc.Stats().settles;
        };
        double standFar, standNear; bool onFar, onNear; Gait g1 = Gait::Still, g2 = Gait::Still;
        const uint32_t settlesFar = run(1.0, &standFar, &onFar, &g1), settlesNear = run(0.1, &standNear, &onNear, &g2);
        Check("Sync A: a stop with the ped 1.0 m short of the pose keeps a Walk to the pose (anchor on the pose) for ~1.5 s before the stand-still; 0.1 m short stands after ~150 ms (old: 150 ms either way)",
              standFar >= 1500 && standFar <= 1800 && onFar && g1 == Gait::Walk && settlesFar == 1 && standNear >= 100 && standNear <= 250 && settlesNear == 0,
              F("1.0 m: stand-still at %.0f ms, anchor on pose %d, gait at 1 s %d, %u settles; 0.1 m: at %.0f ms, %u settles", standFar, int(onFar), int(g1), settlesFar, standNear, settlesNear));
    }
    {
        // The anchor has no floor under a slow pose: a pose at 0.6 m/s gets an anchor 0.48 m ahead (0.8 s of travel), not 1.5 m (old floor).
        LocoConfig cfg; cfg.mode = LocoMode::Anchor;
        LocomotionController lc(cfg);
        LocoInput in = UnitIn(0, 0.6);
        in.vel = V(0.6, 0, 0);
        LocoOutput o;
        for (int i = 0; i < 30; ++i) o = lc.Update(in);
        const double lead = o.anchorPos.x - 100.0;
        Check("Sync A: the anchor leads a slow pose by its speed only (0.6 m/s -> 0.48 m; old floor 1.5 m)", o.moveAnchor && std::fabs(lead - 0.48) < 0.03, F("lead %.3f m", lead));
    }
    {
        // A go-to-entity that keeps failing is not retried every 20 s for ever: the straight fallback doubles for each stall in a row.
        LocoConfig cfg; cfg.mode = LocoMode::Anchor;
        LocomotionController lc(cfg);
        AnimPed ped;
        Path p;
        p.speed = [](double) { return 4.5; };
        p.dirDeg = [](double) { return 0.0; };
        for (double t = 0; t < 200000; t += 1000.0 / 30) {
            StepAnim(lc, ped, p.At(t / 1000.0), 1000.0 / 30, false, true);   // the ped always reads Still: every anchor task "fails"
            p.Advance(t / 1000.0, 1000.0 / 30);
        }
        Check("Sync A: a go-to-entity that always stalls is retried after 20, 40, 80 s (4 anchor stalls in 200 s; old: every ~21 s, 9)",
              lc.Stats().anchorFallbacks >= 3 && lc.Stats().anchorFallbacks <= 5, F("%u anchor stalls", lc.Stats().anchorFallbacks));
    }
    {
        // LocoDiag: skate, idle slide, stop-starts, error and anchor distance from synthetic frames.
        auto frame = [](double nowMs, double pedX, double poseX, double poseSp, Gait g, bool pushed, double pushV, double pedV) {
            LocoDiagFrame f;
            f.dtMs = 1000.0 / 60; f.nowMs = nowMs;
            f.pose = V(poseX, 0, 0); f.pedPos = V(pedX, 0, 0); f.poseSpeed = float(poseSp); f.gait = g;
            f.pushed = pushed; f.push = V(pushV, 0, 0); f.pedVel = V(pedV, 0, 0); f.havePedVel = true;
            return f;
        };
        const double dt = 1000.0 / 60;
        LocoDiag carried, dragged, idle, stuck, brief;
        for (int i = 0; i < 120; ++i) {
            carried.Observe(frame(i * dt, 4.0 * i * dt / 1000.0, 4.0 * i * dt / 1000.0 + 0.2, 4.0, Gait::Run, false, 0, 4.0));
            dragged.Observe(frame(i * dt, 4.0 * i * dt / 1000.0, 4.0 * i * dt / 1000.0 + 0.2, 4.0, Gait::Run, true, 4.0, 0.0));
            idle.Observe(frame(i * dt, 0, 0.3, 0.0, Gait::Still, true, 1.0, 0.0));
            // stuck: the pose runs, the ped stands for frames 20..49 (500 ms), then moves; brief: stands for 4 frames only
            const double stuckX = i < 20 ? 4.0 * i * dt / 1000.0 : i < 50 ? 4.0 * 20 * dt / 1000.0 : 4.0 * (i - 30) * dt / 1000.0;
            stuck.Observe(frame(i * dt, stuckX, 4.0 * i * dt / 1000.0, 4.0, Gait::Run, false, 0, 0));
            const double briefX = i < 20 ? 4.0 * i * dt / 1000.0 : i < 24 ? 4.0 * 20 * dt / 1000.0 : 4.0 * (i - 4) * dt / 1000.0;
            brief.Observe(frame(i * dt, briefX, 4.0 * i * dt / 1000.0, 4.0, Gait::Run, false, 0, 0));
        }
        const LocoDiagSummary a = carried.Summary(), b = dragged.Summary(), c = idle.Summary(), d = stuck.Summary(), e = brief.Summary();
        Check("Sync A: LocoDiag reads 0% skate for a ped carried by its stride, 100% for one dragged by pushes, 2 m of idle slide for 120 frames of a 1 m/s push on a Still ped, 1 stop-start for 500 ms standing under a running pose (0 for 67 ms)",
              a.skate < 0.01 && b.skate > 0.99 && std::fabs(c.idleSlideM - 2.0) < 0.1 && d.stops == 1 && e.stops == 0 && std::fabs(a.derivedSpeedMs - 4.0) < 0.2 && std::fabs(a.meanErrM - 0.2) < 0.01,
              F("carried skate %.3f, dragged %.3f, idle %.2f m, stops %u / %u, derived %.2f m/s, err %.2f m", a.skate, b.skate, c.idleSlideM, d.stops, e.stops, a.derivedSpeedMs, a.meanErrM));
    }
}

#include "t2_onfoot_tests.h"   // T2 (desync round 2): the many-models matrix, the ladder, the nudge
#include "r3_t2_tests.h"      // R3 T2 (desync round 3): why a puppet shows no gait, the quiet window, repairs, a ladder that converges
#include "r4_t1_blend_tests.h" // R4 T1 (desync round 4): Blend, a velocity-driven walking controller, against switchable models of the game
#include "r5_onfoot_tests.h"   // R5 (on-foot round 5): the Blend self-check against task-changing events (grace window, place once, short hold, probation)

int main() {
    std::printf("-- gait\n");
    TestGait();
    std::printf("-- task re-issue\n");
    TestTaskReissue();
    std::printf("-- snap\n");
    TestSnap();
    std::printf("-- velocity\n");
    TestVelocity();
    std::printf("-- heading\n");
    TestHeading();
    std::printf("-- jump / ragdoll\n");
    TestJumpRagdoll();
    std::printf("-- closed loop\n");
    TestClosedLoop();
    TestLongCircle();
    std::printf("-- authoritative gait / debounce\n");
    TestAuthoritativeGait();
    std::printf("-- move rate\n");
    TestMoveRate();
    std::printf("-- pause\n");
    TestPause();
    std::printf("-- aim gate\n");
    TestAimGate();
    std::printf("-- swimming (sync plan W6)\n");
    TestSwimming();
    std::printf("-- W8: churn, soft reset, swim correction\n");
    TestChurn();
    TestSoftReset();
    TestSwimCorrection();
    std::printf("-- W11: gait changes without re-issue, anchor steering, residual push\n");
    TestW11();

    std::printf("-- Sync A: slide metric against RealPed\n");
    TestSyncA();

    T2OnFootTests();
    R3T2Tests();
    R4T1Tests();
    r5::R5Tests();
    if (std::getenv("R4T1_TABLE")) r4::R4Table();
    r4::R4One();

    // gait 3 + task 6 + snap 4 + velocity 6 + heading 2 + jump/ragdoll 4 + closed loop 3 + 60 s circle 1
    // + authoritative gait / debounce 7 + move rate 3 + pause 1 + aim gate 3 + swimming 1
    // + W8: churn 2 + soft reset 2 + swim correction 1 + W11 14 + Sync A 14 + T2 21 (tests/t2_onfoot_tests.h) + R3 T2 21 (tests/r3_t2_tests.h) + R4 T1 33 (tests/r4_t1_blend_tests.h) + R5 20 (tests/r5_onfoot_tests.h)
    const int expected = 3 + 6 + 4 + 6 + 2 + 4 + 3 + 1 + 7 + 3 + 1 + 3 + 1 + 2 + 2 + 1 + 14 + 14 + 21 + 21 + 34 + 20;
    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    if (g_run != expected) {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
