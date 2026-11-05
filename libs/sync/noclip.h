// 0.4.22 noclip (GI: "add noclip mode in quick actions", Tab menu). Pure C++17, single-threaded (the engine's script tick),
// tested by tests/noclip_tests.h (gameplay_test).
//
// What the engine does (apps/engine/engine.cpp "0.4.22 noclip"), while the registry switch noclip is on (unset / 1):
//   * ON (the Tab menu row, or /noclip): the subject is the vehicle the local ped DRIVES, else the ped. The flags noclip
//     changes are read first (Flags: collision via GET_ENTITY_COLLISION_DISABLED, invincibility via GET_PLAYER_INVINCIBLE;
//     gravity has no getter in either crossmap and the engine never turns it off on a local entity, so it reads true), then
//     set: SET_ENTITY_COLLISION off, SET_ENTITY_HAS_GRAVITY off, SET_PLAYER_INVINCIBLE on (and SET_ENTITY_INVINCIBLE on a
//     vehicle subject).
//   * every tick while ON: the keys below move the subject along the gameplay camera's direction (GET_GAMEPLAY_CAM_ROT):
//     SET_ENTITY_COORDS_NO_OFFSET to Out.pos (our own tracked position, so the physics step between two ticks cannot
//     drift it), SET_ENTITY_VELOCITY to Out.vel (what the sender reads and the other players extrapolate with),
//     SET_ENTITY_HEADING to the camera's yaw; the game controls in kDisabledControls are held disabled.
//   * OFF: the subject is put on the ground under it (PlanLanding, GET_GROUND_Z_FOR_3D_COORD), THEN collision, gravity and
//     invincibility are given back exactly as read, so the player never falls to death. A death / swapped ped / vanished
//     vehicle ends it at once with the flags given back and no placement (there is nothing to land).
//
// Keys (documented in the Tab menu): W / S forward / back along the view, A / D strafe, Space up, Ctrl down, Shift fast
// (x5), Alt slow (x0.2). Shift + Alt = the base speed.
#pragma once

#include "protocol/protocol.h"

#include <cmath>
#include <cstdint>

namespace gage::sync::noclip {

    // ---- tuning ---------------------------------------------------------------------------------------------------
    constexpr float  kBaseSpeedMps = 12.0f;      // 43 km/h
    constexpr float  kFastMul = 5.0f;            // Shift: 60 m/s
    constexpr float  kSlowMul = 0.2f;            // Alt: 2.4 m/s
    constexpr float  kMaxDtSec = 0.1f;           // a hitch longer than this moves one 0.1 s step, not the whole gap
    constexpr float  kBoxMarginM = 100.0f;       // stay this far inside the protocol's world box: the server drops a state outside it
    constexpr float  kResyncM = 3.0f;            // the game's position is this far (plus 0.2 s of travel) from ours: something else moved it
    constexpr float  kOriginAboveGroundM = 1.0f; // a ped's (and a car's) origin sits about 1 m above the ground under it
    constexpr float  kHighProbeM = 1000.0f;      // the second ground probe starts this far above the subject (over Chiliad's 800 m)
    constexpr float  kAboveSlackM = 0.5f;        // a ground reading this far above the subject still counts as "under it"
    constexpr float  kSeaBandM = 50.0f;          // a ground height of exactly 0 is the sea only within this band of 0
    constexpr double kLandWaitMs = 1500.0;       // no ground this long (collision not streamed in): back to the last safe spot
    constexpr double kProbeEveryMs = 500.0;      // the engine's periodic ground probe while flying (feeds the safe spot)

