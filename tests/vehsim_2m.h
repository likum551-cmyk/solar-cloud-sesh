// vehsim_2m.h: the TWO-MACHINE vehicle collision simulation (T4, desync overhaul round 2). It extends tests/vehsim.h (the one-way relay +
// receiver) to the situation the collision desync lives in: SEVERAL machines, each running real planar rigid-body physics for the cars
// it controls and a kinematic, velocity-overwritten copy (the REAL VehicleInterpolator + VehicleDriveController + PresentLead + NetClock
// + ClassDelay, fed through the server tick and the wire quantisation) of every other car, with per-machine latency, jitter and loss.
// A server loop ticks at 30 Hz and, depending on the Mode under test, runs the REAL ContactArbiter (libs/sync/vehicle_impact.h) and the
// empty-car claim; the machines run the REAL ImpactTracker + DecideImpactApply. Header-only, pure computation, seeded.
//
// WHAT IS MODELLED (and what is not)
//   game physics  planar oriented boxes (4.6 x 2.0 m, 1.5 t unless a car says otherwise), sequential-impulse contacts with restitution
//                 0.2, Coulomb friction 0.2 and positional correction, 1 ms steps; tyre grip (lateral velocity relaxes at 6 /s), rolling
//                 drag, yaw damping; a driver holds a target speed with 6 m/s^2 (brakes 9) and does not steer; an empty car brakes.
//                 This is a different implementation with different constants from the server's solver (libs/sync/vehicle_impact.h),
//                 so the server is NOT graded against its own model. It is not GTA's solver: real GTA physics cannot be reproduced offline.
//   a copy        a normal dynamic body (its mass is the real car's: it takes its share of an impact INSIDE the frame), whose velocity,
//                 angular velocity and (large errors) pose are overwritten every render frame by the controller, exactly as TickVehicles
//                 does (collision flag = any contact of the copy in the previous frame's physics, Sync B's assumption).
//   the network   as vehsim.h: samples at 30 Hz +-6 ms, quantised exactly as on the wire, uplink latency + jitter, the server's 30 Hz
//                 tick, one VehicleWorld frame per machine per tick (loss, downlink latency + jitter), ageMs stamps, the shared NetClock.
//                 SampleClock is idealised (the server places a sample at its true time).
//   the reference ONE world, all cars real, zero latency: what a perfect single physics instance does. Every error is against it.
//
// METRICS (Result): per scenario and mode, in the reference's frame of time (tRef = its first contact)
//   firstContact*   when / where each machine's OWN car first touched the other car's copy (or real body): the spread across machines is
//                   the "two screens disagree about when/where" number; `refContactMs` is the reference's.
//   trueOverlap*    the two real cars at the same global instant (each in its controller's world): their interpenetration. A single
//                   physics instance has none; two bounces off delayed copies overlap.
//   posErr/velErr   every car's REAL (owner-side) state vs the reference, at tRef+500, +1000, +3000 ms.
//   screenDiv       the cars' relative position as screen A shows it vs screen B (each shows its own car live and the other's copy),
//                   compared with the same quantity one render-delay-free-of-contact... reported raw at tRef+1000/+3000.
//   snaps / pops    drive-controller snaps on copies after the first one, and the largest frame-to-frame jump of any copy.
#pragma once

#include "vehsim.h"
#include "sync/vehicle_impact.h"

#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>

namespace vehsim2 {

using namespace gage::sync;
using vehsim::V3;
using vehsim::Deg;
using vehsim::kPi;
using vehsim::Rng;
namespace proto = gage::proto;
namespace imp = gage::sync::impact;

// ---------------------------------------------------------------------------------------------------------------------
// game physics (the machines' world): independent of libs/sync/vehicle_impact.h on purpose
// ---------------------------------------------------------------------------------------------------------------------
namespace gp {
struct V2 { double x = 0, y = 0; };
inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
inline V2 operator*(V2 a, double k) { return {a.x * k, a.y * k}; }
inline double Dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
inline double Cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
inline double Norm(V2 a) { return std::sqrt(Dot(a, a)); }

struct Body {
    V2 p, v;
    double h = 0, w = 0;
    double m = 1500, hl = 2.3, hw = 1.0;
    double Inertia() const { return m * (4 * hl * hl + 4 * hw * hw) / 12.0; }
    V2 Fwd() const { return {-std::sin(h), std::cos(h)}; }
    V2 Right() const { return {std::cos(h), std::sin(h)}; }
    void Corners(V2 c[4]) const {
        const V2 f = Fwd(), r = Right();
        c[0] = p + f * hl + r * hw; c[1] = p + f * hl - r * hw; c[2] = p - f * hl - r * hw; c[3] = p - f * hl + r * hw;
    }
};
struct Hit { bool hit = false; double depth = 0; V2 n, pt; };
inline Hit Sat(const Body& a, const Body& b) {
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
    // contact point: the centre of the part of b that is inside a along the normal (two deepest vertices of each, averaged)
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
    double rest = 0.2, mu = 0.2, grip = 6.0, rollDrag = 0.1, angDamp = 3.0, baum = 0.25, slop = 0.01;
    std::vector<std::vector<char>> touched;      // pairs that touched in the physics since the flags were cleared
    std::vector<std::vector<V2>> touchPt;        // where (the first time since cleared)
    void Init(int n) { touched.assign(n, std::vector<char>(n, 0)); touchPt.assign(n, std::vector<V2>(n)); }
    void ClearTouched() { for (auto& r : touched) std::fill(r.begin(), r.end(), 0); }
    void Step(double dt, const std::vector<char>& skip) {
        const int n = (int)b.size();
        for (int i = 0; i < n; ++i) {
            if (skip[i]) continue;
            Body& x = b[i];
            const V2 f = x.Fwd(), r = x.Right();
            const double vf = Dot(x.v, f) * std::exp(-rollDrag * dt), vl = Dot(x.v, r) * std::exp(-grip * dt);
            x.v = f * vf + r * vl;
            x.w *= std::exp(-angDamp * dt);
            x.p = x.p + x.v * dt;
            x.h += x.w * dt;
        }
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                if (skip[i] || skip[j]) continue;
                Body &A = b[i], &B = b[j];
                const Hit c = Sat(A, B);
                if (!c.hit) continue;
                if (!touched[i][j]) { touchPt[i][j] = touchPt[j][i] = c.pt; }
                touched[i][j] = touched[j][i] = 1;
                const double iA = 1 / A.m, iB = 1 / B.m, IA = 1 / A.Inertia(), IB = 1 / B.Inertia();
                const V2 rA = c.pt - A.p, rB = c.pt - B.p;
                const V2 vA = A.v + V2{-A.w * rA.y, A.w * rA.x}, vB = B.v + V2{-B.w * rB.y, B.w * rB.x};
                const V2 rel = vB - vA;
                const double vn = Dot(rel, c.n);
                if (vn < 0) {
                    const double den = iA + iB + IA * Cross(rA, c.n) * Cross(rA, c.n) + IB * Cross(rB, c.n) * Cross(rB, c.n);
                    const double jn = -(1 + rest) * vn / den;
                    V2 P = c.n * jn;
                    const V2 t{-c.n.y, c.n.x};
                    const double vt = Dot(rel, t);
                    const double denT = iA + iB + IA * Cross(rA, t) * Cross(rA, t) + IB * Cross(rB, t) * Cross(rB, t);
                    const double jt = std::max(-mu * jn, std::min(mu * jn, -vt / denT));
                    P = P + t * jt;
                    A.v = A.v - P * iA; B.v = B.v + P * iB;
                    A.w -= IA * Cross(rA, P); B.w += IB * Cross(rB, P);
                }
                const double corr = std::max(0.0, c.depth - slop) * baum / (iA + iB);
                A.p = A.p - c.n * (corr * iA); B.p = B.p + c.n * (corr * iB);
            }
    }
};
inline double Overlap(const Body& a, const Body& b) { const Hit c = Sat(a, b); return c.hit ? c.depth : 0.0; }
}   // namespace gp

