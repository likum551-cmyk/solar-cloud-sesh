// vehsim.h: an offline simulation of the whole remote-vehicle receive path (Sync D). It drives the REAL
// controllers (VehicleInterpolator, VehicleDriveController, PresentLead, NetClock, ClassDelay) from a scripted
// sender through a relay with the real wire quantisation, against a simplified physical car. Header-only, pure
// computation, seeded: every number is reproducible. Included by tests/vehicle_drive_test.cpp (and by scratch
// report programs).
//
// THE PIPELINE (what is modelled, and what is not)
//   truth      a vehicle with a kinematic-bicycle yaw rate, tyre grip that damps the sideways velocity (so a hard
//              turn drifts), a scripted throttle/brake, a ramp and ballistic flight (jump), body pitch from the
//              velocity direction and a lean in a turn. Table at 1 ms. The sender's clock IS the server's clock
//              (the engine places the sender's sample on the server's clock with SampleClock: stamp = sample time).
//   sender     samples at `sendHz` (30) with +-`sendJitterMs`; a car that moves at under 0.2 m/s (and spins under
//              0.05 rad/s) is sent at 5 Hz (VehicleSendGate), plus one sample at the instant it stops.
//   quantise   position 1 cm, velocity 1/32 m/s, rotation smallest-three 10 bits (protocol/quant.h), exactly the
//              wire; the angular velocity is NOT sent to the controller (the engine's default, veh_angvel=0).
//   uplink     latency `upMs` + U(0, upJitterMs); the server keeps the newest, ticks at `tickHz` (30), and relays the
//              vehicle every `relayEvery`-th tick (1/2/3 = the 30/15/10 Hz near/mid/far tiers) with
//              ageMs = min(255, tick time - sample time); a frame (every tick, VehicleWorld) reaches the receiver
//              after `downMs` + U(0, downJitterMs) or is lost with probability `loss`.
//   receiver   NetClock.OnFrame per frame, NoteAge per accepted snapshot, Push(tMs = tick*tickMs - ageMs), the
//              render time RenderT(now) - ClassDelay extra (+ PresentLead when `lead`), Sample, drive.Update on a
//              local car, exactly as TickVehicles does.
//   puppet     the local copy: the commands (SET_ENTITY_VELOCITY etc.) take effect at once, then one physics step:
//              tyre grip damps the sideways velocity (8 /s), drag on the forward one (0.2 /s), gravity in the air,
//              the ground (a ramp function) holds it. Its heading changes only through the commanded angular
//              velocity / quaternion.
//   measured   each frame, before the commands, the puppet against the truth AT THE INSTANT THE SAMPLE CLAIMS
//              (the display time on the sender's timeline), so the numbers are the pipeline's error (interpolation,
//              extrapolation, quantisation, controller lag), not the intended latency.
#pragma once

#include "protocol/quant.h"
#include "sync/netclock.h"
#include "sync/vehicle_drive.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

namespace vehsim {

using namespace gage::sync;
namespace proto = gage::proto;

constexpr double kPi = 3.14159265358979323846;
constexpr double kG = 9.81;

inline DriveVec V3(double x, double y, double z) { DriveVec v; v.x = float(x); v.y = float(y); v.z = float(z); return v; }
inline double Len3(const DriveVec& a) { return std::sqrt(double(a.x) * a.x + double(a.y) * a.y + double(a.z) * a.z); }
inline double Dist3(const DriveVec& a, const DriveVec& b) {
    return std::sqrt(double(a.x - b.x) * (a.x - b.x) + double(a.y - b.y) * (a.y - b.y) + double(a.z - b.z) * (a.z - b.z));
}
inline double Deg(double r) { return r * 180.0 / kPi; }

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t Next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double U() { return double(Next() >> 11) / double(1ull << 53); }
    double Range(double a, double b) { return a + (b - a) * U(); }
};

// ---- the truth ---------------------------------------------------------------------------------------------

struct TruthPt {
    DriveVec pos, vel, angVel;
    DriveQuat rot;
    float steer = 0;      // wire steering, -1 left .. +1 right
    bool airborne = false;
};

// Ground height under (x, y): a ramp up along +y at y in [rampY, rampY + rampLen] by rampH, then the flat ground (the
// drop behind the lip), only when rampH > 0.
struct Ground {
    double rampY = 0, rampLen = 0, rampH = 0;
    double At(double /*x*/, double y) const {
        if (!(rampH > 0.0) || y < rampY) return 0.0;
        if (y > rampY + rampLen) return 0.0;
        return rampH * (y - rampY) / rampLen;
    }
};

