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
// Every loop-shaped check also asserts how many frames it measured; the run asserts
// the total number of checks.

#include "sync/netclock.h"
#include "sync/vehicle_drive.h"
#include "sync/hypertune.h"
#include "sync/vehicle_w9.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
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

// ms until a persistent snap with the car held `off` metres beside a target moving at vy
static double PersistentSnapMs(double off, double vy, double fps, int* frames) {
    VehicleDriveController c;
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
            VehicleDriveController c;
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

// ---- hyper tune (libs/sync/hypertune.h, GI 2026-11-02: the Jesko at 500 km/h) ------------------------------
static void TestHyperTune() {
    using namespace gage::sync;
    // 100 m/s stock (360 km/h) -> +38.9 % reaches 138.9 m/s (500 km/h).
    const float p = HyperTopSpeedPercent(100.0f);
    Check("hyper: a 100 m/s car gets the percentage that lands exactly on 500 km/h",
          std::fabs(100.0f * (1.0f + p / 100.0f) - kHyperTargetMs) < 0.01f, std::to_string(p));
    Check("hyper: a slow car is capped at +400 % (never more than 5x stock)", HyperTopSpeedPercent(10.0f) == kHyperMaxPercent,
          std::to_string(HyperTopSpeedPercent(10.0f)));
    Check("hyper: a car already at or above 500 km/h gets 0, an unreadable estimate gets the safe +100 %",
          HyperTopSpeedPercent(150.0f) == 0.0f && HyperTopSpeedPercent(0.0f) == 100.0f && HyperTopSpeedPercent(-5.0f) == 100.0f);
    Check("hyper: the target stays under the server's default vehicleMaxSpeed (150 m/s), so no update is dropped",
          kHyperTargetMs < 150.0f, std::to_string(kHyperTargetMs));
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

int main() {
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

    // quat 6 + interpolation 2 + extrapolation 6 + snap 5 + framerate 3 + closed loop 3 + steer 5 + W6 7 + W7 typical gap 1
    // + hyper tune 4 + W9 11 (RollDeg 1, contact grace 3, no false grace 1, lead weight 1, lead slew 1, lead accuracy 2, bike roll 1, bike log 1)
    const int expected = 6 + 2 + 6 + 5 + 3 + 3 + 5 + 7 + 1 + 4 + 11;
    std::printf("%d/%d passed\n", g_run - g_failed, g_run);
    if (g_run != expected) {
        std::printf("FAIL  ran %d checks, expected %d\n", g_run, expected);
        return 1;
    }
    return g_failed ? 1 : 0;
}