namespace cand {
using namespace gage::sync::impact;
    // ---- CANDIDATE (b'), evaluated and NOT shipped: predict the impact with the shared solver the moment the local physics feels it ----
    //
    // The authoritative impact (ContactArbiter) reaches a controller ~100-170 ms after the contact (sample age, server tick,
    // downlink). Until then the game bounces the car off the kinematic copy, which behaves as a wall that keeps coming at the
    // other car's pre-impact velocity: the car is thrown back far too hard, and the copy ploughs through it a second time. So the
    // machine runs the SAME solver at once, with its own car's exact pre-jump state and the copy's current state, and
    //   * writes the difference between the predicted change and what the game already did onto its own car (every frame of the
    //     jump episode: the game keeps applying its wall response for a few frames, each is cancelled down to the prediction),
    //   * tells the copy's interpolator the other car's post-impact velocity (VehicleInterpolator::ReplaceFrom),
    // and the authoritative message later only reconciles the residual.
    struct CarSnap
    {
        V2 p, v;
        double h = 0, w = 0;
        MassClass mc = MassClass::Car;
    };
    struct CopySnap
    {
        uint16_t id = 0;
        CarSnap s;
    };
    struct Prediction
    {
        bool apply = false;          // write corrOwn / corrW onto the car now (velocity += corrOwn, yaw rate += corrW)
        V2 corrOwn;
        double corrW = 0;
        bool inject = false;         // a new prediction: change the copy `copyId` (velocity += dvOther, yaw rate += dwOther)
        uint16_t copyId = 0;
        V2 dvOther;
        double dwOther = 0;
        double closing = 0;
    };
    class ImpactPredictor
    {
    public:
        struct Cfg
        {
            double marginM = 0.9;        // boxes grown by this: one frame of travel at speed plus the copy's lag
            double episodeMaxMs = 60.0;  // a prediction holds the car to its target this long after the first jump frame (the game spreads one bounce over a few frames)
            double cooldownMs = 100.0;   // a new prediction (a second contact: the car spun into the copy again) may start this long after the last one began
            SolverCfg solver;
        };
        ImpactPredictor() = default;
        explicit ImpactPredictor(const Cfg& c) : cfg_(c) {}
        void Reset() { have_ = false; active_ = false; }
        bool Active() const { return active_; }
        uint32_t Predictions() const { return predictions_; }

        // Once per frame for a car this machine controls. `now` is the car's state after this frame's physics (before the
        // correction is written); jumped / jumpDv / jumpDw come from ImpactTracker::Feed for this frame.
        Prediction Step(double nowMs, const CarSnap& now, bool jumped, V2 jumpDv, double jumpDw, const std::vector<CopySnap>& copies)
        {
            Prediction out;
            const CarSnap pre = have_ ? prev_ : now;
            prev_ = now;
            have_ = true;
            if (active_ && nowMs - start_ > cfg_.episodeMaxMs) active_ = false;
            if (!jumped) return out;
            if (!active_)
            {
                if (nowMs - start_ < cfg_.cooldownMs) return out;
                // The copy this car hit: poses as they are now (the game has just pushed the boxes apart), velocities as they were
                // before the jump. The boxes are grown by a growing margin until one pair touches: the tighter the margin, the better
                // the contact point (it sets the torque), the wider one still finds a hit that was resolved by a few cm.
                const CopySnap* best = nullptr;
                Manifold bm;
                const Shape so = ShapeOf(pre.mc);
                const Pose po{now.p, pre.v, now.h, pre.w};
                for (const double margin : {0.12, 0.35, cfg_.marginM})
                {
                    for (const CopySnap& c : copies)
                    {
                        Shape go = so, gc = ShapeOf(c.s.mc);
                        go.hl += margin; go.hw += margin; gc.hl += margin; gc.hw += margin;
                        const Pose pc{c.s.p, c.s.v, c.s.h, c.s.w};
                        const Manifold m = Collide(po, go, pc, gc);
                        if (m.hit && (!best || m.depth > bm.depth)) { best = &c; bm = m; }
                    }
                    if (best) break;
                }
                if (!best) return out;
                const Pose pc{best->s.p, best->s.v, best->s.h, best->s.w};
                const Resolution r = Resolve(po, so, pc, ShapeOf(best->s.mc), bm, cfg_.solver);
                if (!r.valid) return out;
                active_ = true;
                start_ = nowMs;
                dvPred_ = r.dv[0];
                dwPred_ = std::max(-12.0, std::min(12.0, r.dw[0]));
                applied_ = V2();
                appliedW_ = 0;
                ++predictions_;
                out.inject = true;
                out.copyId = best->id;
                out.dvOther = r.dv[1];
                out.dwOther = std::max(-12.0, std::min(12.0, r.dw[1]));
                out.closing = r.closing;
            }
            applied_ = applied_ + jumpDv;
            appliedW_ += jumpDw;
            out.corrOwn = dvPred_ - applied_;
            out.corrW = dwPred_ - appliedW_;
            out.apply = true;
            applied_ = dvPred_;
            appliedW_ = dwPred_;
            return out;
        }
        // What the current prediction gave the car (for the log) and the other car.
        V2 PredictedDv() const { return dvPred_; }

    private:
        Cfg cfg_;
        bool have_ = false, active_ = false;
        CarSnap prev_;
        double start_ = -1e18;
        V2 dvPred_, applied_;
        double dwPred_ = 0, appliedW_ = 0;
        uint32_t predictions_ = 0;
    };

}   // namespace cand