// The driver's script: acceleration (m/s^2 along the heading) and the front wheel angle (rad, + = left) over time.
struct Script {
    std::function<double(double)> accel = [](double) { return 0.0; };
    std::function<double(double)> steer = [](double) { return 0.0; };
    double v0 = 0.0;          // initial forward speed
    double x0 = 0.0, y0 = 0.0, h0 = 0.0;   // start position, heading (rad, + = counter-clockwise)
    double durMs = 20000;
    Ground ground;
    double wheelbase = 2.8, grip = 8.0;
    double stopBelow = -1.0;  // brakes that would reverse the car hold it at 0 instead (the script's brake)
};

inline DriveQuat BodyQuat(double yaw, double pitch, double roll) {
    return vq::Mul(vq::FromHeadingDeg(Deg(yaw)), vq::Mul(vq::Exp(V3(pitch, 0, 0)), vq::Exp(V3(0, roll, 0))));
}

class Truth {
public:
    explicit Truth(const Script& sc) : sc_(sc) {
        const int n = int(sc.durMs) + 2;
        tab_.resize(n);
        double x = sc.x0, y = sc.y0, z = sc.ground.At(sc.x0, sc.y0), psi = sc.h0, u = sc.v0, vl = 0, vz = 0, prevY = sc.y0;
        bool air = false;
        for (int i = 0; i < n; ++i) {
            const double t = i / 1000.0;
            double a = sc.accel(t);
            const double delta = sc.steer(t);
            // the script's braking never reverses the car: when a < 0 and the speed is spent, it holds at 0
            if (sc.stopBelow >= 0 && a < 0 && u <= sc.stopBelow) { a = 0; u = 0; }
            const double r = u * std::tan(delta) / sc.wheelbase;
            // sideways velocity in the body frame: rotated by the yaw, damped by the tyres (none in the air)
            vl += (-u * r) * 0.001;
            if (!air) vl -= sc.grip * vl * 0.001;
            u += a * 0.001;
            psi += r * 0.001;
            const double fx = -std::sin(psi), fy = std::cos(psi), rx = std::cos(psi), ry = std::sin(psi);
            const double vx = u * fx + vl * rx, vy = u * fy + vl * ry;
            x += vx * 0.001;
            y += vy * 0.001;
            const double gz = sc.ground.At(x, y);
            const double rampEnd = sc.ground.rampY + sc.ground.rampLen;
            if (!air) {
                if (sc.ground.rampH > 0.0 && y > rampEnd && prevY <= rampEnd) {
                    air = true;                                   // the lip: thrown up at the ramp's slope
                    vz = vy * sc.ground.rampH / sc.ground.rampLen;
                } else {
                    z = gz;
                    vz = (sc.ground.rampH > 0.0 && y >= sc.ground.rampY && y <= rampEnd) ? vy * sc.ground.rampH / sc.ground.rampLen : 0.0;
                }
            }
            if (air) {
                vz -= kG * 0.001;
                z += vz * 0.001;
                if (z <= gz) {                                    // landing: a small bounce, then rest
                    z = gz;
                    if (vz < -1.0) vz = -0.2 * vz; else { vz = 0.0; air = false; }
                }
            }
            prevY = y;
            TruthPt& p = tab_[i];
            p.pos = V3(x, y, z);
            p.vel = V3(vx, vy, vz);
            const double vh = std::sqrt(vx * vx + vy * vy);
            const double pitch = vh > 1.0 ? std::atan2(vz, vh) * (u >= 0 ? 1.0 : -1.0) : 0.0;
            const double roll = std::max(-0.1, std::min(0.1, 0.006 * u * r));
            p.rot = BodyQuat(psi, pitch, roll);
            p.angVel = V3(0, 0, r);
            p.steer = float(std::max(-1.0, std::min(1.0, -delta / kSteerLockRad)));
            p.airborne = air;
        }
    }
    TruthPt At(double tMs) const {
        if (tMs <= 0) return tab_.front();
        const size_t i = size_t(tMs);
        if (i + 1 >= tab_.size()) return tab_.back();
        const double u = tMs - double(i);
        TruthPt r = tab_[i];
        const TruthPt& b = tab_[i + 1];
        r.pos = V3(r.pos.x + (b.pos.x - r.pos.x) * u, r.pos.y + (b.pos.y - r.pos.y) * u, r.pos.z + (b.pos.z - r.pos.z) * u);
        r.vel = V3(r.vel.x + (b.vel.x - r.vel.x) * u, r.vel.y + (b.vel.y - r.vel.y) * u, r.vel.z + (b.vel.z - r.vel.z) * u);
        r.rot = vq::Slerp(r.rot, b.rot, u);
        return r;
    }
    const Script& S() const { return sc_; }
    double DurMs() const { return double(tab_.size() - 1); }

private:
    Script sc_;
    std::vector<TruthPt> tab_;
};

