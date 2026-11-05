// vehicle_drive_test: the remote-vehicle interpolator and drive controller
// (libs/sync/vehicle_drive.h) against scripted drivers and a simulated car. Build
// target: vehicle_drive_test (selftest/, never shipped). Pure computation,
// reproducible (seeded generator, simulated time).
//
// The simulated car: SET_ENTITY_COORDS / QUATERNION apply at once; its velocity
// follows the commanded SET_ENTITY_VELOCITY with a first-order lag (100 ms: tyres and
// mass do not follow instantly) and always loses speed to drag (0.2 /s, so holding a
// speed needs a standing position error); its angular velocity follows the command
// with a 60 ms lag; position and rotation integrate the actual velocities. Errors are
// measured as each frame starts (where physics left the car vs that frame's target).
// Mutants (scratch, not shipped) of vehicle_drive.cpp each fail at least one check;
// every check here fails under at least one of: no correction, no sign flip, no slerp,
// per-frame constants, no snaps, no input guards, straight extrapolation, no hold,
// stale accepted, no buffering, swapped slerp weights, exp half-angle bug.
//
//   1. Quaternion math: slerp endpoints and midpoint, double-cover sign flip (slerp,
//      log, angle), normalisation of degenerate input, near-identical slerp, exp/log
//      round trip, rotation and heading conventions.
//   2. Interpolation: a car on a 50 m circle at 30 m/s with a 5 degree roll, 20 Hz
//      snapshots with jittered sample times and latency (reordering included), with and
//      without sent angular velocity: position and rotation error bounds.
//   3. Extrapolation: the same circle through a 300 ms loss (arc extrapolation within
//      bounds, a straight line measurably worse), recovery after it, Holding after the
//      cap; teleport detection and stale / invalid pushes.
//   4. Snap conditions: first frame, teleported, max(5 m, 1 s of travel), persistent
//      error (same time at 30/60/144 fps, never below the tolerance), under floor,
//      Reset; non-finite input refused; degenerate input never yields NaN.
//   5. Framerate independence: position and rotation convergence and the large-error
//      quaternion blend reach the same state at the same time at 30, 60 and 144 fps.
//   6. Closed loop: snapshots -> interpolator -> controller -> simulated car through a
//      straight, a hard turn and a stop, with a mid-run knock, at 30/60/144 fps.
//
//   7. Sender steering (sync plan W2 D): EstimateSteer against a kinematic bicycle with known
//      wheel angles (forward and reversing), the sign (left negative), the hold below 1 m/s,
//      saturation and bad input.
//   8. Sync plan W6: VehPush::Repeat, the interpolator session (a new server session's low timeline is
//      accepted), SnapshotStampMs (aged stamps within 5 cm on a 30 Hz -> 20 Hz relay, tick stamps not),
//      the controller's send gate (5 Hz still, at once on a change or a stop, every call moving).
//   9. Sync plan W7: VehicleInterpolator::TypicalGapMs (the class extra's input on the shared NetClock render clock).
//
//  10. Live test 0.4.2 fixes, W9 item 6 and 7: the contact grace (a knock plays out instead of being overwritten
//      every frame, by the collision flag and by a speed loss alone, then converges back without a snap, and
//      normal driving never opens one), PresentLead (a car within 30 m is drawn within 0.5 m of where it is NOW,
//      blended back beyond 45 m, slewed), and a bike's lean (RollDeg conventions; the roll survives the
//      controller on a banked circle).
//
// 11. Sync B (collision audit, docs/sync-specs/vehicles.md): the contact grace's third trigger (a velocity KICK: the car's own
//      velocity differs from what the controller commanded by more than 3 m/s, in any direction, at any target speed) and its
//      budget, measured on a planar rigid-body model (T-bone with a silent / stuck / late collision flag, a hit on a standing car,
//      normal driving, a body that does not honour its commands, penetration against a car and a wall), and the 10 s contact
//      diagnostics window (libs/sync/vehicle_contact.h).
//
//  12. Sync D (remote vehicle motion and occupants, offline audit): tests/vehsim.h runs the REAL interpolator, controller,
//      PresentLead, NetClock and ClassDelay behind a simulated relay (30 Hz sender, 30/15/10 Hz tiers, 20-80 ms jitter, 3-5 %
//      loss, the wire quantisation) against a simplified physical car, on scripted paths. Checks: Buffering walks the oldest
//      snapshot back along its motion (no pin: the 2.5 m / 14 m pop of a hand-over), DecideHandover (no snap between two other
//      controllers), the whole pipeline's error bounds per scenario and tier, stalls, DriveErrWindow, LateralErrorM,
//      ClassifyDisplacement, EnterWalkMode.
//
// Every loop-shaped check also asserts how many frames it measured; the run asserts
// the total number of checks.

#include "sync/netclock.h"
#include "sync/vehicle_drive.h"
#include "sync/hypertune.h"
#include "sync/vehicle_w9.h"
#include "sync/vehicle_contact.h"
#include "sync/vehicle_syncd.h"
#include "vehsim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

static DriveVec V(double x, double y, double z) {
    DriveVec v;
    v.x = float(x); v.y = float(y); v.z = float(z);
    return v;
}
static DriveQuat Q(double x, double y, double z, double w) {
    DriveQuat q;
    q.x = float(x); q.y = float(y); q.z = float(z); q.w = float(w);
    return q;
}
static DriveQuat Neg(const DriveQuat& q) { return Q(-q.x, -q.y, -q.z, -q.w); }
static double Dist(const DriveVec& a, const DriveVec& b) {
    return std::sqrt(double(a.x - b.x) * (a.x - b.x) + double(a.y - b.y) * (a.y - b.y) + double(a.z - b.z) * (a.z - b.z));
}
static double Len(const DriveVec& a) { return std::sqrt(double(a.x) * a.x + double(a.y) * a.y + double(a.z) * a.z); }
static double Deg(double rad) { return rad * 180.0 / kPi; }
static double AngDeg(const DriveQuat& a, const DriveQuat& b) { return Deg(vq::AngleBetween(a, b)); }
static bool FiniteV(const DriveVec& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
static bool UnitQ(const DriveQuat& q) {
    const double n = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w;
    return vq::Finite(q) && std::fabs(n - 1.0) < 1e-5;
}

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
    double Range(double a, double b) { return a + (b - a) * Uniform(); }
};

// ---- truth trajectories -------------------------------------------------------

struct Truth { DriveVec pos, vel, angVel; DriveQuat rot; float steering = 0; };

// A 50 m circle at 30 m/s (0.6 rad/s), counter-clockwise, 5 degrees of roll.
static Truth Circle(double tMs) {
    const double R = 50, w = 0.6, phi = w * tMs / 1000.0;
    Truth t;
    t.pos = V(R * std::cos(phi), R * std::sin(phi), 10);
    t.vel = V(-30 * std::sin(phi), 30 * std::cos(phi), 0);
    t.angVel = V(0, 0, w);
    const DriveQuat roll = vq::Exp(V(0, 5.0 * kPi / 180.0, 0)); // about the body's forward axis
    t.rot = vq::Mul(vq::FromHeadingDeg(Deg(phi)), roll);
    t.steering = 0.2f;
    return t;
}

// Straight 30 m/s, a hard turn at 20 m/s and 1.2 rad/s, straight again, a 10 m/s^2 stop.
struct Route {
    std::vector<Truth> tab; // 1 ms steps
    Route() {
        double x = 0, y = 0, h = 0;
        const int n = 13001;
        tab.resize(n);
        for (int i = 0; i < n; ++i) {
            const double t = i / 1000.0;
            double v, yaw;
            if (t < 3.0) { v = 30; yaw = 0; }
            else if (t < 3.5) { v = 30 - 20 * (t - 3.0); yaw = 2.4 * (t - 3.0); }
            else if (t < 5.5) { v = 20; yaw = 1.2; }
            else if (t < 6.0) { v = 20; yaw = 1.2 - 2.4 * (t - 5.5); }
            else if (t < 8.0) { v = 20; yaw = 0; }
            else if (t < 10.0) { v = 20 - 10 * (t - 8.0); yaw = 0; }
            else { v = 0; yaw = 0; }
            Truth& s = tab[i];
            s.pos = V(x, y, 5);
            s.vel = V(-v * std::sin(h), v * std::cos(h), 0);
            s.angVel = V(0, 0, yaw);
            s.rot = vq::FromHeadingDeg(Deg(h));
            s.steering = float(yaw * 0.3);
            x += -v * std::sin(h) * 0.001;
            y += v * std::cos(h) * 0.001;
            h += yaw * 0.001;
        }
    }
    Truth At(double tMs) const {
        if (tMs <= 0) return tab.front();
        const size_t i = size_t(tMs);
        if (i + 1 >= tab.size()) return tab.back();
        const double u = tMs - double(i);
        Truth r = tab[i];
        const Truth& b = tab[i + 1];
        r.pos = V(r.pos.x + (b.pos.x - r.pos.x) * u, r.pos.y + (b.pos.y - r.pos.y) * u, r.pos.z);
        r.rot = vq::Slerp(r.rot, b.rot, u);
        return r;
    }
};

// 20 Hz snapshots with jittered sample times and latency, delivered in arrival order.
struct Feed {
    struct Item { double arrive; VehicleSnapshot s; };
    std::vector<Item> items;
    size_t next = 0;
    template <class Fn>
    Feed(Fn truth, double untilMs, bool sendAngVel, uint64_t seed, double dropFrom = -1, double dropTo = -1,
         double latMin = 30, double latMax = 90) {
        Rng r(seed);
        for (int k = 0; k * 50.0 <= untilMs; ++k) {
            const double t = k * 50.0 + r.Range(-10, 10);
            const double lat = r.Range(latMin, latMax);
            if (t >= dropFrom && t < dropTo) continue;
            const Truth tr = truth(t);
            Item it;
            it.arrive = t + lat;
            it.s.tMs = t;
            it.s.pos = tr.pos;
            it.s.vel = tr.vel;
            it.s.rot = tr.rot;
            it.s.hasAngVel = sendAngVel;
            if (sendAngVel) it.s.angVel = tr.angVel;
            it.s.steering = tr.steering;
            items.push_back(it);
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.arrive < b.arrive; });
    }
    int Deliver(double nowMs, VehicleInterpolator& ip) {
        int stale = 0;
        while (next < items.size() && items[next].arrive <= nowMs)
            if (ip.Push(items[next++].s) == VehPush::Stale) ++stale;
        return stale;
    }
};

// ---- the simulated car --------------------------------------------------------

struct SimCar {
    DriveVec pos, vel, angVel;
    DriveQuat rot;
    double velLagMs = 100, angLagMs = 60, drag = 0.2;
    void Apply(const VehicleDriveOutput& o, double dtMs) {
        const double dt = dtMs / 1000.0;
        if (o.snap) { pos = o.snapPos; rot = o.snapRot; }
        if (o.setQuaternion) rot = o.rot;
        if (o.snap) { vel = o.velocity; angVel = o.angularVelocity; }
        else {
            if (o.setVelocity) {
                const double k = 1.0 - std::exp(-dtMs / velLagMs);
                vel = V(vel.x + (o.velocity.x - vel.x) * k, vel.y + (o.velocity.y - vel.y) * k, vel.z + (o.velocity.z - vel.z) * k);
            }
            const double d = std::exp(-drag * dt);
            vel = V(vel.x * d, vel.y * d, vel.z * d);
            if (o.setAngularVelocity) {
                const double k = 1.0 - std::exp(-dtMs / angLagMs);
                angVel = V(angVel.x + (o.angularVelocity.x - angVel.x) * k, angVel.y + (o.angularVelocity.y - angVel.y) * k,
                           angVel.z + (o.angularVelocity.z - angVel.z) * k);
            }
        }
        pos = V(pos.x + vel.x * dt, pos.y + vel.y * dt, pos.z + vel.z * dt);
        rot = vq::Mul(vq::Exp(V(angVel.x * dt, angVel.y * dt, angVel.z * dt)), rot);
    }
};

// An ideal body: takes the commanded velocities at once (for exact rate comparisons).
struct IdealCar {
    DriveVec pos, vel, angVel;
    DriveQuat rot;
    void Apply(const VehicleDriveOutput& o, double dtMs) {
        const double dt = dtMs / 1000.0;
        if (o.snap) { pos = o.snapPos; rot = o.snapRot; }
        if (o.setQuaternion) rot = o.rot;
        vel = o.setVelocity ? o.velocity : V(0, 0, 0);
        angVel = o.setAngularVelocity ? o.angularVelocity : V(0, 0, 0);
        pos = V(pos.x + vel.x * dt, pos.y + vel.y * dt, pos.z + vel.z * dt);
        rot = vq::Mul(vq::Exp(V(angVel.x * dt, angVel.y * dt, angVel.z * dt)), rot);
    }
};

static VehicleDriveInput Input(const VehicleSample& tg, const DriveVec& pos, const DriveQuat& rot, const DriveVec& vel, double dtMs) {
    VehicleDriveInput in;
    in.target = tg;
    in.localPos = pos;
    in.localRot = rot;
    in.localVel = vel;
    in.dtMs = dtMs;
    return in;
}

static VehicleSample Still(const DriveVec& pos, const DriveQuat& rot) {
    VehicleSample s;
    s.pos = pos;
    s.rot = rot;
    s.mode = VehMode::Interpolating;
    return s;
}

// ---- 1. quaternion math ------------------------------------------------------

static void TestQuatMath() {
    const DriveQuat a = vq::Mul(vq::FromHeadingDeg(20), vq::Exp(V(0.3, 0, 0)));
    const DriveQuat b = vq::Mul(vq::FromHeadingDeg(110), vq::Exp(V(0, -0.2, 0.1)));
    const double ab = AngDeg(a, b);
    const DriveQuat m = vq::Slerp(a, b, 0.5);
    Check("slerp endpoints are exact and the midpoint halves the arc",
          AngDeg(vq::Slerp(a, b, 0), a) < 1e-3 && AngDeg(vq::Slerp(a, b, 1), b) < 1e-3 &&
              std::fabs(AngDeg(a, m) - ab / 2) < 1e-3 && std::fabs(AngDeg(m, b) - ab / 2) < 1e-3,
          F("arc %.3f, a-m %.4f, m-b %.4f", ab, AngDeg(a, m), AngDeg(m, b)));

    // 10 and 350 degrees are 20 apart; with b negated, a naive slerp goes the 340 way round
    // (FromHeadingDeg(350) is the half-angle 175 representative, w < 0: dot with h10 < 0)
    const DriveQuat h10 = vq::FromHeadingDeg(10), h350 = vq::FromHeadingDeg(350);
    const double mid = vq::HeadingDeg(vq::Slerp(h10, h350, 0.5));
    const double q1 = vq::HeadingDeg(vq::Slerp(h10, h350, 0.25));
    const double midN = vq::HeadingDeg(vq::Slerp(h10, Neg(h350), 0.5));
    Check("slerp takes the short arc across the double cover (q and -q)",
          vq::Dot(h10, h350) < 0 && std::fabs(mid) < 1e-3 && std::fabs(q1 - 5.0) < 1e-3 && std::fabs(midN) < 1e-3,
          F("dot %.3f, mid %.4f, quarter %.4f, mid(-q) %.4f", vq::Dot(h10, h350), mid, q1, midN));

    const DriveVec la = vq::Log(b), lb = vq::Log(Neg(b));
    Check("log and angle are the same for q and -q, shortest arc",
          Dist(la, lb) < 1e-5 && Len(la) <= kPi + 1e-6 && std::fabs(AngDeg(a, Neg(b)) - ab) < 1e-3 &&
              std::fabs(AngDeg(h10, h350) - 20.0) < 1e-3,
          F("|log| %.5f vs %.5f, angle(h10,-h350) %.4f", Len(la), Len(lb), AngDeg(h10, h350)));

    const DriveQuat z = vq::Normalize(Q(0, 0, 0, 0));
    const DriveQuat nanq = vq::Normalize(Q(std::nan(""), 0, 0, 1));
    const DriveQuat big = vq::Normalize(Q(0, 0, 3, 4));
    const DriveQuat mul = vq::Mul(Q(0, 0, 2, 0), Q(1, 1, 0, 1));
    Check("normalisation: zero and NaN -> identity, scaled -> unit, products unit",
          z.w == 1 && z.x == 0 && nanq.w == 1 && std::fabs(big.z - 0.6f) < 1e-6 && std::fabs(big.w - 0.8f) < 1e-6 && UnitQ(mul),
          F("zero (%g %g %g %g) big (%g %g)", z.x, z.y, z.z, z.w, big.z, big.w));

    // nearly identical: dot ~ 1 - 1e-12 (sin(theta) ~ 1e-6): must not divide by ~0
    const DriveQuat c = vq::FromHeadingDeg(40), d = vq::FromHeadingDeg(40.0001);
    bool ok = true;
    for (int i = 0; i <= 10; ++i) {
        const DriveQuat s = vq::Slerp(c, d, i / 10.0);
        if (!UnitQ(s) || AngDeg(s, c) > 2e-3) ok = false;
    }
    const DriveQuat same = vq::Slerp(c, c, 0.5), same2 = vq::Slerp(c, Neg(c), 0.7);
    Check("slerp of (nearly) identical quaternions is finite and stays put",
          ok && UnitQ(same) && AngDeg(same, c) < 1e-3 && UnitQ(same2) && AngDeg(same2, c) < 1e-3);

    const DriveVec r = V(0.4, -1.1, 2.0);
    const DriveVec rt = vq::Log(vq::Exp(r));
    const DriveVec fwd = vq::Rotate(vq::FromHeadingDeg(90), V(0, 1, 0));
    bool headings = true;
    for (double hdg : {0.0, 45.0, 90.0, 179.0, 180.0, -179.0, -90.0, 359.0}) {
        double back = vq::HeadingDeg(vq::FromHeadingDeg(hdg)) - hdg;
        back = std::fmod(back + 540.0, 360.0) - 180.0;
        if (std::fabs(back) > 1e-3) headings = false;
    }
    Check("exp/log round trip, heading 90 points the nose to -x, heading round trip",
          Dist(r, rt) < 1e-5 && Dist(fwd, V(-1, 0, 0)) < 1e-6 && headings,
          F("rt (%g %g %g) fwd (%g %g %g)", rt.x, rt.y, rt.z, fwd.x, fwd.y, fwd.z));
}

// ---- 2. interpolation --------------------------------------------------------

