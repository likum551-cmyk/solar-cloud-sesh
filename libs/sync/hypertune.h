#pragma once
// Hyper tune: the Solar RP "hyper" cars (GI 2026-11-02: the Jesko with giga acceleration and a 500 km/h top
// speed; GI 2026-11-04: the same for /car dubsta, plus handling that holds it together at that speed). Pure numbers
// and pure math, no game calls: the engine applies them to the vehicle the LOCAL player drives (the owner's client is
// the authority for its car; everyone else sees the result through the normal vehicle sync).
//
//   SET_VEHICLE_CHEAT_POWER_INCREASE(veh, power)         every frame (the game resets it each frame)
//   MODIFY_VEHICLE_TOP_SPEED(veh, HyperTopSpeedPercent)  once per vehicle, from GET_VEHICLE_ESTIMATED_MAX_SPEED
//   SET_VEHICLE_MAX_SPEED(veh, target)                   once per vehicle: the cap the game enforces
//
// The server's anti-cheat limits a VehicleUpdate to vehicleMaxSpeed (default 150 m/s = 540 km/h), which is above the
// target here. T4: an overshoot above the cap (a downhill, a jump, the game's enforcement lagging a frame) is no longer
// DROPPED, which froze the car for every other client for as long as it lasted: up to twice the cap the server clamps the
// replicated velocity to the cap and takes the position (VehicleStore::Rules::maxSpeedSoftFactor); above that it refuses.
// A hyper target above 540 km/h (150 m/s) needs "vehicleMaxSpeed" raised in server.json (config.cpp accepts 10..600 m/s);
// kHyperTargetKmh stays under the shipped default on purpose.
//
// PER-MODEL PROFILES (kHyperProfiles). The Jesko is the original and its row is exactly the numbers above with no handling
// extras (a custom model whose own handling is built for the speed). The Dubsta is a stock 4x4 SUV: tall, soft, and with
// brakes, tyres and a body made for 160 km/h. Its profile adds, with NATIVES AND PER-FRAME VELOCITY WRITES ONLY (nothing
// here touches CHandlingData: no per-build offsets are known):
//
//   toughness (natives, once per car and re-asserted every 2 s; health topped up every 250 ms)
//     SET_VEHICLE_STRONG(true)                  documented: no crash damage (bullets and explosions still hurt)
//     SET_VEHICLE_HAS_STRONG_AXLES(true)        documented: axles do not bend
//     SET_VEHICLE_TYRES_CAN_BURST(false)       documented: bulletproof tyres
//     SET_VEHICLE_WHEELS_CAN_BREAK(false)      (NativeDB gives no text; the name and the BOOL are the whole contract)
//     SET_VEHICLE_ENGINE_CAN_DEGRADE(false), SET_DISABLE_VEHICLE_PETROL_TANK_DAMAGE(true), and body / engine / tank health
//     written back to 1000 when they read lower
//   driving assists (HyperAssistStep below: one SET_ENTITY_VELOCITY and one SET_ENTITY_ANGULAR_VELOCITY per frame at most)
//     downforce        a pull into the road along the car's own down axis, k * v^2 (an acceleration, so it needs no mass)
//     lateral grip     the sideways part of the velocity is removed at a bounded rate: the car follows its nose
//     brake assist     extra deceleration along the direction of travel in proportion to INPUT_VEH_BRAKE
//     steering limit   the yaw rate is capped at a lateral acceleration / speed (what a grip limit would allow), so a full
//                      lock at 400 km/h cannot spin the car
//     anti-roll        the pitch / roll rate (the part of the angular velocity not about the car's up axis) is damped
//   Not done, on purpose: brake force, steering lock, suspension and traction curves are handling data; they have no
//   public native that writes them.
//
// WHY VELOCITY WRITES. APPLY_FORCE_TO_ENTITY has four force flags, a mass-relative switch and no stated unit; a wrong guess
// at 400 km/h throws the car. A velocity change over a known dt is an acceleration in m/s^2 whatever the mass: it can be
// bounded, tested offline and read back (the engine verifies every write against the next read). Every assist is capped so
// that one frame's horizontal change stays under the local ImpactTracker's 1.5 m/s jump threshold (libs/sync/vehicle_impact.h):
// grip + brake <= (22 + 12) / 30 s = 1.13 m/s. The frame length is clamped to kHyperMaxDt.
//
// The world frame is GTA's: metres, z up, a vehicle's forward is its body +y, right is +x, up is +z, and the quaternion
// rotates body -> world. The angular velocity is the WORLD rotation vector in rad/s (GET_ENTITY_ROTATION_VELOCITY; its yaw
// agreed with the rotation-derived yaw rate, ratio 1.00 over 263 samples, in a live 3889 log; pitch and roll are unverified
// like the setter's frame, which is why the engine reads the write back and drops the angular assists if it does not hold).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gage::sync {

    constexpr float kHyperTargetKmh = 500.0f;
    constexpr float kHyperTargetMs = kHyperTargetKmh / 3.6f;   // 138.9 m/s
    constexpr float kHyperPower = 4.0f;                         // cheat power multiplier (1 = stock)
    constexpr float kHyperMaxPercent = 400.0f;                  // never more than 5x the stock top speed

    // MODIFY_VEHICLE_TOP_SPEED takes a percentage added to the handling's top speed (100 = double). From the game's
    // own estimate of the stock top speed (m/s), the percentage that brings it to `targetMs`. An unknown or broken
    // estimate (<= 1 m/s) gets the safe middle value 100; a car already at or above the target gets 0.
    inline float HyperTopSpeedPercent(float estimatedMaxMs, float targetMs = kHyperTargetMs)
    {
        if (!(estimatedMaxMs > 1.0f)) return 100.0f;
        if (estimatedMaxMs >= targetMs) return 0.0f;
        const float pct = (targetMs / estimatedMaxMs - 1.0f) * 100.0f;
        return pct > kHyperMaxPercent ? kHyperMaxPercent : pct;
    }

    // ---- the driving assists -----------------------------------------------------------------------------------------

    // hyper_assist in HKCU\Software\GAGE-MP is a bit mask of these (decimal or 0x hex); unset, empty or unparsable = all.
    // 0 switches every assist off (the toughness natives stay; hyper_tune=0 switches the whole tune off).
    constexpr uint32_t kHyperAssistDownforce = 1u;
    constexpr uint32_t kHyperAssistGrip      = 2u;
    constexpr uint32_t kHyperAssistBrake     = 4u;
    constexpr uint32_t kHyperAssistYaw       = 8u;
    constexpr uint32_t kHyperAssistTilt      = 16u;
    constexpr uint32_t kHyperAssistAll       = 31u;
    constexpr uint32_t kHyperAssistAngular   = kHyperAssistYaw | kHyperAssistTilt;   // the ones that write the angular velocity

    inline uint32_t ParseHyperAssistMask(const char* text)
    {
        if (!text || !*text) return kHyperAssistAll;
        char* end = nullptr;
        const int base = (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) ? 16 : 10;   // not base 0: "010" is ten, not eight
        const unsigned long v = std::strtoul(text, &end, base);
        if (end == text || (end && *end)) return kHyperAssistAll;
        return (uint32_t)v & kHyperAssistAll;
    }

    constexpr float kHyperMaxDt = 1.0f / 30.0f;   // s: a longer frame is treated as this long (a hitch does not get a bigger shove)

    struct HyperVec { float x = 0, y = 0, z = 0; };

    // One car's assist numbers. All zero = no assists (the Jesko).
    struct HyperAssist
    {
        // downforce: acceleration k * v^2 along -up, k = downAtRefMs2 / downRefMs^2, never above downCapMs2
        float downRefMs = 0, downAtRefMs2 = 0, downCapMs2 = 0;
        // lateral grip: the sideways velocity is removed at gripRate * |v_side| (1/s), at most gripCapMs2 * ramp (m/s^2)
        float gripRate = 0, gripCapMs2 = 0;
        // brake assist: deceleration brakeMs2 * pedal * ramp (m/s^2), only while moving forward faster than brakeMinFwdMs
        float brakeMs2 = 0, brakeMinFwdMs = 5.0f;
        // steering limit: |yaw rate| <= yawLatMs2 / speed (rad/s), from yawFromMs
        float yawLatMs2 = 0, yawFromMs = 20.0f;
        // anti-roll: the non-yaw angular velocity decays as exp(-tiltRate * ramp * dt)
        float tiltRate = 0;
        // ramp: 0 at rampFromMs, 1 at rampToMs (m/s), linear: the grip, brake and anti-roll assists fade in with speed
        float rampFromMs = 10.0f, rampToMs = 50.0f;

        bool Any() const { return downAtRefMs2 > 0 || gripCapMs2 > 0 || brakeMs2 > 0 || yawLatMs2 > 0 || tiltRate > 0; }
    };

    struct HyperAssistIn
    {
        HyperVec vel;                   // world, m/s
        HyperVec fwd, right, up;        // the car's body axes in the world (unit vectors, from its quaternion)
        HyperVec angVel;                // world, rad/s
        float brake = 0, accel = 0;     // INPUT_VEH_BRAKE / INPUT_VEH_ACCELERATE, 0..1
        float dt = 0;                   // s since the previous frame
        bool grounded = false;          // at least a wheel on the ground
        uint32_t mask = kHyperAssistAll;
    };

    struct HyperAssistOut
    {
        HyperVec vel, angVel;           // what to write (equal to the input when unchanged)
        bool velChanged = false, angChanged = false, yawClamped = false;
        float speed = 0;                // |vel| of the input, m/s
        float downMs2 = 0, gripMs2 = 0, brakeMs2 = 0;   // the accelerations applied this frame, for the statistics
    };

    namespace hyperdetail {
        inline double Dot(const HyperVec& a, const HyperVec& b) { return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z; }
        inline bool Finite(const HyperVec& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
        inline bool UnitLike(const HyperVec& a) { const double l = std::sqrt(Dot(a, a)); return l > 0.9 && l < 1.1; }
        inline double Clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
    }

    // One frame of the assists. Pure: no state, never NaN (a non-finite or degenerate input returns the input unchanged),
    // frame-rate independent (every term scales with dt).
    inline HyperAssistOut HyperAssistStep(const HyperAssist& a, const HyperAssistIn& in)
    {
        using namespace hyperdetail;
        HyperAssistOut o;
        o.vel = in.vel;
        o.angVel = in.angVel;
        if (!a.Any() || in.mask == 0) return o;
        if (!std::isfinite(in.dt) || !(in.dt > 0.0f)) return o;
        if (!Finite(in.vel) || !Finite(in.fwd) || !Finite(in.right) || !Finite(in.up)) return o;
        if (!UnitLike(in.fwd) || !UnitLike(in.right) || !UnitLike(in.up)) return o;
        if (!std::isfinite(in.brake) || !std::isfinite(in.accel)) return o;
        const double dt = in.dt > kHyperMaxDt ? (double)kHyperMaxDt : (double)in.dt;
        const double v = std::sqrt(Dot(in.vel, in.vel));
        o.speed = (float)v;
        if (!(v >= 1.0)) return o;

        double vel[3] = { in.vel.x, in.vel.y, in.vel.z };
        const double up[3] = { in.up.x, in.up.y, in.up.z };
        const double rt[3] = { in.right.x, in.right.y, in.right.z };
        const double upz = in.up.z;
        const double span = (double)a.rampToMs - (double)a.rampFromMs;
        const double ramp = span > 0.0 ? Clamp01((v - a.rampFromMs) / span) : (v >= a.rampFromMs ? 1.0 : 0.0);

        // 1. downforce: k v^2 along the car's own down axis, only while a wheel is on the ground and the car is roughly upright
        //    (an inverted car must not be pushed away from the road)
        if ((in.mask & kHyperAssistDownforce) && in.grounded && upz > 0.5 && a.downAtRefMs2 > 0 && a.downRefMs > 0)
        {
            const double q = v / a.downRefMs;
            double ad = (double)a.downAtRefMs2 * q * q;
            if (ad > a.downCapMs2) ad = a.downCapMs2;
            for (int i = 0; i < 3; ++i) vel[i] -= up[i] * ad * dt;
            o.downMs2 = (float)ad;
        }

        // 2. lateral grip: the speed along the car's right axis is removed at gripRate (1/s), capped
        if ((in.mask & kHyperAssistGrip) && in.grounded && upz > 0.7 && ramp > 0.0 && a.gripCapMs2 > 0 && a.gripRate > 0)
        {
            const double side = vel[0] * rt[0] + vel[1] * rt[1] + vel[2] * rt[2];
            double acc = (double)a.gripRate * std::fabs(side);
            const double cap = (double)a.gripCapMs2 * ramp;
            if (acc > cap) acc = cap;
            double dv = acc * dt;
            if (dv > std::fabs(side)) dv = std::fabs(side);       // never past zero: no overshoot into the other side
            const double sgn = side >= 0.0 ? 1.0 : -1.0;
            for (int i = 0; i < 3; ++i) vel[i] -= sgn * dv * rt[i];
            o.gripMs2 = (float)(dv / dt);
        }

        // 3. brake assist: the horizontal velocity shrinks along itself while the pedal is down and the car moves forward
        if ((in.mask & kHyperAssistBrake) && in.grounded && ramp > 0.0 && a.brakeMs2 > 0 && in.brake > 0.05f && in.accel < 0.5f)
        {
            const double fwd[3] = { in.fwd.x, in.fwd.y, in.fwd.z };
            const double vf = vel[0] * fwd[0] + vel[1] * fwd[1] + vel[2] * fwd[2];
            const double vh = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1]);
            if (vf > a.brakeMinFwdMs && vh > 0.5)
            {
                const double pedal = Clamp01(in.brake);
                double dv = (double)a.brakeMs2 * pedal * ramp * dt;
                if (dv > vh) dv = vh;                              // never reverses the car
                vel[0] -= dv * vel[0] / vh;
                vel[1] -= dv * vel[1] / vh;
                o.brakeMs2 = (float)(dv / dt);
            }
        }

        // 4. angular: anti-roll damping of everything that is not yaw, then the yaw-rate cap
        const bool angOk = Finite(in.angVel);               // a failed angular read leaves the velocity assists working
        double w[3] = { angOk ? in.angVel.x : 0.0, angOk ? in.angVel.y : 0.0, angOk ? in.angVel.z : 0.0 };
        if (angOk && (in.mask & kHyperAssistAngular))
        {
            if ((in.mask & kHyperAssistTilt) && upz > 0.2 && ramp > 0.0 && a.tiltRate > 0)
            {
                const double wUp = w[0] * up[0] + w[1] * up[1] + w[2] * up[2];
                const double f = std::exp(-(double)a.tiltRate * ramp * dt);
                for (int i = 0; i < 3; ++i) w[i] = wUp * up[i] + f * (w[i] - wUp * up[i]);
            }
            if ((in.mask & kHyperAssistYaw) && in.grounded && upz > 0.7 && v >= a.yawFromMs && a.yawLatMs2 > 0)
            {
                const double lim = (double)a.yawLatMs2 / v;
                const double wUp = w[0] * up[0] + w[1] * up[1] + w[2] * up[2];
                if (std::fabs(wUp) > lim)
                {
                    const double excess = wUp - (wUp > 0.0 ? lim : -lim);
                    for (int i = 0; i < 3; ++i) w[i] -= excess * up[i];
                    o.yawClamped = true;
                }
            }
        }

        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(vel[i]) || !std::isfinite(w[i]))
            {
                HyperAssistOut bad;                            // never emit NaN: the input, untouched
                bad.vel = in.vel;
                bad.angVel = in.angVel;
                bad.speed = o.speed;
                return bad;
            }
        o.vel = { (float)vel[0], (float)vel[1], (float)vel[2] };
        if (angOk) o.angVel = { (float)w[0], (float)w[1], (float)w[2] };
        o.velChanged = o.vel.x != in.vel.x || o.vel.y != in.vel.y || o.vel.z != in.vel.z;
        o.angChanged = angOk && (o.angVel.x != in.angVel.x || o.angVel.y != in.angVel.y || o.angVel.z != in.angVel.z);
        return o;
    }

    // ---- the profiles ------------------------------------------------------------------------------------------------

    struct HyperProfile
    {
        const char* model;      // the /car name (lower case: the engine hashes it with GET_HASH_KEY, which ignores case)
        float targetKmh;        // top speed the game is made to allow; must stay under the server's vehicleMaxSpeed (540 km/h)
        float power;            // SET_VEHICLE_CHEAT_POWER_INCREASE, every frame
        bool tough;             // the toughness natives and the health top-up
        HyperAssist assist;     // the driving assists (all zero = none)
    };

    // The Dubsta's numbers, from first principles; none of them has been felt in a game yet (see the engine's log lines):
    //   downforce   10 m/s^2 at 500 km/h (about 1 g of extra weight, 5.2 m/s^2 at 360 km/h, 1.9 at 216), cap 13: a soft SUV's
    //               suspension has to carry it, so it is a hypercar's lower end, not an F1 car's
    //   grip        22 m/s^2 (2.2 g) at most, rate 5 /s: a slide of 8 m/s (5 degrees at 100 m/s) is gone in 0.4 s
    //   brakes      +12 m/s^2 on top of the stock ~9: 2 g at a hypercar's level, from 252 km/h up, fading out below
    //   steering    the same 22 m/s^2 as a yaw cap: 0.16 rad/s (9 deg/s) at 500 km/h, 0.37 rad/s at 216 km/h, 0.73 rad/s at
    //               108 km/h (more than any turn of a real road), so only a spin is cut
    //   anti-roll   3 /s: a 4 rad/s roll rate is below 1 rad/s in half a second
    constexpr HyperAssist MakeDubstaAssist()
    {
        HyperAssist a;
        a.downRefMs = kHyperTargetMs;
        a.downAtRefMs2 = 10.0f;
        a.downCapMs2 = 13.0f;
        a.gripRate = 5.0f;
        a.gripCapMs2 = 22.0f;
        a.brakeMs2 = 12.0f;
        a.yawLatMs2 = 22.0f;
        a.tiltRate = 3.0f;
        return a;
    }

    // The Jesko row is exactly what it was before profiles existed (target, power, no extras): tests pin it.
    inline constexpr HyperProfile kHyperProfiles[] = {
        { "jesko",  kHyperTargetKmh, kHyperPower, false, HyperAssist() },
        { "dubsta", kHyperTargetKmh, kHyperPower, true,  MakeDubstaAssist() },
    };
    inline constexpr size_t kHyperProfileCount = sizeof(kHyperProfiles) / sizeof(kHyperProfiles[0]);

    constexpr float HyperTargetMs(const HyperProfile& p) { return p.targetKmh / 3.6f; }

    inline const HyperProfile* HyperProfileFor(const char* model)
    {
        if (!model) return nullptr;
        for (size_t i = 0; i < kHyperProfileCount; ++i)
            if (std::strcmp(kHyperProfiles[i].model, model) == 0) return &kHyperProfiles[i];
        return nullptr;
    }

    // Body, engine and petrol tank health run to 1000. True when a read says the part is below it (a failed read, NaN,
    // is not low: nothing is written on no information).
    constexpr float kHyperHealthFull = 1000.0f;
    inline bool HyperHealthLow(float health) { return std::isfinite(health) && health < kHyperHealthFull - 0.5f; }

} // namespace gage::sync