// ---- the puppet (the local copy of the car) -------------------------------------------------------------------

struct Puppet {
    DriveVec pos, vel, angVel;
    DriveQuat rot;
    Ground ground;
    double grip = 8.0, drag = 0.2;
    // The commands take effect at once, then one physics step of dtMs.
    void Apply(const VehicleDriveOutput& o, double dtMs) {
        const double dt = dtMs / 1000.0;
        if (o.snap) { pos = o.snapPos; rot = o.snapRot; }
        if (o.setQuaternion) rot = o.rot;
        if (o.snap) { vel = o.velocity; angVel = o.angularVelocity; }
        else {
            if (o.setVelocity) vel = o.velocity;
            if (o.setAngularVelocity) angVel = o.angularVelocity;
        }
        const double gz = ground.At(pos.x, pos.y);
        const bool air = pos.z > gz + 0.03;
        double vx = vel.x, vy = vel.y, vz = vel.z;
        if (!air) {
            // tyre grip: the sideways (body x) velocity decays, the forward one loses to drag
            const DriveVec f = vq::Rotate(rot, V3(0, 1, 0)), r = vq::Rotate(rot, V3(1, 0, 0));
            const double fl = std::sqrt(double(f.x) * f.x + double(f.y) * f.y);
            if (fl > 1e-3) {
                const double fx = f.x / fl, fy = f.y / fl, rx = r.x / fl, ry = r.y / fl;
                double u = vx * fx + vy * fy, l = vx * rx + vy * ry;
                l *= std::exp(-grip * dt);
                u *= std::exp(-drag * dt);
                vx = u * fx + l * rx;
                vy = u * fy + l * ry;
            }
            if (vz < 0) vz = 0;
        } else {
            vz -= kG * dt;
        }
        vel = V3(vx, vy, vz);
        double nz = pos.z + vz * dt;
        if (nz < gz) { nz = gz; if (vel.z < 0) vel.z = 0; }
        pos = V3(pos.x + vx * dt, pos.y + vy * dt, nz);
        rot = vq::Mul(vq::Exp(V3(angVel.x * dt, angVel.y * dt, angVel.z * dt)), rot);
    }
};

// ---- the run ------------------------------------------------------------------------------------------------

struct NetCfg {
    double sendHz = 30.0, sendJitterMs = 6.0;
    double upMs = 25.0, upJitterMs = 10.0;
    double tickHz = 30.0;
    int relayEvery = 1;             // 1 = 30 Hz near tier, 2 = 15 Hz, 3 = 10 Hz
    double downMs = 25.0, downJitterMs = 30.0;
    double loss = 0.03;
    bool quantise = true;
    bool sendAngVel = false;        // the engine default is false (veh_angvel=0)
    double stallFromMs = -1.0, stallToMs = -1.0;   // every frame whose tick time is in this window is lost (a network hole)
    double handoverMs = -1.0;       // the sender's source changes here: the new controller's car is handoverLagMs behind the old one's
    double handoverLagMs = 0.0;
    uint64_t seed = 1;
    // R3 T5: the REAL stamp. SampleClock places a sample at sender + min(recv - sender), so the constant part of the uplink latency (upMs) is
    // absorbed into the offset: the stamp (and so ageMs) lags the true sample time by upMs. The older sims stamped the true time (false).
    bool stampBias = false;
    // R3 T5: the live log's rhythm: WorldState and VehicleWorld of one tick arrive 0.5 ms apart (601 frames per 10 s at 30 Hz, gap mean 17 ms).
    bool secondStream = false;
};

struct RunCfg {
    NetCfg net;
    double fps = 60.0;
    bool lead = false;              // PresentLead (within 30 m of the local player)
    double leadDistM = 10.0;        // the local player's distance from the car
    double startMs = 0.0;           // the receiver connects at this server time (a stream-in when > 0)
    // The puppet's start: false = at the first sample's pose with zero velocity (how SpawnNetVehicle places it)
    bool spawnMoving = false;
    double handoverMs = -1.0;       // a controller hand-over: interp.Clear + drive.Reset at this time
    bool handoverKeep = false;      // ... unless this is set (the buffer and the controller carry on)
    double measureFromMs = 3000.0;  // errors are measured from here (after the buffer filled)
    double measureToMs = 1e12;
    VehicleDriveConfig drive;
    VehicleInterpConfig interp;
    // R3 T5: the near-car clock (NetClock::NearT + the link lag, the lead taking the difference to the delayed clock): off = round 2's lead
    // (the delay + the class extra). linkLagMs = (our RTT + the owner's RTT) / 2 when on.
    bool nearClock = false;
    double linkLagMs = 0.0;
    PresentLeadConfig leadCfg;
    // Extra hooks (mutants and probes): called per frame after the controller output, before Apply
    std::function<void(VehicleDriveOutput&)> tweak;
    // trace: per frame (now, display time, position error, heading error deg, snap, mode)
    std::function<void(double, double, double, double, bool, int)> trace;
};