static void RunCircle(bool sendAngVel, const char* name) {
    VehicleInterpolator ip;
    Feed feed(Circle, 10000, sendAngVel, sendAngVel ? 11 : 12);
    double maxPos = 0, maxAng = 0, maxVel = 0, maxW = 0;
    int frames = 0, interp = 0, stale = 0;
    const double dt = 1000.0 / 60.0;
    for (double now = 0; now < 10000; now += dt) {
        stale += feed.Deliver(now, ip);
        const double rt = now - 150;
        VehicleSample s;
        if (rt < 500 || !ip.Sample(rt, s)) continue;
        const Truth tr = Circle(rt);
        ++frames;
        if (s.mode == VehMode::Interpolating) ++interp;
        maxPos = std::max(maxPos, Dist(s.pos, tr.pos));
        maxAng = std::max(maxAng, AngDeg(s.rot, tr.rot));
        maxVel = std::max(maxVel, Dist(s.vel, tr.vel));
        maxW = std::max(maxW, Dist(s.angVel, tr.angVel));
    }
    Check(name,
          frames > 500 && interp >= frames * 95 / 100 && maxPos < 0.05 && maxAng < 0.3 && maxVel < 0.1 && maxW < 0.02 && stale > 0,
          F("frames %d interp %d stale %d, pos %.4f m, rot %.3f deg, vel %.3f, w %.4f", frames, interp, stale, maxPos, maxAng, maxVel, maxW));
}

static void TestInterpolation() {
    RunCircle(true, "circle 30 m/s, 20 Hz jittered, angular velocity sent: within 5 cm / 0.3 deg");
    RunCircle(false, "circle 30 m/s, 20 Hz jittered, angular velocity derived: within 5 cm / 0.3 deg");
}

// ---- 3. extrapolation, teleport, push validation ---------------------------------

struct LossResult { double maxPos = 0, maxAng = 0, afterPos = 0; int extrap = 0; };

static LossResult RunLoss(bool arc, bool sendAngVel) {
    VehicleInterpConfig cfg;
    cfg.arcExtrapolation = arc;
    VehicleInterpolator ip(cfg);
    Feed feed(Circle, 8000, sendAngVel, 21, 4000, 4300);
    LossResult r;
    const double dt = 1000.0 / 60.0;
    for (double now = 0; now < 8000; now += dt) {
        feed.Deliver(now, ip);
        const double rt = now - 100;
        VehicleSample s;
        if (rt < 500 || !ip.Sample(rt, s)) continue;
        const Truth tr = Circle(rt);
        if (s.mode == VehMode::Extrapolating && rt > 3900 && rt < 4400) ++r.extrap;
        if (rt > 3900 && rt < 4400) {
            r.maxPos = std::max(r.maxPos, Dist(s.pos, tr.pos));
            r.maxAng = std::max(r.maxAng, AngDeg(s.rot, tr.rot));
        }
        if (rt > 4500) r.afterPos = std::max(r.afterPos, Dist(s.pos, tr.pos));
    }
    return r;
}

static void TestExtrapolation() {
    const LossResult a = RunLoss(true, true), d = RunLoss(true, false), lin = RunLoss(false, true);
    Check("300 ms loss on the circle: arc extrapolation within 0.15 m / 0.5 deg (w sent and derived)",
          a.extrap >= 12 && d.extrap >= 12 && a.maxPos < 0.15 && d.maxPos < 0.15 && a.maxAng < 0.5 && d.maxAng < 0.5,
          F("extrap %d/%d frames, pos %.3f/%.3f m, rot %.3f/%.3f deg", a.extrap, d.extrap, a.maxPos, d.maxPos, a.maxAng, d.maxAng));
    Check("a straight-line extrapolation is measurably worse (the loss test is sensitive)",
          lin.maxPos > 0.3 && lin.maxPos > 3 * a.maxPos, F("straight %.3f m vs arc %.3f m", lin.maxPos, a.maxPos));
    Check("after the loss the interpolation is back within 5 cm", a.afterPos < 0.05 && d.afterPos < 0.05,
          F("%.4f / %.4f m", a.afterPos, d.afterPos));

    // Holding past the cap, then a teleport, stale and invalid pushes
    VehicleInterpolator ip;
    VehicleSnapshot s;
    s.tMs = 0; s.pos = V(0, 0, 0); s.vel = V(0, 20, 0); s.rot = vq::FromHeadingDeg(0); s.hasAngVel = true; s.angVel = V(0, 0, 0.5);
    ip.Push(s);
    s.tMs = 50; s.pos = V(0, 1, 0);
    ip.Push(s);
    VehicleSample o;
    ip.Sample(50 + 400, o);
    const bool extrap = o.mode == VehMode::Extrapolating && std::fabs(vq::HeadingDeg(o.rot) - Deg(0.5 * 0.4)) < 0.01;
    ip.Sample(50 + 900, o);
    const bool hold = o.mode == VehMode::Holding && Len(o.vel) == 0 && Len(o.angVel) == 0 &&
                      std::fabs(vq::HeadingDeg(o.rot) - Deg(0.5 * 0.5)) < 0.01;
    Check("extrapolation turns with w; past 500 ms it holds the capped pose with zero velocity", extrap && hold,
          F("mode %d heading %.3f vel %.3f", int(o.mode), vq::HeadingDeg(o.rot), Len(o.vel)));

    s.tMs = 100; s.pos = V(0, 2, 0);
    const VehPush ok = ip.Push(s);
    s.tMs = 100;
    const VehPush dup = ip.Push(s);
    s.tMs = 80;
    const VehPush old = ip.Push(s);
    s.tMs = 150; s.pos = V(0, 2 + 1 + 10.5, 0); // predicted ~3, 10.5 m off (limit max(10, 0.5 * 20))
    const VehPush tp = ip.Push(s);
    VehicleSample after;
    ip.Sample(120, after);
    const bool tpFlag = after.teleported && after.mode == VehMode::Buffering && ip.Count() == 1;
    ip.Sample(130, after);
    s.tMs = 200; s.pos = V(0, 13.5 + 1 + 9.5, 0); // 9.5 m off: within the limit
    const VehPush near = ip.Push(s);
    Check("stale (duplicate, older) dropped; > max(10 m, 0.5 s) from prediction teleports once",
          ok == VehPush::Accepted && dup == VehPush::Stale && old == VehPush::Stale && tp == VehPush::Teleported && tpFlag &&
              !after.teleported && near == VehPush::Accepted,
          F("ok %d dup %d old %d tp %d near %d count %d", int(ok), int(dup), int(old), int(tp), int(near), ip.Count()));

    VehicleSnapshot bad = s;
    bad.tMs = 300; bad.pos.x = std::nanf("");
    const VehPush b1 = ip.Push(bad);
    bad = s; bad.tMs = 300; bad.rot = Q(0, 0, 0, 0);
    const VehPush b2 = ip.Push(bad);
    bad = s; bad.tMs = 300; bad.hasAngVel = true; bad.angVel.z = INFINITY;
    const VehPush b3 = ip.Push(bad);
    bad = s; bad.tMs = 300; bad.rot = Q(0, 0, 3, 4); // unnormalised: stored normalised
    const VehPush b4 = ip.Push(bad);
    VehicleSample u;
    ip.Sample(300, u);
    Check("invalid pushes (NaN, zero quaternion, inf w) refused; an unnormalised one is normalised",
          b1 == VehPush::Invalid && b2 == VehPush::Invalid && b3 == VehPush::Invalid && b4 == VehPush::Accepted && UnitQ(u.rot),
          F("%d %d %d %d", int(b1), int(b2), int(b3), int(b4)));
}

// ---- 4. snap conditions -------------------------------------------------------

static VehicleSample Moving(const DriveVec& pos, double vy) {
    VehicleSample s = Still(pos, vq::FromHeadingDeg(0));
    s.vel = V(0, vy, 0);
    return s;
}

// The snap tests hold the car still while the controller commands it (open loop): the commanded velocity is never honoured, which
// is exactly what the velocity-kick trigger (Sync B) reads as a hit. They test the snap rules, so the kick trigger is off here.
static VehicleDriveConfig NoKickCfg() {
    VehicleDriveConfig cfg;
    cfg.contactKickMs = 0.0f;
    return cfg;
}

// ms until a persistent snap with the car held `off` metres beside a target moving at vy
static double PersistentSnapMs(double off, double vy, double fps, int* frames) {
    VehicleDriveController c(NoKickCfg());
    const double dt = 1000.0 / fps;
    c.Update(Input(Moving(V(0, 0, 0), vy), V(0, 0, 0), vq::FromHeadingDeg(0), V(0, vy, 0), dt)); // first-frame snap
    double t = 0;
    *frames = 0;
    for (int i = 0; i < int(10000 / dt); ++i) {
        t += dt;
        ++*frames;
        const VehicleDriveOutput o = c.Update(Input(Moving(V(off, 0, 0), vy), V(0, 0, 0), vq::FromHeadingDeg(0), V(0, vy, 0), dt));
        if (o.snap) return t;
    }
    return -1;
}

static void TestSnap() {
    {
        VehicleDriveController c;
        const VehicleSample s = Moving(V(1, 2, 3), 10);
        const VehicleDriveOutput o1 = c.Update(Input(s, V(1, 1.5, 3), vq::FromHeadingDeg(0), V(0, 0, 0), 16));
        const VehicleDriveOutput o2 = c.Update(Input(s, V(1, 1.5, 3), vq::FromHeadingDeg(0), V(0, 10, 0), 16));
        VehicleSample tp = s;
        tp.teleported = true;
        const VehicleDriveOutput o3 = c.Update(Input(tp, V(1, 1.5, 3), vq::FromHeadingDeg(0), V(0, 10, 0), 16));
        c.Reset();
        const VehicleDriveOutput o4 = c.Update(Input(s, V(1, 1.5, 3), vq::FromHeadingDeg(0), V(0, 10, 0), 16));
        Check("snap on the first frame, on teleported and after Reset, not otherwise",
              o1.snap && !o2.snap && o3.snap && o4.snap && Dist(o1.snapPos, s.pos) == 0 && o1.setVelocity && Dist(o1.velocity, s.vel) == 0);
    }
    {
        auto instant = [](double off, double vy) {
            VehicleDriveController c;
            c.Update(Input(Moving(V(0, 0, 0), vy), V(0, 0, 0), vq::FromHeadingDeg(0), V(0, 0, 0), 16));
            return c.Update(Input(Moving(V(off, 0, 0), vy), V(0, 0, 0), vq::FromHeadingDeg(0), V(0, vy, 0), 16)).snap;
        };
        Check("instant snap above max(5 m, 1 s of travel): 5.2 m still, 31 m at 30 m/s; not 4.8 m / 29 m",
              instant(5.2, 0) && !instant(4.8, 0) && instant(31, 30) && !instant(29, 30) && instant(10.5, 10) && !instant(9.5, 10));
    }
    {
        double t[3], t2[3], t3[3];
        int n[3], n2[3], n3[3];
        bool ok = true;
        std::string got;
        for (int i = 0; i < 3; ++i) {
            t[i] = PersistentSnapMs(4.0, 0, kFps[i], &n[i]);   // 1 m over tolerance: 500 ms
            t2[i] = PersistentSnapMs(7.0, 20, kFps[i], &n2[i]); // 7 - (3 + 2) = 2 m over: 250 ms
            t3[i] = PersistentSnapMs(2.9, 0, kFps[i], &n3[i]); // under tolerance: never
            const double fr = 1000.0 / kFps[i];
            if (std::fabs(t[i] - 500) > fr + 1 || std::fabs(t2[i] - 250) > fr + 1 || t3[i] >= 0 || n3[i] < 290) ok = false;
            got += F("%g fps: %.1f / %.1f / %.0f ms (%d frames)  ", kFps[i], t[i], t2[i], t3[i], n3[i]);
        }
        Check("persistent error snaps after the same TIME at 30/60/144 fps; never under the tolerance", ok, got);
    }
    {
        // under floor: target 1 m above, not climbing -> snap after 250 ms; climbing (vz 1) -> never
        auto run = [](double vz, double* when) {
            VehicleDriveController c(NoKickCfg());
            VehicleSample s = Still(V(0, 0, 1), vq::FromHeadingDeg(0));
            s.vel = V(0, 0, vz);
            c.Update(Input(s, V(0, 0, 1), vq::FromHeadingDeg(0), V(0, 0, 0), 10));
            *when = -1;
            for (int i = 1; i <= 200; ++i) {
                const VehicleDriveOutput o = c.Update(Input(s, V(0, 0, 0), vq::FromHeadingDeg(0), V(0, 0, 0), 10));
                if (o.snap) { *when = i * 10.0; return c.Stats().underFloorSnaps; }
            }
            return c.Stats().underFloorSnaps;
        };
        double w1, w2;
        const uint32_t s1 = run(0, &w1), s2 = run(1.0, &w2);
        Check("under floor (GTAN): 1 m below a level target snaps at 250 ms; a climbing target does not",
              s1 == 1 && std::fabs(w1 - 250) <= 10 && s2 == 0 && w2 < 0, F("%.0f ms (%u), climbing %.0f (%u)", w1, s1, w2, s2));
    }
    {
        VehicleDriveController c;
        VehicleSample s = Moving(V(0, 0, 0), 10);
        c.Update(Input(s, V(0, 0, 0), vq::FromHeadingDeg(0), V(0, 0, 0), 16));
        VehicleSample bad = s;
        bad.pos.y = std::nanf("");
        const VehicleDriveOutput r1 = c.Update(Input(bad, V(0, 0, 0), vq::FromHeadingDeg(0), V(0, 0, 0), 16));
        const VehicleDriveOutput r2 = c.Update(Input(s, V(0, 0, 0), Q(0, 0, std::nan(""), 1), V(0, 0, 0), 16));
        const VehicleDriveOutput r3 = c.Update(Input(s, V(0, 0, 0), vq::FromHeadingDeg(0), V(0, 0, 0), std::nan("")));
        const bool refused = !r1.snap && !r1.setVelocity && !r1.setAngularVelocity && !r1.setQuaternion && !r1.setSteering &&
                             !r2.setVelocity && !r3.setAngularVelocity && c.Stats().refused == 3;
        // degenerate but finite input: zero quaternions, antipodal, huge / negative / zero dt
        bool finite = true;
        const DriveQuat qs[4] = {Q(0, 0, 0, 0), Neg(vq::FromHeadingDeg(179.9)), Q(0, 0, 1e-30f, 0), Q(0, 1, 0, 0)};
        const double dts[4] = {0, -5, 1e9, 7};
        for (const DriveQuat& tq : qs)
            for (const DriveQuat& lq : qs)
                for (double dt : dts) {
                    VehicleSample t = Moving(V(0.5, 0, 0), 10);
                    t.rot = tq;
                    const VehicleDriveOutput o = c.Update(Input(t, V(0, 0, 0), lq, V(0, 0, 0), dt));
                    if (!FiniteV(o.velocity) || !FiniteV(o.angularVelocity) || !FiniteV(o.angularVelocityBody) || !UnitQ(o.rot) ||
                        !UnitQ(o.snapRot) || !std::isfinite(o.rotErrorDeg) || !std::isfinite(o.posErrorM))
                        finite = false;
                }
        Check("non-finite input refused (nothing set); degenerate quaternions / dt never yield NaN", refused && finite,
              F("refused %u", c.Stats().refused));
    }
}

// ---- 5. framerate independence ----------------------------------------------------

static void TestFramerate() {
    // position: a still target 2 m away; rotation: 30 degrees off (angular velocity path);
    // blend: 120 degrees off (quaternion path). State after 1/3 s at each frame rate.
    double pos[3], rot[3], blend[3];
    int frames[3];
    for (int i = 0; i < 3; ++i) {
        const double dt = 1000.0 / kFps[i];
        const int n = int(std::lround(kFps[i] / 3.0));
        frames[i] = n;
        for (int which = 0; which < 3; ++which) {
            VehicleDriveController c;
            IdealCar car;
            car.rot = vq::FromHeadingDeg(0);
            const DriveQuat tq = vq::FromHeadingDeg(which == 2 ? 120 : which == 1 ? 30 : 0);
            const VehicleSample tg = Still(which == 0 ? V(2, 0, 0) : V(0, 0, 0), tq);
            c.Update(Input(Still(V(0, 0, 0), vq::FromHeadingDeg(0)), car.pos, car.rot, car.vel, dt)); // started, no error
            for (int k = 0; k < n; ++k) car.Apply(c.Update(Input(tg, car.pos, car.rot, car.vel, dt)), dt);
            if (which == 0) pos[i] = Dist(car.pos, tg.pos);
            else if (which == 1) rot[i] = AngDeg(car.rot, tq);
            else blend[i] = AngDeg(car.rot, tq);
        }
    }
    auto spread = [](const double* v) { return *std::max_element(v, v + 3) - *std::min_element(v, v + 3); };
    Check("position convergence: same error after 1/3 s at 30/60/144 fps (and it converged)",
          frames[0] == 10 && frames[2] == 48 && spread(pos) < 0.02 && pos[1] < 0.6,
          F("%.4f %.4f %.4f m", pos[0], pos[1], pos[2]));
    Check("rotation steering: same angle after 1/3 s at 30/60/144 fps (and it converged)", spread(rot) < 0.3 && rot[1] < 5,
          F("%.3f %.3f %.3f deg", rot[0], rot[1], rot[2]));
    Check("large-error quaternion blend: same angle after 1/3 s at 30/60/144 fps (and it converged)",
          spread(blend) < 1.5 && blend[1] < 10, F("%.3f %.3f %.3f deg", blend[0], blend[1], blend[2]));
}

// ---- 6. closed loop -----------------------------------------------------------

struct LoopResult {
    double maxPos = 0, maxAng = 0, knockPos = 0, preKnock = 0, endPos = 0, endSpeed = 0, endAng = 0;
    uint32_t snaps = 0, blends = 0;
    int frames = 0;
};

static LoopResult RunLoop(const Route& route, double fps, uint64_t seed) {
    VehicleInterpolator ip;
    VehicleDriveController c;
    SimCar car;
    car.rot = vq::FromHeadingDeg(0);
    car.pos = V(30, -20, 5);
    auto truth = [&route](double t) { return route.At(t); };
    Feed feed(truth, 13000, true, seed);
    LoopResult r;
    const double dt = 1000.0 / fps;
    bool knocked = false;
    for (double now = 0; now < 13150; now += dt) {
        feed.Deliver(now, ip);
        const double rt = now - 120;
        VehicleSample s;
        if (rt < 0 || !ip.Sample(rt, s)) continue;
        if (!knocked && rt >= 1500) { // a knock: 2.5 m sideways, 20 degrees of yaw
            knocked = true;
            car.pos = V(car.pos.x + 2.5, car.pos.y, car.pos.z);
            car.rot = vq::Mul(vq::FromHeadingDeg(20), car.rot);
        }
        // the error the frame starts with (the car where physics left it vs this frame's
        // target); measured after Apply it would be off by the frame's own travel (v * dt)
        const double pe = Dist(car.pos, s.pos), ae = AngDeg(car.rot, s.rot);
        const VehicleDriveOutput o = c.Update(Input(s, car.pos, car.rot, car.vel, dt));
        car.Apply(o, dt);
        ++r.frames;
        if (rt > 500 && (rt < 1500 || rt > 2500)) { r.maxPos = std::max(r.maxPos, pe); r.maxAng = std::max(r.maxAng, ae); }
        if (rt > 2500 && rt < 2600) r.knockPos = std::max(r.knockPos, pe);
        if (rt > 1000 && rt < 1500) r.preKnock = std::max(r.preKnock, pe);
        if (rt > 12500) { r.endPos = std::max(r.endPos, pe); r.endSpeed = std::max(r.endSpeed, Len(car.vel)); r.endAng = std::max(r.endAng, ae); }
    }
    r.snaps = c.Stats().snaps;
    r.blends = c.Stats().blends;
    return r;
}