// ---------------------------------------------------------------------------------------------------------------------
// scenes
// ---------------------------------------------------------------------------------------------------------------------
struct CarDef {
    const char* name = "car";
    int ctrl = 0;                               // the machine that simulates it for real at the start
    double x = 0, y = 0, h = 0, v0 = 0;         // GTA heading (0 = +y, ccw), rad
    std::function<double(double)> vTarget;      // the driver's wanted speed (m/s) by time (s); empty car: ignored (brakes)
    bool empty = false;                         // nobody drives it
    bool persist = false;                       // the driver keeps the foot down after a crash (else he brakes 250 ms after his car first touches anything)
    imp::MassClass mc = imp::MassClass::Car;
};
struct Scene {
    std::string name;
    std::vector<CarDef> cars;
    int machines = 2;
    double durMs = 7000;
};

inline std::function<double(double)> Hold(double v) { return [v](double) { return v; }; }

namespace scenes {
using imp::MassClass;
inline CarDef C(const char* n, int ctrl, double x, double y, double h, double v) {
    CarDef c; c.name = n; c.ctrl = ctrl; c.x = x; c.y = y; c.h = h; c.v0 = v; c.vTarget = Hold(v); return c;
}
inline Scene HeadOn() {   // 20 + 20 m/s, 0.4 m lateral offset (a slight overlap of the left fronts)
    Scene s; s.name = "head-on 20+20";
    s.cars.push_back(C("A", 0, 0.0, -50, 0, 20));
    s.cars.push_back(C("B", 1, 0.4, 50, kPi, 20));
    s.durMs = 7000; return s;
}
inline Scene TBone() {    // A 15 m/s north; B 12 m/s east hits A's side
    Scene s; s.name = "t-bone 15/12";
    s.cars.push_back(C("A", 0, 0.0, -52.5, 0, 15));
    s.cars.push_back(C("B", 1, -42.0, 0.0, -kPi / 2, 12));
    s.durMs = 7000; return s;
}
inline Scene RearEnd() {  // A 12 m/s ahead, B 24 m/s behind
    Scene s; s.name = "rear-end 12/24";
    s.cars.push_back(C("A", 0, 0.0, 0, 0, 12));
    s.cars.push_back(C("B", 1, 0.2, -45, 0, 24));
    s.durMs = 7000; return s;
}
inline Scene SideSwipe() { // parallel at 20 m/s, B's heading 4 degrees toward A, lateral gap 2.0 m between the centres' lines
    Scene s; s.name = "side-swipe 20/20";
    s.cars.push_back(C("A", 0, 0.0, 0, 0, 20));
    s.cars.push_back(C("B", 1, 3.8, -6, 0.025, 22));
    s.durMs = 6000; return s;
}
inline Scene IntoParked(bool lastDriverIsB = true) {   // A 15 m/s into B parked (empty); B's controller is its last driver (machine 1)
    Scene s; s.name = "into parked 15";
    s.cars.push_back(C("A", 0, 0.3, -48, 0, 15));
    CarDef b = C("B", lastDriverIsB ? 1 : 0, 0.0, 0.0, 0, 0); b.empty = true; b.vTarget = Hold(0);
    s.cars.push_back(b);
    s.durMs = 7000; return s;
}
inline Scene TwoIntoThird() {   // A from the south and C from the east both into parked B (controller: machine 1)
    Scene s; s.name = "two into a third";
    s.machines = 3;
    s.cars.push_back(C("A", 0, 0.2, -40, 0, 14));
    CarDef b = C("B", 1, 0.0, 0.0, 0, 0); b.empty = true; b.vTarget = Hold(0);
    s.cars.push_back(b);
    s.cars.push_back(C("C", 2, 52.0, 0.5, kPi / 2, 15));   // heading +90 deg = -x ... C moves toward -x
    s.durMs = 8000; return s;
}
inline Scene SustainedPush() {   // A (24 m/s) hits B (driver brakes to 6 m/s) and keeps pushing it for seconds
    Scene s; s.name = "sustained push";
    s.cars.push_back(C("A", 0, 0.0, -45, 0, 22));
    CarDef b = C("B", 1, 0.0, 0.0, 0, 10); b.vTarget = [](double t) { return t < 1.0 ? 10.0 : 6.0; };
    s.cars.push_back(b);
    s.cars[0].vTarget = Hold(24);
    s.cars[0].persist = true; s.cars[1].persist = true;
    s.durMs = 9000; return s;
}
inline Scene LightIntoHeavy() {  // a bike (250 kg) at 18 m/s into a truck (7 t) crossing at 8 m/s: the masses differ 28x
    Scene s; s.name = "bike into truck";
    CarDef a = C("A", 0, 0.0, -46.8, 0, 18); a.mc = MassClass::Light;
    CarDef b = C("B", 1, -20.8, 0.0, -kPi / 2, 8); b.mc = MassClass::Heavy;
    s.cars.push_back(a); s.cars.push_back(b);
    s.durMs = 7000; return s;
}
} // namespace scenes

// ---------------------------------------------------------------------------------------------------------------------
// the run
// ---------------------------------------------------------------------------------------------------------------------
// A bit set: kServer = the server's ContactArbiter + VehicleImpact messages applied by the controllers; kPredict = the machine runs
// the same solver the moment its car's physics feels a hit (ImpactPredictor) and changes the copy; kClaim = a driven car that
// approaches an empty one takes its control.
enum class Mode : int { Current = 0, Server = 1, Predict = 2, ServerPredict = 3, Claim = 4, ServerClaim = 5, PredictClaim = 6, All = 7,
                        Adopt = 8, AdoptServer = 9, AdoptClaim = 12, AdoptServerClaim = 13 };
inline bool Has(Mode m, Mode f) { return (int(m) & int(f)) != 0; }
inline std::string ModeName(Mode m) {
    if (m == Mode::Current) return "current";
    std::string r;
    if (Has(m, Mode::Adopt)) r += "A";
    if (Has(m, Mode::Server)) r += "S";
    if (Has(m, Mode::Predict)) r += "P";
    if (Has(m, Mode::Claim)) r += "C";
    return r;
}

struct Opt {
    Mode mode = Mode::Current;
    double upMs[4] = {25, 25, 25, 25}, downMs[4] = {25, 25, 25, 25};
    double upJitMs = 10, downJitMs = 30, loss = 0.03;
    double fps = 60;
    double sendHz = 30, sendJitMs = 6;
    double tickHz = 30;
    int flagMode = 1;                 // the collision flag: 0 never, 1 previous frame's contacts
    bool lead = true;                 // PresentLead
    bool grace = true;                // the contact grace on copies (VehicleDriveConfig::contactGraceMs 400 / 0)
    bool graceOffWhenImpact = false;  // with impacts arbitrated the grace can be switched off (the copy follows its controller's stream)
    imp::AdoptCfg adoptCfg;
    bool confirm = true;              // the guard ends early when the controller's stream shows the impact
    double adoptCooldownMs = 150.0;
    double guardMs = 300.0;           // the copy's interpolator refuses pushes this long after an injected impact
    uint64_t seed = 1;
    double claimBaseM = 14.0, claimLookS = 0.5;
    double clkOffMs[4] = {123456.0, 7777.0, 65000.0, 31000.0};   // each machine's own clock offset (the sampleMs stamps wrap)
    std::function<void(double, int, int, bool, const gp::Body&)> trace;   // (t ms, machine, car, real?, body) every ms
};

