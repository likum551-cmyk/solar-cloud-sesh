// GAGE protocol quantisation (v2 kinematics, v3 on-foot state): how a Kinematics / PedState
// (floats) becomes the integers that go on the wire and back, and exactly how much precision
// that costs. Header-only, portable C++17.
//
// Precision (the numbers docs/api/protocol.md quotes, proven by tests/quant_test.cpp):
//   position  1 cm steps (round to nearest): error <= 0.5 cm per axis, <= 0.87 cm in 3D,
//             plus <= 0.05 cm of float representation at the map edge (a float has 24 bits);
//             so every on-foot position survives to well under 1 cm.
//   heading   65536 steps per turn: error <= 0.0028 degrees. Headings wrap into [0, 360).
//   velocity  1/32 m/s steps: error <= 1/64 m/s (1.6 cm/s) per axis.
//   coarse    x, y in 0.5 m steps (error <= 0.25 m), z in 5/64 m steps (error <= 0.04 m) anywhere
//             below the +16383.75 m / +3071.96 m edge of the box (the world box ValidKinematics
//             enforces is well inside it); the top cell saturates, so within the last half step at
//             that edge the error is up to one step (0.5 m, 0.078 m). The map-blip / scoreboard
//             channel for players out of relevance range.
//
// Range (the "quantiser box", slightly larger than the world box ValidKinematics enforces so a
// position just outside the world still round-trips and gets rejected by policy, not by the codec):
//   x, y in [-16384, 16384] m, z in [-2048, 3072] m, velocity per axis in [-1000, 1000] m/s.
//   Quantize() saturates to the box; Decode() rejects any wire value outside it.
#pragma once