static void TestClosedLoop() {
    const Route route;
    LoopResult r[3];
    for (int i = 0; i < 3; ++i) r[i] = RunLoop(route, kFps[i], 31 + i);
    bool track = true, knock = true, stop = true;
    std::string g1, g2, g3;
    for (int i = 0; i < 3; ++i) {
        const int minFrames = int(13000 * kFps[i] / 1000 * 0.98);
        if (r[i].frames < minFrames || r[i].maxPos > 1.0 || r[i].maxAng > 4.0 || r[i].snaps != 1) track = false;
        if (r[i].knockPos > 0.3 || r[i].knockPos > r[i].preKnock + 0.05) knock = false;
        if (r[i].endPos > 0.05 || r[i].endSpeed > 0.02 || r[i].endAng > 0.5) stop = false;
        g1 += F("%g fps: %d frames, %.3f m %.2f deg, snaps %u blends %u  ", kFps[i], r[i].frames, r[i].maxPos, r[i].maxAng, r[i].snaps, r[i].blends);
        g2 += F("%.3f m (before the knock %.3f)  ", r[i].knockPos, r[i].preKnock);
        g3 += F("%.4f m %.4f m/s %.3f deg  ", r[i].endPos, r[i].endSpeed, r[i].endAng);
    }
    Check("closed loop, straight + hard turn + stop at 30/60/144 fps: within 1 m / 4 deg, one snap (spawn) only", track, g1);
    Check("closed loop: a 2.5 m / 20 deg knock is pulled back within 1 s without a snap", knock, g2);
    Check("closed loop: settles at the stop (< 5 cm, < 0.02 m/s, < 0.5 deg)", stop, g3);
}

// ---- 7. sender steering estimate (sync plan W2 D) ----------------------------------------------
// A kinematic bicycle: yaw rate w = v * tan(delta) / L, delta the front wheel angle (positive = left,
// counter-clockwise seen from above, as GTA's z-up yaw). The wire steer is -1 full left .. +1 full
// right (protocol.h VehicleUpdate.steer), full lock 0.6 rad, so the truth is -delta / 0.6.
static void TestSteer() {
    const double L = 2.7, lock = 0.6;
    const double deltas[] = {-0.5, -0.3, -0.1, 0.0, 0.1, 0.3, 0.5};
    const int nDeltas = int(sizeof(deltas) / sizeof(deltas[0]));

    // forward at 15 m/s
    int measured = 0;
    double worst = 0;
    bool signOk = true;
    std::string got;
    for (double d : deltas) {
        const double v = 15.0, w = v * std::tan(d) / L;
        const double truth = -d / lock;
        const float est = EstimateSteer(float(v), float(w), float(L), 0.0f);
        ++measured;
        worst = std::max(worst, std::fabs(double(est) - truth));
        if (d > 0.05 && !(est < -0.05f)) signOk = false;   // left is negative
        if (d < -0.05 && !(est > 0.05f)) signOk = false;   // right is positive
        got += F("%+.1f->%+.2f ", d, est);
    }
    Check("steer: a bicycle at 15 m/s with known wheel angles (7 of them) is estimated within 0.1 of the wire value",
          measured == nDeltas && worst <= 0.1, F("%d angles, worst error %.4f; %s", measured, worst, got.c_str()));
    Check("steer: left is negative and right positive (the wire's -1 left .. +1 right), straight is 0",
          signOk && std::fabs(EstimateSteer(15.0f, 0.0f, float(L), 0.7f)) < 1e-6f, "");

    // reversing: the same wheel angle turns the car the other way round, the estimate still follows the wheels
    measured = 0;
    worst = 0;
    for (double d : deltas) {
        const double v = -3.0, w = v * std::tan(d) / L;
        const float est = EstimateSteer(float(v), float(w), float(L), 0.0f);
        ++measured;
        worst = std::max(worst, std::fabs(double(est) - (-d / lock)));
    }
    Check("steer: reversing at 3 m/s gives the same wheel angles (signed speed, not atan2 of a negative one)",
          measured == nDeltas && worst <= 0.1, F("%d angles, worst error %.4f", measured, worst));

    // below 1 m/s the estimate is noise: the previous value is held (either direction)
    const float a = EstimateSteer(0.99f, 3.0f, float(L), 0.37f);
    const float b = EstimateSteer(-0.5f, -2.0f, float(L), -0.42f);
    const float c = EstimateSteer(0.0f, 1.0f, float(L), 0.1f);
    const float d1 = EstimateSteer(1.0f, 0.2f, float(L), 0.9f);   // exactly 1 m/s is estimated
    Check("steer: below 1 m/s the previous value is held (0.99 -> 0.37, -0.5 -> -0.42, 0 -> 0.1); at 1 m/s it is estimated",
          a == 0.37f && b == -0.42f && c == 0.1f && d1 != 0.9f && std::fabs(d1) <= 1.0f,
          F("%g %g %g %g", a, b, c, d1));

    // a hard lock saturates at +-1, a bad input holds the previous value and never returns NaN
    const float hardL = EstimateSteer(5.0f, 3.0f, float(L), 0.0f);    // far more yaw than the lock allows
    const float hardR = EstimateSteer(5.0f, -3.0f, float(L), 0.0f);
    const float nanV = EstimateSteer(NAN, 0.3f, float(L), 0.25f);
    const float nanW = EstimateSteer(15.0f, INFINITY, float(L), 0.25f);
    const float noL = EstimateSteer(15.0f, 0.3f, 0.0f, 0.25f);
    const float nanPrev = EstimateSteer(0.0f, 0.0f, float(L), NAN);
    Check("steer: saturates at -1 / +1; NaN, infinite or zero-wheelbase input holds the previous value; a NaN previous is 0",
          hardL == -1.0f && hardR == 1.0f && nanV == 0.25f && nanW == 0.25f && noL == 0.25f && nanPrev == 0.0f,
          F("%g %g %g %g %g %g", hardL, hardR, nanV, nanW, noL, nanPrev));
}

// ---- 8. sync plan W6: aged stamps, repeats, sessions, the controller's send gate ---------------

// A 30 Hz sender (integer-ms sample times) relayed by a 20 Hz server tick: the server carries the newest sample it
// holds and its age (tick time - receipt time, whole ms), the client stamps tick * tickMs - ageMs. The uplink is a
// constant 20 ms, so the stamped time is the sample time + 20 and the truth at a stamp is Circle(stamp - 20).
// Returns the largest position error of the interpolated, rendered pose; stamped = false uses tick * tickMs alone.
static double RelayedCircleErrorM(bool aged, int* frames) {
    constexpr double kTickMs = 50.0, kUplinkMs = 20.0;
    std::vector<int> sampleMs;
    for (int k = 0; k * 1000 / 30 <= 12000; ++k) sampleMs.push_back(k * 1000 / 30);
    VehicleInterpolator ip;
    size_t newest = 0;
    double worst = 0;
    *frames = 0;
    int64_t tick = 0;
    for (double now = 0; now < 11000; now += 1000.0 / 60.0) {
        while (double(tick + 1) * kTickMs <= now) {
            ++tick;
            const double tickT = double(tick) * kTickMs;
            while (newest + 1 < sampleMs.size() && sampleMs[newest + 1] + kUplinkMs <= tickT) ++newest;
            const double recv = sampleMs[newest] + kUplinkMs;
            const uint32_t age = uint32_t(std::min(255.0, std::floor(tickT - recv)));
            const Truth tr = Circle(sampleMs[newest]);
            VehicleSnapshot s;
            s.tMs = aged ? SnapshotStampMs(tick, kTickMs, age) : tickT;
            s.pos = tr.pos; s.vel = tr.vel; s.rot = tr.rot; s.hasAngVel = true; s.angVel = tr.angVel; s.steering = tr.steering;
            ip.Push(s);
        }
        const double rt = now - 150.0; // a fixed delay behind the receipt
        VehicleSample out;
        if (rt < 1000 || !ip.Sample(rt, out)) continue;
        ++*frames;
        // the stamped timeline is the sample time + the uplink (aged) or the tick time (not): both are compared
        // against where the car really was at the instant the stamp claims, i.e. the aged reading of the truth
        worst = std::max(worst, Dist(out.pos, Circle(rt - kUplinkMs).pos));
    }
    return worst;
}

static void TestW6() {
    // a bit-identical motion with a later stamp is not new information (the smoother's Repeat)
    {
        VehicleInterpolator ip;
        VehicleSnapshot s;
        s.tMs = 100; s.pos = V(1, 2, 3); s.vel = V(0, 0, 0); s.rot = vq::FromHeadingDeg(30); s.steering = 0.1f;
        const VehPush a = ip.Push(s);
        VehicleSnapshot r = s;
        r.tMs = 150;
        const VehPush rep = ip.Push(r);
        const int afterRepeat = ip.Count();
        r.tMs = 200; r.pos.y += 0.5f;                    // moved: new information
        const VehPush moved = ip.Push(r);
        VehicleSnapshot st = r;
        st.tMs = 250; st.steering = 0.3f;                // only the steering differs: still new information
        const VehPush steer = ip.Push(st);
        VehicleSnapshot old = s;
        old.tMs = 90;                                    // older and identical: Stale, not Repeat
        const VehPush stale = ip.Push(old);
        Check("push: identical motion with a later stamp is Repeat (not stored, counted); any change is Accepted; older is Stale",
              a == VehPush::Accepted && rep == VehPush::Repeat && afterRepeat == 1 && moved == VehPush::Accepted &&
                  steer == VehPush::Accepted && stale == VehPush::Stale && ip.Stats().repeats == 1 && ip.Count() == 3,
              F("%d %d count %d %d %d %d repeats %u count %d", int(a), int(rep), afterRepeat, int(moved), int(steer), int(stale),
                (unsigned)ip.Stats().repeats, ip.Count()));
    }
    // a new session starts a new timeline: the server restarted, its tick counter is low again
    {
        VehicleInterpolator ip;
        VehicleSnapshot s;
        s.vel = V(10, 0, 0); s.rot = vq::FromHeadingDeg(0);
        for (int i = 0; i < 5; ++i) { s.tMs = 7200000.0 + 50.0 * i; s.pos = V(0.5f * i, 0, 0); ip.Push(s); }
        s.tMs = 50; s.pos = V(0, 0, 0);
        const VehPush sameSession = ip.Push(s);              // still the old session: the low timeline is Stale
        const uint32_t oldId = ip.Session();
        ip.SetSession(oldId);                                // the same id changes nothing
        const int keptCount = ip.Count();
        ip.SetSession(oldId + 1);
        const int clearedCount = ip.Count();
        const VehPush newSession = ip.Push(s);
        s.tMs = 100; s.pos = V(0.5, 0, 0);
        const VehPush next = ip.Push(s);
        Check("session: a low timeline is Stale inside a session and Accepted after SetSession(new id); the same id keeps the buffer",
              sameSession == VehPush::Stale && keptCount == 5 && clearedCount == 0 && newSession == VehPush::Accepted &&
                  next == VehPush::Accepted && ip.Session() == oldId + 1 && ip.Count() == 2,
              F("%d kept %d cleared %d %d %d session %u count %d", int(sameSession), keptCount, clearedCount, int(newSession),
                int(next), (unsigned)ip.Session(), ip.Count()));
    }
    // sync plan W7: the typical spacing of stored snapshots, the class extra on the shared render clock (netclock.h)
    {
        VehicleInterpolator ip;
        const double none = ip.TypicalGapMs();                 // nothing stored yet
        VehicleSnapshot s;
        s.vel = V(10, 0, 0); s.rot = vq::FromHeadingDeg(0);
        // a car relayed every 3rd tick of a 30 Hz server (100 ms), with one 66 ms and one 133 ms spacing among them
        const double stamps[] = {1000, 1100, 1200, 1266.7, 1400, 1500, 1600, 1700, 1800};
        for (double t : stamps) { s.tMs = t; s.pos.x += 1.0f; ip.Push(s); }
        const double typical = ip.TypicalGapMs();
        s.tMs = 1850; s.pos.x += 1.0f;                         // a moved car: stored, a 50 ms spacing
        ip.Push(s);
        const double beforeRepeat = ip.TypicalGapMs();
        VehicleSnapshot r = s;                                 // bit-identical motion, later stamp: Repeat, not stored
        r.tMs = 1900;
        const VehPush rep = ip.Push(r);
        const double afterRepeat = ip.TypicalGapMs();
        VehicleSnapshot hole = s;                              // a 5 s hole: the buffer is cleared, the spacing kept
        hole.tMs = 1850 + 5000; hole.pos.x += 100.0f;
        ip.Push(hole);
        const double afterHole = ip.TypicalGapMs();
        ip.SetSession(ip.Session() + 1);                       // a new session forgets it
        const double afterSession = ip.TypicalGapMs();
        const double extra3 = ClassExtraMs(typical, 1000.0 / 30.0), extra1 = ClassExtraMs(33.3, 1000.0 / 30.0);
        Check("typical gap: the median stored spacing (100 ms for a car relayed every 3rd 30 Hz tick, outliers ignored), a Repeat is not counted, a long hole keeps it, a new session forgets it; ClassExtraMs maps it to 2 ticks / 0",
              none == 0.0 && std::fabs(typical - 100.0) < 0.5 && rep == VehPush::Repeat && afterRepeat == beforeRepeat &&
                  std::fabs(beforeRepeat - 100.0) < 0.5 && afterHole == afterRepeat && afterSession == 0.0 &&
                  std::fabs(extra3 - 2000.0 / 30.0) < 1e-9 && extra1 == 0.0,
              F("none %.1f typical %.2f repeat %d before %.2f after %.2f afterHole %.2f afterSession %.1f extra3 %.2f extra1 %.2f", none, typical,
                int(rep), beforeRepeat, afterRepeat, afterHole, afterSession, extra3, extra1));
    }
    // the stamp formula
    {
        const double a = SnapshotStampMs(100, 50.0, 30);
        const double b = SnapshotStampMs(100, 50.0, 0);
        const double c = SnapshotStampMs(100, 1000.0 / 30.0, 255);
        Check("stamp: tick * tickMs - ageMs (age 0 is the tick time; 255 is the cap)",
              a == 4970.0 && b == 5000.0 && std::fabs(c - (100 * 1000.0 / 30.0 - 255)) < 1e-9, F("%.3f %.3f %.3f", a, b, c));
    }
    // the stamps matter: a 30 Hz sender relayed at 20 Hz
    {
        int fa = 0, ft = 0;
        const double aged = RelayedCircleErrorM(true, &fa);
        const double tickOnly = RelayedCircleErrorM(false, &ft);
        Check("30 Hz sender relayed at 20 Hz on the circle: aged stamps within 5 cm", fa > 400 && aged < 0.05, F("frames %d error %.4f m", fa, aged));
        Check("... and tick stamps alone are measurably worse (> 20 cm), so the check is sensitive", ft > 400 && tickOnly > 0.20,
              F("frames %d error %.4f m", ft, tickOnly));
    }
    // the controller's send gate: 5 Hz still, immediately on a change or when it stops, every call while moving
    {
        VehicleSendGate g;
        std::vector<double> sentAt;
        auto step = [&](double t, bool moving, bool changed) { if (g.Due(t, moving, changed)) sentAt.push_back(t); };
        for (double t = 0; t < 1000; t += 33) step(t, false, false);                  // still: first at once, then 5 Hz
        const size_t stillSends = sentAt.size();
        sentAt.clear();
        step(1010, false, true);                                                       // a change: immediately
        step(1020, false, false);                                                      // ... and not again
        const size_t changeSends = sentAt.size();
        sentAt.clear();
        for (double t = 1100; t < 1400; t += 33) step(t, true, false);                // moving: every call (10 of them)
        const size_t movingSends = sentAt.size();
        sentAt.clear();
        step(1500, false, false);                                                      // it stopped: one at once (the stop)
        step(1533, false, false);                                                      // then back to 5 Hz
        const size_t stopSends = sentAt.size();
        g.Reset();
        sentAt.clear();
        step(5000, false, false);                                                      // a new controller sends at once
        const size_t resetSends = sentAt.size();
        Check("send gate: still 5 Hz (first at once), a change goes out at once, moving sends every call, a stop goes out at once",
              stillSends == 5 && changeSends == 1 && movingSends == 10 && stopSends == 1 && resetSends == 1,
              F("still %zu change %zu moving %zu stop %zu reset %zu", stillSends, changeSends, movingSends, stopSends, resetSends));
        Check("send gate: IsMoving thresholds (0.2 m/s linear, 0.05 rad/s angular)",
              !VehicleIsMoving(0.1f, 0.01f) && VehicleIsMoving(0.5f, 0.0f) && VehicleIsMoving(0.0f, 0.2f) && !VehicleIsMoving(NAN, NAN),
              "");
    }
}