    // The controls held disabled every tick while noclip is on (DISABLE_CONTROL_ACTION(0, c, true)). Ids from the
    // eControlAction list (the radio ones are the 0.4.19 gate's, libs/sync/radio_off.h). Nothing here is an arrow key,
    // H (headlight), B or Y (the vehicle indicator / key work reads those).
    constexpr int kDisabledControls[] = {
        19,                       // CHARACTER_WHEEL (Alt)
        21, 22, 23,               // SPRINT (Shift), JUMP (Space), ENTER (F)
        24, 25,                   // ATTACK, AIM
        27,                       // PHONE
        30, 31, 32, 33, 34, 35,   // MOVE_LR, MOVE_UD, MOVE_UP_ONLY, MOVE_DOWN_ONLY, MOVE_LEFT_ONLY, MOVE_RIGHT_ONLY (W A S D)
        36,                       // DUCK (Ctrl)
        44, 45,                   // COVER (Q), RELOAD
        59, 60, 63, 64,           // VEH_MOVE_LR, VEH_MOVE_UD, VEH_MOVE_LEFT_ONLY, VEH_MOVE_RIGHT_ONLY
        69, 70,                   // VEH_ATTACK, VEH_ATTACK2
        71, 72,                   // VEH_ACCELERATE, VEH_BRAKE
        75, 76,                   // VEH_EXIT, VEH_HANDBRAKE
        81, 82, 83, 84, 85,       // the radio controls incl. the wheel (VEH_RADIO_WHEEL)
        91, 92,                   // VEH_PASSENGER_AIM, VEH_PASSENGER_ATTACK
        140, 141, 142, 143,       // MELEE_ATTACK_LIGHT / HEAVY / ALTERNATE, MELEE_BLOCK
        257,                      // ATTACK2
        263, 264,                 // MELEE_ATTACK1, MELEE_ATTACK2
    };
    constexpr int kDisabledControlCount = (int)(sizeof(kDisabledControls) / sizeof(kDisabledControls[0]));

    // ---- movement -------------------------------------------------------------------------------------------------
    struct Keys
    {
        bool fwd = false, back = false, left = false, right = false, up = false, down = false, fast = false, slow = false;
        bool Any() const { return fwd || back || left || right || up || down; }
    };

    struct Vec3
    {
        float x = 0, y = 0, z = 0;
    };

    inline float Speed(const Keys& k)
    {
        float s = kBaseSpeedMps;
        if (k.fast) s *= kFastMul;
        if (k.slow) s *= kSlowMul;
        return s;
    }

