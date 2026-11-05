// 0.4.22 "cars shake": the frame of SET_ENTITY_ANGULAR_VELOCITY, and the meters that say whether a car shakes.
//
// 1. THE FRAME. GI's live log of 2026-11-05 (gage.log.1 08:06:52, gage.log 15:26:44) has the engine's own read-back of the angular
//    velocity it had just written: SET_ENTITY_ANGULAR_VELOCITY wrote (-0.019, 0.032, -0.002) and the same-tick
//    GET_ENTITY_ROTATION_VELOCITY said (0.037, -0.001, -0.001). The x / y part keeps its length and turns by the car's heading
//    (237.7 degrees there; the player stood at heading 233): the setter takes its vector in the car's BODY frame, the getter returns
//    the WORLD frame. Written vector w, read vector r: r = R(q) * w, q the car's orientation. The yaw (z) passes for a level car, which
//    is why the old 3 % "tilt" tolerance of the read-back, and every yaw-only user of the setter, looked fine. The hyper assists'
//    anti-roll damping (hypertune.h) and the remote copies' angular command (vehicle_drive.h, `angularVelocity`) both build a WORLD
//    vector and used to hand it to the setter unchanged, so a car's pitch and roll rates were turned by its heading every tick.
//    This header holds the two conversions, the classifier of a write's read-back and a judge that keeps the frame the writers use
//    in line with what the game really does (it flips to the other frame if the read-backs say the first guess was wrong).
//
// 2. THE METERS. "Cars shake-ish" can be the local car (a per-tick write that fights the physics), a remote copy (a correction that
//    overshoots) or the pack's own handling. LocalMeter reads the car the local player drives at the script tick and reports the
//    high-frequency part of its pitch, roll and vertical speed; CopyMeter reads what a copy's drive controller saw each frame (position
//    and rotation error against its target) and reports the high-frequency part and the sign flips of that error. Both are second
//    differences at tick rate, which is blind to a 1-2 Hz suspension bounce (gain (2 pi f dt)^2) and sees a 20-50 Hz buzz at full gain.
//    Pure: tests/vehicle_shake_tests.h.
#pragma once