// ---- hyper tune, part 2: the per-model profiles and the driving assists (GI 2026-11-04: /car dubsta) -----------
// Pure numbers: HyperAssistStep is the whole of what the engine writes each frame. The engine's native calls (strong body,
// the velocity read-back) are not testable here; the log lines in apps/engine/engine.cpp are their proof in a live run.
namespace hypertest {
    // A level car going `speedMs` along its nose (+y), a wheel on the road, no slide, no yaw, nothing pressed.
    static HyperAssistIn Level(double speedMs, uint32_t mask, double dt = 1.0 / 60.0) {
        HyperAssistIn in;
        in.vel = { 0.0f, (float)speedMs, 0.0f };
        in.fwd = { 0.0f, 1.0f, 0.0f };
        in.right = { 1.0f, 0.0f, 0.0f };
        in.up = { 0.0f, 0.0f, 1.0f };
        in.angVel = { 0.0f, 0.0f, 0.0f };
        in.dt = (float)dt;
        in.grounded = true;
        in.mask = mask;
        return in;
    }
    // Rolled by `deg` about the nose: right and up tilt in the x-z plane (up.x = -sin, up.z = cos).
    static void Bank(HyperAssistIn& in, double deg) {
        const double s = std::sin(deg * kPi / 180.0), c = std::cos(deg * kPi / 180.0);
        in.right = { (float)c, 0.0f, (float)s };
        in.up = { (float)-s, 0.0f, (float)c };
    }
    static HyperVec Sub(const HyperVec& a, const HyperVec& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    static double Norm3(const HyperVec& a) { return std::sqrt((double)a.x * a.x + (double)a.y * a.y + (double)a.z * a.z); }
    static bool Same(const HyperVec& a, const HyperVec& b) { return std::memcmp(&a, &b, sizeof(HyperVec)) == 0; }   // bit for bit (NaN included)
    static bool Unchanged(const HyperAssistIn& in, const HyperAssistOut& o) {
        return Same(o.vel, in.vel) && Same(o.angVel, in.angVel) && !o.velChanged && !o.angChanged && !o.yawClamped;
    }

    // A slide: the car goes `fwd` m/s along its nose with `side` m/s sideways, fed back frame by frame at `fps`.
    struct SlideRun { double sec = -1.0, endSide = 0.0; int frames = 0; };
    static SlideRun Slide(const HyperAssist& a, double fwd, double side, double fps, uint32_t mask, double seconds) {
        SlideRun r;
        HyperVec vel = { (float)side, (float)fwd, 0.0f };
        const int n = (int)std::lround(seconds * fps);
        for (int i = 0; i < n; ++i) {
            HyperAssistIn in = Level(0.0, mask, 1.0 / fps);
            in.vel = vel;
            vel = HyperAssistStep(a, in).vel;
            ++r.frames;
            if (r.sec < 0.0 && std::fabs(vel.x) < 1.0) r.sec = (i + 1) / fps;
        }
        r.endSide = vel.x;
        return r;
    }
    // Full brake from `v0` m/s (nothing else pressed) for `seconds`; the speed at the end and the number of frames.
    static double BrakeRun(const HyperAssist& a, double v0, double fps, double seconds, int& frames) {
        HyperVec vel = { 0.0f, (float)v0, 0.0f };
        frames = 0;
        const int n = (int)std::lround(seconds * fps);
        for (int i = 0; i < n; ++i) {
            HyperAssistIn in = Level(0.0, kHyperAssistBrake, 1.0 / fps);
            in.vel = vel;
            in.brake = 1.0f;
            vel = HyperAssistStep(a, in).vel;
            ++frames;
        }
        return Norm3(vel);
    }
    // A roll rate of `roll` rad/s (about the world y axis, the level car's nose) and a yaw rate of `yaw`, damped at 100 m/s.
    static HyperVec TiltRun(const HyperAssist& a, double roll, double yaw, double fps, double seconds, int& frames) {
        HyperVec w = { 0.0f, (float)roll, (float)yaw };
        frames = 0;
        const int n = (int)std::lround(seconds * fps);
        for (int i = 0; i < n; ++i) {
            HyperAssistIn in = Level(100.0, kHyperAssistTilt, 1.0 / fps);
            in.angVel = w;
            w = HyperAssistStep(a, in).angVel;
            ++frames;
        }
        return w;
    }
}

static void TestHyperProfiles() {
    using namespace hypertest;
    const HyperProfile* jesko = HyperProfileFor("jesko");
    const HyperProfile* dubsta = HyperProfileFor("dubsta");
    if (!jesko || !dubsta) {
        Check("hyper profile: the jesko and dubsta rows exist", false);
        return;
    }
    // The assist MATH below is proven on a fixed fixture (the 0.4.13 numbers at a 500 km/h reference), so retuning the
    // production profile (GI 2026-11-05) cannot weaken it; the production numbers are pinned separately right here.
    constexpr double kFixRefMs = 500.0 / 3.6;
    HyperAssist fix;
    fix.downRefMs = (float)kFixRefMs;
    fix.downAtRefMs2 = 10.0f;
    fix.downCapMs2 = 13.0f;
    fix.gripRate = 5.0f;
    fix.gripCapMs2 = 22.0f;
    fix.brakeMs2 = 12.0f;
    fix.yawLatMs2 = 22.0f;
    fix.tiltRate = 3.0f;
    const HyperAssist& A = fix;
    const HyperAssist& P = jesko->assist;

    // -- the production tune, pinned (GI 2026-11-05: Jesko, 600 km/h, power 2.35 = 3.5 - 33 %, handling +15 %) --
    Check("hyper profile: the tune is 600 km/h, power x2.35, +400 % cap; the Jesko row has toughness and the firmer assists",
          kHyperTargetKmh == 600.0f && kHyperPower == 2.35f && kHyperMaxPercent == 400.0f && jesko->targetKmh == 600.0f &&
              jesko->power == 2.35f && jesko->tough && HyperTargetMs(*jesko) == kHyperTargetMs &&
              P.downRefMs == kHyperTargetMs && P.downAtRefMs2 == 14.0f && P.downCapMs2 == 17.0f && P.gripRate == 6.9f && P.gripCapMs2 == 30.0f &&
              P.brakeMs2 == 14.0f && P.yawLatMs2 == 30.0f && P.tiltRate == 4.6f &&
              std::fabs(HyperTopSpeedPercent(100.0f) - 66.66667f) < 1e-3f && std::fabs(HyperTopSpeedPercent(50.0f) - 233.3333f) < 1e-2f &&
              HyperTopSpeedPercent(10.0f) == 400.0f && HyperTopSpeedPercent(170.0f) == 0.0f && HyperTopSpeedPercent(0.0f) == 100.0f,
          F("pct(100) %.4f pct(50) %.4f", HyperTopSpeedPercent(100.0f), HyperTopSpeedPercent(50.0f)));
    HyperAssistIn rich = Level(100.0, kHyperAssistAll);   // a car that every assist would act on: a slide, the pedal, a spin, a roll rate
    rich.vel.x = 5.0f;
    rich.brake = 1.0f;
    rich.angVel = { 2.0f, 1.0f, 1.0f };
    const HyperAssistOut jo = HyperAssistStep(P, rich);
    const HyperAssistOut dout = HyperAssistStep(dubsta->assist, rich);
    Check("hyper profile: the Jesko's assists act (a sliding, braking, spinning car is corrected), and the Dubsta stand-in gets the very same result",
          jo.velChanged && jo.angChanged && dout.vel.x == jo.vel.x && dout.vel.y == jo.vel.y && dout.vel.z == jo.vel.z &&
              dout.angVel.x == jo.angVel.x && dout.angVel.y == jo.angVel.y && dout.angVel.z == jo.angVel.z,
          F("jesko vel %d ang %d", (int)jo.velChanged, (int)jo.angChanged));
    Check("hyper profile: the production grip + brake stay under the impact tracker's 1.5 m/s per frame ((30 + 14) / 30 = 1.47)",
          (P.gripCapMs2 + P.brakeMs2) * kHyperMaxDt < 1.5f && (dubsta->assist.gripCapMs2 + dubsta->assist.brakeMs2) * kHyperMaxDt < 1.5f,
          F("%.3f m/s", (P.gripCapMs2 + P.brakeMs2) * kHyperMaxDt));

    // -- lookup and table invariants --
    Check("hyper profile: lookup by /car name finds the row (lower case), nothing for an unknown, mixed-case or empty name or null",
          HyperProfileFor("jesko") == &kHyperProfiles[0] && HyperProfileFor("dubsta") == &kHyperProfiles[1] && !HyperProfileFor("adder") &&
              !HyperProfileFor("Dubsta") && !HyperProfileFor("") && !HyperProfileFor(nullptr),
          "");
    // 2026-11-05: the pack cars joined (6 rows; two are pack-wide, model nullptr). Every row names a model or a pack (or
    // both), names are lower case and unique per kind, the target sits under the server cap, and every assist set keeps
    // one frame's grip + brake under the impact tracker's 1.5 m/s.
    size_t looked = 0, packRows = 0;
    bool tableOk = kHyperProfileCount == 18;
    const auto lower = [](const char* s) { for (; s && *s; ++s) if (*s >= 'A' && *s <= 'Z') return false; return true; };
    const auto same = [](const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; };
    for (size_t i = 0; i < kHyperProfileCount; ++i) {
        ++looked;
        const HyperProfile& p = kHyperProfiles[i];
        if (p.pack) ++packRows;
        tableOk = tableOk && ((p.model && *p.model) || (p.pack && *p.pack)) && lower(p.model) && lower(p.pack);
        tableOk = tableOk && HyperTargetMs(p) > 0.0f && HyperTargetMs(p) <= 170.0f && p.power >= 1.0f;   // 170 m/s: the shipped vehicleMaxSpeed (apps/server/config.h)
        tableOk = tableOk && (p.assist.gripCapMs2 + p.assist.brakeMs2) * kHyperMaxDt < 1.5f;
        tableOk = tableOk && (p.model ? HyperProfileFor(p.model) == &p : true) && (p.pack ? HyperProfileForPack(p.pack) == &p : true);
        for (size_t j = i + 1; j < kHyperProfileCount; ++j)
            tableOk = tableOk && !same(p.model, kHyperProfiles[j].model) && !same(p.pack, kHyperProfiles[j].pack);
    }
    tableOk = tableOk && packRows == 4 && !HyperProfileForPack("") && !HyperProfileForPack(nullptr) && !HyperProfileForPack("rs6") &&
              HyperProfileForPack("vehicle_supra") && !HyperProfileForPack("vehicle_supra")->model &&
              std::strcmp(HyperProfileName(*HyperProfileForPack("vehicle_supra")), "vehicle_supra") == 0 &&
              std::strcmp(HyperProfileName(kHyperProfiles[0]), "jesko") == 0 && !HyperProfileForPack("vehicle_supra")->tough;
    Check("hyper profile: every row names a model or a pack (lower case, unique), power >= 1, a target within the server's default 170 m/s "
          "cap and a frame-bounded assist; the pack rows resolve by pack name",
          tableOk && looked == kHyperProfileCount, F("%zu rows, %zu pack rows", looked, packRows));
    {
        // GI 2026-11-05: road cars bottomed out under the downforce assist and accelerated too hard: no downforce, power 1.0,
        // the +15 % grip set; the Regera's power is 2/3 of the old 2.4.
        bool roadOk = true;
        int roadRows = 0;
        for (size_t i = 0; i < kHyperProfileCount; ++i) {
            const HyperProfile& p = kHyperProfiles[i];
            if (p.tough || p.targetKmh >= 400.0f) continue;
            ++roadRows;
            roadOk = roadOk && p.power == 1.0f && p.assist.downAtRefMs2 == 0.0f && p.assist.gripCapMs2 == 13.8f &&
                     p.assist.yawLatMs2 == 18.4f && p.assist.Any();
        }
        const HyperProfile* regera = HyperProfileFor("regera");
        HyperAssistIn fast = Level(80.0, kHyperAssistAll);
        const HyperAssistOut ro = HyperAssistStep(HyperProfileFor("rs6")->assist, fast);
        Check("hyper profile: the road cars run at power 1.0 with no downforce (a level car at 80 m/s gets none) and the firmer grip set; "
              "the Regera is at 1.6",
              roadOk && roadRows == 15 && regera && regera->power == 1.6f && ro.downMs2 == 0.0f,
              F("%d road rows, rs6 down %.2f", roadRows, (double)ro.downMs2));
    }
    Check("hyper profile: the Dubsta stand-in has the Jesko's acceleration (power), top speed (600 km/h, inside 400..612), toughness and assists",
          dubsta->power == jesko->power && dubsta->targetKmh == jesko->targetKmh && dubsta->targetKmh >= 400.0f && dubsta->targetKmh <= 612.0f &&
              dubsta->tough && dubsta->assist.Any(),
          F("power %.1f target %.0f", dubsta->power, dubsta->targetKmh));
    bool pctOk = true;
    int pcts = 0;
    for (float est : { 40.0f, 45.0f, 50.0f, 55.0f, 60.0f }) {   // the stock Dubsta is about 45 m/s (160 km/h)
        const float pct = HyperTopSpeedPercent(est, HyperTargetMs(*dubsta));
        pctOk = pctOk && pct > 0.0f && pct < kHyperMaxPercent && std::fabs(est * (1.0f + pct / 100.0f) - HyperTargetMs(*dubsta)) < 0.01f;
        ++pcts;
    }
    Check("hyper profile: a stock Dubsta estimate of 40..60 m/s is lifted exactly to the target, not held by the +400 % cap (it binds below 33.3 m/s)",
          pctOk && pcts == 5 && HyperTopSpeedPercent(33.0f, HyperTargetMs(*dubsta)) == kHyperMaxPercent && HyperTopSpeedPercent(34.0f, HyperTargetMs(*dubsta)) < kHyperMaxPercent,
          F("%d estimates", pcts));

    // -- downforce --
    const HyperAssistOut dRef = HyperAssistStep(A, Level(kFixRefMs, kHyperAssistDownforce));
    const HyperAssistOut dHalf = HyperAssistStep(A, Level(kFixRefMs * 0.5, kHyperAssistDownforce));
    const HyperAssistOut dOver = HyperAssistStep(A, Level(kFixRefMs * 1.5, kHyperAssistDownforce));
    Check("hyper assist: downforce is k v^2 (10 m/s^2 at 500 km/h, 2.5 at half the speed), capped at 13, a pull of a*dt along -up",
          std::fabs(dRef.downMs2 - 10.0f) < 0.01f && std::fabs(dHalf.downMs2 - 2.5f) < 0.01f && std::fabs(dOver.downMs2 - 13.0f) < 0.01f &&
              std::fabs(dRef.vel.z - (-10.0f / 60.0f)) < 1e-4f && dRef.vel.x == 0.0f && std::fabs(dRef.vel.y - kFixRefMs) < 1e-3f,
          F("ref %.3f half %.3f over %.3f vz %.5f", dRef.downMs2, dHalf.downMs2, dOver.downMs2, dRef.vel.z));
    HyperAssistIn tilted = Level(100.0, kHyperAssistDownforce);
    Bank(tilted, 25.0);
    const HyperAssistOut tOut = HyperAssistStep(A, tilted);
    const double tAd = 10.0 * std::pow(100.0 / kFixRefMs, 2.0);
    const HyperVec tD = Sub(tOut.vel, tilted.vel);
    Check("hyper assist: on a banked car the downforce pulls along the car's own -up, not the world's",
          std::fabs(tD.x - (-tilted.up.x * tAd / 60.0)) < 1e-4 && std::fabs(tD.z - (-tilted.up.z * tAd / 60.0)) < 1e-4 && std::fabs(tD.y) < 1e-5 &&
              std::fabs(tD.x) > 0.01,
          F("d (%.5f %.5f %.5f) want x %.5f", tD.x, tD.y, tD.z, -tilted.up.x * tAd / 60.0));
    HyperAssistIn air = Level(kFixRefMs, kHyperAssistDownforce);
    air.grounded = false;
    HyperAssistIn inverted = Level(kFixRefMs, kHyperAssistDownforce);
    inverted.right = { -1.0f, 0.0f, 0.0f };
    inverted.up = { 0.0f, 0.0f, -1.0f };
    HyperAssistIn onSide = Level(kFixRefMs, kHyperAssistDownforce);
    Bank(onSide, 70.0);
    HyperAssistIn banked50 = Level(kFixRefMs, kHyperAssistDownforce);
    Bank(banked50, 50.0);
    const HyperAssistIn maskedOut = Level(kFixRefMs, kHyperAssistAll & ~kHyperAssistDownforce);
    Check("hyper assist: no downforce in the air, upside down, on its side (up.z < 0.5) or with the bit off; still on at 50 degrees of bank",
          Unchanged(air, HyperAssistStep(A, air)) && Unchanged(inverted, HyperAssistStep(A, inverted)) && Unchanged(onSide, HyperAssistStep(A, onSide)) &&
              Unchanged(maskedOut, HyperAssistStep(A, maskedOut)) && HyperAssistStep(A, banked50).downMs2 > 9.0f,
          "");

    // -- lateral grip --
    HyperAssistIn gp = Level(0.0, kHyperAssistGrip);
    gp.vel = { 6.0f, 60.0f, 0.0f };
    HyperAssistIn gn = gp;
    gn.vel.x = -6.0f;
    HyperAssistIn gs = gp;
    gs.vel.x = 2.0f;
    const HyperAssistOut go = HyperAssistStep(A, gp), gno = HyperAssistStep(A, gn), gso = HyperAssistStep(A, gs);
    Check("hyper assist: grip removes the slide at 22 m/s^2 when that is the limit (6 m/s), at 5 /s below it (2 m/s), either side, the nose speed untouched",
          std::fabs(go.vel.x - (6.0f - 22.0f / 60.0f)) < 1e-4f && std::fabs(gno.vel.x + (6.0f - 22.0f / 60.0f)) < 1e-4f &&
              go.vel.y == 60.0f && go.vel.z == 0.0f && std::fabs(go.gripMs2 - 22.0f) < 1e-3f &&
              std::fabs(gso.vel.x - (2.0f - 10.0f / 60.0f)) < 1e-4f && std::fabs(gso.gripMs2 - 10.0f) < 1e-3f,
          F("+6 -> %.4f, -6 -> %.4f, +2 -> %.4f", go.vel.x, gno.vel.x, gso.vel.x));
    const double v30 = 30.0, v9 = 9.0;
    HyperAssistIn r30 = Level(0.0, kHyperAssistGrip);
    r30.vel = { 6.0f, (float)std::sqrt(v30 * v30 - 36.0), 0.0f };
    HyperAssistIn r9 = Level(0.0, kHyperAssistGrip);
    r9.vel = { 6.0f, (float)std::sqrt(v9 * v9 - 36.0), 0.0f };
    const HyperAssistOut r30o = HyperAssistStep(A, r30);
    Check("hyper assist: grip fades in with speed (half the cap at 30 m/s, nothing at 9 m/s)",
          std::fabs(r30o.gripMs2 - 11.0f) < 0.05f && Unchanged(r9, HyperAssistStep(A, r9)), F("30 m/s %.3f", r30o.gripMs2));
    HyperAssist hardGrip = A;
    hardGrip.gripRate = 100.0f;                               // a rate that would overshoot zero in a single frame
    HyperAssistIn os = Level(0.0, kHyperAssistGrip, 1.0 / 30.0);
    os.vel = { 0.5f, 60.0f, 0.0f };
    HyperAssistIn osn = os;
    osn.vel.x = -0.5f;
    const HyperAssistOut oso = HyperAssistStep(hardGrip, os), osno = HyperAssistStep(hardGrip, osn);
    Check("hyper assist: grip never overshoots past zero slide (a 0.5 m/s slide ends at 0, not on the other side), either sign",
          std::fabs(oso.vel.x) < 1e-6f && std::fabs(osno.vel.x) < 1e-6f, F("%.5f / %.5f", oso.vel.x, osno.vel.x));
    const SlideRun s30 = Slide(A, 100.0, 8.0, 30.0, kHyperAssistGrip, 1.0);
    const SlideRun s60 = Slide(A, 100.0, 8.0, 60.0, kHyperAssistGrip, 1.0);
    const SlideRun s144 = Slide(A, 100.0, 8.0, 144.0, kHyperAssistGrip, 1.0);
    const SlideRun sOff = Slide(A, 100.0, 8.0, 60.0, 0, 1.0);
    const double sMax = std::max(s30.sec, std::max(s60.sec, s144.sec)), sMin = std::min(s30.sec, std::min(s60.sec, s144.sec));
    Check("hyper assist: an 8 m/s slide at 100 m/s is gone (<1 m/s) in 0.35..0.6 s at 30, 60 and 144 fps within 0.08 s of each other; assists off, it stays",
          s30.frames == 30 && s60.frames == 60 && s144.frames == 144 && sMin > 0.35 && sMax < 0.6 && sMax - sMin < 0.08 &&
              sOff.frames == 60 && sOff.sec < 0.0 && sOff.endSide == 8.0,
          F("30 fps %.3f s, 60 fps %.3f s, 144 fps %.3f s, off end %.2f", s30.sec, s60.sec, s144.sec, sOff.endSide));

    // -- brake assist --
    HyperAssistIn br = Level(80.0, kHyperAssistBrake);
    br.brake = 1.0f;
    HyperAssistIn brHalf = br;
    brHalf.brake = 0.5f;
    HyperAssistIn brDiag = br;
    const float diag = std::sqrt(0.5f);
    brDiag.fwd = { diag, diag, 0.0f };
    brDiag.right = { diag, -diag, 0.0f };
    brDiag.vel = { 80.0f * diag, 80.0f * diag, 0.0f };
    const HyperAssistOut bo = HyperAssistStep(A, br), bHalf = HyperAssistStep(A, brHalf), bDiag = HyperAssistStep(A, brDiag);
    Check("hyper assist: the brake assist is 12 m/s^2 at full pedal, 6 at half, along the direction of travel (a diagonal car keeps its heading)",
          std::fabs(bo.vel.y - (80.0f - 0.2f)) < 1e-4f && std::fabs(bo.brakeMs2 - 12.0f) < 1e-3f && std::fabs(bHalf.brakeMs2 - 6.0f) < 1e-3f &&
              std::fabs(Norm3(bDiag.vel) - 79.8) < 1e-3 && std::fabs(bDiag.vel.x - bDiag.vel.y) < 1e-4f && bo.vel.x == 0.0f,
          F("vy %.4f, 12/6 -> %.3f/%.3f, diag |v| %.4f", bo.vel.y, bo.brakeMs2, bHalf.brakeMs2, Norm3(bDiag.vel)));
    std::string bad;
    {
        HyperAssistIn x = br;
        if (HyperAssistStep(A, x).brakeMs2 <= 0.0f) bad += " control";
        x = br; x.accel = 0.6f;
        if (HyperAssistStep(A, x).brakeMs2 > 0.0f) bad += " throttle";
        x = br; x.brake = 0.03f;
        if (HyperAssistStep(A, x).brakeMs2 > 0.0f) bad += " light-pedal";
        x = br; x.vel = { 0.0f, -30.0f, 0.0f };
        if (HyperAssistStep(A, x).brakeMs2 > 0.0f) bad += " reversing";
        x = Level(0.0, kHyperAssistBrake); x.brake = 1.0f; x.vel = { 30.0f, 3.0f, 0.0f };   // sliding sideways: 3 m/s along the nose
        if (HyperAssistStep(A, x).brakeMs2 > 0.0f) bad += " sideways";
        x = Level(9.0, kHyperAssistBrake); x.brake = 1.0f;
        if (HyperAssistStep(A, x).brakeMs2 > 0.0f) bad += " no-ramp";
        x = br; x.grounded = false;
        if (HyperAssistStep(A, x).brakeMs2 > 0.0f) bad += " airborne";
        x = br; x.mask = kHyperAssistAll & ~kHyperAssistBrake;
        if (HyperAssistStep(A, x).brakeMs2 > 0.0f) bad += " bit-off";
    }
    Check("hyper assist: no brake assist on the throttle, on a light pedal, reversing, under 5 m/s along the nose, under 10 m/s, airborne or with the bit off",
          bad.empty(), bad);
    HyperAssist hardBrake = A;
    hardBrake.brakeMs2 = 1000.0f;
    hardBrake.rampFromMs = 0.0f;
    hardBrake.rampToMs = 1.0f;
    HyperAssistIn stop = Level(6.0, kHyperAssistBrake, 1.0 / 30.0);
    stop.brake = 1.0f;
    const HyperAssistOut stopO = HyperAssistStep(hardBrake, stop);
    Check("hyper assist: the brake assist stops the car and never reverses it (a 33 m/s^2 shove on a 6 m/s car ends at 0, not -27)",
          stopO.vel.y >= 0.0f && stopO.vel.y < 1e-5f && stopO.vel.x == 0.0f, F("vy %.6f", stopO.vel.y));
    int bf30 = 0, bf144 = 0;
    const double b30 = BrakeRun(A, 80.0, 30.0, 1.0, bf30), b144 = BrakeRun(A, 80.0, 144.0, 1.0, bf144);
    Check("hyper assist: a second of full brake from 80 m/s costs 12 m/s at 30 and at 144 fps alike",
          bf30 == 30 && bf144 == 144 && std::fabs(b30 - 68.0) < 0.05 && std::fabs(b144 - 68.0) < 0.05 && std::fabs(b30 - b144) < 0.02,
          F("30 fps %.4f, 144 fps %.4f", b30, b144));

    // -- the yaw-rate cap --
    HyperAssistIn yw = Level(100.0, kHyperAssistYaw);
    yw.angVel = { 0.3f, 0.0f, 0.5f };
    HyperAssistIn ywn = yw;
    ywn.angVel.z = -0.5f;
    HyperAssistIn ywLow = yw;
    ywLow.angVel.z = 0.1f;
    const HyperAssistOut yo = HyperAssistStep(A, yw), yno = HyperAssistStep(A, ywn), ylo = HyperAssistStep(A, ywLow);
    Check("hyper assist: the yaw rate is cut to 22 m/s^2 / speed (0.22 rad/s at 100 m/s), either way; pitch kept; a turn under it is left alone",
          std::fabs(yo.angVel.z - 0.22f) < 1e-5f && std::fabs(yno.angVel.z + 0.22f) < 1e-5f && yo.angVel.x == 0.3f && yo.yawClamped && yno.yawClamped &&
              Unchanged(ywLow, ylo),
          F("+0.5 -> %.4f, -0.5 -> %.4f", yo.angVel.z, yno.angVel.z));
    HyperAssistIn ywSlow = Level(15.0, kHyperAssistYaw);
    ywSlow.angVel = { 0.0f, 0.0f, 2.0f };
    HyperAssistIn ywAir = yw;
    ywAir.grounded = false;
    HyperAssistIn ywInv = yw;
    ywInv.right = { -1.0f, 0.0f, 0.0f };
    ywInv.up = { 0.0f, 0.0f, -1.0f };
    HyperAssistIn ywOff = yw;
    ywOff.mask = kHyperAssistTilt;                             // every bit but the yaw cap
    ywOff.angVel = { 0.0f, 0.0f, 2.0f };                       // pure yaw: the anti-roll must not touch it either
    HyperAssistIn ywBank = Level(100.0, kHyperAssistYaw);
    Bank(ywBank, 20.0);
    ywBank.angVel = { ywBank.up.x * 0.5f, 0.0f, ywBank.up.z * 0.5f };
    const HyperAssistOut ywBankO = HyperAssistStep(A, ywBank);
    Check("hyper assist: no yaw cap under 20 m/s, in the air, upside down or with the bit off; on a banked car the cap is about the car's up axis",
          Unchanged(ywSlow, HyperAssistStep(A, ywSlow)) && Unchanged(ywAir, HyperAssistStep(A, ywAir)) && Unchanged(ywInv, HyperAssistStep(A, ywInv)) &&
              Unchanged(ywOff, HyperAssistStep(A, ywOff)) && ywBankO.yawClamped &&
              std::fabs(ywBankO.angVel.x - ywBank.up.x * 0.22f) < 1e-4f && std::fabs(ywBankO.angVel.z - ywBank.up.z * 0.22f) < 1e-4f,
          F("banked (%.4f %.4f) want (%.4f %.4f)", ywBankO.angVel.x, ywBankO.angVel.z, ywBank.up.x * 0.22f, ywBank.up.z * 0.22f));
    int latN = 0;
    bool latOk = true;
    for (int speed = 20; speed <= 150; ++speed)                  // 20 m/s (the cap's start) to the server's 150 m/s
        for (float w : { -3.0f, -1.0f, -0.4f, 0.0f, 0.4f, 1.0f, 3.0f }) {
            HyperAssistIn in = Level((double)speed, kHyperAssistYaw);
            in.angVel = { 0.0f, 0.0f, w };
            const HyperAssistOut o = HyperAssistStep(A, in);
            const double lat = (double)speed * std::fabs(o.angVel.z);
            latOk = latOk && lat <= 22.0 + 1e-3 && (std::fabs((double)speed * w) > 22.0 || o.angVel.z == w);
            ++latN;
        }
    Check("hyper assist: from 20 to 150 m/s a full-lock spin (3 rad/s) never exceeds 22 m/s^2 of lateral acceleration, a gentle turn is untouched",
          latOk && latN == 131 * 7, F("%d cases", latN));

    // -- anti-roll --
    HyperAssistIn ti = Level(100.0, kHyperAssistTilt);
    ti.angVel = { 2.0f, 1.0f, 0.3f };
    HyperAssistIn ti30 = Level(30.0, kHyperAssistTilt);
    ti30.angVel = ti.angVel;
    HyperAssistIn ti5 = Level(5.0, kHyperAssistTilt);
    ti5.angVel = ti.angVel;
    const HyperAssistOut tio = HyperAssistStep(A, ti), ti30o = HyperAssistStep(A, ti30);
    const float f100 = std::exp(-3.0f / 60.0f), f30 = std::exp(-3.0f * 0.5f / 60.0f);
    Check("hyper assist: anti-roll damps pitch and roll by exp(-3 ramp dt) and leaves the yaw rate alone; half strength at 30 m/s, none at 5 m/s",
          std::fabs(tio.angVel.x - 2.0f * f100) < 1e-4f && std::fabs(tio.angVel.y - 1.0f * f100) < 1e-4f && std::fabs(tio.angVel.z - 0.3f) < 1e-6f &&
              std::fabs(ti30o.angVel.x - 2.0f * f30) < 1e-4f && Unchanged(ti5, HyperAssistStep(A, ti5)),
          F("x %.5f (want %.5f) y %.5f z %.6f", tio.angVel.x, 2.0f * f100, tio.angVel.y, tio.angVel.z));
    int tf30 = 0, tf144 = 0;
    const HyperVec t30 = TiltRun(A, 4.0, 0.3, 30.0, 0.5, tf30), t144 = TiltRun(A, 4.0, 0.3, 144.0, 0.5, tf144);
    Check("hyper assist: a 4 rad/s roll is down to 0.89 rad/s in half a second at 30 and at 144 fps alike, the yaw rate unchanged throughout",
          tf30 == 15 && tf144 == 72 && t30.y > 0.85f && t30.y < 0.93f && t144.y > 0.85f && t144.y < 0.93f && std::fabs(t30.y - t144.y) < 0.01f &&
              std::fabs(t30.z - 0.3f) < 1e-5f && std::fabs(t144.z - 0.3f) < 1e-5f,
          F("30 fps %.4f, 144 fps %.4f", t30.y, t144.y));

    // -- bad input, the frame clamp, the mask --
    HyperAssistIn base = rich;
    base.dt = 1.0f / 60.0f;
    std::string badIn;
    {
        const float nan = std::nanf(""), inf = INFINITY;
        if (!HyperAssistStep(A, base).velChanged) badIn += " control";
        HyperAssistIn x = base; x.vel.y = nan;
        if (!Unchanged(x, HyperAssistStep(A, x))) badIn += " nan-vel";
        x = base; x.vel.x = inf;
        if (!Unchanged(x, HyperAssistStep(A, x))) badIn += " inf-vel";
        x = base; x.fwd.y = nan;
        if (!Unchanged(x, HyperAssistStep(A, x))) badIn += " nan-fwd";
        x = base; x.up.z = inf;
        if (!Unchanged(x, HyperAssistStep(A, x))) badIn += " inf-up";
        x = base; x.right = { 0.0f, 0.0f, 0.0f };
        if (!Unchanged(x, HyperAssistStep(A, x))) badIn += " zero-axis";
        x = base; x.dt = nan;
        if (!Unchanged(x, HyperAssistStep(A, x))) badIn += " nan-dt";
        x = base; x.dt = 0.0f;
        if (!Unchanged(x, HyperAssistStep(A, x))) badIn += " zero-dt";
        x = base; x.dt = -0.02f;
        if (!Unchanged(x, HyperAssistStep(A, x))) badIn += " negative-dt";
        x = base; x.mask = 0;
        if (!Unchanged(x, HyperAssistStep(A, x))) badIn += " mask-0";
        x = base; x.angVel.y = nan;                              // a failed angular read: the velocity assists keep working
        const HyperAssistOut o = HyperAssistStep(A, x);
        if (!(o.velChanged && !o.angChanged && std::isfinite(o.vel.x) && std::isfinite(o.vel.y) && std::isfinite(o.vel.z))) badIn += " nan-ang";
    }
    Check("hyper assist: a NaN or infinite velocity, axis or frame time, a zero axis, dt <= 0 or mask 0 returns the input untouched; a NaN angular read keeps the velocity assists",
          badIn.empty(), badIn);
    HyperAssistIn longFrame = base;
    longFrame.dt = 0.2f;
    HyperAssistIn clampedFrame = base;
    clampedFrame.dt = kHyperMaxDt;
    HyperAssistIn shortFrame = base;
    const HyperAssistOut lf = HyperAssistStep(A, longFrame), cf = HyperAssistStep(A, clampedFrame), sf = HyperAssistStep(A, shortFrame);
    Check("hyper assist: a 200 ms hitch is treated as a 33 ms frame (the same result bit for bit), and a 17 ms frame is a smaller step",
          Same(lf.vel, cf.vel) && Same(lf.angVel, cf.angVel) && lf.velChanged && !Same(sf.vel, cf.vel) &&
              std::fabs(cf.vel.y - rich.vel.y) > std::fabs(sf.vel.y - rich.vel.y),
          F("hitch vy %.4f, 33 ms vy %.4f, 17 ms vy %.4f", lf.vel.y, cf.vel.y, sf.vel.y));
    struct MaskCase { uint32_t bit; const char* name; };
    std::string maskBad;
    int maskCases = 0;
    for (const MaskCase& mc : { MaskCase{ kHyperAssistDownforce, "down" }, MaskCase{ kHyperAssistGrip, "grip" }, MaskCase{ kHyperAssistBrake, "brake" },
                                MaskCase{ kHyperAssistYaw, "yaw" }, MaskCase{ kHyperAssistTilt, "tilt" } }) {
        HyperAssistIn x = rich;
        x.mask = mc.bit;
        const HyperAssistOut o = HyperAssistStep(A, x);
        const bool d = o.downMs2 > 0.0f, g = o.gripMs2 > 0.0f, b = o.brakeMs2 > 0.0f;
        bool ok = false;
        if (mc.bit == kHyperAssistDownforce) ok = d && !g && !b && !o.angChanged;
        if (mc.bit == kHyperAssistGrip) ok = !d && g && !b && !o.angChanged;
        if (mc.bit == kHyperAssistBrake) ok = !d && !g && b && !o.angChanged;
        if (mc.bit == kHyperAssistYaw) ok = !o.velChanged && o.yawClamped && o.angChanged;
        if (mc.bit == kHyperAssistTilt) ok = !o.velChanged && !o.yawClamped && o.angChanged;
        if (!ok) maskBad += std::string(" ") + mc.name;
        ++maskCases;
    }
    Check("hyper assist: each mask bit switches on exactly its own assist (downforce, grip, brake, yaw cap, anti-roll)", maskBad.empty() && maskCases == 5, maskBad);

    // -- bounds: what the engine writes in one frame, over random cars --
    Rng rng(0x48595045525455ull);
    int fuzzN = 0, fuzzChanged = 0, fuzzClamped = 0;
    bool fuzzOk = true;
    const double dvBound = (13.0 + 22.0 + 12.0) / 30.0 + 1e-3;   // down cap + grip cap + brake, at the 1/30 s frame clamp
    for (int i = 0; i < 20000; ++i) {
        auto unit = [&]() {
            double x, y, z, l;
            do { x = rng.Range(-1, 1); y = rng.Range(-1, 1); z = rng.Range(-1, 1); l = std::sqrt(x * x + y * y + z * z); } while (l < 0.1);
            return std::array<double, 3>{ x / l, y / l, z / l };
        };
        const auto u = unit();
        auto r0 = unit();
        const double d = r0[0] * u[0] + r0[1] * u[1] + r0[2] * u[2];
        std::array<double, 3> f = { r0[0] - d * u[0], r0[1] - d * u[1], r0[2] - d * u[2] };
        const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (fl < 1e-3) continue;
        for (double& c : f) c /= fl;
        const std::array<double, 3> rt = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };   // right = fwd x up
        const auto vd = unit();
        const double speed = rng.Range(0.0, 300.0);
        HyperAssistIn in;
        in.vel = { (float)(vd[0] * speed), (float)(vd[1] * speed), (float)(vd[2] * speed) };
        in.fwd = { (float)f[0], (float)f[1], (float)f[2] };
        in.right = { (float)rt[0], (float)rt[1], (float)rt[2] };
        in.up = { (float)u[0], (float)u[1], (float)u[2] };
        in.angVel = { (float)rng.Range(-5, 5), (float)rng.Range(-5, 5), (float)rng.Range(-5, 5) };
        in.brake = (float)rng.Range(0, 1);
        in.accel = (float)rng.Range(0, 1);
        in.dt = (float)rng.Range(0.0, 0.2);
        in.grounded = rng.Uniform() < 0.8;
        in.mask = (uint32_t)(rng.Next() % 32);
        const HyperAssistOut o = HyperAssistStep(A, in);
        ++fuzzN;
        const bool fin = std::isfinite(o.vel.x) && std::isfinite(o.vel.y) && std::isfinite(o.vel.z) && std::isfinite(o.angVel.x) &&
                         std::isfinite(o.angVel.y) && std::isfinite(o.angVel.z);
        fuzzOk = fuzzOk && fin && Norm3(Sub(o.vel, in.vel)) <= dvBound;
        if (o.velChanged || o.angChanged) ++fuzzChanged;
        if (o.yawClamped) {
            ++fuzzClamped;
            const double wUp = (double)o.angVel.x * in.up.x + (double)o.angVel.y * in.up.y + (double)o.angVel.z * in.up.z;
            fuzzOk = fuzzOk && std::fabs(wUp) <= 22.0 / o.speed + 1e-3;
        }
    }
    Check("hyper assist: over 20000 random cars, speeds, frames and masks the output is finite, one frame changes the velocity by at most 1.57 m/s, a clamped yaw obeys its limit",
          fuzzOk && fuzzN > 19900 && fuzzChanged > 5000 && fuzzClamped > 500, F("%d cars, %d changed, %d yaw-clamped", fuzzN, fuzzChanged, fuzzClamped));
    int levelN = 0;
    double levelWorst = 0.0;
    for (int s2 = 0; s2 <= 400; ++s2)
        for (float slide : { 0.0f, 3.0f, 8.0f, 30.0f })
            for (float pedal : { 0.0f, 1.0f })
                for (double dt : { 1.0 / 30.0, 1.0 / 60.0, 0.2 }) {
                    HyperAssistIn in = Level(0.0, kHyperAssistAll, dt);
                    in.vel = { slide, (float)(s2 * 0.5), 0.0f };
                    in.brake = pedal;
                    const HyperAssistOut o = HyperAssistStep(A, in);
                    levelWorst = std::max(levelWorst, std::sqrt((double)(o.vel.x - in.vel.x) * (o.vel.x - in.vel.x) + (double)(o.vel.y - in.vel.y) * (o.vel.y - in.vel.y)));
                    ++levelN;
                }
    Check("hyper assist: on the road a frame never changes the horizontal velocity by 1.5 m/s (the impact tracker's hit threshold): bounded by 1.13 m/s",
          levelN == 401 * 4 * 2 * 3 && levelWorst <= (A.gripCapMs2 + A.brakeMs2) * kHyperMaxDt + 1e-3 && (A.gripCapMs2 + A.brakeMs2) * kHyperMaxDt < 1.5f,
          F("%d cases, worst %.4f m/s", levelN, levelWorst));