struct Metrics {
    int frames = 0;
    double posMean = 0, posP95 = 0, posMax = 0;
    double angMean = 0, angMax = 0;
    double latMax = 0, latMean = 0;       // sideways error vs the truth path (perpendicular to the truth heading)
    double lonMax = 0;                    // along the path
    double speedRippleRms = 0;            // rms of (puppet speed - truth speed at the display time), m/s
    double accelSpikeMax = 0;             // the largest frame-to-frame change of the puppet's own velocity, m/s^2
    uint32_t snaps = 0, persistentSnaps = 0, underFloorSnaps = 0, blends = 0, graces = 0;
    uint32_t pushed = 0, repeats = 0, stale = 0, teleports = 0;
    double settleMs = -1;                 // from startMs: first time pos error < 0.3 m and heading < 2 deg for 0.5 s
    double endPosErr = 0, endAngErr = 0;  // mean over the last 1 s of the measured window
    double sideDriftEnd = 0;              // signed lateral error at the end (m)
    double headingFlipMax = 0;            // largest heading error while the truth is slower than 0.5 m/s (deg)
    double headingStillErr = 0;           // mean heading error while the truth is slower than 0.5 m/s
    double posStillMax = 0;               // largest position error while the truth is slower than 0.5 m/s
    double leadOvershootMax = 0;          // present-lead: largest |display - truth| in the lead window
    double zBounceMax = 0;                // largest vertical error while the truth is on the ground
    double zFloatMax = 0;                 // largest vertical error while the truth is in the air
    double popMax = 0;                    // largest per-frame displacement beyond the truth's own (m): a snap or a jump
    int pops = 0;                         // frames with a pop over 0.3 m
    // R3 T5: the copy against the owner's TRUE position at the same global instant (the pipeline's error plus the network latency the
    // sender's stamp does not carry), and how much that error moves from frame to frame (a steady lag does not; a flapping clock does).
    double nowMean = 0, nowP95 = 0, nowMax = 0;
    double stepP95 = 0, stepMax = 0;      // |change of (copy - truth now)| per frame, m
    double leadMean = 0, leadMax = 0;     // ms
    int nowFrames = 0;
    // R3 T5: the shared clock over the measured window (rises are counted from the start of the run)
    double delayMeanMs = 0, delayMaxMs = 0, delayMinMs = 1e9;
    uint32_t delayRises = 0, delayRisesBig = 0;
    double riseMeanMs = 0, riseMaxMs = 0;
    double runSec = 0;
    uint32_t resyncs = 0, baselineSteps = 0;
};

struct Frame { double arrive; int64_t tick; std::vector<VehicleSnapshot> snaps; };

