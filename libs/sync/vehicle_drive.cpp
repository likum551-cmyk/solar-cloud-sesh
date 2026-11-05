// Remote-vehicle drive (see vehicle_drive.h for the design and its constants).

#include "vehicle_drive.h"

#include <algorithm>
#include <cmath>

namespace gage::sync {

namespace {

    const double kPi = 3.14159265358979323846;

    struct D3 { double x = 0, y = 0, z = 0; };
    struct DQ { double x = 0, y = 0, z = 0, w = 1; };

    D3 ToD(const DriveVec& v) { return {v.x, v.y, v.z}; }
    DriveVec ToF(const D3& v) { DriveVec r; r.x = float(v.x); r.y = float(v.y); r.z = float(v.z); return r; }
    D3 Add(const D3& a, const D3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    D3 Sub(const D3& a, const D3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    D3 Scale(const D3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
    double DotV(const D3& a, const D3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    double Len(const D3& a) { return std::sqrt(DotV(a, a)); }
    D3 Cross(const D3& a, const D3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    D3 Lerp(const D3& a, const D3& b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }

    bool FiniteV(const DriveVec& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    DQ ToDQ(const DriveQuat& q) { return {q.x, q.y, q.z, q.w}; }
    DriveQuat ToFQ(const DQ& q) { DriveQuat r; r.x = float(q.x); r.y = float(q.y); r.z = float(q.z); r.w = float(q.w); return r; }

    DQ NormD(const DQ& q)
    {
        const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (!std::isfinite(n) || n < 1e-12) return DQ{};
        return {q.x / n, q.y / n, q.z / n, q.w / n};
    }

    DQ MulD(const DQ& a, const DQ& b)
    {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    DQ ConjD(const DQ& q) { return {-q.x, -q.y, -q.z, q.w}; }

    DQ ExpD(const D3& r)
    {
        const double th = Len(r);
        if (!std::isfinite(th)) return DQ{};
        if (th < 1e-9) return NormD({r.x * 0.5, r.y * 0.5, r.z * 0.5, 1.0});
        const double s = std::sin(th * 0.5) / th;
        return NormD({r.x * s, r.y * s, r.z * s, std::cos(th * 0.5)});
    }

    // Rotation vector of q on the shortest arc (|result| <= pi).
    D3 LogD(DQ q)
    {
        q = NormD(q);
        if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w}; // double cover: q and -q are one rotation
        const double vn = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
        if (vn < 1e-9) return {q.x * 2.0, q.y * 2.0, q.z * 2.0}; // ~identity: sin(a/2) ~ a/2, w ~ 1
        const double ang = 2.0 * std::atan2(vn, q.w);
        const double s = ang / vn;
        return {q.x * s, q.y * s, q.z * s};
    }

    D3 RotateD(const DQ& q, const D3& v)
    {
        // v' = v + 2w (u x v) + 2 u x (u x v), u = (x, y, z)
        const D3 u{q.x, q.y, q.z};
        const D3 t = Scale(Cross(u, v), 2.0);
        return Add(Add(v, Scale(t, q.w)), Cross(u, t));
    }

    DQ SlerpD(DQ a, DQ b, double t)
    {
        a = NormD(a);
        b = NormD(b);
        double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; } // shortest arc
        if (d > 0.9995)
            return NormD({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
        if (d > 1.0) d = 1.0;
        const double th = std::acos(d);
        const double s = std::sin(th);
        const double wa = std::sin((1.0 - t) * th) / s, wb = std::sin(t * th) / s;
        return NormD({a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
    }

    D3 SafeV(const D3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) ? v : D3{}; }

} // namespace

// ---------------------------------------------------------------- vq

namespace vq {
    DriveQuat Normalize(const DriveQuat& q) { return ToFQ(NormD(ToDQ(q))); }
    DriveQuat Mul(const DriveQuat& a, const DriveQuat& b) { return ToFQ(NormD(MulD(ToDQ(a), ToDQ(b)))); }
    DriveQuat Conj(const DriveQuat& q) { return ToFQ(ConjD(NormD(ToDQ(q)))); }
    double Dot(const DriveQuat& a, const DriveQuat& b)
    {
        const DQ x = NormD(ToDQ(a)), y = NormD(ToDQ(b));
        return x.x * y.x + x.y * y.y + x.z * y.z + x.w * y.w;
    }
    DriveQuat Slerp(const DriveQuat& a, const DriveQuat& b, double t)
    {
        if (!std::isfinite(t)) t = 0;
        return ToFQ(SlerpD(ToDQ(a), ToDQ(b), t));
    }
    DriveQuat Exp(const DriveVec& r) { return ToFQ(ExpD(ToD(r))); }
    DriveVec Log(const DriveQuat& q) { return ToF(SafeV(LogD(ToDQ(q)))); }
    DriveVec Rotate(const DriveQuat& q, const DriveVec& v) { return ToF(SafeV(RotateD(NormD(ToDQ(q)), ToD(v)))); }
    double AngleBetween(const DriveQuat& a, const DriveQuat& b)
    {
        return Len(LogD(MulD(NormD(ToDQ(b)), ConjD(NormD(ToDQ(a))))));
    }
    DriveQuat FromHeadingDeg(double h)
    {
        if (!std::isfinite(h)) h = 0;
        return Exp(ToF({0, 0, std::fmod(h, 360.0) * kPi / 180.0}));
    }
    double HeadingDeg(const DriveQuat& q)
    {
        const D3 f = RotateD(NormD(ToDQ(q)), {0, 1, 0});
        if (std::fabs(f.x) < 1e-12 && std::fabs(f.y) < 1e-12) return 0; // nose straight up / down
        double h = std::atan2(-f.x, f.y) * 180.0 / kPi;
        if (h <= -180.0) h += 360.0;
        return h;
    }
    double RollDeg(const DriveQuat& q)
    {
        const DQ n = NormD(ToDQ(q));
        const D3 f = RotateD(n, {0, 1, 0}), r = RotateD(n, {1, 0, 0});
        const double fh = std::sqrt(f.x * f.x + f.y * f.y);
        if (fh < 1e-6) return 0;                                   // nose straight up / down: no horizon to lean against
        const D3 r0 = Scale(Cross(f, {0, 0, 1}), 1.0 / fh);        // the level right axis for this heading
        return std::atan2(DotV(Cross(r0, r), f), DotV(r0, r)) * 180.0 / kPi;
    }
    bool Finite(const DriveQuat& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }
}

// ---------------------------------------------------------------- interpolator

VehicleInterpolator::VehicleInterpolator(const VehicleInterpConfig& cfg) : cfg_(cfg) {}

void VehicleInterpolator::Clear()
{
    head_ = 0;
    count_ = 0;
    teleportPending_ = false;
    guardUntil_ = -1e18;
    guardConfirm_ = false;
}

void VehicleInterpolator::ReplaceFrom(const VehicleSnapshot& in, double guardMs, const DriveVec* preVel, float confirmFraction)
{
    if (!std::isfinite(in.tMs) || !FiniteV(in.pos) || !FiniteV(in.vel) || !vq::Finite(in.rot) || !std::isfinite(in.steering) ||
        (in.hasAngVel && !FiniteV(in.angVel)) || !std::isfinite(guardMs))
        return;
    VehicleSnapshot s = in;
    s.rot = vq::Normalize(in.rot);
    if (!s.hasAngVel) s.angVel = DriveVec{};
    s.src = kSnapSrcCopy;                     // 0.4.30: our own pose, never paired with the owner's for a derived omega
    while (count_ > 0 && !(At(count_ - 1).tMs < s.tMs))
        --count_;
    ring_[(head_ + count_) % kVehicleRing] = s;
    ++count_;
    guardUntil_ = s.tMs + (guardMs > 0.0 ? guardMs : 0.0);
    guardConfirm_ = false;
    if (preVel && FiniteV(*preVel) && std::isfinite(confirmFraction))
    {
        const D3 dv = Sub(ToD(s.vel), ToD(*preVel));
        guardDvLen_ = float(Len(dv));
        guardPre_ = *preVel;
        guardFrac_ = confirmFraction;
        guardConfirm_ = guardDvLen_ > 0.5f;
    }
    ++stats_.injected;
}

void VehicleInterpolator::SetSession(uint32_t id)
{
    if (id == session_)
        return;
    session_ = id;
    Clear();
    gapHead_ = 0;
    gapCount_ = 0;
}

double VehicleInterpolator::TypicalGapMs() const
{
    if (gapCount_ == 0) return 0.0;
    double g[kGapRing];
    for (int i = 0; i < gapCount_; ++i) g[i] = gaps_[i];
    std::nth_element(g, g + gapCount_ / 2, g + gapCount_);
    return g[gapCount_ / 2];
}

DriveVec VehicleInterpolator::OmegaOf(int i) const
{
    const VehicleSnapshot& s = At(i);
    if (s.hasAngVel) return s.angVel;
    // derived from a neighbour pair within maxBridgeMs: q_b = exp(w dt) q_a (world frame)
    auto derive = [&](int a, int b, DriveVec& w) {
        const VehicleSnapshot &sa = At(a), &sb = At(b);
        const double dt = (sb.tMs - sa.tMs) / 1000.0;
        if (!(dt > 0.0) || sb.tMs - sa.tMs > cfg_.maxBridgeMs) return false;
        if (cfg_.omegaGuard)
        {
            // two machines' poses (the copy's adopted bounce, then a standing owner): the gap between them is not a rate, and a parked car has
            // none. Only when the later one stands: a MOVING owner after an adoption keeps the derived rate (the 2-machine sims lose 0.5 m on a
            // T-bone without it, R3 T5 / R4 T2).
            if (sa.src != sb.src && Len(ToD(sb.vel)) < cfg_.omegaStillMs)
            {
                ++stats_.omegaCross;
                w = DriveVec{};
                return true;
            }
            if (Len(ToD(sa.vel)) < cfg_.omegaStillMs && Len(ToD(sb.vel)) < cfg_.omegaStillMs)
            {
                ++stats_.omegaStill;                               // a standing pair: quantisation / settling only
                w = DriveVec{};
                return true;
            }
        }
        w = ToF(SafeV(Scale(LogD(MulD(ToDQ(sb.rot), ConjD(ToDQ(sa.rot)))), 1.0 / dt)));
        return true;
    };
    DriveVec w;
    if (i > 0 && derive(i - 1, i, w)) return w;
    if (i + 1 < count_ && derive(i, i + 1, w)) return w;
    return DriveVec{};
}

// R4 T2: the horizontal speed after tSec of a tangential acceleration aT from s0 (never below 0), as a factor of s0.
static double AccelSpeedFactor(const DriveVec& v, double aT, double tSec)
{
    const double s0 = std::sqrt(double(v.x) * v.x + double(v.y) * v.y);
    if (!(aT != 0.0) || !(s0 > 1.0) || !(tSec > 0.0)) return 1.0;
    const double te = aT < 0.0 ? std::fmin(tSec, s0 / -aT) : tSec;
    return std::fmax(0.0, 1.0 + aT * te / s0);
}

double VehicleInterpolator::TangentialAccel() const
{
    if (!cfg_.accelExtrapolation || count_ < 2) return 0.0;
    const VehicleSnapshot& n = At(count_ - 1);
    int pick = -1;
    for (int i = count_ - 2; i >= 0; --i)
    {
        const double span = n.tMs - At(i).tMs;
        if (span > cfg_.accelMaxSpanMs) break;
        if (span >= cfg_.accelMinSpanMs) { pick = i; break; }
        if (span >= 40.0) pick = i;      // the best shorter pair if no longer one exists
    }
    if (pick < 0) return 0.0;
    const VehicleSnapshot& o = At(pick);
    const double span = (n.tMs - o.tMs) / 1000.0;
    const double sn = std::sqrt(double(n.vel.x) * n.vel.x + double(n.vel.y) * n.vel.y);
    const double so = std::sqrt(double(o.vel.x) * o.vel.x + double(o.vel.y) * o.vel.y);
    const double a = (sn - so) / span;
    if (!std::isfinite(a) || std::fabs(a) > cfg_.accelRejectMs2) return 0.0;      // a collision is not an acceleration
    return a > cfg_.accelMaxMs2 ? cfg_.accelMaxMs2 : (a < -cfg_.accelMaxMs2 ? -cfg_.accelMaxMs2 : a);
}

void VehicleInterpolator::Extrapolate(const VehicleSnapshot& s, const DriveVec& wf, double dtMs, DriveVec& pos,
                                      DriveQuat& rot, double aT) const
{
    const double t = dtMs / 1000.0;
    const D3 w = ToD(wf), v = ToD(s.vel);
    rot = ToFQ(NormD(MulD(ExpD(Scale(w, t)), ToDQ(s.rot))));
    const double wl = Len(w);
    D3 disp;
    if (cfg_.arcExtrapolation && wl > 1e-6 && wl <= cfg_.arcMaxRadS)
    {
        // the velocity turns with the body: integral of exp(w tau) v over [0, t], closed form
        const D3 n = Scale(w, 1.0 / wl);
        const D3 vPar = Scale(n, DotV(v, n));
        const D3 vPerp = Sub(v, vPar);
        const double th = wl * t;
        disp = Add(Add(Scale(vPar, t), Scale(vPerp, std::sin(th) / wl)), Scale(Cross(n, vPerp), (1.0 - std::cos(th)) / wl));
    }
    else
    {
        disp = Scale(v, t);
    }
    if (aT != 0.0 && t > 0.0)
    {
        // the travelled distance with the speed changing linearly (a stop ends it): the arc / line above scaled in the horizontal plane
        const double s0 = std::sqrt(v.x * v.x + v.y * v.y);
        if (s0 > 1.0)
        {
            const double te = aT < 0.0 ? std::fmin(t, s0 / -aT) : t;
            const double kd = (s0 * te + 0.5 * aT * te * te) / (s0 * t);
            disp.x *= kd;
            disp.y *= kd;
        }
    }
    pos = ToF(Add(ToD(s.pos), SafeV(disp)));
}

VehPush VehicleInterpolator::Push(const VehicleSnapshot& in)
{
    if (!std::isfinite(in.tMs) || !FiniteV(in.pos) || !FiniteV(in.vel) || !vq::Finite(in.rot) ||
        !std::isfinite(in.steering) || (in.hasAngVel && !FiniteV(in.angVel)))
    {
        ++stats_.invalid;
        return VehPush::Invalid;
    }
    const DQ qd = ToDQ(in.rot);
    if (qd.x * qd.x + qd.y * qd.y + qd.z * qd.z + qd.w * qd.w < 1e-8)
    {
        ++stats_.invalid;
        return VehPush::Invalid;
    }
    VehicleSnapshot s = in;
    s.rot = vq::Normalize(in.rot);
    if (!s.hasAngVel) s.angVel = DriveVec{};

    if (s.tMs <= guardUntil_)
    {
        // guarded: refused, unless it already shows the impact (the controller's stream has caught up)
        if (guardConfirm_ && Len(Sub(ToD(s.vel), ToD(guardPre_))) >= double(guardFrac_) * double(guardDvLen_))
            guardUntil_ = -1e18;
        else
        {
            ++stats_.guarded;
            return VehPush::Stale;
        }
    }
    VehPush result = VehPush::Accepted;
    if (count_ > 0)
    {
        const VehicleSnapshot& newest = At(count_ - 1);
        if (!(s.tMs > newest.tMs))
        {
            ++stats_.stale;
            return VehPush::Stale;
        }
        // Bit-identical motion with a later stamp: not stored (see VehPush::Repeat). Compared on the
        // normalised, stored form, so an unnormalised copy of the same rotation is not a Repeat by accident.
        if (s.hasAngVel == newest.hasAngVel && s.pos.x == newest.pos.x && s.pos.y == newest.pos.y && s.pos.z == newest.pos.z &&
            s.vel.x == newest.vel.x && s.vel.y == newest.vel.y && s.vel.z == newest.vel.z && s.rot.x == newest.rot.x &&
            s.rot.y == newest.rot.y && s.rot.z == newest.rot.z && s.rot.w == newest.rot.w &&
            s.angVel.x == newest.angVel.x && s.angVel.y == newest.angVel.y && s.angVel.z == newest.angVel.z &&
            s.steering == newest.steering)
        {
            ++stats_.repeats;
            return VehPush::Repeat;
        }
        const double gap = s.tMs - newest.tMs;
        if (gap > cfg_.maxBridgeMs)
        {
            Clear(); // no interpolation across a long hole; the controller's snap rules judge the jump
            ++stats_.bridgeResets;
        }
        else
        {
            gaps_[gapHead_] = gap;
            gapHead_ = (gapHead_ + 1) % kGapRing;
            if (gapCount_ < kGapRing) ++gapCount_;
            DriveVec pred;
            DriveQuat predQ;
            Extrapolate(newest, OmegaOf(count_ - 1), gap < cfg_.extrapolateMaxMs ? gap : cfg_.extrapolateMaxMs, pred, predQ);
            const double d = Len(Sub(ToD(s.pos), ToD(pred)));
            const double lim = std::fmax(cfg_.teleportMinM, cfg_.teleportPerSpeedS * Len(ToD(newest.vel)));
            if (d > lim)
            {
                Clear();
                teleportPending_ = true;
                ++stats_.teleports;
                result = VehPush::Teleported;
            }
        }
    }
    if (count_ == kVehicleRing)
    {
        head_ = (head_ + 1) % kVehicleRing;
        --count_;
    }
    ring_[(head_ + count_) % kVehicleRing] = s;
    ++count_;
    ++stats_.accepted;
    return result;
}

bool VehicleInterpolator::Sample(double t, VehicleSample& out)
{
    if (count_ == 0 || !std::isfinite(t)) return false;
    out = VehicleSample{};
    out.teleported = teleportPending_;
    teleportPending_ = false;

    const VehicleSnapshot& oldest = At(0);
    const VehicleSnapshot& newest = At(count_ - 1);
    if (t < oldest.tMs)
    {
        out.mode = VehMode::Buffering;
        out.steering = oldest.steering;
        const DriveVec w = OmegaOf(0);
        // Sync D: walk back along the snapshot's own motion (see vehicle_drive.h, 4.), not a pin at its position.
        const double back = std::fmin(oldest.tMs - t, cfg_.bufferBackMaxMs > 0.0 ? cfg_.bufferBackMaxMs : 0.0);
        if (back > 0.0)
        {
            Extrapolate(oldest, w, -back, out.pos, out.rot);
            const double wl = Len(ToD(w));
            out.vel = (cfg_.arcExtrapolation && wl > 1e-6 && wl <= cfg_.arcMaxRadS)
                          ? ToF(SafeV(RotateD(ExpD(Scale(ToD(w), -back / 1000.0)), ToD(oldest.vel))))
                          : oldest.vel;
        }
        else
        {
            out.pos = oldest.pos;
            out.rot = oldest.rot;
            out.vel = oldest.vel;
        }
        out.angVel = w;
        return true;
    }
    if (t >= newest.tMs)
    {
        const double dt = t - newest.tMs;
        const bool hold = dt > cfg_.extrapolateMaxMs;
        const double aT = TangentialAccel();
        Extrapolate(newest, OmegaOf(count_ - 1), hold ? cfg_.extrapolateMaxMs : dt, out.pos, out.rot, aT);
        out.steering = newest.steering;
        if (hold)
        {
            out.mode = VehMode::Holding; // velocity and angular velocity zero
        }
        else
        {
            out.mode = VehMode::Extrapolating;
            const DriveVec w = OmegaOf(count_ - 1);
            const double wl = Len(ToD(w));
            // the velocity turned with the body along the arc (or not, as in Extrapolate)
            out.vel = (cfg_.arcExtrapolation && wl > 1e-6 && wl <= cfg_.arcMaxRadS)
                          ? ToF(SafeV(RotateD(ExpD(Scale(ToD(w), dt / 1000.0)), ToD(newest.vel))))
                          : newest.vel;
            if (aT != 0.0)
            {
                const double f = AccelSpeedFactor(newest.vel, aT, dt / 1000.0);
                out.vel.x = float(out.vel.x * f);
                out.vel.y = float(out.vel.y * f);
            }
            out.angVel = w;
        }
        return true;
    }
    int i = 0;
    while (i + 2 < count_ && At(i + 1).tMs <= t) ++i;
    const VehicleSnapshot &a = At(i), &b = At(i + 1);
    const double span = b.tMs - a.tMs;
    const double u = span > 0 ? (t - a.tMs) / span : 1.0;
    const double spanS = span / 1000.0;

    // cubic Hermite with velocity tangents clamped to 3x the chord (smoother.h rule)
    const D3 p0 = ToD(a.pos), p1 = ToD(b.pos);
    const double chord = Len(Sub(p1, p0));
    auto tangent = [&](const DriveVec& v) {
        D3 m = Scale(ToD(v), spanS);
        const double ml = Len(m), cap = 3.0 * chord;
        if (ml > cap) m = ml > 0 ? Scale(m, cap / ml) : D3{};
        return m;
    };
    const D3 m0 = tangent(a.vel), m1 = tangent(b.vel);
    const double u2 = u * u, u3 = u2 * u;
    const double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    out.pos = ToF(SafeV(Add(Add(Scale(p0, h00), Scale(m0, h10)), Add(Scale(p1, h01), Scale(m1, h11)))));
    out.vel = ToF(Lerp(ToD(a.vel), ToD(b.vel), u));
    out.rot = ToFQ(SlerpD(ToDQ(a.rot), ToDQ(b.rot), u));
    if (a.hasAngVel && b.hasAngVel) out.angVel = ToF(Lerp(ToD(a.angVel), ToD(b.angVel), u));
    else if (spanS > 0) out.angVel = ToF(SafeV(Scale(LogD(MulD(ToDQ(b.rot), ConjD(ToDQ(a.rot)))), 1.0 / spanS)));
    out.steering = float(a.steering + (double(b.steering) - a.steering) * u);
    out.mode = VehMode::Interpolating;
    return true;
}

// ---------------------------------------------------------------- controller

VehicleDriveController::VehicleDriveController(const VehicleDriveConfig& cfg) : cfg_(cfg) {}

void VehicleDriveController::Reset()
{
    started_ = false;
    offAcc_ = 0;
    underFloorMs_ = 0;
    graceOn_ = false;
    proxGrace_ = false;
    graceAgeMs_ = cooldownMs_ = 0;
    haveCmd_ = false;
    kickN_ = 0;
    kickMuteMs_ = 0;
}

void VehicleDriveController::EndGrace(double cooldownMs)
{
    graceOn_ = false;
    proxGrace_ = false;
    graceAgeMs_ = 0;
    cooldownMs_ = cooldownMs > 0.0 ? cooldownMs : 0.0;
    haveCmd_ = false;    // the next frame's velocity is the adopted one: no deviation test against the last command
}

VehicleDriveOutput VehicleDriveController::Update(const VehicleDriveInput& in)
{
    VehicleDriveOutput o;
    ++stats_.frames;
    const VehicleSample& tg = in.target;
    if (!FiniteV(tg.pos) || !FiniteV(tg.vel) || !FiniteV(tg.angVel) || !vq::Finite(tg.rot) || !std::isfinite(tg.steering) ||
        !FiniteV(in.localPos) || !FiniteV(in.localVel) || !vq::Finite(in.localRot) || !std::isfinite(in.dtMs))
    {
        ++stats_.refused;
        return o; // nothing trustworthy to act on
    }
    const double dt = in.dtMs < 0.0 ? 0.0 : (in.dtMs > 1000.0 ? 1000.0 : in.dtMs);
    const double dtS = dt / 1000.0;

    const DQ tq = NormD(ToDQ(tg.rot)), lq = NormD(ToDQ(in.localRot));
    const D3 tp = ToD(tg.pos), lp = ToD(in.localPos), tv = ToD(tg.vel), tw = ToD(tg.angVel);
    const D3 e = Sub(tp, lp);
    const double dist = Len(e);
    const double speed = Len(tv);
    const D3 rErr = LogD(MulD(tq, ConjD(lq)));
    const double rAng = Len(rErr);
    o.posErrorM = float(dist);
    o.rotErrorDeg = float(rAng * 180.0 / kPi);

    // ---- contact grace (W9 item 6): open on a contact, age, close ----
    // A contact is the engine's collision flag, or a loss of speed along the target's direction of travel that the
    // controller did not command: the velocity it wrote last frame minus the car's own now, projected on the travel
    // direction, above contactDecelMs. Lateral deviation is not used: tyre grip eats a sideways correction
    // by itself and a controller pushing a 2 m error would look like a crash.
    double graceW = 1.0;
    if (cfg_.contactGraceMs > 0.0f)
    {
        if (graceOn_)
        {
            graceAgeMs_ += dt;
            if (graceAgeMs_ >= cfg_.contactGraceMs)
            {
                graceOn_ = false;
                proxGrace_ = false;
                cooldownMs_ = cfg_.contactCooldownMs;
            }
        }
        else if (cooldownMs_ > 0.0)
            cooldownMs_ = std::fmax(0.0, cooldownMs_ - dt);
        // What physics did to the velocity written last frame (Sync B: also the diagnostic devMs, whatever the state).
        clockMs_ += dt;
        if (kickMuteMs_ > 0.0)
            kickMuteMs_ = std::fmax(0.0, kickMuteMs_ - dt);
        D3 devV{};
        bool haveDev = false;
        if (haveCmd_ && dt <= cfg_.contactMaxFrameMs)
        {
            devV = Sub(ToD(lastCmdVel_), ToD(in.localVel));
            haveDev = true;
            o.devMs = float(Len(devV));
        }
        // R3 T5: a grace the PROXIMITY trigger opened (the copy is about to overlap our car) is upgraded by the first real contact evidence:
        // the flag, the speed loss or the kick then report contactOpened as usual, so the engine adopts the copy's response to the hit.
        const bool canOpen = !graceOn_ && cooldownMs_ <= 0.0 && started_ && !tg.teleported;
        const bool canUpgrade = graceOn_ && proxGrace_ && started_ && !tg.teleported;
        if (canOpen || canUpgrade)
        {
            bool byDecel = false, byKick = false;
            if (haveDev && speed >= cfg_.contactMinSpeedMs)
            {
                const D3 dir = Scale(tv, 1.0 / speed);
                byDecel = DotV(devV, dir) > cfg_.contactDecelMs;
            }
            if (haveDev && cfg_.contactKickMs > 0.0f && kickMuteMs_ <= 0.0)
                byKick = Len(devV) > cfg_.contactKickMs;
            const bool hard0 = in.contact || byDecel || byKick;
            const bool hard = hard0 && in.ownNear;
            if (hard0 && !in.ownNear && !(canOpen && in.proximity))
            {
                // R4 T2: a touch of something that is not ours (the world, a ped, another copy): the owner's stream stays the truth, no grace
                ++stats_.contactsIgnored;
                if (in.contact) ++stats_.ignoredByFlag;
                o.contactIgnored = in.contact ? 1 : (byDecel ? 2 : 3);
            }
            if (hard || (canOpen && in.proximity))
            {
                if (canUpgrade) ++stats_.proximityUpgrades;
                graceOn_ = true;
                graceAgeMs_ = 0;
                proxGrace_ = !hard;
                ++stats_.contacts;
                if (in.contact) { ++stats_.contactsByFlag; o.contactOpened = 1; }
                else if (byDecel) { ++stats_.contactsByDecel; o.contactOpened = 2; }
                else if (!byKick) { ++stats_.contactsByProximity; o.contactOpened = 4; }
                else
                {
                    ++stats_.contactsByKick;
                    o.contactOpened = 3;
                    // the budget: the last contactKickStormN kicks that stood alone (no flag, no speed loss) within contactKickStormMs
                    const int n = cfg_.contactKickStormN < 8 ? cfg_.contactKickStormN : 8;
                    if (n > 0)
                    {
                        if (kickN_ < 8) ++kickN_;
                        for (int i = 7; i > 0; --i) kickAtMs_[i] = kickAtMs_[i - 1];
                        kickAtMs_[0] = clockMs_;
                        if (kickN_ >= n && clockMs_ - kickAtMs_[n - 1] <= cfg_.contactKickStormMs)
                        {
                            kickMuteMs_ = cfg_.contactKickMuteMs;
                            kickN_ = 0;
                            ++stats_.kickMutes;
                        }
                    }
                }
            }
        }
        if (graceOn_)
        {
            const double u = std::fmin(1.0, graceAgeMs_ / cfg_.contactGraceMs);
            graceW = cfg_.contactSoftFloor + (1.0 - cfg_.contactSoftFloor) * (u * u * (3.0 - 2.0 * u));
        }
    }
    else
        graceOn_ = false;
    o.graceActive = graceOn_;
    o.graceWeight = float(graceW);

    // ---- snap ----
    // Inside a grace only a teleport or a gross error snaps (the crash plays out); the persistent-error accumulator is not fed.
    const double snapDist = std::fmax(cfg_.snapMinM, cfg_.snapTravelS * speed) * (graceOn_ ? cfg_.contactHardSnapMul : 1.0);
    bool snap = !started_ || tg.teleported || dist > snapDist;
    if (!snap && !graceOn_)
    {
        const double tol = cfg_.offToleranceM + cfg_.offTolPerSpeedS * speed;
        offAcc_ += float((dist - tol) * dtS);
        if (offAcc_ < 0.0f) offAcc_ = 0.0f;
        if (offAcc_ >= cfg_.offLimitMs) { snap = true; ++stats_.persistentSnaps; }
    }
    if (!snap && !graceOn_)
    {
        const double dz = tp.z - lp.z;
        const bool under = dz > cfg_.underFloorMinM && dz < cfg_.underFloorMaxM && std::fabs(tv.z) < cfg_.underFloorVzMs;
        underFloorMs_ = under ? underFloorMs_ + dt : 0.0;
        if (underFloorMs_ >= cfg_.underFloorMs) { snap = true; ++stats_.underFloorSnaps; }
    }
    started_ = true;
    o.setSteering = true;
    o.steering = tg.steering;
    if (snap)
    {
        o.snap = true;
        o.snapPos = tg.pos;
        o.snapRot = ToFQ(tq);
        o.setVelocity = true;
        o.velocity = tg.vel;
        o.setAngularVelocity = true;
        o.angularVelocity = tg.angVel;
        o.angularVelocityBody = ToF(SafeV(RotateD(ConjD(tq), tw)));
        offAcc_ = 0;
        underFloorMs_ = 0;
        graceOn_ = false;
        cooldownMs_ = 0;
        haveCmd_ = false;                               // a snap's velocity is the target's: no deviation test next frame
        o.graceActive = false;
        o.graceWeight = 1.0f;
        ++stats_.snaps;
        return o;
    }

    // ---- velocity: target v + discrete-exact gain * error, capped; no correction in the deadband ----
    // 0.4.30 ride freedom: the vertical error the copy's own suspension may keep (rideTolM) is not corrected, only its excess. The bands
    // are for a body resting on its wheels: they fade to nothing (continuously, no step in the command) as the owner's or the copy's
    // vertical speed goes from rideVzFullMs to rideVzZeroMs (a jump, a fall), and the tilt band also as the target leans past upright.
    const double vzMax = std::fmax(std::fabs(tv.z), std::fabs((double)in.localVel.z));
    const double rideW = vzMax <= kRideVzFullMs ? 1.0 : (vzMax >= kRideVzZeroMs ? 0.0 : (kRideVzZeroMs - vzMax) / (kRideVzZeroMs - kRideVzFullMs));
    D3 eC = e;
    if (cfg_.rideTolM > 0.0f && rideW > 0.0)
    {
        const double tol = cfg_.rideTolM * rideW;
        eC.z = e.z > 0.0 ? std::fmax(0.0, e.z - tol) : std::fmin(0.0, e.z + tol);
    }
    const double distC = Len(eC);
    o.setVelocity = true;
    o.velocity = tg.vel;
    if (distC > cfg_.deadbandM)
    {
        double g = cfg_.corrGainBase + cfg_.corrGainPerM * distC;
        if (speed < cfg_.stillSpeedMs) g *= cfg_.stillGainMul;
        const double gEff = dtS > 0.0 ? (1.0 - std::exp(-g * dtS)) / dtS : g;
        D3 c = Scale(eC, gEff);
        const double cl = Len(c), cap = cfg_.corrMaxBaseMs + cfg_.corrMaxPerSpeed * speed;
        if (cl > cap && cl > 0.0) c = Scale(c, cap / cl);
        o.velocity = ToF(SafeV(Add(tv, c)));
    }
    if (graceOn_)
    {
        // the car's own velocity (the impact's outcome) plus a weight w of the way to the full command
        const D3 lv = ToD(in.localVel), full = ToD(o.velocity);
        o.velocity = ToF(SafeV(Add(lv, Scale(Sub(full, lv), graceW))));
    }
    lastCmdVel_ = o.velocity;
    haveCmd_ = true;

    // ---- rotation: angular velocity steering, quaternion blend for big errors / a flipped car ----
    const bool flipped = RotateD(tq, {0, 0, 1}).z < 0.0;
    D3 w = tw;
    // 0.4.30 ride freedom: the pitch / roll error the suspension may keep (tiltTolDeg) is not corrected, only its excess; yaw as before. Yaw
    // is the part of the rotation error along the TARGET's up axis (on a slope or a bank the world vertical is not the body's yaw axis), the
    // tilt is the rest. The band fades out with rideW (airborne) and as the target leans from 30 to 37 deg off upright (a car on its side:
    // its heading would sit in the "tilt"). The large-error blend below still uses the full error.
    D3 rC = rErr;
    if (cfg_.tiltTolDeg > 0.0f && rideW > 0.0)
    {
        const D3 u = RotateD(tq, {0, 0, 1});
        const double upW = u.z >= 0.866 ? 1.0 : (u.z <= 0.8 ? 0.0 : (u.z - 0.8) / 0.066);
        const double tol = cfg_.tiltTolDeg * kPi / 180.0 * rideW * upW;
        if (tol > 0.0)
        {
            const D3 yawPart = Scale(u, DotV(rErr, u));
            const D3 tiltPart = Sub(rErr, yawPart);
            const double tilt = Len(tiltPart);
            const double s = tilt > tol ? (tilt - tol) / tilt : 0.0;
            rC = Add(yawPart, Scale(tiltPart, s));
        }
    }
    if (graceOn_ && graceW < 0.5)
    {
        // the crash plays out: rotation is physics' own until the weight passes a half
        o.setAngularVelocity = false;
        o.angularVelocityBody = DriveVec{};
        return o;
    }
    if (graceOn_)
    {
        // no quaternion blend inside a grace; angular velocity steering only (below)
        const double k = cfg_.rotGain;
        const double kEff = dtS > 0.0 ? (1.0 - std::exp(-k * dtS)) / dtS : k;
        D3 c = Scale(rC, kEff * graceW);
        const double cl = Len(c);
        if (cl > cfg_.rotCorrMaxRadS && cl > 0.0) c = Scale(c, cfg_.rotCorrMaxRadS / cl);
        w = SafeV(Add(tw, c));
        o.angularVelocityBody = ToF(SafeV(RotateD(ConjD(lq), w)));
    }
    else if (rAng > cfg_.blendAboveDeg * kPi / 180.0 || flipped)
    {
        const double tau = cfg_.blendTauMs > 0 ? cfg_.blendTauMs : 1.0;
        const DQ q = SlerpD(lq, tq, 1.0 - std::exp(-dt / tau));
        o.setQuaternion = true;
        o.rot = ToFQ(q);
        ++stats_.blends;
        o.angularVelocityBody = ToF(SafeV(RotateD(ConjD(q), w)));
    }
    else
    {
        const double k = cfg_.rotGain;
        const double kEff = dtS > 0.0 ? (1.0 - std::exp(-k * dtS)) / dtS : k;
        D3 c = Scale(rC, kEff);
        const double cl = Len(c);
        if (cl > cfg_.rotCorrMaxRadS && cl > 0.0) c = Scale(c, cfg_.rotCorrMaxRadS / cl);
        w = SafeV(Add(tw, c));
        o.angularVelocityBody = ToF(SafeV(RotateD(ConjD(lq), w)));
    }
    o.setAngularVelocity = true;
    o.angularVelocity = ToF(w);
    return o;
}

double PresentLead::Weight(const PresentLeadConfig& cfg, double distM)
{
    if (!std::isfinite(distM)) return 0.0;
    if (distM <= cfg.fullWithinM) return 1.0;
    if (distM >= cfg.zeroBeyondM || !(cfg.zeroBeyondM > cfg.fullWithinM)) return 0.0;
    return (cfg.zeroBeyondM - distM) / (cfg.zeroBeyondM - cfg.fullWithinM);
}

double PresentLead::Update(double distM, double wantMs, double dtMs)
{
    const double want = (std::isfinite(wantMs) && wantMs > 0.0) ? std::fmin(wantMs, cfg_.maxLeadMs) : 0.0;
    const double target = want * Weight(cfg_, distM);
    if (!have_)
    {
        have_ = true;
        leadMs_ = target;
        return leadMs_;
    }
    const double dt = !std::isfinite(dtMs) || dtMs < 0.0 ? 0.0 : (dtMs > 1000.0 ? 1000.0 : dtMs);
    const double step = cfg_.slewMsPerMs * dt;
    const double d = target - leadMs_;
    leadMs_ += d > step ? step : (d < -step ? -step : d);
    return leadMs_;
}

bool VehicleIsMoving(float speedMs, float angSpeedRadS)
{
    return (std::isfinite(speedMs) && speedMs > 0.2f) || (std::isfinite(angSpeedRadS) && angSpeedRadS > 0.05f);
}

bool VehicleSendGate::Due(double nowMs, bool moving, bool changed)
{
    const bool stopped = wasMoving_ && !moving;
    wasMoving_ = moving;
    if (!started_ || moving || changed || stopped || !(nowMs - lastMs_ < stillMs_))
    {
        started_ = true;
        lastMs_ = nowMs;
        return true;
    }
    return false;
}

float EstimateSteer(float vFwd, float yawRate, float wheelbase, float prev, float lock, float holdBelowMps)
{
    const float held = std::isfinite(prev) ? std::max(-1.0f, std::min(1.0f, prev)) : 0.0f;
    if (!std::isfinite(vFwd) || !std::isfinite(yawRate) || !std::isfinite(wheelbase) || !std::isfinite(lock) ||
        !std::isfinite(holdBelowMps) || !(wheelbase > 0.0f) || !(lock > 0.0f))
        return held;
    if (std::fabs(vFwd) < holdBelowMps || vFwd == 0.0f)
        return held;                            // a standstill: yaw rate / speed is noise
    // Kinematic bicycle: yawRate = v * tan(delta) / L, so delta = atan(L * yawRate / v). The ratio keeps
    // v's sign (reversing with the wheels turned left yaws clockwise: the signs cancel); atan2 with a
    // negative v would return an angle past 90 degrees.
    const double delta = std::atan(double(wheelbase) * double(yawRate) / double(vFwd));
    const double steer = -delta / double(lock);   // the wire: left negative, right positive
    return float(std::max(-1.0, std::min(1.0, steer)));
}

}