    // -- the registry mask and the health top-up --
    Check("hyper assist: hyper_assist parses decimal and 0x hex (010 is ten), keeps the five bits, and falls back to all on empty, null or garbage",
          ParseHyperAssistMask(nullptr) == kHyperAssistAll && ParseHyperAssistMask("") == kHyperAssistAll && ParseHyperAssistMask("0") == 0u &&
              ParseHyperAssistMask("5") == 5u && ParseHyperAssistMask("0x1F") == 31u && ParseHyperAssistMask("0X0a") == 10u && ParseHyperAssistMask("010") == 10u &&
              ParseHyperAssistMask("99") == (99u & kHyperAssistAll) && ParseHyperAssistMask("abc") == kHyperAssistAll && ParseHyperAssistMask("7x") == kHyperAssistAll,
          F("010 -> %u, 99 -> %u", ParseHyperAssistMask("010"), ParseHyperAssistMask("99")));
    Check("hyper tune: health is low below 999.5 (damaged, zero, negative), not at 1000 or above, and a failed (NaN) read is not low",
          HyperHealthLow(999.0f) && HyperHealthLow(0.0f) && HyperHealthLow(-50.0f) && HyperHealthLow(500.0f) && !HyperHealthLow(1000.0f) &&
              !HyperHealthLow(999.6f) && !HyperHealthLow(1200.0f) && !HyperHealthLow(std::nanf("")) && !HyperHealthLow(INFINITY),
          "");
}
constexpr int kHyperTuneChecks = 4 + 31;   // TestHyperTune's own four + TestHyperProfiles (keep in step: the run asserts the total)

