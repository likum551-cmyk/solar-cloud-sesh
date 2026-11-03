// vehicle_impact.h: ONE resolution per contact between two vehicles that different clients simulate (T4, desync overhaul round 2,
// docs/sync-specs/vehicle-collisions.md). Pure C++17, header-only, no game calls, no clocks, no allocation per call beyond the
// arbiter's own small tables; the SERVER runs ContactArbiter, the ENGINE runs ImpactTracker + DecideImpactApply, the tests run both.
//
// THE PROBLEM THIS SOLVES
//   Every vehicle is simulated by exactly one client (its controller). A car-to-car contact between two controllers is therefore
//   two independent physics events: each machine bounces its OWN car off a 126-193 ms old kinematic copy of the other, at its own
//   contact time and place, and broadcasts the result; the other machine then converges on that different post-impact state. The
//   two screens disagree about WHERE and WHEN the cars touched and about what came out (T0 findings section 8, measured by
//   tests/vehsim_2m.h: the numbers are in the spec).
//
// THE DESIGN (server-arbitrated contact resolution, "one physics instance per contact")
//   1. The server already receives both cars' true motion (VehicleUpdate: position, velocity, rotation, yaw rate) stamped on ONE
//      time base (SampleClock). ContactArbiter keeps the last few samples of every vehicle and, for each pair whose controllers
//      differ, interpolates both to a common instant, finds the first touching instant by bisection (the 33 ms tick would
//      otherwise report a contact that is already 1.3 m deep at 40 m/s closing) and solves ONE planar two-body impulse there
//      (oriented boxes, mass class, restitution, Coulomb friction): momentum is conserved, both cars get the same Newton's-third-law
//      impulse, and the result does not depend on any screen's render delay.
//   2. Each controller gets its own half as a VehicleImpact message (reliable): the velocity and yaw-rate change of ITS car, the
//      contact time on ITS OWN sample clock (the VehicleUpdate.sampleMs domain, so no round-trip estimate is needed), the normal
//      and the closing speed.
//   3. The controller's game has already bounced the car off the copy (it cannot be switched off: every native that does is
//      unknown here), so applying the full impulse would count the hit twice. ImpactTracker measures what the local physics did
//      (velocity jumps no driver input can produce); DecideImpactApply applies only the DIFFERENCE, authoritative minus local.
//      A server impact the local car never felt (the copy was not where the server's boxes say) is applied only if the copy is
//      near; a local jump the server never confirmed stays as the game made it.
//   4. The copy of the OTHER car needs no special treatment: it follows its controller's stream, and that controller applied the
//      same impulse. The contact grace (vehicle_drive.h, W9/Sync B) that lets a copy follow local physics for 400 ms is switched
//      off when impacts are being arbitrated (it is the "third pose of the same car").
//   Cars with the SAME controller (the common case after the empty-car claim in apps/server/vehicles.h) never go through the
//   arbiter: one machine's physics resolves them natively.
//
// CHEATING SURFACE: the server computes the impulse from the two reported tracks, so a client cannot invent an impact for a car it
// does not control; it can only drive its own (already authoritative) track into another. Bounded by: trusted controllers only,
// closing speed from the DISPLACEMENT-corrected speeds (never above the server's vehicleMaxSpeed), |dv| capped (kMaxDvMs), a
// per-pair cooldown, and the receiving client's own cap and gate.
#pragma once