inline Metrics Run(const Truth& truth, const RunCfg& rc) {
    const NetCfg& nc = rc.net;
    Rng rng(nc.seed);
    const double tickMs = 1000.0 / nc.tickHz;
    const double durMs = truth.DurMs();

    // ---- sender: samples ----
    struct Sample { double t; double arrive; VehicleSnapshot s; };
    std::vector<Sample> samples;
    {
        const double stillStep = 200.0, sendStep = 1000.0 / nc.sendHz;
        double t = 0;
        bool wasMoving = true;
        double lastSent = -1e9;
        while (t < durMs) {
            const double tt = std::max(0.0, std::min(durMs, t + rng.Range(-nc.sendJitterMs, nc.sendJitterMs)));
            const TruthPt p = truth.At((nc.handoverMs >= 0 && tt >= nc.handoverMs) ? tt - nc.handoverLagMs : tt);
            const bool moving = VehicleIsMoving(float(Len3(p.vel)), float(Len3(p.angVel)));
            bool send = false;
            if (moving) send = true;
            else if (wasMoving) send = true;                                   // the stop goes out at once
            else if (tt - lastSent >= stillStep) send = true;
            wasMoving = moving;
            if (send) {
                lastSent = tt;
                VehicleSnapshot s;
                s.tMs = tt;
                proto::Kinematics k;
                k.x = p.pos.x; k.y = p.pos.y; k.z = p.pos.z; k.vx = p.vel.x; k.vy = p.vel.y; k.vz = p.vel.z;
                proto::Quat q; q.x = p.rot.x; q.y = p.rot.y; q.z = p.rot.z; q.w = p.rot.w;
                if (nc.quantise) {
                    const proto::Kinematics d = proto::Dequantize(proto::Quantize(k));
                    const proto::Quat dq = proto::DequantizeQuat(proto::QuantizeQuat(q));
                    s.pos = V3(d.x, d.y, d.z); s.vel = V3(d.vx, d.vy, d.vz); s.rot = DriveQuat{dq.x, dq.y, dq.z, dq.w};
                } else {
                    s.pos = p.pos; s.vel = p.vel; s.rot = p.rot;
                }
                s.hasAngVel = nc.sendAngVel;
                if (nc.sendAngVel) s.angVel = p.angVel;
                s.steering = p.steer;
                Sample sm; sm.t = tt; sm.arrive = tt + nc.upMs + rng.Range(0, nc.upJitterMs); sm.s = s;
                samples.push_back(sm);
            }
            t += moving ? sendStep : 20.0;                                       // a still car is polled at 50 Hz, sent at 5
        }
        std::stable_sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.arrive < b.arrive; });
    }

    // ---- server: ticks, the newest sample per tick, the relay tier ----
    std::vector<Frame> frames;
    {
        size_t next = 0;
        int newest = -1;
        for (int64_t tick = 0; double(tick) * tickMs < durMs + 500; ++tick) {
            const double tt = double(tick) * tickMs;
            while (next < samples.size() && samples[next].arrive <= tt) {
                if (newest < 0 || samples[next].t > samples[size_t(newest)].t) newest = int(next);
                ++next;
            }
            Frame f;
            f.tick = tick;
            f.arrive = tt + nc.downMs + rng.Range(0, nc.downJitterMs);
            const bool lost = rng.U() < nc.loss || (tt >= nc.stallFromMs && tt < nc.stallToMs);
            const bool relayTick = (tick % nc.relayEvery) == 0;
            if (relayTick && newest >= 0) {
                const Sample& sm = samples[size_t(newest)];
                // a still car's newest sample is relayed again and again: the receiver drops the identical ones (VehPush::Repeat)
                VehicleSnapshot s = sm.s;
                const double age = std::min(255.0, std::floor(std::max(0.0, tt - sm.t - (nc.stampBias ? nc.upMs : 0.0))));
                s.tMs = SnapshotStampMs(tick, tickMs, uint32_t(age));
                f.snaps.push_back(s);
            }
            if (!lost) frames.push_back(f);
        }
        std::stable_sort(frames.begin(), frames.end(), [](const Frame& a, const Frame& b) { return a.arrive < b.arrive; });
    }

    // ---- receiver + puppet ----
    NetClockConfig ncfg = NetClockConfig::ForTickHz(nc.tickHz);
    NetClock clock(ncfg);
    VehicleInterpolator ip(rc.interp);
    VehicleDriveController ctl(rc.drive);
    PresentLead lead(rc.leadCfg);
    ClassDelay extra;
    Puppet pup;
    pup.ground = truth.S().ground;
    pup.grip = truth.S().grip;
    bool spawned = false;

    Metrics m;
    std::vector<double> posErrs, nowErrs, stepErrs;
    double lastEx = 0, lastEy = 0, lastEz = 0, delaySum = 0;
    bool haveLastErr = false;
    double sumPos = 0, sumAng = 0, sumLat = 0, sumSpeedSq = 0, stillAngSum = 0;
    int stillFrames = 0, speedFrames = 0;
    double lastTime = -1;
    DriveVec lastVel; bool haveLastVel = false;
    DriveVec lastPos; bool haveLastPos = false;
    double settleStart = -1;
    bool handed = false;
    double endPosSum = 0, endAngSum = 0; int endN = 0;
    size_t fi = 0;
    double originMs = 0;                  // the first frame's tick time: the clock's timeline starts there (Unwrap is 0 at the first frame)
    bool haveOrigin = false;
    Rng frng(nc.seed ^ 0xABCDEF);
    double now = rc.startMs + 0.0;
    // the local clock is the server clock; frames arrive late by their latency, which NetClock measures
    for (; now < durMs; ) {
        const double dt = std::max(4.0, (1000.0 / rc.fps) * frng.Range(0.95, 1.05));
        now += dt;
        while (fi < frames.size() && frames[fi].arrive <= now) {
            const Frame& f = frames[fi++];
            if (f.tick * tickMs < rc.startMs) continue;                          // before the receiver connected
            if (!haveOrigin) { haveOrigin = true; originMs = double(f.tick) * tickMs; }
            clock.OnFrame(uint32_t(f.tick), f.arrive);
            if (nc.secondStream) clock.OnFrame(uint32_t(f.tick), f.arrive + 0.5);
            for (const VehicleSnapshot& s : f.snaps) {
                if (rc.handoverMs >= 0 && !handed && f.tick * tickMs >= rc.handoverMs && !rc.handoverKeep) {
                    ip.Clear();
                    ctl.Reset();
                    handed = true;
                }
                VehicleSnapshot sr = s;
                sr.tMs -= originMs;                                              // the receiver's timeline
                const VehPush r = ip.Push(sr);
                if (r == VehPush::Accepted || r == VehPush::Teleported) clock.NoteAge(std::max(0.0, double(f.tick) * tickMs - s.tMs));
            }
        }
        if (!clock.Ready()) continue;
        const double ex = extra.UpdateGap(ip.TypicalGapMs(), clock.TickMs(), now, clock.Config().maxSlew);
        double add = 0;
        const double renderT = clock.RenderT(now);
        if (rc.lead) {
            const double want = rc.nearClock ? NearLeadWantMs(clock.NearT(now), rc.linkLagMs, renderT, ex) : clock.DelayMs() + ex;
            add = lead.Update(rc.leadDistM, want, dt);
        }
        const double disp = renderT - ex + add;
        if (rc.nearClock) ip.SetExtrapolateMaxMs(rc.interp.extrapolateMaxMs + add);
        VehicleSample s;
        if (!ip.Sample(disp, s)) continue;
        const TruthPt tp = truth.At(disp + originMs);
        const TruthPt tn = truth.At(now);
        if (!spawned) {
            spawned = true;
            pup.pos = s.pos;
            pup.rot = s.rot;
            pup.vel = rc.spawnMoving ? s.vel : V3(0, 0, 0);
            pup.angVel = V3(0, 0, 0);
            lastTime = now;
        }
        VehicleDriveInput in;
        in.target = s;
        in.localPos = pup.pos;
        in.localVel = pup.vel;
        in.localRot = pup.rot;
        in.dtMs = now - lastTime;
        // the error each frame starts with
        const double pe = Dist3(pup.pos, tp.pos);
        const double peNow = Dist3(pup.pos, tn.pos);
        const double ae = Deg(vq::AngleBetween(pup.rot, tp.rot));
        VehicleDriveOutput o = ctl.Update(in);
        if (rc.tweak) rc.tweak(o);
        if (rc.trace) rc.trace(now, disp, pe, ae, o.snap, int(s.mode));
        const bool measure = now >= rc.measureFromMs && now <= rc.measureToMs;
        if (measure) {
            ++m.frames;
            posErrs.push_back(pe);
            sumPos += pe; sumAng += ae;
            m.posMax = std::max(m.posMax, pe);
            m.angMax = std::max(m.angMax, ae);
            // lateral / longitudinal vs the truth heading
            const DriveVec f = vq::Rotate(tp.rot, V3(0, 1, 0));
            const double fl = std::sqrt(double(f.x) * f.x + double(f.y) * f.y);
            if (fl > 1e-3) {
                const double fx = f.x / fl, fy = f.y / fl;
                const double dx = pup.pos.x - tp.pos.x, dy = pup.pos.y - tp.pos.y;
                const double lon = dx * fx + dy * fy, lat = dx * fy - dy * fx;   // + = to the right of the truth heading
                m.lonMax = std::max(m.lonMax, std::fabs(lon));
                m.latMax = std::max(m.latMax, std::fabs(lat));
                sumLat += std::fabs(lat);
                m.sideDriftEnd = lat;
            }
            const double tspeed = Len3(tp.vel);
            const double pspeed = Len3(pup.vel);
            sumSpeedSq += (pspeed - tspeed) * (pspeed - tspeed);
            ++speedFrames;
            if (tspeed < 0.5) {
                ++stillFrames;
                stillAngSum += ae;
                m.headingFlipMax = std::max(m.headingFlipMax, ae);
                m.posStillMax = std::max(m.posStillMax, pe);
            }
            if (haveLastVel && in.dtMs > 0 && o.setVelocity) {
                const double a = Dist3(o.velocity, lastVel) / (in.dtMs / 1000.0);
                m.accelSpikeMax = std::max(m.accelSpikeMax, a);
            }
            {
                const double ex2 = double(pup.pos.x) - tn.pos.x, ey2 = double(pup.pos.y) - tn.pos.y, ez2 = double(pup.pos.z) - tn.pos.z;
                if (haveLastErr) {
                    const double st = std::sqrt((ex2 - lastEx) * (ex2 - lastEx) + (ey2 - lastEy) * (ey2 - lastEy) + (ez2 - lastEz) * (ez2 - lastEz));
                    stepErrs.push_back(st);
                    m.stepMax = std::max(m.stepMax, st);
                }
                lastEx = ex2; lastEy = ey2; lastEz = ez2; haveLastErr = true;
                nowErrs.push_back(peNow);
                m.nowMean += peNow;
                m.nowMax = std::max(m.nowMax, peNow);
                delaySum += clock.DelayMs();
                m.delayMaxMs = std::max(m.delayMaxMs, clock.DelayMs());
                m.delayMinMs = std::min(m.delayMinMs, clock.DelayMs());
                m.leadMean += lead.LeadMs();
                m.leadMax = std::max(m.leadMax, lead.LeadMs());
            }
            if (haveLastPos && in.dtMs > 0) {
                const double dx = pup.pos.x - lastPos.x - tp.vel.x * in.dtMs / 1000.0, dy = pup.pos.y - lastPos.y - tp.vel.y * in.dtMs / 1000.0;
                const double pop = std::sqrt(dx * dx + dy * dy);
                m.popMax = std::max(m.popMax, pop);
                if (pop > 0.3) ++m.pops;
            }
            if (tp.airborne) m.zFloatMax = std::max(m.zFloatMax, std::fabs(double(pup.pos.z) - tp.pos.z));
            else m.zBounceMax = std::max(m.zBounceMax, std::fabs(double(pup.pos.z) - tp.pos.z));
            if (rc.lead) m.leadOvershootMax = std::max(m.leadOvershootMax, pe);
            if (now > durMs - 1000.0) { endPosSum += pe; endAngSum += ae; ++endN; }
        }
        if (settleStart < 0 && pe < 0.3 && ae < 2.0) settleStart = now;
        if (pe >= 0.3 || ae >= 2.0) settleStart = -1;
        if (m.settleMs < 0 && settleStart >= 0 && now - settleStart >= 500.0) m.settleMs = settleStart - rc.startMs;
        lastPos = pup.pos; haveLastPos = true;
        pup.Apply(o, in.dtMs);
        if (o.setVelocity) { lastVel = o.velocity; haveLastVel = true; }
        lastTime = now;
    }
    if (m.frames > 0) {
        m.posMean = sumPos / m.frames;
        m.angMean = sumAng / m.frames;
        m.latMean = sumLat / m.frames;
        std::sort(posErrs.begin(), posErrs.end());
        m.posP95 = posErrs[size_t(posErrs.size() * 0.95)];
        if (speedFrames > 0) m.speedRippleRms = std::sqrt(sumSpeedSq / speedFrames);
        if (!nowErrs.empty()) {
            m.delayMeanMs = delaySum / double(nowErrs.size());
            m.nowFrames = int(nowErrs.size());
            m.nowMean /= double(nowErrs.size());
            m.leadMean /= double(nowErrs.size());
            std::sort(nowErrs.begin(), nowErrs.end());
            m.nowP95 = nowErrs[size_t(double(nowErrs.size()) * 0.95)];
        }
        if (!stepErrs.empty()) {
            std::sort(stepErrs.begin(), stepErrs.end());
            m.stepP95 = stepErrs[size_t(double(stepErrs.size()) * 0.95)];
        }
        if (stillFrames > 0) m.headingStillErr = stillAngSum / stillFrames;
    }
    if (endN > 0) { m.endPosErr = endPosSum / endN; m.endAngErr = endAngSum / endN; }
    m.delayRises = clock.Stats().delayRises;
    m.delayRisesBig = clock.Stats().delayRisesBig;
    m.riseMeanMs = clock.Stats().delayRises ? clock.Stats().delayRiseSumMs / double(clock.Stats().delayRises) : 0.0;
    m.riseMaxMs = clock.Stats().delayRiseMaxMs;
    m.resyncs = clock.Stats().resyncs;
    m.baselineSteps = clock.Stats().baselineSteps;
    m.runSec = durMs / 1000.0;
    m.snaps = ctl.Stats().snaps;
    m.persistentSnaps = ctl.Stats().persistentSnaps;
    m.underFloorSnaps = ctl.Stats().underFloorSnaps;
    m.blends = ctl.Stats().blends;
    m.graces = ctl.Stats().contacts;
    m.pushed = ip.Stats().accepted;
    m.repeats = ip.Stats().repeats;
    m.stale = ip.Stats().stale;
    m.teleports = ip.Stats().teleports;
    return m;
}