// ---- hyper tune (libs/sync/hypertune.h, GI 2026-11-02: the Jesko at 500 km/h; 2026-11-05: 600 km/h) ----------------
static void TestHyperTune() {
    using namespace gage::sync;
    // 100 m/s stock (360 km/h) -> +66.7 % reaches 166.7 m/s (600 km/h).
    const float p = HyperTopSpeedPercent(100.0f);
    Check("hyper: a 100 m/s car gets the percentage that lands exactly on 600 km/h",
          std::fabs(100.0f * (1.0f + p / 100.0f) - kHyperTargetMs) < 0.01f, std::to_string(p));
    Check("hyper: a slow car is capped at +400 % (never more than 5x stock)", HyperTopSpeedPercent(10.0f) == kHyperMaxPercent,
          std::to_string(HyperTopSpeedPercent(10.0f)));
    Check("hyper: a car already at or above 600 km/h gets 0, an unreadable estimate gets the safe +100 %",
          HyperTopSpeedPercent(170.0f) == 0.0f && HyperTopSpeedPercent(0.0f) == 100.0f && HyperTopSpeedPercent(-5.0f) == 100.0f);
    Check("hyper: the target stays under the server's default vehicleMaxSpeed (170 m/s, apps/server/config.h), so no update is clamped",
          kHyperTargetMs < 170.0f, std::to_string(kHyperTargetMs));
    TestHyperProfiles();   // GI 2026-11-04: the per-model profiles and the driving assists
}

// ---- 10. W9: contact grace, present-time lead, bike lean ---------------------------------------------------

// A 20 m/s straight car (heading 0, +y) driven by the controller on the SimCar at 60 fps. At 2.0 s a knock changes the
// car's own velocity (sideways +6 m/s; and along the travel direction `lossMs` m/s lost) and, when `flag`, the contact
// flag is true for that one frame. Returns the largest lateral error (m) in [2.0, 2.7] s, the error at 6 s, the number
// of snaps after the first frame, and how many graces opened.
struct KnockResult { double peakLateral = 0, endErr = 0, minWeight = 1; uint32_t extraSnaps = 0, contacts = 0, byFlag = 0, byDecel = 0; int frames = 0; };

static KnockResult RunKnock(bool flag, double lossMs, double graceMs = -1.0, double sideMs = 6.0) {
    VehicleDriveConfig cfg;                              // the defaults the engine uses (graceMs < 0), or an explicit grace (0 = off)
    if (graceMs >= 0.0) cfg.contactGraceMs = float(graceMs);
    VehicleDriveController c(cfg);
    SimCar car;
    car.pos = V(0, 0, 0);
    car.vel = V(0, 20, 0);
    car.rot = vq::FromHeadingDeg(0);
    KnockResult r;
    const double dt = 1000.0 / 60.0;
    bool knocked = false;
    for (double t = 0; t < 6000; t += dt) {
        VehicleSample tg;
        tg.pos = V(0, 20.0 * t / 1000.0, 0);
        tg.vel = V(0, 20, 0);
        tg.rot = vq::FromHeadingDeg(0);
        tg.mode = VehMode::Interpolating;
        bool contact = false;
        if (!knocked && t >= 2000) {
            knocked = true;
            contact = flag;
            car.vel = V(car.vel.x + sideMs, car.vel.y - lossMs, car.vel.z);
        }
        VehicleDriveInput in = Input(tg, car.pos, car.rot, car.vel, dt);
        in.contact = contact;
        const double lateral = std::fabs(double(car.pos.x - tg.pos.x));
        const VehicleDriveOutput o = c.Update(in);
        car.Apply(o, dt);
        ++r.frames;
        if (t >= 2000 && t <= 2700) { r.peakLateral = std::max(r.peakLateral, lateral); r.minWeight = std::min(r.minWeight, double(o.graceWeight)); }
        if (t >= 5900) r.endErr = std::max(r.endErr, Dist(car.pos, tg.pos));
    }
    r.extraSnaps = c.Stats().snaps - 1;
    r.contacts = c.Stats().contacts;
    r.byFlag = c.Stats().contactsByFlag;
    r.byDecel = c.Stats().contactsByDecel;
    return r;
}

// The distance error of a PresentLead-drawn 30 m/s circle car, `distM` from the local player, 20 Hz snapshots with
// jitter and latency 30..90 ms, drawn 120 ms behind the receipt. `lead` false = the old fixed delay.
static double LeadErrorM(double distM, bool lead, int* frames) {
    Feed feed([](double t) { return Circle(t); }, 12000, true, 77);
    VehicleInterpolator ip;
    PresentLead pl;
    double worst = 0;
    *frames = 0;
    const double dt = 1000.0 / 60.0;
    for (double now = 0; now < 11000; now += dt) {
        feed.Deliver(now, ip);
        double rt = now - 120.0;
        const double l = lead ? pl.Update(distM, 120.0, dt) : 0.0;
        rt += l;
        VehicleSample s;
        if (now < 2500 || !ip.Sample(rt, s)) continue;
        ++*frames;
        // truth: where the car is at `now` on the sender's timeline (the sample timeline IS the sender's: no clock offset)
        worst = std::max(worst, Dist(s.pos, Circle(now).pos));
    }
    return worst;
}

static void TestW9() {
    // RollDeg conventions: + = right side down, about the body's forward axis, independent of heading and pitch
    {
        const DriveQuat lean5 = vq::Exp(V(0, 5.0 * kPi / 180.0, 0));
        const double r0 = vq::RollDeg(vq::FromHeadingDeg(0)), r5 = vq::RollDeg(lean5), rm = vq::RollDeg(vq::Exp(V(0, -20.0 * kPi / 180.0, 0)));
        const double rh = vq::RollDeg(vq::Mul(vq::FromHeadingDeg(133), lean5));                        // any heading
        const double rp = vq::RollDeg(vq::Mul(vq::FromHeadingDeg(40), vq::Mul(vq::Exp(V(0.2, 0, 0)), lean5)));   // nose up 11 deg
        Check("RollDeg: 0 level, +5 for a 5 degree lean to the right (rotation about +y), -20 to the left, the same at any heading and with the nose pitched up",
              std::fabs(r0) < 1e-6 && std::fabs(r5 - 5.0) < 1e-4 && std::fabs(rm + 20.0) < 1e-4 && std::fabs(rh - 5.0) < 1e-3 &&
                  std::fabs(rp - 5.0) < 0.1,
              F("%.5f %.5f %.5f %.5f %.4f", r0, r5, rm, rh, rp));
    }
    // the contact grace: a knock plays out (flag and speed loss), without a snap, then converges back
    {
        const KnockResult withFlag = RunKnock(true, 0.0), byDecel = RunKnock(false, 6.0), off = RunKnock(true, 0.0, 0.0),
                          calm = RunKnock(false, 0.0, -1.0, 0.0);   // no knock: the sim's own standing error (drag) at 6 s
        Check("contact grace: with the collision flag a 6 m/s sideways knock plays out (> 0.8 m lateral in 0.7 s) where the old every-frame overwrite holds it under 0.5 m",
              withFlag.peakLateral > 0.8 && off.peakLateral < 0.5 && off.contacts == 0 && withFlag.contacts == 1 && withFlag.byFlag == 1,
              F("grace %.3f m (contacts %u), without %.3f m (contacts %u)", withFlag.peakLateral, withFlag.contacts, off.peakLateral,
                off.contacts));
        Check("contact grace: a loss of speed along the travel direction alone (no flag, 6 m/s lost in one frame) opens it too",
              byDecel.contacts == 1 && byDecel.byDecel == 1 && byDecel.peakLateral > 0.8, F("contacts %u by decel %u, %.3f m", byDecel.contacts,
                                                                                             byDecel.byDecel, byDecel.peakLateral));
        Check("contact grace: no snap while it plays out and the car converges back within 4 s (to within 5 cm of the undisturbed run); the weight starts soft (0.15)",
              withFlag.extraSnaps == 0 && byDecel.extraSnaps == 0 && calm.contacts == 0 && withFlag.endErr < calm.endErr + 0.05 &&
                  byDecel.endErr < calm.endErr + 0.05 && withFlag.minWeight < 0.2 && withFlag.frames > 340,
              F("snaps %u %u, end error %.3f %.3f m (undisturbed %.3f), min weight %.3f, %d frames", withFlag.extraSnaps, byDecel.extraSnaps,
                withFlag.endErr, byDecel.endErr, calm.endErr, withFlag.minWeight, withFlag.frames));
    }
    // normal driving never opens a grace (the closed loop with its knock of POSITION, which changes no velocity)
    {
        const Route route;
        uint32_t contacts = 0;
        for (int i = 0; i < 3; ++i) {
            VehicleInterpolator ip;
            VehicleDriveController c;
            SimCar car;
            car.velLagMs = 1e-3;                 // Sync B: a car that honours SET_ENTITY_VELOCITY within the frame (the kick trigger's premise)
            car.rot = vq::FromHeadingDeg(0);
            car.pos = V(30, -20, 5);
            Feed feed([&route](double t) { return route.At(t); }, 13000, true, 31 + i);
            const double dt = 1000.0 / kFps[i];
            bool knocked = false;
            for (double now = 0; now < 13150; now += dt) {
                feed.Deliver(now, ip);
                const double rt = now - 120;
                VehicleSample s;
                if (rt < 0 || !ip.Sample(rt, s)) continue;
                if (!knocked && rt >= 1500) { knocked = true; car.pos = V(car.pos.x + 2.5, car.pos.y, car.pos.z); }
                car.Apply(c.Update(Input(s, car.pos, car.rot, car.vel, dt)), dt);
            }
            contacts += c.Stats().contacts;
        }
        Check("contact grace: a route of straights, a hard turn and a stop at 30/60/144 fps (with a positional knock) opens no grace", contacts == 0,
              F("%u", contacts));
    }
    // present-time lead
    {
        PresentLeadConfig cfg;
        Check("PresentLead weight: 1 within 30 m, linear to 0 at 45 m (0.5 at 37.5 m), 0 beyond or for a non-finite distance",
              PresentLead::Weight(cfg, 0) == 1.0 && PresentLead::Weight(cfg, 30) == 1.0 && std::fabs(PresentLead::Weight(cfg, 37.5) - 0.5) < 1e-9 &&
                  PresentLead::Weight(cfg, 45) == 0.0 && PresentLead::Weight(cfg, 300) == 0.0 && PresentLead::Weight(cfg, std::nan("")) == 0.0);
        PresentLead a;
        const double first = a.Update(10, 400, 16);                    // the first call takes the target; capped at 250 ms
        PresentLead b;
        b.Update(100, 120, 16);                                          // far: 0
        double l = 0;
        for (int i = 0; i < 3; ++i) l = b.Update(10, 120, 16);          // close: slewed at 0.5 ms per ms, 8 ms a frame
        const double afterThree = l;
        for (int i = 0; i < 30; ++i) l = b.Update(10, 120, 16);
        Check("PresentLead: capped at 250 ms, slewed at 0.5 ms/ms (3 frames of 16 ms move it 24 ms, not 120), settles at the full delay",
              first == 250.0 && std::fabs(afterThree - 24.0) < 1e-9 && std::fabs(l - 120.0) < 1e-9, F("%.2f %.2f %.2f", first, afterThree, l));
    }
    {
        int fl = 0, fo = 0, ff = 0;
        const double near20 = LeadErrorM(20, true, &fl), old20 = LeadErrorM(20, false, &fo), far60 = LeadErrorM(60, true, &ff);
        Check("present-time lead: a 30 m/s car 20 m away is drawn within 0.5 m of where it is now (without the lead it is 3.6 m behind)",
              fl > 400 && near20 < 0.5 && old20 > 3.0, F("frames %d: lead %.3f m, old %.3f m", fl, near20, old20));
        Check("present-time lead: beyond 45 m the car is drawn in the past as before (the same error as without the lead)",
              ff > 400 && fo > 400 && std::fabs(far60 - old20) < 0.05, F("%.3f vs %.3f m", far60, old20));
    }
    // a bike on a banked circle: the lean survives the controller (item 7)
    {
        // 20 m/s on a 40 m circle (0.5 rad/s) leaning 30 degrees; snapshots at 20 Hz; SimCar at 60 fps
        auto lean = [](double tMs) {
            const double R = 40, w = 0.5, phi = w * tMs / 1000.0;
            Truth t;
            t.pos = V(R * std::cos(phi), R * std::sin(phi), 10);
            t.vel = V(-20 * std::sin(phi), 20 * std::cos(phi), 0);
            t.angVel = V(0, 0, w);
            t.rot = vq::Mul(vq::FromHeadingDeg(Deg(phi)), vq::Exp(V(0, 30.0 * kPi / 180.0, 0)));
            return t;
        };
        Feed feed(lean, 12000, true, 9);
        VehicleInterpolator ip;
        VehicleDriveController c;
        SimCar car;
        const Truth t0 = lean(0);
        car.pos = t0.pos; car.vel = t0.vel; car.rot = t0.rot;
        RollTracker rt;
        double worstRoll = 0, worstAng = 0;
        int frames = 0;
        const double dt = 1000.0 / 60.0;
        for (double now = 0; now < 11000; now += dt) {
            feed.Deliver(now, ip);
            VehicleSample s;
            if (now < 150 || !ip.Sample(now - 120, s)) continue;
            const double err = vq::RollDeg(car.rot) - vq::RollDeg(s.rot);
            const VehicleDriveOutput o = c.Update(Input(s, car.pos, car.rot, car.vel, dt));
            car.Apply(o, dt);
            if (now > 3000) { worstRoll = std::max(worstRoll, std::fabs(err)); worstAng = std::max(worstAng, AngDeg(car.rot, s.rot)); rt.Add(err, Len(car.vel)); ++frames; }
        }
        const RollTracker::Window w = rt.Take();
        Check("bike: a 30 degree lean on a 40 m circle at 20 m/s keeps its roll through the controller (within 2 degrees of the target, rotation within 5)",
              frames > 400 && worstRoll < 2.0 && worstAng < 5.0 && std::fabs(vq::RollDeg(lean(5000).rot) - 30.0) < 0.1,
              F("frames %d roll error %.3f deg, rotation error %.3f deg", frames, worstRoll, worstAng));
        Check("bike log: the RollTracker window carries the frames, the largest and mean roll error and the top speed, and clears on Take",
              w.frames == (uint32_t)frames && std::fabs(w.maxErrDeg - worstRoll) < 1e-3 && w.meanErrDeg <= w.maxErrDeg && w.maxSpeedMs > 15.0 &&
                  rt.Take().frames == 0,
              F("%u frames, max %.3f mean %.3f speed %.1f", w.frames, w.maxErrDeg, w.meanErrDeg, w.maxSpeedMs));
    }
}