#include "sync/vehicle_drive.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gage::sync::shake {

    // ---- 1. the frame of the angular velocity setter ----------------------------------------------------------------

    // body -> world: what the game does with the vector SET_ENTITY_ANGULAR_VELOCITY is given (the live log's finding).
    inline DriveVec BodyToWorld(const DriveQuat& q, const DriveVec& v) { return vq::Rotate(vq::Normalize(q), v); }
    // world -> body: what a writer must hand the setter to get the world angular velocity `v`.
    inline DriveVec WorldToBody(const DriveQuat& q, const DriveVec& v) { return vq::Rotate(vq::Conj(vq::Normalize(q)), v); }

    inline double Dist3(const DriveVec& a, const DriveVec& b)
    {
        const double dx = (double)a.x - b.x, dy = (double)a.y - b.y, dz = (double)a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    enum class AngFrame : uint8_t
    {
        Neither = 0,   // the read-back matches neither reading (the physics stepped, the write was dropped, a bad read)
        Body = 1,      // the read-back is R(q) * wrote: the setter takes the body frame
        World = 2,     // the read-back is wrote: the setter takes the world frame
        Both = 3       // both readings fit (a vector along the vertical, or a heading near 0): no evidence either way
    };

    // What one write told: `wrote` is the vector passed to the setter, `readWorld` what GET_ENTITY_ROTATION_VELOCITY returned in the same
    // tick, `q` the orientation the setter saw. The tolerance is 0.01 rad/s plus 2 % of the written length.
    inline AngFrame ClassifyAngWrite(const DriveQuat& q, const DriveVec& wrote, const DriveVec& readWorld)
    {
        if (!std::isfinite(wrote.x) || !std::isfinite(wrote.y) || !std::isfinite(wrote.z) || !std::isfinite(readWorld.x) ||
            !std::isfinite(readWorld.y) || !std::isfinite(readWorld.z))
            return AngFrame::Neither;
        const double mag = std::sqrt((double)wrote.x * wrote.x + (double)wrote.y * wrote.y + (double)wrote.z * wrote.z);
        const double tol = 0.01 + 0.02 * mag;
        const bool asBody = Dist3(BodyToWorld(q, wrote), readWorld) <= tol;
        const bool asWorld = Dist3(wrote, readWorld) <= tol;
        if (asBody && asWorld) return AngFrame::Both;
        if (asBody) return AngFrame::Body;
        if (asWorld) return AngFrame::World;
        return AngFrame::Neither;
    }

    // The frame every writer of the angular velocity uses. Starts on the body frame (the live log's finding) when the fix is on and
    // never moves when it is off ("veh_shake_fix=0": the pre-0.4.22 world-frame writes, the read-backs are still counted). With the fix
    // on it follows the evidence: kFlipLead more world-only read-backs than body-only ones (or the other way round) flips it, once per
    // lead, so a wrong first guess costs a handful of writes and not the session.
    class AngFrameJudge
    {
    public:
        static constexpr uint32_t kFlipLead = 8;
        struct Counts { uint32_t body = 0, world = 0, both = 0, neither = 0; };

        void Configure(bool fixOn)
        {
            *this = AngFrameJudge();
            locked_ = !fixOn;
            useBody_ = fixOn;
        }
        bool UseBody() const { return useBody_; }
        bool Locked() const { return locked_; }
        uint32_t Flips() const { return flips_; }
        const Counts& Total() const { return total_; }

        void Add(AngFrame f)
        {
            Bump(total_, f);
            Bump(win_, f);
            if (f == AngFrame::Body) ++evBody_;
            else if (f == AngFrame::World) ++evWorld_;
            if (locked_) return;
            if (useBody_ && evWorld_ >= evBody_ + kFlipLead) { useBody_ = false; ++flips_; }
            else if (!useBody_ && evBody_ >= evWorld_ + kFlipLead) { useBody_ = true; ++flips_; }
        }
        Counts TakeWindow()
        {
            const Counts w = win_;
            win_ = Counts();
            return w;
        }

    private:
        static void Bump(Counts& c, AngFrame f)
        {
            switch (f)
            {
            case AngFrame::Body: ++c.body; break;
            case AngFrame::World: ++c.world; break;
            case AngFrame::Both: ++c.both; break;
            default: ++c.neither; break;
            }
        }
        bool useBody_ = true, locked_ = false;
        uint32_t evBody_ = 0, evWorld_ = 0, flips_ = 0;
        Counts total_, win_;
    };

    // ---- 2. the meters -----------------------------------------------------------------------------------------------

    constexpr double kShakeDeg = 180.0 / 3.14159265358979323846;

    // The car the local player drives, one sample per script tick (about 10 ms). Every statistic is a second difference of a sample
    // series taken at tick rate; a gap of under 4 ms or over 30 ms between two samples (two samples in one frame, a hitch) restarts the
    // history so a pause is never read as a jolt. Three speed bands by the horizontal speed, because the hyper assists ramp in from 10 m/s:
    // a buzz that exists only above it points at them.
    class LocalMeter
    {
    public:
        static constexpr int kBands = 3;
        static constexpr double kBandEdgeMs[kBands - 1] = { 10.0, 30.0 };
        struct Band
        {
            uint32_t ticks = 0, vzFlips = 0;
            double seconds = 0, speedSum = 0, pitchSq = 0, rollSq = 0, pitchMax = 0, rollMax = 0;
            double PitchRms() const { return ticks ? std::sqrt(pitchSq / ticks) : 0.0; }
            double RollRms() const { return ticks ? std::sqrt(rollSq / ticks) : 0.0; }
            double MeanSpeed() const { return ticks ? speedSum / ticks : 0.0; }
            double FlipsPerS() const { return seconds > 0.0 ? vzFlips / seconds : 0.0; }
        };
        struct Window
        {
            Band band[kBands];
            uint32_t ticks = 0, restarts = 0;
        };
        static int BandOf(double speedMs)
        {
            int b = 0;
            while (b < kBands - 1 && speedMs >= kBandEdgeMs[b]) ++b;
            return b;
        }

        // vel: world velocity (m/s), rot: the car's orientation.
        void Add(double nowMs, const DriveVec& vel, const DriveQuat& rot)
        {
            if (!std::isfinite(nowMs) || !std::isfinite(vel.x) || !std::isfinite(vel.y) || !std::isfinite(vel.z) || !vq::Finite(rot))
            {
                Restart();
                return;
            }
            const double dt = nowMs - lastMs_;
            const bool contiguous = n_ > 0 && dt >= 4.0 && dt <= 30.0;
            lastMs_ = nowMs;
            if (!contiguous)
            {
                if (n_ > 0) ++w_.restarts;
                n_ = 0;
            }
            const DriveVec f = vq::Rotate(rot, { 0.0f, 1.0f, 0.0f }), r = vq::Rotate(rot, { 1.0f, 0.0f, 0.0f });
            const double pitch = std::asin(std::max(-1.0, std::min(1.0, (double)f.z))) * kShakeDeg;
            const double roll = std::asin(std::max(-1.0, std::min(1.0, (double)r.z))) * kShakeDeg;
            const double vz = vel.z;
            if (n_ >= 2)
            {
                const double sp = std::sqrt((double)vel.x * vel.x + (double)vel.y * vel.y);
                Band& b = w_.band[BandOf(sp)];
                // the second difference for UNEVEN ticks (the script tick is 10.2 ms +- 2 ms): the miss of a straight-line prediction from
                // the two samples before, p - (p1 + (p1 - p2) * dt / dt1), which is p - 2 p1 + p2 when the ticks are even. Without the
                // ratio an uneven tick on a car that is rolling at 20 deg/s would read as 0.08 degree of "buzz".
                const double ratio = dt / dt1_;
                const double hp = pitch - p1_ - (p1_ - p2_) * ratio, hr = roll - r1_ - (r1_ - r2_) * ratio;
                ++b.ticks;
                ++w_.ticks;
                b.seconds += dt / 1000.0;
                b.speedSum += sp;
                b.pitchSq += hp * hp;
                b.rollSq += hr * hr;
                b.pitchMax = std::max(b.pitchMax, std::fabs(hp));
                b.rollMax = std::max(b.rollMax, std::fabs(hr));
                const double d1 = vz - z1_, d0 = z1_ - z2_;
                if (d1 * d0 < 0.0 && std::fabs(d1) > 0.1 && std::fabs(d0) > 0.1) ++b.vzFlips;
            }
            p2_ = p1_; p1_ = pitch;
            r2_ = r1_; r1_ = roll;
            z2_ = z1_; z1_ = vz;
            if (n_ > 0) dt1_ = dt;                     // the gap between the last two samples (the first sample after a restart has none)
            if (n_ < 2) ++n_;
        }
        void Restart() { n_ = 0; }
        Window Take()
        {
            const Window w = w_;
            w_ = Window();
            return w;
        }

    private:
        Window w_;
        int n_ = 0;
        double lastMs_ = 0, dt1_ = 10.0, p1_ = 0, p2_ = 0, r1_ = 0, r2_ = 0, z1_ = 0, z2_ = 0;
    };

    // One remote copy, one sample per drive frame: the position error (target - local, metres) and the rotation error vector (radians)
    // the controller worked from. Bucketed by the target's speed (parked < 0.5 m/s <= slow < 3 m/s <= moving), because a copy that buzzes
    // while it is parked is a different bug from one that buzzes at speed. A "flip" is the error vector pointing the opposite way from
    // one frame to the next while it is still more than 2 cm (0.1 degree): the copy overshooting its target every frame.
    class CopyMeter
    {
    public:
        enum Kind { Parked = 0, Slow = 1, Moving = 2, kKinds = 3 };
        struct Bucket
        {
            uint32_t frames = 0, posFlips = 0, rotFlips = 0;
            double posSq = 0, rotSq = 0, posMax = 0, rotMax = 0;     // second differences: metres, degrees
            double PosHfRmsMm() const { return frames ? std::sqrt(posSq / frames) * 1000.0 : 0.0; }
            double RotHfRmsMdeg() const { return frames ? std::sqrt(rotSq / frames) * 1000.0 : 0.0; }
            void Merge(const Bucket& o)
            {
                frames += o.frames; posFlips += o.posFlips; rotFlips += o.rotFlips;
                posSq += o.posSq; rotSq += o.rotSq;
                posMax = std::max(posMax, o.posMax); rotMax = std::max(rotMax, o.rotMax);
            }
        };
        struct Window
        {
            Bucket bucket[kKinds];
            uint32_t restarts = 0;
            void Merge(const Window& o)
            {
                for (int i = 0; i < kKinds; ++i) bucket[i].Merge(o.bucket[i]);
                restarts += o.restarts;
            }
        };
        static Kind KindOf(double targetSpeedMs) { return targetSpeedMs < 0.5 ? Parked : (targetSpeedMs < 3.0 ? Slow : Moving); }

        // restart: the frame snapped or the controller refused it, so the error before it says nothing about the error after it.
        void Add(double dtMs, const DriveVec& posErr, const DriveVec& rotErrRad, double targetSpeedMs, bool restart)
        {
            if (!std::isfinite(dtMs) || !std::isfinite(targetSpeedMs) || !Finite(posErr) || !Finite(rotErrRad))
                restart = true;
            if (restart)
            {
                if (n_ > 0) ++w_.restarts;
                n_ = 0;
                return;                                // the next frame starts a new history
            }
            if (!(dtMs >= 3.0 && dtMs <= 40.0))        // a pause or two frames in one tick: this sample starts a new history
            {
                if (n_ > 0) ++w_.restarts;
                n_ = 0;
            }
            const DriveVec rotDeg = { (float)(rotErrRad.x * kShakeDeg), (float)(rotErrRad.y * kShakeDeg), (float)(rotErrRad.z * kShakeDeg) };
            if (n_ >= 2)
            {
                Bucket& b = w_.bucket[KindOf(targetSpeedMs)];
                ++b.frames;
                // the miss of a straight-line prediction from the two frames before (uneven frame times: the gap ratio), not p - 2 p1 + p2
                const double ratio = dtMs / dt1_;
                const double hp = Len(Sub(Sub(posErr, p1_), Scale(Sub(p1_, p2_), ratio)));
                const double hr = Len(Sub(Sub(rotDeg, r1_), Scale(Sub(r1_, r2_), ratio)));
                b.posSq += hp * hp;
                b.rotSq += hr * hr;
                b.posMax = std::max(b.posMax, hp);
                b.rotMax = std::max(b.rotMax, hr);
                if (Dot(posErr, p1_) < 0.0 && Len(posErr) > 0.02 && Len(p1_) > 0.02) ++b.posFlips;
                if (Dot(rotDeg, r1_) < 0.0 && Len(rotDeg) > 0.1 && Len(r1_) > 0.1) ++b.rotFlips;
            }
            p2_ = p1_; p1_ = posErr;
            r2_ = r1_; r1_ = rotDeg;
            dt1_ = dtMs;                               // the gap that led to this sample, the divisor of the next frame's ratio
            if (n_ < 2) ++n_;
        }
        Window Take()
        {
            const Window w = w_;
            w_ = Window();
            return w;
        }

    private:
        static bool Finite(const DriveVec& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        static double Len(const DriveVec& v) { return std::sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z); }
        static double Dot(const DriveVec& a, const DriveVec& b) { return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z; }
        static DriveVec Scale(const DriveVec& a, double k) { return { (float)(a.x * k), (float)(a.y * k), (float)(a.z * k) }; }
        static DriveVec Sub(const DriveVec& a, const DriveVec& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        Window w_;
        int n_ = 0;
        double dt1_ = 16.0;
        DriveVec p1_, p2_, r1_, r2_;
    };

}   // namespace gage::sync::shake