    // World-axis velocity (m/s) for the held keys and the gameplay camera's rotation in degrees (GET_GAMEPLAY_CAM_ROT: yaw
    // is the heading, 0 = +Y and counter-clockwise, so forward on the ground is (-sin yaw, cos yaw); pitch positive looks up).
    // W / S follow the full view direction, A / D are horizontal, Space / Ctrl are world up / down. The sum is normalised, so
    // a diagonal is no faster, and opposite keys cancel.
    inline Vec3 Velocity(const Keys& k, float yawDeg, float pitchDeg)
    {
        const float rad = 3.14159265358979f / 180.0f;
        const float sy = std::sin(yawDeg * rad), cy = std::cos(yawDeg * rad);
        const float sp = std::sin(pitchDeg * rad), cp = std::cos(pitchDeg * rad);
        const float f = (k.fwd ? 1.0f : 0.0f) - (k.back ? 1.0f : 0.0f);
        const float s = (k.right ? 1.0f : 0.0f) - (k.left ? 1.0f : 0.0f);
        const float v = (k.up ? 1.0f : 0.0f) - (k.down ? 1.0f : 0.0f);
        Vec3 d;
        d.x = f * (-sy * cp) + s * cy;
        d.y = f * (cy * cp) + s * sy;
        d.z = f * sp + v;
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < 1e-4f)
            return Vec3();
        const float m = Speed(k) / len;
        d.x *= m; d.y *= m; d.z *= m;
        return d;
    }

    inline float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    struct Stepped
    {
        Vec3 pos;
        Vec3 vel;                 // the velocity actually travelled: an axis held at the box edge reads 0
        bool clamped = false;
    };

    // pos + vel * dt (dt clamped to 0..kMaxDtSec), kept inside the protocol's world box minus kBoxMarginM.
    inline Stepped Advance(const Vec3& pos, const Vec3& vel, float dtSec)
    {
        const float dt = ClampF(dtSec, 0.0f, kMaxDtSec);
        Stepped s;
        s.pos.x = pos.x + vel.x * dt;
        s.pos.y = pos.y + vel.y * dt;
        s.pos.z = pos.z + vel.z * dt;
        s.vel = vel;
        const float xyLo = proto::kWorldMinXY + kBoxMarginM, xyHi = proto::kWorldMaxXY - kBoxMarginM;
        const float zLo = proto::kWorldMinZ + kBoxMarginM, zHi = proto::kWorldMaxZ - kBoxMarginM;
        const float cx = ClampF(s.pos.x, xyLo, xyHi), cy = ClampF(s.pos.y, xyLo, xyHi), cz = ClampF(s.pos.z, zLo, zHi);
        if (cx != s.pos.x) { s.pos.x = cx; s.vel.x = 0; s.clamped = true; }
        if (cy != s.pos.y) { s.pos.y = cy; s.vel.y = 0; s.clamped = true; }
        if (cz != s.pos.z) { s.pos.z = cz; s.vel.z = 0; s.clamped = true; }
        return s;
    }

    // GTA heading for a camera yaw: 0..360.
    inline float NormHeading(float deg)
    {
        float h = std::fmod(deg, 360.0f);
        if (h < 0.0f) h += 360.0f;
        return h;
    }

    // ---- the flags noclip changes and gives back ------------------------------------------------------------------
    struct Flags
    {
        bool collision = true;    // the subject collides (GET_ENTITY_COLLISION_DISABLED false)
        bool gravity = true;      // no getter exists: true, the engine never turns it off on a local entity
        bool invincible = false;  // GET_PLAYER_INVINCIBLE on the local player
    };

    // ---- the ground under the subject, when noclip ends -----------------------------------------------------------
    struct GroundRead
    {
        bool found = false;       // GET_GROUND_Z_FOR_3D_COORD returned true
        float z = 0;
    };
    // The native writes 0 for a start point below the ground (alloc8or), so an exact 0 is not a reading of solid ground
    // (the sea is also 0): GroundValid is "found and not 0".
    inline bool GroundValid(const GroundRead& g) { return g.found && std::fabs(g.z) > 1e-3f; }

    struct SafeSpot
    {
        bool have = false;
        float x = 0, y = 0, z = 0;   // an origin height (ground + kOriginAboveGroundM)
    };

    enum class LandKind : uint8_t { Skip, Place, Wait, Fallback };
    struct Landing
    {
        LandKind kind = LandKind::Skip;
        float x = 0, y = 0, z = 0;   // where the subject's origin goes (Place / Fallback)
    };

    // Where noclip's end puts the subject. (x, y, z) is where it hovers; below = the ground probe from z, above = the probe
    // from kHighProbeM higher; waitedMs = how long the landing has been waiting.
    //   1. ground under it (valid, not above z + kAboveSlackM): Place there, x and y unchanged;
    //   2. else the high probe's ground (the subject is inside the terrain, or the first probe failed): Place there;
    //   3. else both probes read exactly 0 and z is within kSeaBandM of 0: the sea, Place at 0;
    //   4. else nothing usable (collision not streamed in yet): Wait, until kLandWaitMs, then Fallback to the safe spot (the
    //      last place a probe verified, or where noclip began). With no safe spot at all it stays where it is (Place).
    inline Landing PlanLanding(float x, float y, float z, const GroundRead& below, const GroundRead& above, double waitedMs,
                               const SafeSpot& safe)
    {
        Landing l;
        l.x = x; l.y = y;
        if (GroundValid(below) && below.z <= z + kAboveSlackM)
        {
            l.kind = LandKind::Place;
            l.z = below.z + kOriginAboveGroundM;
            return l;
        }
        if (GroundValid(above))
        {
            l.kind = LandKind::Place;
            l.z = above.z + kOriginAboveGroundM;
            return l;
        }
        if ((below.found || above.found) && std::fabs(z) <= kSeaBandM && !GroundValid(below) && !GroundValid(above))
        {
            l.kind = LandKind::Place;
            l.z = kOriginAboveGroundM;
            return l;
        }
        if (waitedMs < kLandWaitMs)
        {
            l.kind = LandKind::Wait;
            l.z = z;
            return l;
        }
        if (safe.have)
        {
            l.kind = LandKind::Fallback;
            l.x = safe.x; l.y = safe.y; l.z = safe.z;
            return l;
        }
        l.kind = LandKind::Place;
        l.z = z;
        return l;
    }

    // ---- the state machine ----------------------------------------------------------------------------------------
    enum class Phase : uint8_t { Off, On, Landing };
    enum class Subject : uint8_t { None, Ped, Vehicle };
    enum class Refusal : uint8_t { None, Disabled, NotReady, Passenger };
    enum class EndWhy : uint8_t { None, Toggled, SwitchedOff, NotReady, SubjectGone };

    struct In
    {
        double  nowMs = 0;
        bool    enabled = true;       // the noclip switch (HKCU\Software\GAGE-MP, unset / 1 = on)
        bool    toggle = false;       // a request this tick (the Tab menu row, /noclip)
        bool    ready = false;        // alive, entered, no respawn / switch in progress
        int32_t entity = 0;           // the subject now: the vehicle the ped drives, else the ped (0 = none)
        Subject subject = Subject::None;
        bool    passenger = false;    // seated but not the driver
        Flags   read;                 // the flags as read this tick (used only when starting)
        Vec3    pos;                  // the subject's position as read this tick
        Keys    keys;
        float   yawDeg = 0, pitchDeg = 0;   // GET_GAMEPLAY_CAM_ROT
        bool    probed = false;       // the engine filled below / above this tick
        GroundRead below, above;
    };

    struct Out
    {
        Refusal refused = Refusal::None;
        bool    begin = false;        // apply the noclip flags now (the gate saved In::read)
        bool    move = false;         // set the subject to pos / vel / heading
        Vec3    pos, vel;
        float   heading = 0;
        bool    resynced = false;     // the game's position was adopted (something else moved the subject)
        bool    hold = false;         // landing, waiting for ground: keep the subject still at pos and request collision there
        bool    end = false;          // restore the flags (after placing the subject)
        EndWhy  why = EndWhy::None;
        Flags   restore;
        int32_t entity = 0;           // begin / end: the entity the flags belong to
        Subject subject = Subject::None;
        Landing land;
    };

    class Gate
    {
    public:
        Out Tick(const In& in)
        {
            Out out;
            if (phase_ == Phase::Off)
            {
                if (!in.toggle)
                    return out;
                if (!in.enabled) { out.refused = Refusal::Disabled; return out; }
                if (!in.ready || in.subject == Subject::None || in.entity == 0) { out.refused = Refusal::NotReady; return out; }
                if (in.passenger) { out.refused = Refusal::Passenger; return out; }
                saved_ = in.read;
                entity_ = in.entity;
                subject_ = in.subject;
                pos_ = in.pos;
                safe_.have = true; safe_.x = in.pos.x; safe_.y = in.pos.y; safe_.z = in.pos.z;
                lastMs_ = in.nowMs;
                lastSpeed_ = 0;
                phase_ = Phase::On;
                ++starts_;
                out.begin = true;
                out.entity = entity_;
                out.subject = subject_;
                return out;
            }

            // On or Landing: the reasons it ends at once (nothing to land: the subject is gone or the player is not alive).
            EndWhy why = EndWhy::None;
            if (!in.ready) why = EndWhy::NotReady;
            else if (in.entity != entity_) why = EndWhy::SubjectGone;
            if (why != EndWhy::None)
                return Finish(why, Landing());

            if (phase_ == Phase::On)
            {
                if (in.toggle) why = EndWhy::Toggled;
                else if (!in.enabled) why = EndWhy::SwitchedOff;
                if (why == EndWhy::None)
                    return Move(in);
                phase_ = Phase::Landing;
                landWhy_ = why;
                landStartMs_ = in.nowMs;
            }
            else if (in.toggle)
                ++ignoredToggles_;                 // already landing

            const GroundRead none;
            const Landing l = PlanLanding(in.pos.x, in.pos.y, in.pos.z, in.probed ? in.below : none, in.probed ? in.above : none,
                                          in.nowMs - landStartMs_, safe_);
            if (l.kind == LandKind::Wait)
            {
                out.hold = true;
                out.pos = in.pos;
                out.entity = entity_;
                out.subject = subject_;
                return out;
            }
            return Finish(landWhy_, l);
        }

        Phase   GetPhase() const { return phase_; }
        bool    Active() const { return phase_ != Phase::Off; }
        int32_t Entity() const { return entity_; }
        Subject GetSubject() const { return subject_; }
        const Flags& Saved() const { return saved_; }
        const SafeSpot& Safe() const { return safe_; }
        uint32_t Starts() const { return starts_; }
        uint32_t Ends() const { return ends_; }
        uint32_t Resyncs() const { return resyncs_; }
        uint32_t IgnoredToggles() const { return ignoredToggles_; }

    private:
        Out Move(const In& in)
        {
            Out out;
            const double gapMs = in.nowMs - lastMs_;
            lastMs_ = in.nowMs;
            const float dt = ClampF((float)(gapMs / 1000.0), 0.0f, kMaxDtSec);
            const float dx = in.pos.x - pos_.x, dy = in.pos.y - pos_.y, dz = in.pos.z - pos_.z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > kResyncM + 0.2f * lastSpeed_)
            {
                pos_ = in.pos;                      // something else moved the subject (a respawn, a server correction): follow it
                out.resynced = true;
                ++resyncs_;
            }
            const Vec3 v = Velocity(in.keys, in.yawDeg, in.pitchDeg);
            const Stepped s = Advance(pos_, v, dt);
            pos_ = s.pos;
            lastSpeed_ = std::sqrt(s.vel.x * s.vel.x + s.vel.y * s.vel.y + s.vel.z * s.vel.z);
            if (in.probed && GroundValid(in.below) && in.below.z <= in.pos.z + kAboveSlackM)
            {
                safe_.have = true; safe_.x = in.pos.x; safe_.y = in.pos.y; safe_.z = in.below.z + kOriginAboveGroundM;
            }
            out.move = true;
            out.pos = s.pos;
            out.vel = s.vel;
            out.heading = NormHeading(in.yawDeg);
            out.entity = entity_;
            out.subject = subject_;
            return out;
        }

        Out Finish(EndWhy why, const Landing& land)
        {
            Out out;
            out.end = true;
            out.why = why;
            out.restore = saved_;
            out.entity = entity_;
            out.subject = subject_;
            out.land = land;
            phase_ = Phase::Off;
            ++ends_;
            return out;
        }

        Phase    phase_ = Phase::Off;
        Flags    saved_;
        int32_t  entity_ = 0;
        Subject  subject_ = Subject::None;
        Vec3     pos_;
        SafeSpot safe_;
        double   lastMs_ = 0, landStartMs_ = 0;
        float    lastSpeed_ = 0;
        EndWhy   landWhy_ = EndWhy::None;
        uint32_t starts_ = 0, ends_ = 0, resyncs_ = 0, ignoredToggles_ = 0;
    };

} // namespace gage::sync::noclip