// ---- 11. Sync B: the collision audit (docs/sync-specs/vehicles.md), a planar contact simulation -----------------------------------
//
// Real GTA physics is not modelled and cannot be here: what follows is the controller against a plausible rigid-body world, so the
// cases the audit named (a hit the collision flag does not report, a hit on a car that stands, a flag that never falls or comes a frame
// late, a car pushed into something it cannot move) can be measured instead of argued. Bodies are oriented boxes in the plane with an
// impulse + positional-correction contact (restitution 0.25, 240 Hz sub-steps), tyre grip (lateral velocity relaxes at 6 /s), rolling
// drag and yaw damping; a "script frame" reads the puppet, runs the controller and writes SET_ENTITY_VELOCITY / ANGULAR_VELOCITY /
// QUATERNION / COORDS at once (a body that honours its command within the frame; the lag model of SimCar is the opposite and is used
// below for exactly that reason). The contact flag is what the game would report: any contact of the puppet in the previous frame's
// physics (mode 1), the same one frame late (2), never (0), or stuck true (3).
namespace pl {
struct V2 { double x = 0, y = 0; };
static V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
static V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
static V2 operator*(V2 a, double k) { return {a.x * k, a.y * k}; }
static double Dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
static double Cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
static double Norm(V2 a) { return std::sqrt(Dot(a, a)); }
struct Body {
    V2 p, v;
    double h = 0, w = 0;                 // heading rad (0 = +y, as GTA), yaw rate rad/s (counter-clockwise)
    double m = 1500, hl = 2.3, hw = 1.0; // mass kg, half length, half width (a 4.6 x 2.0 m car)
    bool fixed = false;
    double Inertia() const { return m * (4 * hl * hl + 4 * hw * hw) / 12.0; }
    V2 Fwd() const { return {-std::sin(h), std::cos(h)}; }
    V2 Right() const { return {std::cos(h), std::sin(h)}; }
    void Corners(V2 c[4]) const {
        const V2 f = Fwd(), r = Right();
        c[0] = p + f * hl + r * hw; c[1] = p + f * hl - r * hw; c[2] = p - f * hl - r * hw; c[3] = p - f * hl + r * hw;
    }
};
struct Hit { bool hit = false; double depth = 0; V2 n, pt; };
static Hit Sat(const Body& a, const Body& b) {   // separating axis; the normal points from a to b
    Hit r;
    V2 ca[4], cb[4];
    a.Corners(ca); b.Corners(cb);
    const V2 axes[4] = {a.Fwd(), a.Right(), b.Fwd(), b.Right()};
    double best = 1e9;
    V2 bn;
    for (const V2& ax : axes) {
        double mn1 = 1e9, mx1 = -1e9, mn2 = 1e9, mx2 = -1e9;
        for (int i = 0; i < 4; ++i) {
            double d = Dot(ca[i], ax); mn1 = std::min(mn1, d); mx1 = std::max(mx1, d);
            d = Dot(cb[i], ax); mn2 = std::min(mn2, d); mx2 = std::max(mx2, d);
        }
        const double o = std::min(mx1, mx2) - std::max(mn1, mn2);
        if (o <= 0) return r;
        if (o < best) { best = o; bn = ax; }
    }
    if (Dot(bn, b.p - a.p) < 0) bn = bn * -1.0;
    r.hit = true; r.depth = best; r.n = bn;
    double dmin = 1e9, dmax = -1e9;
    V2 p1, p2;
    for (int i = 0; i < 4; ++i) {
        const double d1 = Dot(cb[i], bn), d2 = Dot(ca[i], bn);
        if (d1 < dmin) { dmin = d1; p1 = cb[i]; }
        if (d2 > dmax) { dmax = d2; p2 = ca[i]; }
    }
    r.pt = (p1 + p2) * 0.5;
    return r;
}
struct World {
    std::vector<Body> b;
    double rest = 0.25, grip = 6.0, rollDrag = 0.1, angDamp = 3.0, baum = 0.25, slop = 0.01;
    std::vector<std::vector<char>> touched;
    void Step(double dt) {
        const int n = (int)b.size();
        for (Body& x : b) {
            if (x.fixed) continue;
            const V2 f = x.Fwd(), r = x.Right();
            const double vf = Dot(x.v, f) * std::exp(-rollDrag * dt), vl = Dot(x.v, r) * std::exp(-grip * dt);
            x.v = f * vf + r * vl;
            x.w *= std::exp(-angDamp * dt);
            x.p = x.p + x.v * dt;
            x.h += x.w * dt;
        }
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                Body &A = b[i], &B = b[j];
                if (A.fixed && B.fixed) continue;
                const Hit c = Sat(A, B);
                if (!c.hit) continue;
                touched[i][j] = touched[j][i] = 1;
                const double iA = A.fixed ? 0 : 1 / A.m, iB = B.fixed ? 0 : 1 / B.m;
                const double IA = A.fixed ? 0 : 1 / A.Inertia(), IB = B.fixed ? 0 : 1 / B.Inertia();
                const V2 rA = c.pt - A.p, rB = c.pt - B.p;
                const V2 vA = A.v + V2{-A.w * rA.y, A.w * rA.x}, vB = B.v + V2{-B.w * rB.y, B.w * rB.x};
                const double vn = Dot(vB - vA, c.n);
                if (vn < 0) {
                    const double den = iA + iB + IA * Cross(rA, c.n) * Cross(rA, c.n) + IB * Cross(rB, c.n) * Cross(rB, c.n);
                    const double jn = -(1 + rest) * vn / den;
                    A.v = A.v - c.n * (jn * iA); B.v = B.v + c.n * (jn * iB);
                    A.w -= IA * Cross(rA, c.n * jn); B.w += IB * Cross(rB, c.n * jn);
                }
                const double corr = std::max(0.0, c.depth - slop) * baum / std::max(1e-9, iA + iB);
                A.p = A.p - c.n * (corr * iA); B.p = B.p + c.n * (corr * iB);
            }
    }
};
static double Overlap(const Body& a, const Body& b) { const Hit c = Sat(a, b); return c.hit ? c.depth : 0.0; }
}   // namespace pl

struct PlTarget { pl::V2 p, v; double h = 0, w = 0; };
static PlTarget PlStraight(pl::V2 p0, pl::V2 v, double tMs) {
    const double s = tMs / 1000.0;
    return {p0 + v * s, v, pl::Norm(v) > 0 ? std::atan2(-v.x, v.y) : 0.0, 0.0};
}
static pl::Body PlCar(pl::V2 p, double h, pl::V2 v) { pl::Body b; b.p = p; b.h = h; b.v = v; return b; }

struct PlRun {
    double peakOverlap = 0, aMinSpeed = 1e9, aEndSpeed = 0, pupMaxDisp = 0, devMax = 0, wallEndY = 0;
    uint32_t contacts = 0, byFlag = 0, byDecel = 0, byKick = 0, kickMutes = 0, snaps = 0, persistent = 0;
    int firstGraceFrame = -1, firstDevFrame = -1, edges = 0;
    double graceMs = 0, hardMs = 0;
};
struct PlScn {
    std::function<PlTarget(double)> target;
    pl::World w;                         // body 0 = the puppet, 1 = the local car (A), further ones are fixed or parked
    bool aThrottle = false;              // A holds aSpeed with 6 m/s^2 (a driver with the foot down)
    double aSpeed = 0, tEndMs = 5000;
    int flagMode = 1;                    // 0 never, 1 previous frame's contacts, 2 the same one frame late, 3 stuck true
    double fps = 60;
    VehicleDriveConfig cfg;
};

static PlRun RunPl(PlScn s) {
    VehicleDriveController c(s.cfg);
    pl::World& w = s.w;
    const int n = (int)w.b.size();
    w.touched.assign(n, std::vector<char>(n, 0));
    PlRun r;
    bool lastFlag = false, prevTouch = false, lateTouch = false;
    const double dtF = 1000.0 / s.fps;
    const int sub = std::max(1, (int)std::ceil(dtF / (1000.0 / 240.0)));
    int frame = 0;
    for (double t = 0; t < s.tEndMs; t += dtF, ++frame) {
        pl::Body& P = w.b[0];
        const PlTarget tg = s.target(t);
        VehicleDriveInput in;
        in.target.pos = V(tg.p.x, tg.p.y, 0);
        in.target.vel = V(tg.v.x, tg.v.y, 0);
        in.target.rot = vq::FromHeadingDeg(Deg(tg.h));
        in.target.angVel = V(0, 0, tg.w);
        in.target.mode = VehMode::Interpolating;
        in.localPos = V(P.p.x, P.p.y, 0);
        in.localVel = V(P.v.x, P.v.y, 0);
        in.localRot = vq::FromHeadingDeg(Deg(P.h));
        in.dtMs = t == 0 ? 0 : dtF;
        const bool flag = s.flagMode == 0 ? false : s.flagMode == 3 ? true : s.flagMode == 2 ? lateTouch : prevTouch;
        in.contact = flag && !lastFlag;
        lastFlag = flag;
        if (in.contact) ++r.edges;
        const VehicleDriveOutput o = c.Update(in);
        r.devMax = std::max(r.devMax, double(o.devMs));
        if (o.devMs > 3.0f && r.firstDevFrame < 0) r.firstDevFrame = frame;
        if (o.graceActive && r.firstGraceFrame < 0) r.firstGraceFrame = frame;
        (o.graceActive ? r.graceMs : r.hardMs) += dtF;
        if (o.snap) { P.p = {o.snapPos.x, o.snapPos.y}; P.h = vq::HeadingDeg(o.snapRot) * kPi / 180.0; }
        else if (o.setQuaternion) P.h = vq::HeadingDeg(o.rot) * kPi / 180.0;
        if (o.setVelocity) P.v = {o.velocity.x, o.velocity.y};
        if (o.setAngularVelocity) P.w = o.angularVelocity.z;
        for (auto& row : w.touched) std::fill(row.begin(), row.end(), 0);
        for (int k = 0; k < sub; ++k) {
            if (s.aThrottle && n > 1) {
                pl::Body& A = w.b[1];
                const double vf = pl::Dot(A.v, A.Fwd());
                if (vf < s.aSpeed) A.v = A.v + A.Fwd() * std::min(6.0 * dtF / 1000.0 / sub, s.aSpeed - vf);
            }
            w.Step(dtF / 1000.0 / sub);
        }
        bool touch = false;
        for (int j = 1; j < n; ++j) if (w.touched[0][j]) touch = true;
        lateTouch = prevTouch;                    // mode 2 reads last frame's flag value
        prevTouch = touch;
        for (int j = 1; j < n; ++j) r.peakOverlap = std::max(r.peakOverlap, pl::Overlap(w.b[0], w.b[j]));
        if (n > 1 && t > 200) r.aMinSpeed = std::min(r.aMinSpeed, pl::Norm(w.b[1].v));
        r.pupMaxDisp = std::max(r.pupMaxDisp, pl::Norm(w.b[0].p - tg.p));
    }
    r.contacts = c.Stats().contacts; r.byFlag = c.Stats().contactsByFlag; r.byDecel = c.Stats().contactsByDecel;
    r.byKick = c.Stats().contactsByKick; r.kickMutes = c.Stats().kickMutes;
    r.snaps = c.Stats().snaps - 1; r.persistent = c.Stats().persistentSnaps;
    if (n > 1) r.aEndSpeed = pl::Norm(w.b[1].v);
    r.wallEndY = w.b[0].p.y;
    return r;
}

// the scenarios (all on the RECEIVER: the puppet follows its sender's straight path, A is the local player's real car)
static PlScn PlTBone() {            // A (12 m/s, from the left) hits the side of a puppet doing 15 m/s
    PlScn s;
    s.target = [](double t) { return PlStraight({0, 0}, {0, 15}, t); };
    s.w.b.push_back(PlCar({0, 0}, 0, {0, 15}));
    s.w.b.push_back(PlCar({-12, 10.8}, -kPi / 2, {12, 0}));
    s.aThrottle = true; s.aSpeed = 12; s.tEndMs = 4000;
    return s;
}
static PlScn PlRamStanding() {      // A (15 m/s) rear-ends a puppet whose sender stands still
    PlScn s;
    s.target = [](double) { return PlTarget{{0, 0}, {0, 0}, 0, 0}; };
    s.w.b.push_back(PlCar({0, 0}, 0, {0, 0}));
    s.w.b.push_back(PlCar({0, -40}, 0, {0, 15}));
    s.aThrottle = true; s.aSpeed = 15; s.tEndMs = 5000;
    return s;
}
static PlScn PlThroughParked() {    // the sender's path runs through A, which stands (the sender's own screen shows A elsewhere)
    PlScn s;
    s.target = [](double t) { return PlStraight({0, -30}, {0, 15}, t); };
    s.w.b.push_back(PlCar({0, -30}, 0, {0, 15}));
    s.w.b.push_back(PlCar({0, 0}, 0, {0, 0}));
    s.tEndMs = 5000;
    return s;
}
static PlScn PlIntoWall() {         // the same path, a fixed wall (an obstacle that exists on this machine only)
    PlScn s = PlThroughParked();
    pl::Body wall; wall.p = {0, 0}; wall.hl = 0.5; wall.hw = 6; wall.fixed = true;
    s.w.b[1] = wall;
    return s;
}
static PlScn PlCircle(double tEndMs) {   // normal driving: 30 m/s on a 50 m circle, nothing near
    PlScn s;
    s.target = [](double t) {
        const double R = 50, wz = 0.6, ph = wz * t / 1000.0;
        return PlTarget{{R * std::cos(ph) - R, R * std::sin(ph)}, {-30 * std::sin(ph), 30 * std::cos(ph)}, ph, wz};
    };
    s.w.b.push_back(PlCar({0, 0}, 0, {0, 30}));
    s.tEndMs = tEndMs;
    return s;
}

static void TestSyncB() {
    // the audit's table in one place (vehicle_drive_test prints it; the report quotes it)
    // 1. a side hit the collision flag does not report
    {
        PlScn on = PlTBone(), off = PlTBone();
        on.flagMode = off.flagMode = 0;
        off.cfg.contactKickMs = 0.0f;                          // the W9 trigger set: flag and speed loss only
        const PlRun a = RunPl(on), b = RunPl(off);
        Check("Sync B kick: a 12 m/s side hit on a moving puppet that the collision flag does NOT report opens a grace (by kick) in the impact frame, where flag + speed loss open none (the lateral hit is no loss along the travel direction)",
              a.contacts == 1 && a.byKick == 1 && a.firstGraceFrame == a.firstDevFrame && a.firstGraceFrame > 0 && b.contacts == 0 && a.devMax > 3.0,
              F("kick: %u graces (%u by kick) at frame %d, deviation >3 m/s first at %d, max %.1f m/s; W9 triggers: %u graces", a.contacts, a.byKick,
                a.firstGraceFrame, a.firstDevFrame, a.devMax, b.contacts));
    }
    // 2. a hit on a car that stands
    {
        PlScn on = PlRamStanding(), off = PlRamStanding();
        on.flagMode = off.flagMode = 0;
        off.cfg.contactKickMs = 0.0f;
        const PlRun a = RunPl(on), b = RunPl(off);
        Check("Sync B kick: a puppet whose sender stands still, rammed at 15 m/s with a silent flag, gets a grace (the speed-loss trigger needs a target faster than 3 m/s, so it never could)",
              a.byKick >= 1 && a.firstGraceFrame > 0 && b.contacts == 0 && a.pupMaxDisp > b.pupMaxDisp,
              F("kick: %u graces, puppet pushed %.2f m (W9 triggers: %u graces, %.2f m)", a.byKick, a.pupMaxDisp, b.contacts, b.pupMaxDisp));
    }
    // 3. a flag that never falls: one rising edge (before anything happened), no flag grace afterwards; and one that arrives a frame late
    {
        PlScn stuck = PlTBone(), stuckOld = PlTBone(), late = PlTBone(), lateOld = PlTBone();
        stuck.flagMode = stuckOld.flagMode = 3;
        late.flagMode = lateOld.flagMode = 2;
        stuckOld.cfg.contactKickMs = 0.0f;
        lateOld.cfg.contactKickMs = 0.0f;
        const PlRun a = RunPl(stuck), b = RunPl(stuckOld), c = RunPl(late), d = RunPl(lateOld);
        Check("Sync B flag failure modes: a flag stuck true gives no edge at the hit (W9: no grace at all, kick: one), a flag one frame late opens the grace one frame after the impulse (W9) where the kick opens it in the impact frame",
              a.edges <= 1 && a.contacts == 1 && a.byKick == 1 && b.contacts == 0 && c.firstGraceFrame == c.firstDevFrame && d.firstGraceFrame > d.firstDevFrame,
              F("stuck: %d edge(s), kick %u graces / W9 %u; late: grace at frame %d (impulse %d) / W9 grace at %d (impulse %d)", a.edges, a.contacts, b.contacts,
                c.firstGraceFrame, c.firstDevFrame, d.firstGraceFrame, d.firstDevFrame));
    }
    // 4. normal driving: a body that honours its command deviates by a fraction of a m/s, never opens a kick
    {
        uint32_t kicks = 0, contacts = 0;
        double worst = 0;
        std::string got;
        for (int i = 0; i < 3; ++i) {
            PlScn s = PlCircle(10000);
            s.fps = kFps[i];
            const PlRun r = RunPl(s);
            kicks += r.byKick; contacts += r.contacts;
            worst = std::max(worst, r.devMax);
            got += F("%g fps: dev %.2f  ", kFps[i], r.devMax);
        }
        Check("Sync B kick: a 30 m/s circle at 30/60/144 fps against a body that honours its command opens no grace and deviates by under 1 m/s (the 3 m/s trigger has 3x margin)",
              kicks == 0 && contacts == 0 && worst < 1.0, got);
    }
    // 5. a body that does NOT honour the command (SimCar's 100 ms lag): the kick budget mutes itself, the damage is bounded
    {
        auto run = [](bool budget, uint32_t* mutes, uint32_t* contacts) {
            VehicleDriveConfig cfg;
            if (!budget) cfg.contactKickStormN = 0;
            const Route route;
            VehicleInterpolator ip;
            VehicleDriveController c(cfg);
            SimCar car;
            car.velLagMs = 200;                                 // a sluggish body: it follows a command with a 200 ms lag
            car.rot = vq::FromHeadingDeg(0);
            car.pos = V(30, -20, 5);
            Feed feed([&route](double t) { return route.At(t); }, 13000, true, 31);
            double nextKnock = 1500;                            // and a 2.5 m positional disturbance every second (a big correction each time)
            const double dt = 1000.0 / 60.0;
            for (double now = 0; now < 13150; now += dt) {
                feed.Deliver(now, ip);
                VehicleSample s;
                if (now - 120 < 0 || !ip.Sample(now - 120, s)) continue;
                if (now - 120 >= nextKnock) { nextKnock += 1000; car.pos = V(car.pos.x + 2.5, car.pos.y, car.pos.z); }
                car.Apply(c.Update(Input(s, car.pos, car.rot, car.vel, dt)), dt);
            }
            *mutes = c.Stats().kickMutes;
            *contacts = c.Stats().contactsByKick;
            return c.Stats().contactsByKick;
        };
        uint32_t mutesOn = 0, kicksOn = 0, mutesOff = 0, kicksOff = 0;
        run(true, &mutesOn, &kicksOn);
        run(false, &mutesOff, &kicksOff);
        Check("Sync B kick budget: against a body that lags its commands (a big correction reads as a kick) 3 kicks in 6 s mute the trigger for 15 s: a 13 s route with a disturbance every second opens 3 kick graces, not one per disturbance",
              mutesOn >= 1 && kicksOn <= 3 && mutesOff == 0 && kicksOff >= 6,
              F("budget: %u kick graces, %u mutes; no budget: %u kick graces", kicksOn, mutesOn, kicksOff));
    }
    // 6. penetration: the controller never drives a puppet more than a few cm into what it hits, and never launches the car it hits past its own speed
    {
        PlScn tb = PlTBone(), ram = PlRamStanding(), thr = PlThroughParked(), wall = PlIntoWall();
        tb.flagMode = ram.flagMode = thr.flagMode = wall.flagMode = 1;
        const PlRun a = RunPl(tb), b = RunPl(ram), c = RunPl(thr), d = RunPl(wall);
        Check("Sync B penetration (planar model, default controller, flag reported): T-bone, ram, a sender path through a standing car and into a wall stay under 0.5 m of overlap; the car in the path is pushed at most to the puppet's speed",
              a.peakOverlap < 0.5 && b.peakOverlap < 0.5 && c.peakOverlap < 0.5 && d.peakOverlap < 0.5 && c.aEndSpeed < 15.5,
              F("peak overlap: T-bone %.2f, ram %.2f, through parked %.2f (car pushed to %.1f m/s), wall %.2f m (puppet ends at y %.1f: the wall is at 0; %u snap(s), %u persistent)",
                a.peakOverlap, b.peakOverlap, c.peakOverlap, c.aEndSpeed, d.peakOverlap, d.wallEndY, d.snaps, d.persistent));
    }
    // 7. what the grace does to the local car (the numbers the report quotes)
    {
        PlScn soft = PlTBone(), hard = PlTBone();
        soft.flagMode = hard.flagMode = 1;
        hard.cfg.contactGraceMs = 0.0f;                         // the old every-frame overwrite
        const PlRun a = RunPl(soft), b = RunPl(hard);
        Check("Sync B contact grace (planar model): with the grace the local car keeps more of its speed through the T-bone and the puppet gives more than with the every-frame overwrite",
              a.aMinSpeed > b.aMinSpeed && a.pupMaxDisp > b.pupMaxDisp,
              F("local car's lowest speed %.2f vs %.2f m/s; puppet pushed %.2f vs %.2f m", a.aMinSpeed, b.aMinSpeed, a.pupMaxDisp, b.pupMaxDisp));
    }
    // 8. the diagnostics window
    {
        ContactWindow cw;
        ContactFrame f;
        f.overwrote = true;
        for (int i = 0; i < 10; ++i) { f.flagRead = true; f.flag = false; f.devMs = 0.1f; cw.Add(f); }   // quiet frames
        f.devMs = 6.0f; f.flag = false; cw.Add(f);                                                       // the impulse frame, flag late
        f.devMs = 0.5f; f.flag = true; f.edge = true; f.opened = 1; f.graceActive = true; cw.Add(f);       // the edge frame
        f.edge = false; f.opened = 0; f.localCarDistM = 1.2f;
        for (int i = 0; i < 3; ++i) cw.Add(f);                                                           // flag stays true, grace in force
        f.graceActive = false; f.localCarDistM = 2.4f; cw.Add(f);                                        // a full-weight frame with the flag true
        cw.AddPuppetNear(2.0f);
        const ContactWindow::Window w = cw.Take();
        const ContactWindow::Window e = cw.Take();
        Check("Sync B diagnostics window: flag duty, edges, the deviation in the frame before an edge (late flag), graces by trigger, frames overwritten at full weight while the flag was true, distances; Take() clears",
              w.frames == 16 && w.flagReads == 16 && w.flagTrue == 5 && w.edges == 1 && w.opened[1] == 1 && w.flagTrueHard == 1 && w.graceFrames == 4 &&
                  std::fabs(w.preEdgeDevMax - 6.0f) < 1e-6f && std::fabs(w.edgeDevMax - 0.5f) < 1e-6f && std::fabs(w.devMax - 6.0f) < 1e-6f &&
                  w.devOver3 == 1 && w.devOver1 == 1 && std::fabs(w.minLocalM - 1.2f) < 1e-6f && w.localUnder3m == 4 && w.localUnder1_5m == 3 &&
                  w.puppetUnder3m == 1 && std::fabs(w.minPuppetM - 2.0f) < 1e-6f && w.Interesting() && e.frames == 0 && !e.Interesting() && e.minLocalM < 0.0f,
              F("%u frames, flag true %u, edges %u, hard %u, grace frames %u, pre-edge dev %.1f, edge dev %.1f, min local %.1f m", w.frames, w.flagTrue, w.edges,
                w.flagTrueHard, w.graceFrames, w.preEdgeDevMax, w.edgeDevMax, w.minLocalM));
    }
}