#include "sync/gtaids.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gage::sync::impact {

    constexpr double kPi = 3.14159265358979323846;

    struct V2 { double x = 0, y = 0; };
    inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
    inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
    inline V2 operator*(V2 a, double k) { return {a.x * k, a.y * k}; }
    inline double Dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
    inline double Cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
    inline double Len(V2 a) { return std::sqrt(Dot(a, a)); }
    inline bool Finite(V2 a) { return std::isfinite(a.x) && std::isfinite(a.y); }
    // GTA: heading 0 = +y, counter-clockwise; forward = (-sin h, cos h), right = (cos h, sin h).
    inline V2 Fwd(double h) { return {-std::sin(h), std::cos(h)}; }
    inline V2 Right(double h) { return {std::cos(h), std::sin(h)}; }
    inline double WrapPi(double a)
    {
        while (a > kPi) a -= 2.0 * kPi;
        while (a < -kPi) a += 2.0 * kPi;
        return a;
    }

    // ---- shapes ----
    // The server has no model dimensions (no GET_MODEL_DIMENSIONS native is available): a vehicle is an oriented box chosen by its
    // class. Bikes are narrow and light, most cars are ~4.6 x 2.0 m and 1.5 t, trucks and buses long and heavy.
    enum class MassClass : uint8_t { Light = 0, Car = 1, Heavy = 2 };
    struct Shape
    {
        double hl = 2.3, hw = 1.0, mass = 1500.0;   // half length, half width (m), mass (kg)
        double Inertia() const { return mass * (4.0 * hl * hl + 4.0 * hw * hw) / 12.0; }
    };
    inline Shape ShapeOf(MassClass c)
    {
        switch (c)
        {
        case MassClass::Light: return {1.0, 0.45, 250.0};
        case MassClass::Heavy: return {4.0, 1.35, 7000.0};
        default: return {2.3, 1.0, 1500.0};
        }
    }

    struct Pose
    {
        V2 p, v;
        double h = 0;   // heading, rad (GTA convention above)
        double w = 0;   // yaw rate, rad/s, counter-clockwise
    };

    // ---- contact geometry: separating axis on two oriented boxes ----
    struct Manifold
    {
        bool hit = false;
        double depth = 0;
        double tangentOverlap = 0;   // how far the two boxes' extents overlap ALONG the contact face (perpendicular to n)
        V2 n;    // unit, from a to b
        V2 pt;   // one contact point (the middle of the two deepest vertices)
    };

    inline Manifold Collide(const Pose& a, const Shape& sa, const Pose& b, const Shape& sb)
    {
        Manifold m;
        auto corners = [](const Pose& p, const Shape& s, V2 c[4]) {
            const V2 f = Fwd(p.h), r = Right(p.h);
            c[0] = p.p + f * s.hl + r * s.hw;
            c[1] = p.p + f * s.hl - r * s.hw;
            c[2] = p.p - f * s.hl - r * s.hw;
            c[3] = p.p - f * s.hl + r * s.hw;
        };
        V2 ca[4], cb[4];
        corners(a, sa, ca);
        corners(b, sb, cb);
        const V2 axes[4] = {Fwd(a.h), Right(a.h), Fwd(b.h), Right(b.h)};
        double best = 1e18;
        int bi = -1;
        for (int k = 0; k < 4; ++k)
        {
            const V2& ax = axes[k];
            double mn1 = 1e18, mx1 = -1e18, mn2 = 1e18, mx2 = -1e18;
            for (int i = 0; i < 4; ++i)
            {
                double d = Dot(ca[i], ax);
                mn1 = std::min(mn1, d);
                mx1 = std::max(mx1, d);
                d = Dot(cb[i], ax);
                mn2 = std::min(mn2, d);
                mx2 = std::max(mx2, d);
            }
            const double o = std::min(mx1, mx2) - std::max(mn1, mn2);
            if (o <= 0) return m;
            if (o < best) { best = o; bi = k; }
        }
        m.hit = true;
        m.depth = best;
        // Reference box = the one whose face normal won; incident = the other. The contact point is the middle of the incident face
        // clipped to the reference face (face-face contacts spread over the overlap, a corner contact stays at the corner).
        const bool refIsA = bi < 2;
        const Pose& R = refIsA ? a : b;
        const Pose& I = refIsA ? b : a;
        const Shape& sR = refIsA ? sa : sb;
        const Shape& sI = refIsA ? sb : sa;
        const V2 ax = axes[bi];
        V2 nr = ax;
        if (Dot(nr, I.p - R.p) < 0) nr = nr * -1.0;
        m.n = refIsA ? nr : nr * -1.0;               // from a to b
        const bool axisIsFwd = (bi % 2) == 0;
        const double extN = axisIsFwd ? sR.hl : sR.hw;     // reference extent along nr
        const double extT = axisIsFwd ? sR.hw : sR.hl;     // ... and along the tangent
        const V2 tr{-nr.y, nr.x};
        {
            double mn1 = 1e18, mx1 = -1e18, mn2 = 1e18, mx2 = -1e18;
            for (int i = 0; i < 4; ++i)
            {
                double d = Dot(ca[i], tr);
                mn1 = std::min(mn1, d); mx1 = std::max(mx1, d);
                d = Dot(cb[i], tr);
                mn2 = std::min(mn2, d); mx2 = std::max(mx2, d);
            }
            m.tangentOverlap = std::min(mx1, mx2) - std::max(mn1, mn2);
        }
        const V2 refCenter = R.p + nr * extN;
        // the incident face: the one of I whose outward normal is most against nr
        const V2 fI = Fwd(I.h), rI = Right(I.h);
        const V2 cand[4] = {fI, fI * -1.0, rI, rI * -1.0};
        int ci = 0;
        double bd = 1e18;
        for (int k = 0; k < 4; ++k)
        {
            const double d = Dot(cand[k], nr);
            if (d < bd) { bd = d; ci = k; }
        }
        const V2 ni = cand[ci];
        const bool incFwd = ci < 2;
        const double extIn = incFwd ? sI.hl : sI.hw;      // along ni
        const double extIt = incFwd ? sI.hw : sI.hl;      // along the face
        const V2 ti{-ni.y, ni.x};
        const V2 ic = I.p + ni * extIn;
        V2 e0 = ic - ti * extIt, e1 = ic + ti * extIt;
        // clip the segment to |tangent coordinate relative to refCenter| <= extT
        auto tcoord = [&](V2 p) { return Dot(p - refCenter, tr); };
        double t0 = tcoord(e0), t1 = tcoord(e1);
        auto clipTo = [&](double lim, double sign) {   // keep sign * t <= lim
            const double d0 = lim - sign * t0, d1 = lim - sign * t1;
            if (d0 < 0 && d1 < 0) { e0 = e1 = refCenter; t0 = t1 = 0; return false; }
            if (d0 < 0) { const double u = d0 / (d0 - d1); e0 = e0 + (e1 - e0) * u; t0 = tcoord(e0); }
            else if (d1 < 0) { const double u = d1 / (d1 - d0); e1 = e1 + (e0 - e1) * u; t1 = tcoord(e1); }
            return true;
        };
        bool ok = clipTo(extT, +1.0) && clipTo(extT, -1.0);
        V2 pt{0, 0};
        int kept = 0;
        if (ok)
        {
            const double s0 = Dot(nr, refCenter);
            for (const V2& e : {e0, e1})
            {
                if (s0 - Dot(nr, e) >= -0.02) { pt = pt + e; ++kept; }
            }
        }
        if (kept > 0) m.pt = pt * (1.0 / kept);
        else
        {
            // no clipped point under the reference face (shallow, rotated): the middle of the two boxes' overlap along the normal
            m.pt = (R.p + I.p) * 0.5;
        }
        return m;
    }

    // ---- the two-body impulse ----
    struct SolverCfg
    {
        double restitution = 0.25;   // a car crumples: little bounce
        double friction = 0.30;      // Coulomb, tangential impulse <= friction * normal impulse
        double minClosingMs = 1.2;   // below this closing speed along the normal nothing is resolved (a touch, a sustained push)
    };

    struct Resolution
    {
        bool valid = false;
        V2 dv[2];            // velocity change of a, b (m/s)
        double dw[2] = {0, 0};   // yaw-rate change of a, b (rad/s)
        V2 n, pt;
        double closing = 0;  // m/s along n, > 0 = approaching, before the impulse
        double j = 0;        // normal impulse (N s)
    };

    inline Resolution Resolve(const Pose& a, const Shape& sa, const Pose& b, const Shape& sb, const Manifold& m, const SolverCfg& cfg)
    {
        Resolution r;
        if (!m.hit) return r;
        const double iA = 1.0 / sa.mass, iB = 1.0 / sb.mass, IA = 1.0 / sa.Inertia(), IB = 1.0 / sb.Inertia();
        const V2 rA = m.pt - a.p, rB = m.pt - b.p;
        const V2 vA = a.v + V2{-a.w * rA.y, a.w * rA.x}, vB = b.v + V2{-b.w * rB.y, b.w * rB.x};
        const V2 rel = vB - vA;
        const double vn = Dot(rel, m.n);
        r.n = m.n;
        r.pt = m.pt;
        r.closing = -vn;
        if (!(-vn > cfg.minClosingMs)) return r;
        const double rAn = Cross(rA, m.n), rBn = Cross(rB, m.n);
        const double den = iA + iB + IA * rAn * rAn + IB * rBn * rBn;
        const double jn = -(1.0 + cfg.restitution) * vn / den;
        V2 P = m.n * jn;   // impulse on b; -P on a
        // Coulomb friction along the tangent
        const V2 t{-m.n.y, m.n.x};
        const double vt = Dot(rel, t);
        const double rAt = Cross(rA, t), rBt = Cross(rB, t);
        const double denT = iA + iB + IA * rAt * rAt + IB * rBt * rBt;
        double jt = -vt / denT;
        const double lim = cfg.friction * jn;
        jt = std::max(-lim, std::min(lim, jt));
        P = P + t * jt;
        r.dv[0] = P * (-iA);
        r.dv[1] = P * iB;
        r.dw[0] = -IA * Cross(rA, P);
        r.dw[1] = IB * Cross(rB, P);
        r.j = jn;
        r.valid = Finite(r.dv[0]) && Finite(r.dv[1]) && std::isfinite(r.dw[0]) && std::isfinite(r.dw[1]);
        return r;
    }

    // ---- server side: the arbiter ----

    // One accepted VehicleUpdate sample of one vehicle, on the SERVER's time base (SampleClock placed).
    struct ArbSample
    {
        double tMs = 0;       // server time the sample was taken at
        V2 p;                 // position (x, y)
        V2 v;                 // velocity (x, y)
        double h = 0;         // heading, rad
        double w = 0;         // yaw rate, rad/s
        uint16_t stamp = 0;   // the sender's own sampleMs (its clock, wraps at 65536): lets the impact be told on the sender's clock
    };

    constexpr uint16_t kNoCtrl = 0xFFFF;
    constexpr double kMaxDvMs = 30.0;   // no impact changes a car's velocity by more than this

    struct ImpactOut
    {
        uint32_t seq = 0;
        uint16_t id[2] = {0, 0};          // vehicle ids (a, b)
        uint16_t ctrl[2] = {kNoCtrl, kNoCtrl};
        MassClass mc[2] = {MassClass::Car, MassClass::Car};
        double tMs = 0;                   // server time of the contact
        V2 dv[2];
        double dw[2] = {0, 0};
        V2 n, pt;                         // n from a to b
        double closing = 0;
        double depth = 0;                 // deepest overlap of the true-size boxes within 120 ms after the touch (0: they only came within the margin)
        uint16_t stamp[2] = {0, 0};       // the contact time on each controller's own sample clock
        bool stampOk[2] = {false, false};
    };

    class ContactArbiter
    {
    public:
        struct Cfg
        {
            double broadM = 14.0;          // pairs farther apart than this (newest samples) are not examined
            double maxExtrapMs = 120.0;    // a moving vehicle is extrapolated at most this far past its newest sample
            double rearmApartMs = 120.0;   // a pair must be seen apart this long before it can impact again
            double cooldownMs = 250.0;     // and no sooner than this after an impact
            double stillMs = 0.5;          // below this speed a vehicle is "still" (its pose is valid at any later time)
            double marginM = 0.30;         // each box is grown by this: the controllers' own physics keeps their tracks from overlapping
            double minFaceM = 0.10;        // the true boxes must overlap by this along the contact face (else two cars in adjacent lanes would 'hit' nose to nose)
            double scanStepMs = 4.0;       // the pair is scanned for the first touching instant at this step ...
            double scanBackMs = 260.0;     // ... back to this far before the common instant (a bounce may already show in the samples)
            double firstScanMs = 120.0;    // the first scan of a pair that just came into range looks back this far
            int bisect = 12;
            SolverCfg solver;
        };
        struct Live { uint16_t id = 0; uint16_t ctrl = kNoCtrl; MassClass mc = MassClass::Car; bool trusted = true; };
        struct Stats
        {
            uint64_t pairsExamined = 0, overlaps = 0, impacts = 0, softTouches = 0, sameCtrl = 0, untrusted = 0, capped = 0, noData = 0;
            double maxClosing = 0;
        };

        ContactArbiter() = default;
        explicit ContactArbiter(const Cfg& c) : cfg_(c) {}
        const Cfg& cfg() const { return cfg_; }
        const Stats& stats() const { return stats_; }
        void ResetStats() { stats_ = Stats(); }

        // A sample the vehicle's controller sent (in arrival order). A new controller restarts the history (another clock).
        void Note(uint16_t id, uint16_t ctrl, const ArbSample& s)
        {
            if (id >= kMaxIds) return;
            Ring& r = ring_[id];
            if (r.ctrl != ctrl)
            {
                r = Ring();
                r.ctrl = ctrl;
            }
            if (r.n > 0 && !(s.tMs > r.At(0).tMs)) return;   // not newer
            r.head = (r.head + kRing - 1) % kRing;
            r.s[r.head] = s;
            if (r.n < kRing) ++r.n;
        }
        void Forget(uint16_t id)
        {
            if (id < kMaxIds) ring_[id] = Ring();
            for (auto it = pairs_.begin(); it != pairs_.end();)
                it = (it->a == id || it->b == id) ? pairs_.erase(it) : it + 1;
        }

        // The pose of vehicle `id` at server time tMs (interpolated between samples, extrapolated at most maxExtrapMs past the
        // newest one unless it is still). False: no data for that instant.
        bool PoseAt(uint16_t id, double tMs, Pose& out) const
        {
            if (id >= kMaxIds || ring_[id].n == 0) return false;
            const Ring& r = ring_[id];
            const ArbSample& nw = r.At(0);
            if (tMs >= nw.tMs)
            {
                const double dt = tMs - nw.tMs;
                const bool still = Len(nw.v) < cfg_.stillMs && std::fabs(nw.w) < 0.05;
                if (!still && dt > cfg_.maxExtrapMs) return false;
                const double s = dt / 1000.0;
                out.p = nw.p + nw.v * s;
                out.v = nw.v;
                out.h = nw.h + nw.w * s;
                out.w = nw.w;
                return true;
            }
            for (int i = 0; i + 1 < r.n; ++i)
            {
                const ArbSample& b = r.At(i);
                const ArbSample& a = r.At(i + 1);
                if (tMs >= a.tMs)
                {
                    const double span = b.tMs - a.tMs;
                    const double u = span > 1e-6 ? (tMs - a.tMs) / span : 0.0;
                    // cubic Hermite on position with the sampled velocities as tangents
                    const double s = span / 1000.0, u2 = u * u, u3 = u2 * u;
                    const double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
                    out.p = a.p * h00 + a.v * (h10 * s) + b.p * h01 + b.v * (h11 * s);
                    out.v = a.v + (b.v - a.v) * u;
                    out.h = a.h + WrapPi(b.h - a.h) * u;
                    out.w = a.w + (b.w - a.w) * u;
                    return true;
                }
            }
            return false;   // older than the oldest sample
        }

        // The sender's clock stamp of server time tMs for vehicle `id` (sample stamps differ by the sender's clock, offset by a
        // constant): false when tMs is outside the history.
        bool StampAt(uint16_t id, double tMs, uint16_t& out) const
        {
            if (id >= kMaxIds || ring_[id].n == 0) return false;
            const Ring& r = ring_[id];
            const ArbSample& nw = r.At(0);
            // stamp(t) = stamp(newest) + (t - newest.t) in whole ms, valid in and slightly beyond the history
            if (tMs < r.At(r.n - 1).tMs - 50.0 || tMs > nw.tMs + 400.0) return false;
            out = static_cast<uint16_t>(static_cast<int64_t>(nw.stamp) + static_cast<int64_t>(std::llround(tMs - nw.tMs)));
            return true;
        }

        // Examine every pair; append the impacts found to `out`. nowMs is the server time of the tick (only used to bound the history).
        void Step(double nowMs, const std::vector<Live>& live, std::vector<ImpactOut>& out)
        {
            (void)nowMs;
            const size_t n = live.size();
            for (size_t i = 0; i < n; ++i)
                for (size_t j = i + 1; j < n; ++j)
                    Examine(live[i], live[j], out);
        }

    private:
        static constexpr int kRing = 8;
        static constexpr uint16_t kMaxIds = 256;
        struct Ring
        {
            uint16_t ctrl = kNoCtrl;
            ArbSample s[kRing];
            int head = 0, n = 0;
            const ArbSample& At(int i) const { return s[(head + i) % kRing]; }   // 0 = newest
        };
        struct PairState
        {
            uint16_t a = 0, b = 0;
            bool inContact = false;
            double scanT = -1e18;          // the pair has been examined up to this server time
            double lastOverlapT = -1e18;
            double lastImpactT = -1e18;
        };

        PairState& Pair(uint16_t a, uint16_t b)
        {
            for (PairState& p : pairs_)
                if (p.a == a && p.b == b) return p;
            PairState p;
            p.a = a;
            p.b = b;
            pairs_.push_back(p);
            return pairs_.back();
        }

        // Boxes grown by the touch margin: the controllers' own physics keeps their tracks from overlapping (each stops at the
        // other's copy), so a real contact shows as a gap of about zero, not as penetration. Velocities are the sampled ones, held
        // (zero-order) from the latest sample at or before t: interpolating them across a bounce would blend the pre- and
        // post-impact velocities of the very sample pair the bounce falls in.
        bool Touch(const Live& A, const Live& B, double t, double margin, Pose& pa, Pose& pb, Manifold& m) const
        {
            if (!PoseAt(A.id, t, pa) || !PoseAt(B.id, t, pb)) return false;
            Shape sa = ShapeOf(A.mc), sb = ShapeOf(B.mc);
            sa.hl += margin; sa.hw += margin; sb.hl += margin; sb.hw += margin;
            m = Collide(pa, sa, pb, sb);
            return m.hit;
        }
        void HoldVelocity(uint16_t id, double t, Pose& p) const
        {
            const Ring& r = ring_[id];
            for (int i = 0; i < r.n; ++i)
                if (r.At(i).tMs <= t) { p.v = r.At(i).v; p.w = r.At(i).w; return; }
            p.v = r.At(r.n - 1).v;
            p.w = r.At(r.n - 1).w;
        }

        void Examine(const Live& A, const Live& B, std::vector<ImpactOut>& out)
        {
            if (A.id >= kMaxIds || B.id >= kMaxIds || ring_[A.id].n == 0 || ring_[B.id].n == 0) return;
            if (A.ctrl == kNoCtrl || B.ctrl == kNoCtrl) return;
            if (A.ctrl == B.ctrl) { ++stats_.sameCtrl; return; }
            const ArbSample& na = ring_[A.id].At(0);
            const ArbSample& nb = ring_[B.id].At(0);
            const V2 d = na.p - nb.p;
            PairState& ps = Pair(std::min(A.id, B.id), std::max(A.id, B.id));
            if (Dot(d, d) > cfg_.broadM * cfg_.broadM)
            {
                ps.scanT = -1e18;
                ps.inContact = false;
                return;
            }
            const bool stillA = Len(na.v) < cfg_.stillMs && std::fabs(na.w) < 0.05;
            const bool stillB = Len(nb.v) < cfg_.stillMs && std::fabs(nb.w) < 0.05;
            // the common instant: the older of the two newest samples (a still car's pose is valid at any later time)
            double T = 1e18;
            if (!stillA) T = std::min(T, na.tMs);
            if (!stillB) T = std::min(T, nb.tMs);
            if (stillA && stillB)
            {
                // two cars at rest can still be one at rest and one that just stopped (its last moving sample is in the history)
                T = std::max(na.tMs, nb.tMs);
            }
            if (!A.trusted || !B.trusted) { ++stats_.untrusted; ps.scanT = T; return; }
            ++stats_.pairsExamined;
            if (ps.scanT < -1e17) ps.scanT = T - cfg_.firstScanMs;
            double from = std::max(ps.scanT, T - cfg_.scanBackMs);
            if (T <= from) { ps.scanT = std::max(ps.scanT, T); return; }
            bool prev = false;
            double prevT = from;
            {
                Pose xa, xb;
                Manifold xm;
                prev = Touch(A, B, from, cfg_.marginM, xa, xb, xm);
            }
            for (double t = from + cfg_.scanStepMs;; t += cfg_.scanStepMs)
            {
                const bool last = t >= T;
                const double tt = last ? T : t;
                Pose xa, xb;
                Manifold xm;
                const bool now = Touch(A, B, tt, cfg_.marginM, xa, xb, xm);
                if (now)
                {
                    ++stats_.overlaps;
                    ps.lastOverlapT = tt;
                    if (!prev && !ps.inContact && tt - ps.lastImpactT >= cfg_.cooldownMs)
                        if (Fire(A, B, ps, prevT, tt, out)) ps.inContact = true;
                }
                else if (ps.inContact && tt - ps.lastOverlapT >= cfg_.rearmApartMs)
                    ps.inContact = false;
                prev = now;
                prevT = tt;
                if (last) break;
            }
            ps.scanT = T;
        }

        // The pair was apart at lo and touches at hi: find the first touching instant and resolve the impact there.
        bool Fire(const Live& A, const Live& B, PairState& ps, double lo, double hi, std::vector<ImpactOut>& out)
        {
            for (int k = 0; k < cfg_.bisect; ++k)
            {
                const double mid = 0.5 * (lo + hi);
                Pose xa, xb;
                Manifold xm;
                if (Touch(A, B, mid, cfg_.marginM, xa, xb, xm)) hi = mid; else lo = mid;
            }
            const double Tc = hi;
            Pose ca, cb;
            Manifold cm;
            if (!Touch(A, B, Tc, cfg_.marginM, ca, cb, cm)) return false;
            // Two cars in adjacent lanes whose BOXES (grown by the margin) meet nose to nose are passing, not hitting: along the contact face the
            // true boxes must overlap by minFaceM, else it is a graze (remembered as a touch, not resolved).
            if (cm.tangentOverlap - 2.0 * cfg_.marginM < cfg_.minFaceM)
            {
                ++stats_.softTouches;
                ps.lastImpactT = Tc;
                return true;
            }
            // the bounce has not happened yet at Tc: the velocities are the ones held from before it
            HoldVelocity(A.id, Tc - 1.0, ca);
            HoldVelocity(B.id, Tc - 1.0, cb);
            Shape sa = ShapeOf(A.mc), sb = ShapeOf(B.mc);
            // depth with the true boxes at the instant the boxes (margin included) touched is ~ -2 margin: report the depth a little later
            Pose da, db;
            Manifold dm;
            double depth = 0;
            for (double dt = 0; dt <= 120.0; dt += 8.0)
            {
                Shape ta = sa, tb = sb;
                if (!PoseAt(A.id, Tc + dt, da) || !PoseAt(B.id, Tc + dt, db)) break;
                dm = Collide(da, ta, db, tb);
                if (dm.hit) depth = std::max(depth, dm.depth);
            }
            const Resolution res = Resolve(ca, sa, cb, sb, cm, cfg_.solver);
            ps.lastImpactT = Tc;
            if (!res.valid)
            {
                ++stats_.softTouches;
                return true;   // a touch (a pass, a slow contact): remembered so it does not fire again until the pair has parted
            }
            ImpactOut o;
            o.seq = 0;
            o.id[0] = A.id; o.id[1] = B.id;
            o.ctrl[0] = A.ctrl; o.ctrl[1] = B.ctrl;
            o.mc[0] = A.mc; o.mc[1] = B.mc;
            o.tMs = Tc;
            for (int k = 0; k < 2; ++k)
            {
                V2 dv = res.dv[k];
                double dw = res.dw[k];
                const double len = Len(dv);
                if (len > kMaxDvMs) { dv = dv * (kMaxDvMs / len); ++stats_.capped; }
                dw = std::max(-12.0, std::min(12.0, dw));
                o.dv[k] = dv;
                o.dw[k] = dw;
            }
            o.n = res.n;
            o.pt = res.pt;
            o.closing = res.closing;
            o.depth = depth;
            o.stampOk[0] = StampAt(A.id, Tc, o.stamp[0]);
            o.stampOk[1] = StampAt(B.id, Tc, o.stamp[1]);
            ++stats_.impacts;
            stats_.maxClosing = std::max(stats_.maxClosing, res.closing);
            out.push_back(o);
            return true;
        }

        Cfg cfg_;
        Stats stats_;
        Ring ring_[kMaxIds];
        std::vector<PairState> pairs_;
    };

    // ---- client side: what the local physics did, and what to apply ----

    // Velocity jumps of the car the local client controls that no driver input produces: a collision. Fed once per game frame with
    // the car's velocity (xy) and yaw rate on the local clock. A jump is a frame whose velocity change exceeds max(minFrameDvMs,
    // accelMs2 * dt); consecutive jump frames (a bounce lasts a few) are one episode.
    class ImpactTracker
    {
    public:
        struct Episode { double t0 = 0, t1 = 0; V2 dv; double dw = 0; };
        struct Cfg
        {
            double minFrameDvMs = 1.5;   // velocity change in one frame that can be a hit
            double accelMs2 = 60.0;      // ... or the accelerations no driver or brake produces (the larger of the two applies)
            double maxFrameMs = 100.0;   // a longer frame gap is not judged
            double keepMs = 3000.0;
        };
        ImpactTracker() = default;
        explicit ImpactTracker(const Cfg& c) : cfg_(c) {}

        void Reset() { have_ = false; n_ = 0; open_ = false; }
        // True when this frame was a jump frame.
        bool Feed(double tMs, V2 v, double w)
        {
            if (!Finite(v) || !std::isfinite(w) || !std::isfinite(tMs)) return false;
            bool jump = false;
            frameDv_ = V2();
            frameDw_ = 0;
            if (have_ && tMs > lastT_ && tMs - lastT_ <= cfg_.maxFrameMs)
            {
                const double dt = (tMs - lastT_) / 1000.0;
                const V2 dv = v - lastV_;
                const double lim = std::max(cfg_.minFrameDvMs, cfg_.accelMs2 * dt);
                if (Len(dv) > lim)
                {
                    jump = true;
                    if (!open_)
                    {
                        open_ = true;
                        cur_ = Episode();
                        cur_.t0 = tMs;
                    }
                    cur_.t1 = tMs;
                    cur_.dv = cur_.dv + dv;
                    cur_.dw += w - lastW_;
                    frameDv_ = dv;
                    frameDw_ = w - lastW_;
                }
            }
            if (!jump && open_)
            {
                open_ = false;
                Push(cur_);
            }
            lastT_ = tMs; lastV_ = v; lastW_ = w; have_ = true;
            // forget old episodes
            while (n_ > 0 && tMs - eps_[(head_ + n_ - 1) % kEps].t1 > cfg_.keepMs) --n_;
            return jump;
        }
        // This frame's velocity / yaw-rate change when Feed returned true (else zero).
        V2 FrameDv() const { return frameDv_; }
        double FrameDw() const { return frameDw_; }
        // The caller wrote `corr` / `cw` onto the car (a prediction's correction): the next frame must not read it as a jump, and the
        // episode's ledger counts it, so a later authoritative message is compared with what the car has really been given.
        void NoteWritten(V2 vAfter, double wAfter, V2 corr, double cw)
        {
            lastV_ = vAfter;
            lastW_ = wAfter;
            if (open_) { cur_.dv = cur_.dv + corr; cur_.dw += cw; }
            else if (n_ > 0) { Episode& e = eps_[head_]; e.dv = e.dv + corr; e.dw += cw; }
        }
        // Sum of the episodes whose start lies in [t0, t1] (an open one counts as far as it has gone).
        bool Sum(double t0, double t1, V2& dv, double& dw) const
        {
            dv = V2(); dw = 0;
            bool any = false;
            for (int i = 0; i < n_; ++i)
            {
                const Episode& e = eps_[(head_ + i) % kEps];
                if (e.t0 >= t0 && e.t0 <= t1) { dv = dv + e.dv; dw += e.dw; any = true; }
            }
            if (open_ && cur_.t0 >= t0 && cur_.t0 <= t1) { dv = dv + cur_.dv; dw += cur_.dw; any = true; }
            return any;
        }
        uint32_t Episodes() const { return total_ + (open_ ? 1u : 0u); }

    private:
        static constexpr int kEps = 8;
        void Push(const Episode& e)
        {
            head_ = (head_ + kEps - 1) % kEps;
            eps_[head_] = e;
            if (n_ < kEps) ++n_;
            ++total_;
        }
        Cfg cfg_;
        bool have_ = false, open_ = false;
        double lastT_ = 0, lastW_ = 0;
        V2 lastV_, frameDv_;
        double frameDw_ = 0;
        Episode cur_;
        Episode eps_[kEps];
        int head_ = 0, n_ = 0;
        uint32_t total_ = 0;
    };

    // ---- client side: the copy's own response to a hit becomes its trajectory ----
    //
    // A copy that takes an impulse inside a frame (the controller sees |last command - the car's velocity| above its kick
    // threshold, or the collision flag) has just shown the OTHER car's response to the hit, from the game's own two-body physics. The
    // controller's next command would pull it back to the stream, which still carries the pre-impact motion for the whole
    // network latency: the copy ploughs back through the car it hit, and that car (the local one) is hit again. So the response is
    // adopted: VehicleInterpolator::ReplaceFrom stores it as the copy's newest snapshot and refuses the stale stream until the
    // stream shows the impact (or the guard runs out). The adopted change is bounded: a contact the physics solved violently
    // (interlocked bodies) must not throw the copy away.
    struct AdoptCfg
    {
        double minDvMs = 2.0;       // below this change of velocity nothing is adopted (tyre noise, a bump)
        double maxDvMs = 30.0;      // ... above this the change is scaled down to it
        double maxYawRadS = 8.0;    // the adopted yaw rate is clamped to this
        double lateralKeep = 1.0;   // the share of the SIDEWAYS part of the change that is adopted (1: all of it). Measured in tests/vehsim_2m.h: dropping it
                                    // (0) makes a T-bone on a good link 3x worse and does not help at 120 ms, so the whole change is adopted
    };
    struct AdoptOut
    {
        bool adopt = false;
        double vx = 0, vy = 0, vz = 0;
        double yawRate = 0;
        double dv = 0;              // size of the change that was adopted
    };
    // streamVel: the velocity the interpolator's sample (the pre-impact motion) had; actualVel / actualYawRate: what the copy's body
    // did. The vertical velocity stays the stream's: suspension and landing are not an impact.
    // headingRad: the copy's heading (GTA: 0 = +y, counter-clockwise), to split the change into forward and sideways.
    inline AdoptOut DecideAdopt(double sx, double sy, double sz, double ax, double ay, double yawRate, double headingRad, const AdoptCfg& cfg)
    {
        AdoptOut o;
        if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(sz) || !std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(yawRate) ||
            !std::isfinite(headingRad))
            return o;
        double dx = ax - sx, dy = ay - sy;
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len < cfg.minDvMs) return o;
        if (len > cfg.maxDvMs) { dx *= cfg.maxDvMs / len; dy *= cfg.maxDvMs / len; }
        {
            const double fx = -std::sin(headingRad), fy = std::cos(headingRad);
            const double fwd = dx * fx + dy * fy;
            const double lx = dx - fwd * fx, ly = dy - fwd * fy;     // the sideways part
            dx = fwd * fx + lx * cfg.lateralKeep;
            dy = fwd * fy + ly * cfg.lateralKeep;
        }
        o.adopt = true;
        o.vx = sx + dx;
        o.vy = sy + dy;
        o.vz = sz;
        o.yawRate = std::max(-cfg.maxYawRadS, std::min(cfg.maxYawRadS, yawRate));
        o.dv = std::sqrt(dx * dx + dy * dy);
        return o;
    }

    struct ApplyCfg
    {
        double lookBackMs = 300.0;    // local jumps from this long before the contact ...
        double lookAheadMs = 450.0;   // ... to this long after it belong to it
        double maxAgeMs = 1500.0;     // a message older than this (its contact time vs now) is not applied
        double minDvMs = 0.6;         // an authoritative change below this is ignored
        double maxCorrMs = 35.0;      // cap of one correction
        double maxCorrRadS = 6.0;
        double minPhantomDepthM = 0.10;   // a server impact this car's physics did not feel is applied only if the server's boxes truly overlapped
    };
    enum class ApplyWhy : uint8_t { Applied = 0, NoLocalContact, Tiny, Stale, Duplicate, Disabled };
    struct ApplyDecision
    {
        ApplyWhy why = ApplyWhy::Tiny;
        V2 dv;                       // add to the car's velocity
        double dw = 0;               // add to its yaw rate
        V2 dvLocal;                  // what the local physics had done in the window
        bool localContact = false;
    };

    // dvAuth/dwAuth: the arbiter's change for OUR car; tContactMs: its contact time on our local clock; localNear: our copy of the
    // other car was within a few metres of ours around that time (the engine's own check).
    inline ApplyDecision DecideImpactApply(const ImpactTracker& trk, V2 dvAuth, double dwAuth, double tContactMs, double nowMs,
                                           bool localNear, double depthM, const ApplyCfg& cfg)
    {
        ApplyDecision d;
        if (!Finite(dvAuth) || !std::isfinite(dwAuth) || !std::isfinite(tContactMs) || !std::isfinite(nowMs)) { d.why = ApplyWhy::Stale; return d; }
        if (nowMs - tContactMs > cfg.maxAgeMs || tContactMs - nowMs > 1000.0) { d.why = ApplyWhy::Stale; return d; }
        if (Len(dvAuth) < cfg.minDvMs && std::fabs(dwAuth) < 0.2) { d.why = ApplyWhy::Tiny; return d; }
        V2 dvL;
        double dwL = 0;
        d.localContact = trk.Sum(tContactMs - cfg.lookBackMs, tContactMs + cfg.lookAheadMs, dvL, dwL);
        if (!d.localContact && !(localNear && depthM >= cfg.minPhantomDepthM)) { d.why = ApplyWhy::NoLocalContact; return d; }
        // Subtract only what points the same way (a local jump from something else, a wall, must not be "corrected" away).
        if (d.localContact && Dot(dvL, dvAuth) <= 0.0) dvL = V2();
        if (d.localContact && dwL * dwAuth <= 0.0) dwL = 0.0;
        d.dvLocal = dvL;
        V2 corr = dvAuth - dvL;
        double cw = dwAuth - dwL;
        const double cl = Len(corr);
        if (cl > cfg.maxCorrMs) corr = corr * (cfg.maxCorrMs / cl);
        cw = std::max(-cfg.maxCorrRadS, std::min(cfg.maxCorrRadS, cw));
        d.dv = corr;
        d.dw = cw;
        d.why = ApplyWhy::Applied;
        return d;
    }

    // The server knows a vehicle only by its model hash (no GET_MODEL_DIMENSIONS, no mass native): the common light and heavy ones by
    // name, everything else is a car. (GET_HASH_KEY of the lower-cased model name; gtaids.h Joaat.)
    inline MassClass MassClassOfModel(uint32_t model)
    {
        static const char* const kLight[] = {"bmx", "cruiser", "fixter", "scorcher", "tribike", "tribike2", "tribike3", "akuma", "bagger", "bati", "bati2",
                                             "bf400", "carbonrs", "chimera", "cliffhanger", "daemon", "daemon2", "defiler", "diablous", "diablous2",
                                             "double", "enduro", "esskey", "faggio", "faggio2", "faggio3", "fcr", "fcr2", "gargoyle", "hakuchou",
                                             "hakuchou2", "hexer", "innovation", "lectro", "manchez", "nemesis", "nightblade", "pcj", "ratbike",
                                             "ruffian", "sanchez", "sanchez2", "sanctus", "shotaro", "sovereign", "thrust", "vader", "vindicator",
                                             "vortex", "wolfsbane", "zombiea", "zombieb", "stryder", "avarus", "reever", "deathbike", "deathbike2"};
        static const char* const kHeavy[] = {"phantom", "phantom2", "phantom3", "packer", "hauler", "hauler2", "benson", "mule", "mule2", "mule3", "mule4",
                                             "pounder", "pounder2", "biff", "flatbed", "tiptruck", "tiptruck2", "mixer", "mixer2", "rubble", "dump",
                                             "handler", "cutter", "bus", "coach", "airbus", "tourbus", "rentalbus", "pbus", "pbus2", "trash", "trash2",
                                             "stockade", "stockade3", "barracks", "barracks2", "barracks3", "rhino", "insurgent", "insurgent2", "apc",
                                             "scarab", "scarab2", "scarab3", "khanjali", "firetruk", "brickade", "boxville", "boxville2", "boxville3",
                                             "boxville4", "boxville5", "rallytruck", "bulldozer", "terbyte", "cerberus", "cerberus2", "cerberus3",
                                             "monster", "monster3", "monster4", "monster5", "bruiser", "bruiser2", "bruiser3", "dune3", "marshall"};
        for (const char* n : kLight)
            if (Joaat(n) == model) return MassClass::Light;
        for (const char* n : kHeavy)
            if (Joaat(n) == model) return MassClass::Heavy;
        return MassClass::Car;
    }

    // A sender-clock stamp (VehicleUpdate.sampleMs domain, u16 whole ms) back to the sender's full clock: the instant within
    // +-32.7 s of `nowMs` whose low 16 bits are `stamp`.
    inline double UnwrapStampMs(uint16_t stamp, double nowMs)
    {
        const uint16_t nowLow = static_cast<uint16_t>(static_cast<uint64_t>(nowMs < 0 ? 0 : nowMs) & 0xFFFFu);
        const int16_t delta = static_cast<int16_t>(static_cast<uint16_t>(stamp - nowLow));
        return std::floor(nowMs) + static_cast<double>(delta);
    }

    inline ApplyDecision DecideImpactApply(const ImpactTracker& trk, V2 dvAuth, double dwAuth, double tContactMs, double nowMs,
                                           bool localNear, double depthM)
    {
        return DecideImpactApply(trk, dvAuth, dwAuth, tContactMs, nowMs, localNear, depthM, ApplyCfg());
    }

    // Mass class from a GET_VEHICLE_CLASS value (the engine knows it; the server has to guess from the model).
    inline MassClass MassClassOfVehicleClass(int cls)
    {
        if (cls == 8 || cls == 13) return MassClass::Light;                       // motorcycles, cycles
        if (cls == 10 || cls == 11 || cls == 17 || cls == 19 || cls == 20) return MassClass::Heavy;   // industrial, utility, service, military, commercial
        return MassClass::Car;
    }

}   // namespace gage::sync::impact