// ---- the scenarios -------------------------------------------------------------------------------------------

namespace scn {

inline Script Straight(double v, double durMs = 20000) {
    Script s; s.v0 = v; s.durMs = durMs;
    s.accel = [](double) { return 0.0; };
    return s;
}
// 25 m/s, a lane change: +/-0.06 rad for 0.7 s each way with a 0.8 s gap
inline Script LaneChange(double v = 25.0) {
    Script s; s.v0 = v; s.durMs = 16000;
    s.steer = [](double t) {
        const double a = 0.06;
        if (t >= 5.0 && t < 5.7) return a;
        if (t >= 5.7 && t < 6.4) return -a;
        if (t >= 7.2 && t < 7.9) return -a;
        if (t >= 7.9 && t < 8.6) return a;
        return 0.0;
    };
    return s;
}
// 30 m/s, a hard brake at 9 m/s^2 from 5 s to a stop, then 8 s of standing
inline Script HardBrake(double v = 30.0) {
    Script s; s.v0 = v; s.durMs = 20000; s.stopBelow = 0.0;
    s.accel = [](double t) { return (t >= 5.0) ? -9.0 : 0.0; };
    return s;
}
// the same with a slight yaw while it comes to rest (a car stopping in a bend)
inline Script BrakeInBend(double v = 20.0) {
    Script s; s.v0 = v; s.durMs = 20000; s.stopBelow = 0.0;
    s.accel = [](double t) { return (t >= 5.0) ? -7.0 : 0.0; };
    s.steer = [](double t) { return (t >= 4.5 && t < 8.0) ? 0.08 : 0.0; };
    return s;
}
// reverse: from rest, back up to -8 m/s with a turn, then stop
inline Script Reverse() {
    Script s; s.v0 = 0; s.durMs = 20000; s.stopBelow = -1.0;
    s.accel = [](double t) {
        if (t < 3.0) return 0.0;
        if (t < 5.0) return -4.0;
        if (t < 9.0) return 0.0;
        if (t < 10.0) return 8.0;
        return 0.0;
    };
    s.steer = [](double t) { return (t >= 6.0 && t < 8.0) ? 0.3 : 0.0; };
    return s;
}
// a hairpin: 12 m/s, full lock 0.5 rad for 3.5 s, then straight
inline Script Hairpin() {
    Script s; s.v0 = 12; s.durMs = 18000;
    s.steer = [](double t) { return (t >= 4.0 && t < 7.5) ? 0.5 : 0.0; };
    return s;
}
// a 30 m/s run up a 2 m / 15 m ramp that ends at y = 215 (a jump), then on
inline Script Jump() {
    Script s; s.v0 = 30; s.durMs = 14000;
    s.y0 = 0;
    s.ground.rampY = 200; s.ground.rampLen = 15; s.ground.rampH = 2.0;
    return s;
}
// parked: no motion at all (the car's own rest jitter is not modelled: a standing car's samples are identical)
// a crawl: from rest up to `v` m/s in 2 s, then constant (parking speed)
inline Script Crawl(double v) {
    Script s; s.v0 = 0; s.durMs = 16000; s.stopBelow = 0.0;
    s.accel = [v](double t) { return t >= 2.0 && t < 4.0 ? v / 2.0 : 0.0; };
    return s;
}
inline Script Parked() { Script s; s.v0 = 0; s.durMs = 12000; return s; }
// a gentle curve at 139 m/s (500 km/h), steering 0.004 rad
inline Script HyperCurve() {
    Script s; s.v0 = 139; s.durMs = 20000;
    s.steer = [](double t) { return (t >= 4.0 && t < 12.0) ? 0.004 : 0.0; };
    return s;
}
inline Script HyperStraight() { return Straight(139.0, 20000); }

} // namespace scn

inline void Print(const char* name, const Metrics& m) {
    std::printf("%-34s fr %5d pos %.3f/%.3f/%.3f ang %.2f/%.2f lat %.3f/%.3f lon %.3f rip %.2f spike %.0f snaps %u/%u/%u bl %u gr %u settle %.0f end %.3f/%.2f"
                " still(h %.2f/%.2f p %.3f) z %.2f/%.2f pop %.2f/%d\n",
                name, m.frames, m.posMean, m.posP95, m.posMax, m.angMean, m.angMax, m.latMean, m.latMax, m.lonMax, m.speedRippleRms,
                m.accelSpikeMax, m.snaps, m.persistentSnaps, m.underFloorSnaps, m.blends, m.graces, m.settleMs, m.endPosErr, m.endAngErr,
                m.headingStillErr, m.headingFlipMax, m.posStillMax, m.zBounceMax, m.zFloatMax, m.popMax, m.pops);
}

} // namespace vehsim