struct Result {
    // contact on each machine's own screen
    double refContactMs = -1;
    double contactMs[4] = {-1, -1, -1, -1};
    gp::V2 contactPt[4], refContactPt;
    double contactSpreadMs = -1, contactPlaceM = -1;
    // interpenetration of the two real cars at the same global instant
    double trueOverlapMax = 0, trueOverlapMs = 0;
    // owner-side state vs the reference
    double posErr[3] = {0, 0, 0}, velErr[3] = {0, 0, 0}, yawErr[3] = {0, 0, 0};   // max over cars at +500, +1000, +3000
    double posErrCar[3][3] = {};                                                   // [car][time]
    // cross-screen relative position disagreement of the first two cars
    double screenDiv[3] = {0, 0, 0};
    uint32_t snaps = 0, pops = 0, graces = 0, claims = 0, predictions = 0;
    double popMax = 0;
    // impacts
    uint32_t impacts = 0, applied = 0, noLocal = 0, tiny = 0, stale = 0;
    double corrMax = 0;
    double postSpeed[3] = {0, 0, 0};   // cars' speeds at +1000
    double refSpeed[3] = {0, 0, 0};
    double maxPenetrationScreen = 0;   // deepest overlap of a machine's real car with a copy on that machine
    imp::ContactArbiter::Stats arb;     // the arbiter's counters
    struct Applied { double t; int m, car; imp::V2 dvAuth, dvLocal, corr; double dwAuth, dwCorr; int why; double tcLocalAgeMs; };
    std::vector<Applied> log;          // every impact message a machine decided on
};

struct Sample { double t, arrive; int car; VehicleSnapshot s; imp::ArbSample a; int ctrl; };
struct Frame {
    double arrive; int64_t tick;
    struct Ent { int car; VehicleSnapshot s; };
    std::vector<Ent> ents;
    std::vector<std::pair<int, int>> ctrlNow;    // (car, controller) as the server says
};
struct ImpMsg { double arrive; int car; uint32_t seq; imp::V2 dv; double dw; uint16_t stamp; double depth; };

inline void SetBodyFromDef(gp::Body& b, const CarDef& c) {
    b.p = {c.x, c.y}; b.h = c.h; b.v = gp::V2{-std::sin(c.h), std::cos(c.h)} * c.v0; b.w = 0;
    const imp::Shape sh = imp::ShapeOf(c.mc);
    b.hl = sh.hl; b.hw = sh.hw; b.m = sh.mass;
}

// The driver of a real car: hold the target speed along the heading (accel 6, brake 9); an empty car brakes to a stop.
inline void Drive(gp::Body& b, const CarDef& c, double tSec, double dt, bool crashed) {
    const double vf = gp::Dot(b.v, b.Fwd());
    const double want = (c.empty || (crashed && !c.persist)) ? 0.0 : c.vTarget(tSec);
    double a = 0;
    if (vf < want) a = std::min(6.0 * dt, want - vf);
    else if (vf > want) a = -std::min(9.0 * dt, vf - want);
    b.v = b.v + b.Fwd() * a;
}

// The reference: one world, everybody real.
struct RefRun {
    std::vector<std::vector<gp::Body>> traj;   // [ms][car]
    double contactMs = -1; gp::V2 contactPt;
};
inline RefRun RunRef(const Scene& sc) {
    RefRun r;
    gp::World w;
    for (const CarDef& c : sc.cars) { gp::Body b; SetBodyFromDef(b, c); w.b.push_back(b); }
    const int n = int(sc.cars.size());
    w.Init(n);
    std::vector<char> skip(n, 0);
    std::vector<double> touchMs(n, -1);
    for (int ms = 0; ms < int(sc.durMs); ++ms) {
        for (int i = 0; i < n; ++i) Drive(w.b[i], sc.cars[i], ms / 1000.0, 0.001, touchMs[i] >= 0 && ms - touchMs[i] >= 250.0);
        w.ClearTouched();
        w.Step(0.001, skip);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (i != j && w.touched[i][j] && touchMs[i] < 0) touchMs[i] = ms;
        if (r.contactMs < 0)
            for (int i = 0; i < n && r.contactMs < 0; ++i)
                for (int j = i + 1; j < n; ++j)
                    if (w.touched[i][j]) { r.contactMs = ms; r.contactPt = w.touchPt[i][j]; break; }
        r.traj.push_back(w.b);
    }
    return r;
}

struct Recv {
    VehicleInterpolator ip;
    VehicleDriveController ctl;
    PresentLead lead;
    ClassDelay extra;
    bool prevTouch = false, lastFlag = false;
    double lastDriveMs = -1;
    uint32_t snapsSeen = 0;
    gp::V2 lastPos; bool haveLast = false;
    VehicleSample lastTarget; bool haveTarget = false;   // the controller's last target (the copy as the machine shows it)
    double lastDisp = 0;
    explicit Recv(const VehicleDriveConfig& dc) : ctl(dc) {}
};

struct Mach {
    NetClock clock;
    double originMs = 0; bool haveOrigin = false;
    gp::World w;
    std::vector<std::unique_ptr<Recv>> rc;
    std::vector<imp::ImpactTracker> trk;
    std::vector<cand::ImpactPredictor> pred;
    std::vector<char> real;           // this machine simulates car c for real
    std::vector<Frame> frames;        // pending, unsorted
    std::vector<ImpMsg> msgs;
    double nextFrameMs = 0;
    Rng rng;
    std::vector<double> nextSend;     // per car, when this machine samples it
    std::vector<char> hadContact;
    std::vector<double> ownTouchMs;   // when our real car c first touched anything on this machine (the driver brakes 250 ms later)
    Mach(const NetClockConfig& c, uint64_t seed) : clock(c), rng(seed) {}
};

inline double WrapAngleAbs(double a) { return std::fabs(imp::WrapPi(a)); }