#include "protocol/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gage::proto {

    constexpr float kQuantMaxXY = 16384.0f;  // m, +-
    constexpr float kQuantMinZ  = -2048.0f;  // m
    constexpr float kQuantMaxZ  = 3072.0f;   // m
    constexpr float kQuantMaxVel = 1000.0f;  // m/s per axis, +-

    constexpr int32_t kPosPerMeter = 100;    // position unit: 1 cm
    constexpr int32_t kVelPerMps   = 32;     // velocity unit: 1/32 m/s
    constexpr int32_t kQMaxXY  = 16384 * kPosPerMeter;    //  1 638 400
    constexpr int32_t kQMinZ   = -2048 * kPosPerMeter;    // -204 800
    constexpr int32_t kQMaxZ   = 3072 * kPosPerMeter;     //  307 200
    constexpr int32_t kQMaxVel = 1000 * kVelPerMps;       //  32 000 (fits an int16)

    constexpr int kBitsXY = 22, kBitsZ = 20; // signed widths of the absolute position fields (64 bits in all)

    // Guaranteed error bounds of one quantise/dequantise round trip, in the unit of the field.
    constexpr double kMaxPosError     = 0.005 + 0.0005; // m per axis (round to 1 cm + float ulp at 16 km)
    constexpr double kMaxHeadingError = 360.0 / 65536.0 / 2.0 + 2e-5; // degrees (half a step + float ulp at 360)
    constexpr double kMaxVelError     = 1.0 / 64.0 + 1e-9; // m/s per axis

    // The wire form of one player's state.
    struct QKin
    {
        int32_t x = 0, y = 0, z = 0;       // cm
        uint16_t heading = 0;              // 0..65535 = [0, 360) degrees
        int16_t vx = 0, vy = 0, vz = 0;    // 1/32 m/s

        bool operator==(const QKin& o) const
        {
            return x == o.x && y == o.y && z == o.z && heading == o.heading && vx == o.vx && vy == o.vy && vz == o.vz;
        }
        bool operator!=(const QKin& o) const { return !(*this == o); }
    };

    // True when every field is inside the quantiser box (what Decode checks on the wire).
    inline bool QInRange(const QKin& q)
    {
        return q.x >= -kQMaxXY && q.x <= kQMaxXY && q.y >= -kQMaxXY && q.y <= kQMaxXY && q.z >= kQMinZ &&
               q.z <= kQMaxZ && q.vx >= -kQMaxVel && q.vx <= kQMaxVel && q.vy >= -kQMaxVel && q.vy <= kQMaxVel &&
               q.vz >= -kQMaxVel && q.vz <= kQMaxVel;
    }

    namespace qdetail {
        inline int32_t RoundClamp(double v, int32_t lo, int32_t hi)
        {
            if (std::isnan(v)) return 0;      // documented: a NaN field becomes 0 (+-Inf saturates below)
            const double r = std::floor(v + 0.5); // round half up: deterministic on every platform
            if (!(r > (double)lo)) return lo;
            if (r > (double)hi) return hi;
            return (int32_t)r;
        }
    }

    // Float state -> wire integers. Positions and velocities saturate at the quantiser box;
    // heading wraps into [0, 360). A NaN field becomes 0 and +-Inf saturates (callers
    // validate first; Encode(PlayerState) refuses non-finite input outright).
    inline QKin Quantize(const Kinematics& k)
    {
        QKin q;
        q.x = qdetail::RoundClamp((double)k.x * kPosPerMeter, -kQMaxXY, kQMaxXY);
        q.y = qdetail::RoundClamp((double)k.y * kPosPerMeter, -kQMaxXY, kQMaxXY);
        q.z = qdetail::RoundClamp((double)k.z * kPosPerMeter, kQMinZ, kQMaxZ);
        double h = std::isfinite(k.heading) ? std::fmod((double)k.heading, 360.0) : 0.0;
        if (h < 0) h += 360.0;
        q.heading = (uint16_t)((int64_t)std::floor(h * (65536.0 / 360.0) + 0.5) & 0xFFFF); // 360.0 wraps to 0
        q.vx = (int16_t)qdetail::RoundClamp((double)k.vx * kVelPerMps, -kQMaxVel, kQMaxVel);
        q.vy = (int16_t)qdetail::RoundClamp((double)k.vy * kVelPerMps, -kQMaxVel, kQMaxVel);
        q.vz = (int16_t)qdetail::RoundClamp((double)k.vz * kVelPerMps, -kQMaxVel, kQMaxVel);
        return q;
    }

    // Wire integers -> float state. Always finite. Heading lands in [0, 360).
    inline Kinematics Dequantize(const QKin& q)
    {
        Kinematics k;
        k.x = (float)((double)q.x / kPosPerMeter);
        k.y = (float)((double)q.y / kPosPerMeter);
        k.z = (float)((double)q.z / kPosPerMeter);
        k.heading = (float)((double)q.heading * (360.0 / 65536.0));
        k.vx = (float)((double)q.vx / kVelPerMps);
        k.vy = (float)((double)q.vy / kVelPerMps);
        k.vz = (float)((double)q.vz / kVelPerMps);
        return k;
    }

    // ---- the on-foot state (v3): PedState on the wire ----

    // The wire form of a PedState. Invariant of every QPed this library produces or accepts:
    // jumpSeq is 0..3, shotSeq 0..15, and ax/ay/az are 0 unless kPfAiming is set in flags. health and
    // armor (v4) only travel in WorldState entries (the server's values); a PlayerState leaves them at
    // their defaults. v5: vehId is 0 unless move is kMoveEnteringVehicle or kMoveExitingVehicle.
    struct QPed
    {
        uint8_t move = 0;
        uint8_t jumpSeq = 0;
        uint16_t flags = 0;
        uint32_t weapon = 0;
        int16_t ax = 0, ay = 0, az = 0; // aim offset, decimetres (kAimPerMeter), each in +-32767
        uint8_t shotSeq = 0;            // v4: 0..15
        uint8_t health = kDefaultHealth; // v4
        uint8_t armor = 0;               // v4
        uint8_t vehId = 0;               // v5: the vehicle of move 5 / 6

        bool operator==(const QPed& o) const
        {
            return move == o.move && jumpSeq == o.jumpSeq && flags == o.flags && weapon == o.weapon && ax == o.ax &&
                   ay == o.ay && az == o.az && shotSeq == o.shotSeq && health == o.health && armor == o.armor &&
                   vehId == o.vehId;
        }
        bool operator!=(const QPed& o) const { return !(*this == o); }
    };

    // One player's whole quantised sample: what the delta baselines and the server's idle detector
    // compare. A change of move, flags, jumpSeq, weapon or aim is a change like a move of 1 cm.
    struct QState
    {
        QKin k;
        QPed p;
        bool operator==(const QState& o) const { return k == o.k && p == o.p; }
        bool operator!=(const QState& o) const { return !(*this == o); }
    };

    constexpr int32_t kQMaxAim = 32767; // the i16 range minus -32768, so every wire value re-encodes to itself

    // True when the wire values are legal: a known move, no reserved flag, jumpSeq in 2 bits, aim
    // in +-kQMaxAim and zero unless aiming, and (v5) a vehicle id only with move 5 or 6 (what Decode checks).
    inline bool QPedInRange(const QPed& p)
    {
        if (p.move > kMoveMax || p.jumpSeq > 3 || p.shotSeq > 15 || (p.flags & ~kPfKnown) != 0) return false;
        if (p.vehId != 0 && !MoveNamesVehicle(p.move)) return false;
        if (p.ax < -kQMaxAim || p.ay < -kQMaxAim || p.az < -kQMaxAim) return false;
        if (!(p.flags & kPfAiming) && (p.ax | p.ay | p.az) != 0) return false;
        return true;
    }

    // PedState -> wire. move and flags are copied (Encode refuses a move above 7; Decode refuses
    // 7 and reserved flags), jumpSeq keeps its low 2 bits, the aim is 0 unless kPfAiming is set.
    // An aim offset whose largest axis exceeds kMaxAimMeters is scaled down along its direction;
    // then each axis rounds to the nearest decimetre (half up). A non-finite aim becomes 0 (callers
    // validate first; Encode(PlayerState) refuses it outright).
    inline QPed QuantizePed(const PedState& s)
    {
        QPed q;
        q.move = s.move;
        q.jumpSeq = (uint8_t)(s.jumpSeq & 3u);
        q.shotSeq = (uint8_t)(s.shotSeq & 15u);
        q.health = s.health;
        q.armor = s.armor;
        q.vehId = MoveNamesVehicle(s.move) ? s.vehId : (uint8_t)0;
        q.flags = s.flags;
        q.weapon = s.weapon;
        if ((s.flags & kPfAiming) && std::isfinite(s.aimX) && std::isfinite(s.aimY) && std::isfinite(s.aimZ))
        {
            double x = (double)s.aimX * kAimPerMeter, y = (double)s.aimY * kAimPerMeter, z = (double)s.aimZ * kAimPerMeter;
            const double m = std::fmax(std::fabs(x), std::fmax(std::fabs(y), std::fabs(z)));
            if (m > (double)kQMaxAim)
            {
                const double scale = (double)kQMaxAim / m;
                x *= scale;
                y *= scale;
                z *= scale;
            }
            q.ax = (int16_t)qdetail::RoundClamp(x, -kQMaxAim, kQMaxAim);
            q.ay = (int16_t)qdetail::RoundClamp(y, -kQMaxAim, kQMaxAim);
            q.az = (int16_t)qdetail::RoundClamp(z, -kQMaxAim, kQMaxAim);
        }
        return q;
    }

    // Wire -> PedState. The aim lands on whole decimetres (0 when not aiming).
    inline PedState DequantizePed(const QPed& q)
    {
        PedState s;
        s.move = q.move;
        s.jumpSeq = q.jumpSeq;
        s.shotSeq = q.shotSeq;
        s.health = q.health;
        s.armor = q.armor;
        s.vehId = q.vehId;
        s.flags = q.flags;
        s.weapon = q.weapon;
        s.aimX = (float)((double)q.ax / kAimPerMeter);
        s.aimY = (float)((double)q.ay / kAimPerMeter);
        s.aimZ = (float)((double)q.az / kAimPerMeter);
        return s;
    }

    inline QState QuantizeState(const Kinematics& k, const PedState& s) { return QState{Quantize(k), QuantizePed(s)}; }

    // ---- vehicles (v4): rotation, angular velocity, steering ----
    //
    // Rotation: "smallest three". Of the unit quaternion's four components the one with the largest
    // magnitude is dropped (made positive first: q and -q are the same rotation) and rebuilt from the
    // others as sqrt(1 - a^2 - b^2 - c^2). The other three each lie in [-1/sqrt2, +1/sqrt2] and travel as
    // 10 bits: q = round(c * sqrt2 * 511) + 511, so 0..1022 (1023 is refused; 0 is exact, the identity
    // round-trips exactly). u32 layout: bits 30..31 the dropped component's index (0 x, 1 y, 2 z, 3 w),
    // bits 20..29, 10..19 and 0..9 the remaining three in x, y, z, w order.
    // Precision: each small component is within half a step (0.00069) and the rebuilt one follows, so
    // the rotation error is at most 0.28 degrees (analytic worst case, all four components 0.5; the
    // encoder's best-of-four choice below can only lower it);
    // tests/quant_test.cpp measures the bound over a million random rotations.
    constexpr int32_t kQuatHalf = 511;          // steps per 1/sqrt2
    constexpr int32_t kQuatMaxCode = 1022;      // 1023 is never written and refused
    constexpr double  kMaxQuatErrorDeg = 0.28;  // degrees of rotation, worst case
    constexpr int32_t kAngVelPerRad = 16;       // angular velocity: 1/16 rad/s steps, i8
    constexpr int32_t kSteerSteps = 127;        // steering: -1..1 in 1/127 steps, i8

    // True when the three codes are legal: each 0..1022 and their squares (about the centre) sum to at
    // most 1, so the dropped component is real.
    inline bool QuatWireValid(uint32_t v)
    {
        int64_t sum = 0;
        for (int i = 0; i < 3; i++)
        {
            const int32_t c = (int32_t)((v >> (10 * i)) & 1023u);
            if (c > kQuatMaxCode) return false;
            sum += (int64_t)(c - kQuatHalf) * (c - kQuatHalf);
        }
        return sum <= 2LL * kQuatHalf * kQuatHalf;
    }

    // Wire -> unit quaternion (callers checked QuatWireValid; an invalid sum rebuilds the dropped
    // component as 0).
    inline Quat DequantizeQuat(uint32_t v)
    {
        const int big = (int)(v >> 30);
        double c[4] = {0, 0, 0, 0};
        double sum = 0;
        int slot = 2;
        for (int i = 0; i < 4; i++)
        {
            if (i == big) continue;
            const int32_t code = (int32_t)((v >> (10 * slot)) & 1023u);
            c[i] = (double)(code - kQuatHalf) / kQuatHalf * 0.70710678118654752;
            sum += c[i] * c[i];
            slot--;
        }
        c[big] = sum < 1.0 ? std::sqrt(1.0 - sum) : 0.0;
        Quat q;
        q.x = (float)c[0];
        q.y = (float)c[1];
        q.z = (float)c[2];
        q.w = (float)c[3];
        return q;
    }

    namespace qdetail {
        // The code of unit quaternion `c` with component `big` dropped (made non-negative first).
        inline uint32_t QuatCodeAt(const double c[4], int big)
        {
            const double sign = c[big] < 0 ? -1.0 : 1.0;
            uint32_t out = (uint32_t)big << 30;
            int slot = 2; // the first remaining component goes to bits 20..29
            for (int i = 0; i < 4; i++)
            {
                if (i == big) continue;
                const int32_t code = RoundClamp(c[i] * sign * 1.4142135623730951 * kQuatHalf, -kQuatHalf, kQuatHalf) + kQuatHalf;
                out |= (uint32_t)code << (10 * slot);
                slot--;
            }
            return out;
        }
    }

    // Any finite quaternion of non-zero length (normalised here); a zero-length or non-finite one
    // encodes as the identity (callers validate first: Encode refuses non-finite input outright).
    // All four choices of the dropped component are tried and the valid code that decodes closest to
    // the input wins (the largest component first, so it wins a tie; it is the classic choice and
    // almost always the best). Decoding a code and encoding it again therefore gives the same code
    // (that choice reproduces its own input exactly), so a relay (the server re-encoding a driver's
    // rotation for everybody else) adds no second rounding. Cost: 4 trial encodes, a few dozen flops.
    inline uint32_t QuantizeQuat(const Quat& in)
    {
        double c[4] = {(double)in.x, (double)in.y, (double)in.z, (double)in.w};
        double len2 = 0;
        for (double v : c) len2 += v * v;
        if (!std::isfinite(len2) || len2 < 1e-12) return (uint32_t)((3u << 30) | (511u << 20) | (511u << 10) | 511u);
        const double inv = 1.0 / std::sqrt(len2);
        int big = 0;
        for (int i = 0; i < 4; i++)
        {
            c[i] *= inv;
            if (std::fabs(c[i]) > std::fabs(c[big])) big = i;
        }
        uint32_t best = qdetail::QuatCodeAt(c, big);
        double bestErr = -1;
        for (int n = 0; n < 4; n++)
        {
            const int i = (big + n) & 3;
            const uint32_t code = qdetail::QuatCodeAt(c, i);
            if (!QuatWireValid(code)) continue;
            const Quat d = DequantizeQuat(code);
            const double dc[4] = {(double)d.x, (double)d.y, (double)d.z, (double)d.w};
            double plus = 0, minus = 0; // q and -q are the same rotation
            for (int k = 0; k < 4; k++)
            {
                plus += (dc[k] - c[k]) * (dc[k] - c[k]);
                minus += (dc[k] + c[k]) * (dc[k] + c[k]);
            }
            const double err = std::min(plus, minus);
            if (bestErr < 0 || err < bestErr)
            {
                bestErr = err;
                best = code;
            }
        }
        return best;
    }

    // The GTA heading (degrees, [0, 360), 0 = facing +Y, counter-clockwise) of a rotation: its yaw.
    inline float QuatHeading(const Quat& q)
    {
        const double yaw = std::atan2(2.0 * ((double)q.w * q.z + (double)q.x * q.y), 1.0 - 2.0 * ((double)q.y * q.y + (double)q.z * q.z));
        double deg = yaw * (180.0 / 3.14159265358979323846);
        if (deg < 0) deg += 360.0;
        if (deg >= 360.0) deg -= 360.0;
        return (float)deg;
    }

    // A pure yaw rotation for a GTA heading in degrees.
    inline Quat QuatFromHeading(float headingDeg)
    {
        const double h = (double)headingDeg * (3.14159265358979323846 / 180.0) * 0.5;
        Quat q;
        q.z = (float)std::sin(h);
        q.w = (float)std::cos(h);
        return q;
    }

    // Angular velocity and steering: a signed byte, -127..127 (-128 is never written and refused).
    inline int8_t QuantizeAngVel(float radPerSec)
    {
        return (int8_t)qdetail::RoundClamp((double)radPerSec * kAngVelPerRad, -127, 127);
    }
    inline float DequantizeAngVel(int8_t q) { return (float)((double)q / kAngVelPerRad); }
    inline int8_t QuantizeSteer(float steer)
    {
        return (int8_t)qdetail::RoundClamp((double)steer * kSteerSteps, -127, 127);
    }
    inline float DequantizeSteer(int8_t q) { return (float)((double)q / kSteerSteps); }

    // ---- the coarse channel: three u16 per player ----

    struct QCoarse
    {
        uint16_t x = 0, y = 0, z = 0;
        bool operator==(const QCoarse& o) const { return x == o.x && y == o.y && z == o.z; }
        bool operator!=(const QCoarse& o) const { return !(*this == o); }
    };

    constexpr double kCoarseXYStep = 0.5;          // m: 32768 m / 65536
    constexpr double kCoarseZStep  = 5.0 / 64.0;   // m: 5120 m / 65536
    constexpr double kMaxCoarseXYError = 0.25 + 1e-6;
    constexpr double kMaxCoarseZError  = 5.0 / 128.0 + 1e-6;

    inline QCoarse QuantizeCoarse(const Kinematics& k)
    {
        auto u16 = [](double v) {
            const double r = std::floor(v + 0.5);
            if (!(r > 0.0)) return (uint16_t)0;
            if (r > 65535.0) return (uint16_t)65535;
            return (uint16_t)r;
        };
        QCoarse c;
        c.x = u16(((double)k.x + kQuantMaxXY) / kCoarseXYStep);
        c.y = u16(((double)k.y + kQuantMaxXY) / kCoarseXYStep);
        c.z = u16(((double)k.z - kQuantMinZ) / kCoarseZStep);
        return c;
    }

    inline QCoarse QuantizeCoarse(const QKin& q)
    {
        return QuantizeCoarse(Dequantize(q));
    }

    // Floats at the centre of the cell; x and y in metres.
    inline void DequantizeCoarse(const QCoarse& c, float& x, float& y, float& z)
    {
        x = (float)((double)c.x * kCoarseXYStep - kQuantMaxXY);
        y = (float)((double)c.y * kCoarseXYStep - kQuantMaxXY);
        z = (float)((double)c.z * kCoarseZStep + kQuantMinZ);
    }

}