// ---- 12. Sync D: the receive pipeline against a simulated relay and car (tests/vehsim.h) ---------------------------

constexpr int kSyncDChecks = 11;

static void TestSyncD() {
    // Buffering: the oldest snapshot walked BACK along its velocity, not pinned (the pop after a hand-over / reset / teleport)
    {
        auto make = [](double bufferBackMs) {
            VehicleInterpConfig cfg;
            cfg.bufferBackMaxMs = bufferBackMs;
            VehicleInterpolator ip(cfg);
            VehicleSnapshot s;
            s.tMs = 1000; s.pos = V(5, 100, 2); s.vel = V(0, 25, 0); s.rot = vq::FromHeadingDeg(0);
            ip.Push(s);
            return ip;
        };
        VehicleInterpolator a = make(300), pinned = make(0);
        VehicleSample s100, s500, s0, sp;
        const bool ok = a.Sample(900, s100) && a.Sample(500, s500) && a.Sample(1000, s0) && pinned.Sample(900, sp);
        Check("Buffering: 100 ms before the only snapshot a 25 m/s car is 2.5 m behind it (not pinned on it), capped at 300 ms (7.5 m back at 500 ms); at its own time exactly there; the velocity is kept; bufferBackMaxMs 0 is the old pin",
              ok && s100.mode == VehMode::Buffering && std::fabs(s100.pos.y - 97.5f) < 0.01 && std::fabs(s500.pos.y - 92.5f) < 0.01 &&
                  s0.mode != VehMode::Buffering && std::fabs(s0.pos.y - 100.0f) < 1e-3 && std::fabs(s100.vel.y - 25.0f) < 1e-3 &&
                  std::fabs(sp.pos.y - 100.0f) < 1e-3,
              F("100 ms: %.3f, 500 ms: %.3f, own time: %.3f, vel %.2f, pinned %.3f", s100.pos.y, s500.pos.y, s0.pos.y, s100.vel.y, sp.pos.y));
    }
    // DecideHandover
    {
        const uint16_t me = 3, none = 0xFFFF;
        const HandoverAction third = DecideHandover(1, 2, me), toMe = DecideHandover(1, me, me), fromMe = DecideHandover(me, 2, me),
                             toNone = DecideHandover(1, none, me), fromNone = DecideHandover(none, 2, me), same = DecideHandover(2, 2, me),
                             meNone = DecideHandover(me, none, me);
        Check("DecideHandover: between two other controllers (or from / to nobody) nothing is cleared or snapped; to or from us both are; no change is none",
              !third.clearInterp && !third.resetDrive && !toNone.clearInterp && !toNone.resetDrive && !fromNone.clearInterp &&
                  !fromNone.resetDrive && toMe.clearInterp && toMe.resetDrive && fromMe.clearInterp && fromMe.resetDrive &&
                  meNone.clearInterp && meNone.resetDrive && !same.clearInterp && !same.resetDrive);
    }
    // A controller hand-over at 25 m/s and at 139 m/s (the hyper car): the old behaviour (buffer pinned, cleared and snapped)
    // against the new (walk-back Buffering, nothing cleared between two other controllers)
    {
        struct R { vehsim::Metrics oldM, bufM, newM; };
        auto run = [](double v) {
            R r;
            vehsim::Truth t(vehsim::scn::Straight(v));
            vehsim::RunCfg oldc; oldc.handoverMs = 6000; oldc.interp.bufferBackMaxMs = 0.0;               // the old Buffering + Clear + Reset
            vehsim::RunCfg bufc; bufc.handoverMs = 6000;                                                  // walk-back Buffering, still cleared + snapped (a hand-over to / from us, a bridge reset)
            vehsim::RunCfg newc; newc.handoverMs = 6000;
            const HandoverAction ha = DecideHandover(1, 2, 3);
            newc.handoverKeep = !ha.clearInterp && !ha.resetDrive;                                       // between two other controllers: nothing cleared
            r.oldM = vehsim::Run(t, oldc);
            r.bufM = vehsim::Run(t, bufc);
            r.newM = vehsim::Run(t, newc);
            return r;
        };
        const R a = run(25.0), b = run(139.0);
        Check("hand-over at 25 m/s: the old pin-clear-snap pops the car 2.5 m and snaps twice; the walk-back Buffering alone (a clear + snap still) stays under 0.3 m; with the buffer and controller kept it also snaps only at the spawn",
              a.oldM.posMax > 2.0 && a.oldM.snaps == 2 && a.bufM.posMax < 0.3 && a.bufM.snaps == 2 && a.newM.posMax < 0.3 && a.newM.snaps == 1 && a.newM.pops == 0 &&
                  a.oldM.frames > 900 && a.newM.frames > 900,
              F("old %.2f m (snaps %u, pop %.2f), buffering fix %.3f m (snaps %u), kept %.3f m (snaps %u, pop %.2f)", a.oldM.posMax, a.oldM.snaps, a.oldM.popMax,
                a.bufM.posMax, a.bufM.snaps, a.newM.posMax, a.newM.snaps, a.newM.popMax));
        Check("hand-over at 139 m/s (500 km/h): the old one pops it 14 m; walk-back Buffering and the kept buffer both stay under 1 m",
              b.oldM.posMax > 10.0 && b.bufM.posMax < 1.0 && b.newM.posMax < 1.0 && b.newM.snaps == 1,
              F("old %.2f m, buffering fix %.3f m, kept %.3f m (snaps %u)", b.oldM.posMax, b.bufM.posMax, b.newM.posMax, b.newM.snaps));
    }
    // The whole pipeline, per scenario and relay tier: error against the sender at the instant the sample claims
    {
        struct Sc { const char* name; vehsim::Script s; };
        const Sc scs[] = {{"straight", vehsim::scn::Straight(25)}, {"lane change", vehsim::scn::LaneChange()}, {"hard brake", vehsim::scn::HardBrake()},
                          {"brake in a bend", vehsim::scn::BrakeInBend()}, {"reverse", vehsim::scn::Reverse()}, {"hairpin", vehsim::scn::Hairpin()},
                          {"jump", vehsim::scn::Jump()}, {"parked", vehsim::scn::Parked()}};
        bool ok = true;
        std::string got;
        int frames = 0;
        for (int tier = 1; tier <= 3; ++tier)
            for (const Sc& sc : scs) {
                vehsim::Truth t(sc.s);
                vehsim::RunCfg rc;
                rc.net.relayEvery = tier;
                rc.net.upJitterMs = 30; rc.net.downJitterMs = 60; rc.net.loss = 0.05;                   // 20-80 ms jitter, 5 % loss
                rc.net.seed = 11 + tier;
                const vehsim::Metrics m = vehsim::Run(t, rc);
                frames += m.frames;
                const bool good = m.frames > 400 && m.posMax < 0.4 && m.posMean < 0.12 && m.angMax < 15.0 && m.snaps == 1 && m.pops == 0 &&
                                  m.latMax < 0.5 && m.zBounceMax < 0.3 && m.zFloatMax < 0.4 && m.posStillMax < 0.2;
                if (!good) {
                    ok = false;
                    got += F("[%s t%d: %.2f m / mean %.3f, %.1f deg, lat %.2f, snaps %u, pops %d, z %.2f/%.2f, still %.2f] ", sc.name, tier, m.posMax, m.posMean,
                             m.angMax, m.latMax, m.snaps, m.pops, m.zBounceMax, m.zFloatMax, m.posStillMax);
                }
            }
        Check("pipeline at 30/15/10 Hz relay with 20-80 ms jitter and 5 % loss: straight, lane change, hard brake, brake in a bend, reverse, hairpin, jump, parked all stay within 0.4 m / 15 deg of the instant the sample claims, one snap (the spawn), no pops",
              ok && frames > 10000, ok ? F("%d frames", frames) : got);
    }
    // 500 km/h: the error is the timing noise times 139 m/s (1 ms = 14 cm), so the bound is wider; speed caps and quantisation hold
    {
        bool ok = true;
        std::string got;
        double worst = 0;
        for (int tier = 1; tier <= 3; ++tier)
            for (int curve = 0; curve < 2; ++curve) {
                vehsim::Truth t(curve ? vehsim::scn::HyperCurve() : vehsim::scn::HyperStraight());
                vehsim::RunCfg rc;
                rc.net.relayEvery = tier;
                rc.net.seed = 21 + tier;
                const vehsim::Metrics m = vehsim::Run(t, rc);
                worst = std::max(worst, m.posMax);
                if (!(m.frames > 800 && m.posMax < 3.0 && m.angMax < 8.0 && m.snaps == 1 && m.pops <= 3)) {
                    ok = false;
                    got += F("[t%d %s: %.2f m, %.1f deg, snaps %u, pops %d] ", tier, curve ? "curve" : "straight", m.posMax, m.angMax, m.snaps, m.pops);
                }
            }
        Check("hyper car at 139 m/s (straight and a gentle curve, three tiers): within 3 m / 8 deg, one snap (1 ms of render-clock noise is 14 cm there: the 10 Hz tier's delay change costs 2.5 m)", ok, ok ? F("worst %.2f m", worst) : got);
    }
    // a network hole: 300 ms is bridged without a snap; 1.2 s snaps once (persistent error) and the car is back within 0.2 m two seconds later
    {
        vehsim::Truth t(vehsim::scn::Straight(25));
        vehsim::RunCfg a; a.net.stallFromMs = 7000; a.net.stallToMs = 7300;
        vehsim::RunCfg b; b.net.stallFromMs = 7000; b.net.stallToMs = 8200;
        const vehsim::Metrics ma = vehsim::Run(t, a), mb = vehsim::Run(t, b);
        Check("a 300 ms network hole is bridged with no snap and no pop; a 1.2 s hole costs at most one more snap and the car is back on the path (< 0.2 m) at the end",
              ma.snaps == 1 && ma.pops == 0 && ma.posMax < 0.5 && mb.snaps <= 2 && mb.endPosErr < 0.2,
              F("300 ms: snaps %u, pops %d, %.2f m; 1.2 s: snaps %u, end %.3f m", ma.snaps, ma.pops, ma.posMax, mb.snaps, mb.endPosErr));
    }
    // present-time lead on a hairpin: the car is drawn near the present (an estimate), within 1 m / 20 degrees of where it will be
    {
        vehsim::Truth t(vehsim::scn::Hairpin());
        vehsim::RunCfg rc; rc.lead = true;
        const vehsim::Metrics m = vehsim::Run(t, rc);
        Check("present lead on a hairpin at 12 m/s: within 1.2 m / 20 degrees of the present, one snap",
              m.frames > 700 && m.posMax < 1.2 && m.angMax < 20.0 && m.snaps == 1, F("%.2f m, %.1f deg, snaps %u", m.posMax, m.angMax, m.snaps));
    }
    // the diagnostics
    {
        DriveErrWindow w;
        w.Add(0.5, 4.0, 0.2, VehMode::Interpolating, false);
        w.Add(1.5, 2.0, 0.4, VehMode::Extrapolating, true);
        w.Add(std::nan(""), 1.0, 1.0, VehMode::Holding, false);          // refused
        const DriveErrWindow::Window a = w.Take(), b = w.Take();
        const DriveQuat east = vq::FromHeadingDeg(-90.0);               // forward is +x
        Check("DriveErrWindow: frames, mean and max, snaps and modes of a window, a non-finite sample refused, Take clears",
              a.frames == 2 && std::fabs(a.posMeanM - 1.0f) < 1e-6 && a.posMaxM == 1.5f && std::fabs(a.rotMeanDeg - 3.0f) < 1e-6 && a.rotMaxDeg == 4.0f &&
                  std::fabs(a.latMeanM - 0.3f) < 1e-6 && a.latMaxM == 0.4f && a.snaps == 1 && a.interpolating == 1 && a.extrapolating == 1 && a.holding == 0 &&
                  b.frames == 0 && b.posMaxM == 0.0f);
        Check("LateralErrorM: the sideways part only (2 m ahead = 0, 1 m to the left or right = 1), at any heading; a target pointing straight up has none",
              std::fabs(LateralErrorM(V(0, 0, 0), vq::FromHeadingDeg(0), V(0, 2, 0))) < 1e-6 && std::fabs(LateralErrorM(V(0, 0, 0), vq::FromHeadingDeg(0), V(1, 0, 0)) - 1.0) < 1e-6 &&
                  std::fabs(LateralErrorM(V(0, 0, 0), vq::FromHeadingDeg(0), V(-1, 5, 0)) - 1.0) < 1e-6 &&
                  std::fabs(LateralErrorM(V(10, 10, 0), east, V(13, 10, 0))) < 1e-5 && std::fabs(LateralErrorM(V(10, 10, 0), east, V(13, 11, 0)) - 1.0) < 1e-5 &&
                  LateralErrorM(V(0, 0, 0), vq::Exp(V(-kPi / 2, 0, 0)), V(1, 1, 0)) == 0.0);
    }
    {
        Check("ClassifyDisplacement: never warped here is not a displacement; then another seat of the same car, another car, outside, unreadable",
              ClassifyDisplacement(false, true, true, true) == SeatDisplacement::NotSeatedBefore && ClassifyDisplacement(true, true, true, true) == SeatDisplacement::OtherSeat &&
                  ClassifyDisplacement(true, true, false, true) == SeatDisplacement::OtherVehicle && ClassifyDisplacement(true, true, false, false) == SeatDisplacement::Outside &&
                  ClassifyDisplacement(true, false, true, true) == SeatDisplacement::Unknown);
    }
}

#include "t4_collision_tests.h"
#include "r3_t5_tests.h"
#include "r4_t2_tests.h"

int main() {
    if (std::getenv("R4T2_REPORT")) { r4t2::Report(); return 0; }   // R4 T2: the scratch report (no checks)
    std::printf("-- quaternion math\n");
    TestQuatMath();
    std::printf("-- interpolation\n");
    TestInterpolation();
    std::printf("-- extrapolation / push\n");
    TestExtrapolation();
    std::printf("-- snap\n");
    TestSnap();
    std::printf("-- framerate\n");
    TestFramerate();
    std::printf("-- closed loop\n");
    TestClosedLoop();
    std::printf("-- sender steering estimate\n");
    TestSteer();
    std::printf("-- sync plan W6: stamps, repeats, sessions, send gate\n");
    TestW6();
    std::printf("-- hyper tune\n");
    TestHyperTune();
    std::printf("-- W9: contact grace, present-time lead, bike lean\n");
    TestW9();
    std::printf("-- Sync B: collision audit (planar contact simulation)\n");
    TestSyncB();

    std::printf("-- Sync D: the receive pipeline against a simulated relay and car, hand-over, diagnostics\n");
    TestSyncD();

    std::printf("-- T4: vehicle collisions (arbiter, impact apply, copy adoption, two-machine simulation)\n");
    t4::TestT4();

    r3t5::TestR3T5();

    r4t2::TestR4T2();

    // quat 6 + interpolation 2 + extrapolation 6 + snap 5 + framerate 3 + closed loop 3 + steer 5 + W6 7 + W7 typical gap 1
    // + hyper tune kHyperTuneChecks (4 + the profiles and assists) + W9 11 + Sync B 8 (kSyncBChecks as listed in its header) + Sync D kSyncDChecks
    const int expected = 6 + 2 + 6 + 5 + 3 + 3 + 5 + 7 + 1 + kHyperTuneChecks + 11 + 8 + kSyncDChecks + t4::kT4Checks + r3t5::kR3T5Checks + r4t2::kR4T2Checks;
    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    if (g_run != expected) {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