inline Result Run(const Scene& sc, const Opt& o) {
    Result R;
    const int nc = int(sc.cars.size()), nm = sc.machines;
    const RefRun ref = RunRef(sc);
    R.refContactMs = ref.contactMs;
    R.refContactPt = ref.contactPt;
    Rng rng(o.seed);
    const double tickMs = 1000.0 / o.tickHz;
    const bool useImpulse = Has(o.mode, Mode::Server);
    const bool usePredict = Has(o.mode, Mode::Predict);
    const bool useClaim = Has(o.mode, Mode::Claim);
    const bool useAdopt = Has(o.mode, Mode::Adopt);

    VehicleDriveConfig dc;
    if (!o.grace || ((useImpulse || usePredict) && o.graceOffWhenImpact)) dc.contactGraceMs = 0.0f;
    const NetClockConfig ncfg = NetClockConfig::ForTickHz(o.tickHz);
    std::vector<std::unique_ptr<Mach>> M;
    for (int m = 0; m < nm; ++m) {
        auto mc = std::make_unique<Mach>(ncfg, o.seed * 1000 + m);
        mc->w.Init(nc);
        for (int c = 0; c < nc; ++c) {
            gp::Body b; SetBodyFromDef(b, sc.cars[c]);
            mc->w.b.push_back(b);
            mc->rc.push_back(std::make_unique<Recv>(dc));
            mc->trk.emplace_back();
            mc->pred.emplace_back();
            mc->real.push_back(sc.cars[c].ctrl == m ? 1 : 0);
            mc->nextSend.push_back(rng.Range(0, 33));
            mc->hadContact.push_back(0);
            mc->ownTouchMs.push_back(-1);
        }
        M.push_back(std::move(mc));
    }
    // the server's view
    std::vector<int> ctrl(nc);
    std::vector<double> ctrlSince(nc, -1e9);
    for (int c = 0; c < nc; ++c) ctrl[c] = sc.cars[c].ctrl;
    std::vector<char> haveNewest(nc, 0);
    std::vector<Sample> newest(nc);
    std::vector<Sample> uplink;   // pending uplink samples
    imp::ContactArbiter arb;
    uint32_t impSeq = 0;
    int64_t tick = 0;
    double nextTick = 0;
    std::vector<char> skipNone(nc, 0);
    std::vector<double> prevFlagUsed;

    // metrics state
    double penetMax = 0;
    bool firstContactSeen[4] = {false, false, false, false};
    std::vector<gp::V2> lastCopyPos(nc * nm);
    std::vector<char> haveLastCopy(nc * nm, 0);
    auto realBody = [&](int c) -> const gp::Body& { return M[ctrl[c]]->w.b[c]; };
    const double tRef = ref.contactMs;
    const double checkAt[3] = {500, 1000, 3000};
    bool checked[3] = {false, false, false};

    for (double t = 0; t < sc.durMs; t += 1.0) {
        // ---- uplink -> server ----
        for (size_t i = 0; i < uplink.size();) {
            if (uplink[i].arrive <= t) {
                const Sample& s = uplink[i];
                if (s.ctrl == ctrl[s.car]) {          // a sample from a controller the server no longer has is refused
                    if (!haveNewest[s.car] || s.t > newest[s.car].t) { newest[s.car] = s; haveNewest[s.car] = 1; }
                    arb.Note(uint16_t(s.car), uint16_t(s.ctrl), s.a);
                }
                uplink.erase(uplink.begin() + long(i));
            } else ++i;
        }
        // ---- server tick ----
        if (t >= nextTick) {
            nextTick += tickMs;
            ++tick;
            const double tt = double(tick) * tickMs;
            // the empty-car claim (apps/server/vehicles.h in the real thing): a driven car approaching an empty car takes its control
            if (useClaim) {
                for (int e = 0; e < nc; ++e) {
                    if (!sc.cars[e].empty || !haveNewest[e] || tt - ctrlSince[e] < 1500.0) continue;
                    for (int d = 0; d < nc; ++d) {
                        if (d == e || sc.cars[d].empty || !haveNewest[d] || ctrl[d] == ctrl[e]) continue;
                        const imp::V2 dp = newest[d].a.p - newest[e].a.p;
                        const double dist = imp::Len(dp);
                        const double sp = imp::Len(newest[d].a.v);
                        if (sp < 3.0) continue;
                        const double closing = -imp::Dot(newest[d].a.v, dp) / std::max(dist, 0.1);
                        if (closing > 1.0 && dist < o.claimBaseM + sp * o.claimLookS) {
                            ctrl[e] = ctrl[d];
                            ctrlSince[e] = tt;
                            haveNewest[e] = 1;
                            arb.Forget(uint16_t(e));
                            ++R.claims;
                            break;
                        }
                    }
                }
            }
            for (int m = 0; m < nm; ++m) {
                Frame f;
                f.tick = tick;
                f.arrive = tt + o.downMs[m] + rng.Range(0, o.downJitMs);
                const bool lost = rng.U() < o.loss;
                for (int c = 0; c < nc; ++c) {
                    f.ctrlNow.push_back({c, ctrl[c]});
                    if (ctrl[c] == m || !haveNewest[c]) continue;
                    Frame::Ent e; e.car = c; e.s = newest[c].s;
                    const double age = std::min(255.0, std::floor(tt - newest[c].t));
                    e.s.tMs = SnapshotStampMs(tick, tickMs, uint32_t(age));
                    f.ents.push_back(e);
                }
                if (!lost) M[m]->frames.push_back(std::move(f));
            }
            if (useImpulse) {
                std::vector<imp::ContactArbiter::Live> live;
                for (int c = 0; c < nc; ++c) {
                    if (!haveNewest[c]) continue;
                    imp::ContactArbiter::Live l; l.id = uint16_t(c); l.ctrl = uint16_t(ctrl[c]); l.mc = sc.cars[c].mc; l.trusted = true;
                    live.push_back(l);
                }
                std::vector<imp::ImpactOut> outs;
                arb.Step(tt, live, outs);
                for (const imp::ImpactOut& io : outs) {
                    ++R.impacts;
                    ++impSeq;
                    for (int k = 0; k < 2; ++k) {
                        const int m = io.ctrl[k];
                        if (m < 0 || m >= nm || !io.stampOk[k]) continue;
                        ImpMsg im; im.arrive = tt + o.downMs[m] + rng.Range(0, o.downJitMs);   // reliable: never lost
                        im.car = io.id[k]; im.seq = impSeq; im.dv = io.dv[k]; im.dw = io.dw[k]; im.stamp = io.stamp[k]; im.depth = io.depth;
                        M[m]->msgs.push_back(im);
                    }
                }
            }
        }
        // ---- machines ----
        for (int m = 0; m < nm; ++m) {
            Mach& mc = *M[m];
            const double nowLocal = t + o.clkOffMs[m];
            // control changes the server announced arrive with the frames
            if (t >= mc.nextFrameMs) {
                const double dtF = std::max(4.0, (1000.0 / o.fps) * mc.rng.Range(0.95, 1.05));
                mc.nextFrameMs = t + dtF;
                // frames that arrived
                std::sort(mc.frames.begin(), mc.frames.end(), [](const Frame& a, const Frame& b) { return a.arrive < b.arrive; });
                size_t used = 0;
                for (; used < mc.frames.size() && mc.frames[used].arrive <= t; ++used) {
                    const Frame& f = mc.frames[used];
                    if (!mc.haveOrigin) { mc.haveOrigin = true; mc.originMs = double(f.tick) * tickMs; }
                    mc.clock.OnFrame(uint32_t(f.tick), f.arrive + o.clkOffMs[m]);
                    for (const auto& cn : f.ctrlNow) {
                        const int c = cn.first, cm = cn.second;
                        const char wasReal = mc.real[c];
                        const char nowReal = (cm == m) ? 1 : 0;
                        if (nowReal && !wasReal) {            // we take over: the car stays where it is, our updates start from it
                            mc.real[c] = 1;
                            mc.trk[c].Reset();
                            mc.pred[c].Reset();
                            mc.nextSend[c] = t;
                        } else if (!nowReal && wasReal) {      // somebody else controls it now: a copy again
                            mc.real[c] = 0;
                            mc.rc[c]->ip.Clear();
                            mc.rc[c]->ctl.Reset();
                            mc.rc[c]->lead.Reset();
                            mc.rc[c]->lastDriveMs = -1;
                        }
                    }
                    for (const Frame::Ent& e : f.ents) {
                        if (mc.real[e.car]) continue;
                        VehicleSnapshot sr = e.s;
                        sr.tMs -= mc.originMs;
                        const VehPush r = mc.rc[e.car]->ip.Push(sr);
                        if (r == VehPush::Accepted || r == VehPush::Teleported) mc.clock.NoteAge(std::max(0.0, double(f.tick) * tickMs - e.s.tMs));
                    }
                }
                mc.frames.erase(mc.frames.begin(), mc.frames.begin() + long(used));
                // the copies
                gp::Body* own = nullptr;
                for (int c = 0; c < nc; ++c) if (mc.real[c]) { own = &mc.w.b[c]; break; }
                for (int c = 0; c < nc; ++c) {
                    if (mc.real[c] || !mc.clock.Ready()) continue;
                    Recv& rc = *mc.rc[c];
                    gp::Body& P = mc.w.b[c];
                    const double ex = rc.extra.UpdateGap(rc.ip.TypicalGapMs(), mc.clock.TickMs(), nowLocal, mc.clock.Config().maxSlew);
                    const double dtLast = rc.lastDriveMs > 0 ? t - rc.lastDriveMs : 0.0;
                    double add = 0;
                    if (o.lead) add = rc.lead.Update(own ? gp::Norm(P.p - own->p) : 1e9, mc.clock.DelayMs() + ex, dtLast);
                    VehicleSample s;
                    const double dispT = mc.clock.RenderT(nowLocal) - ex + add;
                    if (!rc.ip.Sample(dispT, s)) continue;
                    rc.lastTarget = s; rc.haveTarget = true; rc.lastDisp = dispT;
                    VehicleDriveInput in;
                    in.target = s;
                    in.localPos = V3(P.p.x, P.p.y, 0);
                    in.localVel = V3(P.v.x, P.v.y, 0);
                    in.localRot = vq::FromHeadingDeg(Deg(P.h));
                    in.dtMs = rc.lastDriveMs > 0 ? t - rc.lastDriveMs : 0.0;
                    bool anyTouch = false;
                    for (int j = 0; j < nc; ++j) if (j != c && mc.w.touched[c][j]) anyTouch = true;
                    const bool flag = o.flagMode == 0 ? false : rc.prevTouch;
                    (void)anyTouch;
                    in.contact = flag && !rc.lastFlag;
                    rc.lastFlag = flag;
                    if (!rc.haveLast) { P.p = {s.pos.x, s.pos.y}; P.h = vq::HeadingDeg(s.rot) * kPi / 180.0; P.v = {0, 0}; rc.haveLast = true; }
                    const VehicleDriveOutput out = rc.ctl.Update(in);
                    if (useAdopt && out.contactOpened && !out.snap) {
                        // the copy took an impulse inside the last frame: that IS the other car's response to the hit. Adopt it as the
                        // copy's trajectory (the stream, still showing the pre-impact motion, is refused until it shows the impact) and
                        // do not overwrite the body this frame.
                        const imp::AdoptOut ad = imp::DecideAdopt(s.vel.x, s.vel.y, s.vel.z, P.v.x, P.v.y, P.w, P.h, o.adoptCfg);
                        if (ad.adopt) {
                            VehicleSnapshot sn;
                            sn.tMs = dispT; sn.pos = V3(P.p.x, P.p.y, 0); sn.rot = vq::FromHeadingDeg(Deg(P.h));
                            sn.vel = V3(ad.vx, ad.vy, ad.vz); sn.hasAngVel = true; sn.angVel = V3(0, 0, ad.yawRate); sn.steering = 0;
                            const DriveVec preVelAdopt = s.vel;   // the stream's (pre-impact) velocity of this car
                            rc.ip.ReplaceFrom(sn, o.guardMs, o.confirm ? &preVelAdopt : nullptr);
                            ++R.predictions;
                            rc.ctl.EndGrace(o.adoptCooldownMs);
                            rc.lastDriveMs = t;
                            rc.prevTouch = false;
                            for (int j = 0; j < nc; ++j) if (j != c && mc.w.touched[c][j]) rc.prevTouch = true;
                            continue;
                        }
                    }
                    if (out.snap) { P.p = {out.snapPos.x, out.snapPos.y}; P.h = vq::HeadingDeg(out.snapRot) * kPi / 180.0; if (rc.snapsSeen++ > 0) ++R.snaps; }
                    else if (out.setQuaternion) P.h = vq::HeadingDeg(out.rot) * kPi / 180.0;
                    if (out.setVelocity) P.v = {out.velocity.x, out.velocity.y};
                    if (out.setAngularVelocity) P.w = out.angularVelocity.z;
                    // pop: the copy's own jump beyond the sender's motion
                    const int key = c * nm + m;
                    if (haveLastCopy[key] && in.dtMs > 0) {
                        const double jump = gp::Norm(P.p - lastCopyPos[key]) - gp::Norm(P.v) * in.dtMs / 1000.0;
                        R.popMax = std::max(R.popMax, jump);
                        if (jump > 0.5) { ++R.pops; if (std::getenv("T4POP")) std::printf("  [pop t=%.0f m%d car%d jump %.2f snap=%d err %.2f dev %.2f grace=%d adopt?]\n", t, m, c, jump, int(out.snap), out.posErrorM, out.devMs, int(out.graceActive)); }
                    }
                    lastCopyPos[key] = P.p; haveLastCopy[key] = 1;
                    rc.lastDriveMs = t;
                    // the collision flag for next frame: any contact of this copy in the physics since the last frame
                    rc.prevTouch = false;
                    for (int j = 0; j < nc; ++j) if (j != c && mc.w.touched[c][j]) rc.prevTouch = true;
                }
                // our real cars: feed the tracker, apply arrived impact messages
                for (int c = 0; c < nc; ++c) {
                    if (!mc.real[c]) continue;
                    gp::Body& B = mc.w.b[c];
                    const bool jumped = mc.trk[c].Feed(nowLocal, imp::V2{B.v.x, B.v.y}, B.w);
                    if (usePredict) {
                        cand::CarSnap own; own.p = {B.p.x, B.p.y}; own.v = {B.v.x, B.v.y}; own.h = B.h; own.w = B.w; own.mc = sc.cars[c].mc;
                        std::vector<cand::CopySnap> copies;
                        for (int j = 0; j < nc; ++j) {
                            if (mc.real[j] || !mc.rc[j]->haveTarget) continue;
                            const VehicleSample& ts = mc.rc[j]->lastTarget;
                            cand::CopySnap cs; cs.id = uint16_t(j); cs.s.p = {ts.pos.x, ts.pos.y}; cs.s.v = {ts.vel.x, ts.vel.y};
                            cs.s.h = vq::HeadingDeg(ts.rot) * kPi / 180.0; cs.s.w = ts.angVel.z; cs.s.mc = sc.cars[j].mc;
                            copies.push_back(cs);
                        }
                        if (std::getenv("T4DBG") && jumped) for (auto& cs : copies) std::printf("  [t=%.0f m%d car%d] own p(%.2f,%.2f) v(%.2f,%.2f) h %.2f w %.2f | copy%d p(%.2f,%.2f) v(%.2f,%.2f) h %.2f w %.2f\n", t, m, c, own.p.x, own.p.y, own.v.x, own.v.y, own.h, own.w, cs.id, cs.s.p.x, cs.s.p.y, cs.s.v.x, cs.s.v.y, cs.s.h, cs.s.w);
                        const cand::Prediction pr = mc.pred[c].Step(nowLocal, own, jumped, mc.trk[c].FrameDv(), mc.trk[c].FrameDw(), copies);
                        if (std::getenv("T4DBG") && (jumped || pr.apply)) std::printf("  [t=%.0f m%d car%d] jumped=%d jumpDv(%.2f,%.2f) jumpDw %.2f apply=%d corr(%.2f,%.2f) corrW %.2f inject=%d dvOther(%.2f,%.2f) own v(%.2f,%.2f) w %.2f\n", t, m, c, jumped, mc.trk[c].FrameDv().x, mc.trk[c].FrameDv().y, mc.trk[c].FrameDw(), pr.apply, pr.corrOwn.x, pr.corrOwn.y, pr.corrW, pr.inject, pr.dvOther.x, pr.dvOther.y, B.v.x, B.v.y, B.w);
                        if (pr.apply) {
                            B.v = B.v + gp::V2{pr.corrOwn.x, pr.corrOwn.y};
                            B.w += pr.corrW;
                            mc.trk[c].NoteWritten(imp::V2{B.v.x, B.v.y}, B.w, pr.corrOwn, pr.corrW);
                        }
                        if (pr.inject) {
                            ++R.predictions;
                            Recv& cr = *mc.rc[pr.copyId];
                            VehicleSample s0;
                            if (cr.ip.Sample(cr.lastDisp, s0)) {
                                VehicleSnapshot sn;
                                sn.tMs = cr.lastDisp; sn.pos = s0.pos; sn.rot = s0.rot;
                                sn.vel = V3(s0.vel.x + pr.dvOther.x, s0.vel.y + pr.dvOther.y, s0.vel.z);
                                sn.hasAngVel = true; sn.angVel = V3(s0.angVel.x, s0.angVel.y, s0.angVel.z + pr.dwOther);
                                sn.steering = 0;
                                cr.ip.ReplaceFrom(sn, o.guardMs);
                                gp::Body& cb = mc.w.b[pr.copyId];
                                cb.v = cb.v + gp::V2{pr.dvOther.x, pr.dvOther.y};
                                cb.w += pr.dwOther;
                            }
                        }
                    }
                }
                std::sort(mc.msgs.begin(), mc.msgs.end(), [](const ImpMsg& a, const ImpMsg& b) { return a.arrive < b.arrive; });
                size_t mu = 0;
                for (; mu < mc.msgs.size() && mc.msgs[mu].arrive <= t; ++mu) {
                    const ImpMsg& im = mc.msgs[mu];
                    if (!mc.real[im.car]) continue;
                    gp::Body& B = mc.w.b[im.car];
                    bool nearCopy = false;
                    for (int j = 0; j < nc; ++j)
                        if (j != im.car && !mc.real[j] && gp::Norm(mc.w.b[j].p - B.p) < 7.0) nearCopy = true;
                    const double tcLocal = imp::UnwrapStampMs(im.stamp, nowLocal);
                    const imp::ApplyDecision d = imp::DecideImpactApply(mc.trk[im.car], im.dv, im.dw, tcLocal, nowLocal, nearCopy, im.depth);
                    R.log.push_back({t, m, im.car, im.dv, d.dvLocal, d.dv, im.dw, d.dw, int(d.why), nowLocal - tcLocal});
                    if (d.why == imp::ApplyWhy::Applied) {
                        B.v = B.v + gp::V2{d.dv.x, d.dv.y};
                        B.w += d.dw;
                        ++R.applied;
                        R.corrMax = std::max(R.corrMax, imp::Len(d.dv));
                    } else if (d.why == imp::ApplyWhy::NoLocalContact) ++R.noLocal;
                    else if (d.why == imp::ApplyWhy::Tiny) ++R.tiny;
                    else ++R.stale;
                }
                mc.msgs.erase(mc.msgs.begin(), mc.msgs.begin() + long(mu));
                mc.w.ClearTouched();   // the flags are read once per frame (the collision flag of the NEXT frame)
            }
            // the senders: a real car is sampled at its send times
            for (int c = 0; c < nc; ++c) {
                if (!mc.real[c] || t < mc.nextSend[c]) continue;
                mc.nextSend[c] = t + 1000.0 / o.sendHz + mc.rng.Range(-o.sendJitMs, o.sendJitMs);
                const gp::Body& B = mc.w.b[c];
                const bool moving = VehicleIsMoving(float(gp::Norm(B.v)), float(std::fabs(B.w)));
                if (!moving && std::fmod(t, 200.0) > 33.0) continue;   // a still car is sent at 5 Hz
                Sample sm;
                sm.t = t; sm.car = c; sm.ctrl = m;
                proto::Kinematics k; k.x = float(B.p.x); k.y = float(B.p.y); k.z = 0; k.vx = float(B.v.x); k.vy = float(B.v.y); k.vz = 0;
                const DriveQuat q = vq::FromHeadingDeg(Deg(B.h));
                proto::Quat pq; pq.x = q.x; pq.y = q.y; pq.z = q.z; pq.w = q.w;
                const proto::Kinematics d = proto::Dequantize(proto::Quantize(k));
                const proto::Quat dq = proto::DequantizeQuat(proto::QuantizeQuat(pq));
                sm.s.tMs = t; sm.s.pos = V3(d.x, d.y, 0); sm.s.vel = V3(d.vx, d.vy, 0); sm.s.rot = DriveQuat{dq.x, dq.y, dq.z, dq.w};
                sm.s.hasAngVel = false; sm.s.steering = 0;
                sm.a.tMs = t; sm.a.p = {d.x, d.y}; sm.a.v = {d.vx, d.vy};
                sm.a.h = vq::HeadingDeg(sm.s.rot) * kPi / 180.0; sm.a.w = B.w;
                sm.a.stamp = uint16_t(uint64_t(t + o.clkOffMs[m]) & 0xFFFFu);
                sm.arrive = t + o.upMs[m] + rng.Range(0, o.upJitMs);
                uplink.push_back(sm);
            }
            // physics of this machine for 1 ms
            for (int c = 0; c < nc; ++c)
                if (mc.real[c]) Drive(mc.w.b[c], sc.cars[c], t / 1000.0, 0.001, mc.ownTouchMs[c] >= 0 && t - mc.ownTouchMs[c] >= 250.0);
            mc.w.Step(0.001, skipNone);
            if (o.trace) for (int c = 0; c < nc; ++c) o.trace(t, m, c, mc.real[c] != 0, mc.w.b[c]);
            // first contact of a real car with anything on this machine; penetration with a copy
            for (int c = 0; c < nc; ++c) {
                if (!mc.real[c]) continue;
                for (int j = 0; j < nc; ++j) {
                    if (j == c) continue;
                    if (mc.w.touched[c][j] && !mc.hadContact[c] && !firstContactSeen[m]) {
                        firstContactSeen[m] = true;
                        R.contactMs[m] = t;
                        R.contactPt[m] = mc.w.touchPt[c][j];
                    }
                    if (mc.w.touched[c][j]) { mc.hadContact[c] = 1; if (mc.ownTouchMs[c] < 0) mc.ownTouchMs[c] = t; }
                    if (!mc.real[j]) penetMax = std::max(penetMax, gp::Overlap(mc.w.b[c], mc.w.b[j]));
                }
            }
        }
        // ---- global metrics ----
        for (int i = 0; i < nc; ++i)
            for (int j = i + 1; j < nc; ++j) {
                const double ov = gp::Overlap(realBody(i), realBody(j));
                if (ov > 0.0) {
                    R.trueOverlapMax = std::max(R.trueOverlapMax, ov);
                    if (ov > 0.3) R.trueOverlapMs += 1.0;
                }
            }
        if (tRef >= 0) {
            for (int k = 0; k < 3; ++k) {
                if (checked[k] || t < tRef + checkAt[k]) continue;
                checked[k] = true;
                const int ms = std::min(int(t), int(ref.traj.size()) - 1);
                for (int c = 0; c < nc; ++c) {
                    const gp::Body& rb = ref.traj[ms][c];
                    const gp::Body& b = realBody(c);
                    const double pe = gp::Norm(b.p - rb.p), ve = gp::Norm(b.v - rb.v), ye = WrapAngleAbs(b.h - rb.h);
                    R.posErr[k] = std::max(R.posErr[k], pe);
                    R.velErr[k] = std::max(R.velErr[k], ve);
                    R.yawErr[k] = std::max(R.yawErr[k], ye);
                    if (c < 3) R.posErrCar[c][k] = pe;
                    if (k == 1 && c < 3) { R.postSpeed[c] = gp::Norm(b.v); R.refSpeed[c] = gp::Norm(rb.v); }
                }
                // cross-screen relative position of car 0 -> 1: screen of ctrl[0] shows car0 real + car1 copy; screen of ctrl[1] the opposite
                if (nc >= 2 && ctrl[0] != ctrl[1]) {
                    const gp::V2 relA = M[ctrl[0]]->w.b[1].p - M[ctrl[0]]->w.b[0].p;
                    const gp::V2 relB = M[ctrl[1]]->w.b[1].p - M[ctrl[1]]->w.b[0].p;
                    R.screenDiv[k] = gp::Norm(relA - relB);
                }
            }
        }
    }
    R.maxPenetrationScreen = penetMax;
    R.arb = arb.stats();
    for (int m = 0; m < nm; ++m) {
        for (int c = 0; c < nc; ++c) R.graces += M[m]->rc[c]->ctl.Stats().contacts;
    }
    double tmin = 1e18, tmax = -1e18; int cnt = 0;
    for (int m = 0; m < nm; ++m) if (R.contactMs[m] >= 0) { tmin = std::min(tmin, R.contactMs[m]); tmax = std::max(tmax, R.contactMs[m]); ++cnt; }
    if (cnt >= 2) R.contactSpreadMs = tmax - tmin;
    if (cnt >= 2) {
        double mx = 0;
        for (int a = 0; a < nm; ++a) for (int b = a + 1; b < nm; ++b)
            if (R.contactMs[a] >= 0 && R.contactMs[b] >= 0) mx = std::max(mx, gp::Norm(R.contactPt[a] - R.contactPt[b]));
        R.contactPlaceM = mx;
    }
    return R;
}

inline void PrintHeader() {
    std::printf("%-26s %-6s | contact ms (ref/m0/m1/spread) place | overlap max/ms | posErr .5/1/3 s | velErr .5/1/3 s | yawErr 1s | snaps pops popMax | imp app noloc pred | pen\n", "scene", "mode");
}
inline void Print(const char* scene, Mode mode, const Result& r) {
    std::printf("%-26s %-6s | %5.0f %5.0f %5.0f %5.0f %5.2f | %5.2f %5.0f | %5.2f %5.2f %5.2f | %5.2f %5.2f %5.2f | %4.2f | %2u %2u %5.2f | %2u %2u %2u %2u | %4.2f\n",
                scene, ModeName(mode).c_str(), r.refContactMs, r.contactMs[0], r.contactMs[1], r.contactSpreadMs, r.contactPlaceM, r.trueOverlapMax, r.trueOverlapMs,
                r.posErr[0], r.posErr[1], r.posErr[2], r.velErr[0], r.velErr[1], r.velErr[2], r.yawErr[1], r.snaps, r.pops, r.popMax, r.impacts, r.applied,
                r.noLocal, r.predictions, r.maxPenetrationScreen);
}

} // namespace vehsim2
