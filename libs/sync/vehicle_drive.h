// Remote-vehicle drive: makes the local copy of a vehicle whose driver is another
// player follow that player's snapshots, once per game frame. Two parts:
//
//   VehicleInterpolator  the vehicle's own snapshot buffer (position, velocity,
//                        rotation quaternion, angular velocity, steering): Hermite
//                        position + slerp rotation between snapshots, extrapolation
//                        along the angular velocity past the newest one.
//   VehicleDriveController  the per-frame commands that make the GTA vehicle follow
//                        that sample through its own physics (velocity + angular
//                        velocity steering), snapping only when it cannot.
//
//   ...net poll:     interp.Push(snapshot);                       // any order; late ones dropped
//   ...render frame: VehicleSample s; if (interp.Sample(renderT, s)) {
//                      VehicleDriveInput in; in.target = s;      // + local GET_ENTITY_COORDS /
//                      in.localPos/localRot/localVel = ...;      //   _QUATERNION / _VELOCITY, dtMs
//                      const VehicleDriveOutput o = drive.Update(in);
//                      if (o.snap)          SET_ENTITY_COORDS_NO_OFFSET(o.snapPos) + SET_ENTITY_QUATERNION(o.snapRot)
//                      if (o.setQuaternion) SET_ENTITY_QUATERNION(o.rot)
//                      if (o.setVelocity)   SET_ENTITY_VELOCITY(o.velocity)
//                      if (o.setAngularVelocity) SET_ENTITY_ANGULAR_VELOCITY(o.angularVelocity)  (frame: see RISK)
//                      if (o.setSteering)   SET_VEHICLE_STEER_BIAS / CustomSteeringAngle(o.steering)
//                    }
//
// renderT is a time on the snapshots' own timeline: the connection's shared render clock
// (NetClock::RenderT(now) minus the car's class extra, libs/sync/netclock.h), the very clock the
// RemoteSync peds read, so a car shows the same instant as the player beside it.
// Pure C++17, no OS or game headers, no allocation; single-threaded; one interpolator
// and one controller per remote vehicle. Every rule is time-based (dtMs), so 30, 60
// and 144 fps behave alike. Never outputs NaN: non-finite input is refused (Invalid /
// an empty output) and every quaternion is normalised.
//
// CONVENTIONS. GTA world: metres, z up, a vehicle's forward is its body +y. A
// quaternion (x, y, z, w) rotates body -> world (GET_ENTITY_QUATERNION order). Heading
// in degrees: 0 = +y, counter-clockwise (GET_ENTITY_HEADING). Angular velocity is a
// WORLD-frame rotation vector in rad/s unless named "Body". Steering is whatever unit
// the sender sends (radians in GTA's CustomSteeringAngle); it is interpolated, never
// scaled.
//
// WHY ANGULAR VELOCITY, NOT A PER-FRAME QUATERNION
//   GTA Network (Client/Sync/Entity/Vehicle.cs, platform SyncPed.cs VMultiVehiclePos)
//   writes MainVehicle.Quaternion EVERY frame (slerp last -> newest by elapsed /
//   average latency, alpha up to 1.5). That is a teleport of the orientation each
//   frame: the physics solver never owns the body's rotation, so suspension, tyre
//   contact and collisions are reset every frame, and at speed the body visibly
//   fights it (wheels skip, the chassis jitters against kerbs). RAGECOOP-V
//   (Sync/Entities/Vehicle/SyncedVehicle.cs DisplayVehicle), the later design, steers
//   the rotation through RotationVelocity = remote angular velocity + 0.2 * error
//   (degrees), and writes the quaternion (slerp 0.5 toward the target) only when the
//   error passes 50 degrees or the car is upside down. Physics then stays the owner of
//   the body between frames, and the orientation converges with a time constant. This
//   module does the RAGECOOP thing, framerate independent: angular velocity steering
//   normally, a per-frame quaternion blend only for large errors / a flipped car, a
//   hard SET_ENTITY_QUATERNION only on a snap.
//
// HOW IT WORKS: VehicleInterpolator (constants are VehicleInterpConfig defaults)
//  1. Push. Snapshots carry their own time tMs (the server timeline: tick * tickMs -
//     ageMs, as RemoteSync places them). One not newer than the newest stored is
//     Stale (duplicate / reordered); non-finite or a zero quaternion is Invalid. A
//     snapshot with a later stamp but bit-identical motion is a Repeat (not stored). A
//     snapshot more than maxBridgeMs (500, the smoother's) after the newest one drops
//     the older history (no interpolation across a long hole). A snapshot farther than
//     max(teleportMinM 10, teleportPerSpeedS 0.5 * |v|) from where the newest one
//     predicted it (extrapolated as in 3) drops the history and is Teleported: the next
//     Sample has teleported = true.
//  2. Interpolate. Between the two snapshots around renderT: cubic Hermite position
//     with the velocities as tangents (tangent length clamped to 3x the chord, the
//     smoother's rule), linear velocity, slerp rotation on the shortest arc, angular
//     velocity lerped when both snapshots carry it, otherwise derived from the pair
//     (log(q1 * q0^-1) / dt), steering lerped.
//  3. Extrapolate. Past the newest snapshot, for at most extrapolateMaxMs (500 ms;
//     ours, longer than the smoother's 250 ms: a car keeps its momentum, a 300 ms loss
//     must not stop it): rotation = exp(w * dt) * q (w: the snapshot's, or derived from
//     the last two snapshots within maxBridgeMs, else 0); position along the ARC the
//     velocity sweeps when it turns with the body at w (closed form; a car whose tyres
//     grip turns its velocity with it), or a straight line when |w| > arcMaxRadS (2,
//     a spin: the velocity does not follow the body) or arcExtrapolation is off.
//     Beyond the cap: Holding at the capped pose with zero velocity.
//  4. Before the oldest snapshot (just after a teleport / a reset / a first sight): Buffering. Sync D: the car is
//     shown where it was `oldest.tMs - t` before that snapshot, walked BACK along its velocity and angular velocity
//     (the same arc as 3, run in reverse), for at most bufferBackMaxMs (300 ms; further back it holds at that pose).
//     Until Sync D it was pinned at the oldest snapshot as it is, so a moving car's target stood still at the
//     snapshot's position for the whole render delay (~110 ms) and then moved on: at 25 m/s a hand-over, a first
//     sight or a bridge reset popped the car 2.7 m ahead of where it was shown and braked it there; at 139 m/s
//     (the hyper car) 15 m. Its velocity is still the snapshot's (a moving car is not braked).
//
// HOW IT WORKS: VehicleDriveController (constants are VehicleDriveConfig defaults)
//  1. Snap (SET_ENTITY_COORDS + QUATERNION, then velocity and angular velocity as the
//     target's): first frame, Reset(), target.teleported, a position error above
//     max(snapMinM 5 m, snapTravelS 1 s * |v|), a PERSISTENT error (the excess of the
//     error over offToleranceM 3 + offTolPerSpeedS 0.1 * |v| integrated over time,
//     clamped at 0, snaps at offLimitMs 0.5 m*s: RAGECOOP IsOff's tolerance and its
//     60 fps limit, per second instead of per frame), and the GTAN under-floor case
//     (target higher by 0.4 .. 10 m while its vz is ~0, for underFloorMs 250 ms).
//  2. Velocity: v = target v + g * e, e = target pos - local pos (3D), with
//     g = corrGainBase 3 /s (GTAN: VehicleVelocity + vecDif * 3) + corrGainPerM 1 /(m*s)
//     * |e| (RAGECOOP: Velocity + dist * (pred - cur)), x stillGainMul 3 when the
//     target speed is below stillSpeedMs 0.1 (RAGECOOP multiplies by 10 there; 3 keeps
//     30 fps stable). Applied as the exact discrete gain (1 - exp(-g dt)) / dt, so a
//     frame removes the same fraction of error per unit time at every frame rate. The
//     correction is capped at corrMaxBaseMs 8 + corrMaxPerSpeed 0.25 * |v| m/s (ours:
//     anti-fling; RAGECOOP has no cap, dist^2 at 9 m is 81 m/s). Within deadbandM
//     0.03 m (RAGECOOP) no correction is added: the velocity set is the target's.
//     (RAGECOOP sets nothing there; then a stopped car creeps in a limit cycle on the
//     deadband edge, which the closed-loop test showed: holding the target's velocity
//     parks it.)
//  3. Rotation. Error r = log(target q * local q^-1) (world rotation vector, shortest
//     arc). |r| <= blendAboveDeg (50, RAGECOOP) and the target not upside down:
//     angular velocity = target w + k * r, k = rotGain 10 /s (RAGECOOP 0.2 rad/s per
//     degree = 11.5 /s), discrete-exact as above, capped at rotCorrMaxRadS 4. Else:
//     quaternion = slerp(local, target, 1 - exp(-dt / blendTauMs)) with blendTauMs
//     24 ms (RAGECOOP's slerp 0.5 per frame at 60 fps), angular velocity = target w.
//  4. Steering: the sample's steering, every frame (RAGECOOP CustomSteeringAngle;
//     GTAN lerps it, the interpolator already does).
//
//  5. Contact grace (W9 item 6). The commands above overwrite the car's velocity every frame, so any impulse
//     (the local player's car ramming this one, a wall) is erased within a frame and the crash never plays. A
//     contact (VehicleDriveInput.contact, the engine's HAS_ENTITY_COLLIDED_WITH_ANYTHING, or a measured loss of
//     speed along the direction of travel of more than contactDecelMs 4 m/s against what the controller
//     itself commanded last frame) opens a grace of contactGraceMs 400: no snap of any kind, the velocity
//     written is the car's own plus a weight w of the way to the full command (w rises from contactSoftFloor
//     0.15 to 1 along a smoothstep), no quaternion blend, and the angular velocity is left to physics until
//     w >= 0.5. After it the ordinary correction converges the car back; a new contact is not looked for for
//     contactCooldownMs 800 (a flag that stays set while scraping must not keep the car soft for ever).
//     Sync B adds a third trigger that needs no game flag: a KICK, |velocity commanded last frame - the car's own
//     velocity now| above contactKickMs 3 m/s in any direction (a sideways T-bone, a hit on a car that stands), checked
//     after the flag and the speed loss.
//
// PresentLead (W9 item 6, below): a remote car within 30 m of the local player is drawn at the present time,
// not ~110 ms in the past: the interpolator extrapolates by the render delay (capped), weighted 1 within
// fullWithinM 30 and 0 beyond zeroBeyondM 45, slewed so the weight never jumps the car.
//
// RISK: SET_ENTITY_ANGULAR_VELOCITY's frame. RAGECOOP feeds its RotationVelocity
// (SHVDN: GET_ENTITY_ROTATION_VELOCITY) straight back into the setter; whether that
// pair is world or body frame is not verified here. Both are output
// (angularVelocity, angularVelocityBody); they agree for a level car's yaw. Verify
// in game (roll a car on its side, compare) before relying on pitch/roll.
#pragma once

#include <cstdint>

namespace gage::sync {

    struct DriveVec { float x = 0, y = 0, z = 0; };
    struct DriveQuat { float x = 0, y = 0, z = 0, w = 1; }; // body -> world

    // ---- quaternion math (double inside, float out; every result normalised and finite) ----
    namespace vq {
        DriveQuat Normalize(const DriveQuat& q);            // zero / non-finite -> identity
        DriveQuat Mul(const DriveQuat& a, const DriveQuat& b); // a * b (b first, then a)
        DriveQuat Conj(const DriveQuat& q);
        double Dot(const DriveQuat& a, const DriveQuat& b);
        // Shortest-arc slerp; t outside [0, 1] extrapolates along the same arc. Near-
        // identical inputs (|dot| > 0.9995) use normalised lerp (no division by ~0).
        DriveQuat Slerp(const DriveQuat& a, const DriveQuat& b, double t);
        DriveQuat Exp(const DriveVec& rotVec);              // rotation vector (rad) -> quaternion
        DriveVec Log(const DriveQuat& q);                   // quaternion -> rotation vector, |angle| <= pi
        DriveVec Rotate(const DriveQuat& q, const DriveVec& v);
        double AngleBetween(const DriveQuat& a, const DriveQuat& b); // rad, [0, pi]
        DriveQuat FromHeadingDeg(double headingDeg);        // yaw about +z
        double HeadingDeg(const DriveQuat& q);              // of the body +y axis, (-180, 180]
        // Lean about the body's forward axis, degrees: + = right side down (a rotation about +y by +a is +a). A bike's
        // lean rides in the quaternion; 0 for a level body; sign flips with upside-down are not special-cased.
        double RollDeg(const DriveQuat& q);
        bool Finite(const DriveQuat& q);
    }

    // ---- interpolation buffer ----

    constexpr int kVehicleRing = 32; // snapshots kept (1.6 s at 20 Hz)

    struct VehicleSnapshot
    {
        double tMs = 0;          // sample time on the snapshots' timeline
        DriveVec pos, vel;       // m, m/s
        DriveQuat rot;
        DriveVec angVel;         // world rad/s, used when hasAngVel
        bool hasAngVel = false;  // false: derived from successive quaternions
        float steering = 0;
    };

    enum class VehMode : uint8_t { Buffering, Interpolating, Extrapolating, Holding };

    struct VehicleSample
    {
        DriveVec pos, vel;
        DriveQuat rot;
        DriveVec angVel;         // world rad/s
        float steering = 0;
        VehMode mode = VehMode::Buffering;
        bool teleported = false; // the first Sample after a Teleported push
    };

    // Repeat (sync plan W6): a snapshot with a LATER stamp whose motion (position, velocity, rotation, angular
    // velocity and its presence, steering) is bit-identical to the newest stored one is not new information (a
    // standing car re-sent by the server, or the same sample re-sent with a larger age). It is not stored:
    // storing it would freeze the Hermite interpolation between two identical points while the velocity says
    // "moving" (a zero chord clamps the tangent), then jump. The pose extrapolates; a standing car holds.
    enum class VehPush : uint8_t { Accepted, Teleported, Stale, Invalid, Repeat };

    struct VehicleInterpConfig
    {
        double extrapolateMaxMs = 500.0;  // ours: a car keeps momentum through a 300 ms loss
        double maxBridgeMs = 500.0;       // smoother.h maxBridgeMs
        float teleportMinM = 10.0f;       // RAGECOOP DisplayVehicle snaps at 10 m
        float teleportPerSpeedS = 0.5f;   // ours: half a second of travel at speed
        double bufferBackMaxMs = 300.0;   // Sync D: Buffering walks the oldest snapshot back along its motion at most this long (0 = pinned, as before)
        bool arcExtrapolation = true;     // ours: velocity turns with the body
        float arcMaxRadS = 2.0f;          // ours: above this |w| (a spin) extrapolate straight
    };

    struct VehicleInterpStats { uint32_t accepted = 0, teleports = 0, stale = 0, invalid = 0, bridgeResets = 0, repeats = 0, injected = 0, guarded = 0; };

    // The snapshot time of a vehicle in a server frame (sync plan W6): the frame's own time (tick * tickMs)
    // minus VehicleState.ageMs (how long before the frame the controller's sample reached the server), so a
    // car shows the instant its sample was taken and not the instant the server happened to relay it. A
    // duplicate relay of one sample carries the same stamp and is Stale; ageMs saturates at 255.
    inline double SnapshotStampMs(int64_t tick, double tickMs, uint32_t ageMs)
    {
        return double(tick) * tickMs - double(ageMs);
    }

    class VehicleInterpolator
    {
    public:
        explicit VehicleInterpolator(const VehicleInterpConfig& cfg = VehicleInterpConfig());
        VehPush Push(const VehicleSnapshot& s);
        // T4 (docs/sync-specs/vehicle-collisions.md): an impact the local machine knows about before the car's own stream
        // does. `s` is the car's state AFTER the impact at s.tMs (pose from Sample at that instant, velocity and angular
        // velocity already changed). Every stored snapshot at or after s.tMs is dropped (they show the car's motion without
        // the impact), `s` is stored, and pushes up to s.tMs + guardMs are refused (counted as guarded): the controller's
        // samples from that stretch are either pre-impact (its own contact came later) or its un-reconciled bounce; the
        // car's extrapolation along `s` shows the impact meanwhile, and the first sample after the guard takes over.
        //   preVel (optional): the car's velocity before the impact. With it the guard ends EARLY, at the first pushed sample
        //   that shows the impact (its velocity is at least confirmFraction of the impact's change away from preVel): the
        //   controller's own stream has caught up, whatever its latency and whenever its own contact came.
        void ReplaceFrom(const VehicleSnapshot& s, double guardMs, const DriveVec* preVel = nullptr, float confirmFraction = 0.5f);
        bool Sample(double renderTMs, VehicleSample& out); // false: nothing stored (or bad time)
        void Clear();
        int Count() const { return count_; }
        const VehicleInterpStats& Stats() const { return stats_; }

        // The timeline's session (sync plan W6 review fix 2). A new server session restarts the server's tick
        // counter, so its snapshots carry LOW stamps that Push would drop as Stale for as long as the previous
        // session had run. SetSession(id) with an id other than the current one clears the buffer (and a
        // pending teleport flag); the same id changes nothing. The caller numbers sessions (one per Welcome).
        uint32_t Session() const { return session_; }
        void SetSession(uint32_t id);

        // The median spacing (ms) of the last kGapRing STORED snapshots' stamps (spacings over maxBridgeMs are not
        // counted); 0 until two are stored. A hand-over or teleport (Clear) keeps it, a new session forgets it. The
        // engine turns it into the car's class extra on the shared render clock: sync::ClassExtraMs(gap, tickMs)
        // is 0 for a car the server relays every tick and (k - 1) ticks for one it relays every k-th (netclock.h).
        double TypicalGapMs() const;

    private:
        static constexpr int kGapRing = 8;
        const VehicleSnapshot& At(int i) const { return ring_[(head_ + i) % kVehicleRing]; }
        DriveVec OmegaOf(int i) const; // snapshot i's angular velocity (given or derived)
        void Extrapolate(const VehicleSnapshot& s, const DriveVec& w, double dtMs, DriveVec& pos, DriveQuat& rot) const;

        VehicleInterpConfig cfg_;
        VehicleSnapshot ring_[kVehicleRing];
        int head_ = 0, count_ = 0;
        bool teleportPending_ = false;
        double guardUntil_ = -1e18;
        bool guardConfirm_ = false;
        DriveVec guardPre_;
        float guardDvLen_ = 0, guardFrac_ = 0.5f;
        uint32_t session_ = 0;
        double gaps_[kGapRing] = {};
        int gapHead_ = 0, gapCount_ = 0;
        VehicleInterpStats stats_;
    };

    // ---- per-frame controller ----

    struct VehicleDriveConfig
    {
        float corrGainBase = 3.0f;     // 1/s   GTAN VMultiVehiclePos: vecDif * 3
        float corrGainPerM = 1.0f;     // 1/(m*s) RAGECOOP: dist * (pred - cur)
        float corrMaxBaseMs = 8.0f;    // m/s   ours: anti-fling cap ...
        float corrMaxPerSpeed = 0.25f; //       ... + a quarter of the speed
        float stillSpeedMs = 0.1f;     // RAGECOOP: Velocity.Length() < 0.1
        float stillGainMul = 3.0f;     // RAGECOOP x10, ours x3 (stable at 30 fps)
        float deadbandM = 0.03f;       // RAGECOOP: dist > 0.03 (no correction inside)

        float rotGain = 10.0f;         // 1/s   RAGECOOP calirot(deg) * 0.2 = 11.5 /s
        float rotCorrMaxRadS = 4.0f;   // ours
        float blendAboveDeg = 50.0f;   // RAGECOOP: calirot.Length() > 50 -> slerp
        float blendTauMs = 24.0f;      // RAGECOOP slerp 0.5 per frame at 60 fps

        float snapMinM = 5.0f, snapTravelS = 1.0f;          // brief: max(5 m, 1 s of travel)
        float offToleranceM = 3.0f, offTolPerSpeedS = 0.1f; // RAGECOOP IsOff tolerance 3 (+ ours)
        float offLimitMs = 0.5f;                            // RAGECOOP IsOff 30 m*frames at 60 fps
        float underFloorMinM = 0.4f, underFloorMaxM = 10.0f, underFloorVzMs = 0.01f; // GTAN StuckVehicleCheck
        float underFloorMs = 250.0f;                        // ours (GTAN: 2 consecutive frames)

        // W9 item 6, contact grace (see the doc block, 5.): 0 grace disables it.
        float contactGraceMs = 400.0f;      // how long the correction is soft after a contact
        float contactSoftFloor = 0.15f;     // the weight of the full command at the start of the grace
        float contactDecelMs = 4.0f;        // m/s lost along the travel direction in one frame vs the last command = a contact
        // Sync B (collision audit): a KICK is any change of the car's own velocity the controller did not command, in ANY direction
        // and at ANY target speed: |last command - the car's velocity now| above this (m/s, 3D). The collision flag alone left a
        // sideways knock, a hit on a standing car and a flag that never falls (or arrives a frame late) without a grace: the
        // impulse was erased by the next command. Tyre grip, drag and the ground move a commanded velocity by well under
        // 1 m/s in one frame (vehicle_drive_test measures it), a car-to-car hit by 5..9. 0 = off.
        float contactKickMs = 3.0f;
        // The kick rests on an assumption nobody has verified in the game: that the car honours SET_ENTITY_VELOCITY within a frame.
        // A body that does not (it lags the command) would report a "kick" in every frame, so kicks that are the ONLY reason for a
        // grace are budgeted: contactKickStormN of them within contactKickStormMs mute the kick trigger for contactKickMuteMs
        // (the flag and the speed loss keep working), and the mute is counted (stats.kickMutes, the 10 s line).
        int contactKickStormN = 3;
        float contactKickStormMs = 6000.0f;
        float contactKickMuteMs = 15000.0f;
        float contactMinSpeedMs = 3.0f;     // the deceleration test only for a target faster than this
        float contactCooldownMs = 800.0f;   // after a grace, no new contact is looked for for this long
        float contactMaxFrameMs = 150.0f;   // a frame gap longer than this: the deceleration test is skipped (a hitch)
        float contactHardSnapMul = 4.0f;    // inside a grace a snap needs this many times the usual distance
    };

    struct VehicleDriveInput
    {
        VehicleSample target;     // VehicleInterpolator::Sample at this frame's render time
        DriveVec localPos, localVel; // GET_ENTITY_COORDS / GET_ENTITY_VELOCITY of the local copy
        DriveQuat localRot;       // GET_ENTITY_QUATERNION
        double dtMs = 0;          // since the previous Update (clamped to [0, 1000])
        bool contact = false;     // W9: HAS_ENTITY_COLLIDED_WITH_ANYTHING of the local copy this frame (INFERRED to ignore the ground)
    };

    struct VehicleDriveOutput
    {
        bool snap = false;        // SET_ENTITY_COORDS_NO_OFFSET(snapPos) + SET_ENTITY_QUATERNION(snapRot)
        DriveVec snapPos;
        DriveQuat snapRot;

        bool setVelocity = false; // SET_ENTITY_VELOCITY
        DriveVec velocity;

        bool setAngularVelocity = false; // SET_ENTITY_ANGULAR_VELOCITY
        DriveVec angularVelocity;        // world rad/s
        DriveVec angularVelocityBody;    // the same in the body frame (see RISK)

        bool setQuaternion = false; // SET_ENTITY_QUATERNION (large-error blend, not on a snap)
        DriveQuat rot;

        bool setSteering = false;
        float steering = 0;

        float posErrorM = 0;      // diagnostics: before this frame's commands
        float rotErrorDeg = 0;
        bool graceActive = false; // W9: a contact grace is in force this frame (soft correction)
        float graceWeight = 1.0f; // ... and the weight of the full command (1 outside a grace)
        // Sync B: diagnostics. devMs = |the velocity commanded last frame - the car's own now| (3D, m/s; 0 when there was no
        // previous command or the frame gap was a hitch): how much physics did not honour. contactOpened = what opened a grace THIS frame.
        float devMs = 0;
        uint8_t contactOpened = 0; // 0 none, 1 collision flag, 2 speed loss along the travel direction, 3 kick (any-direction deviation)
    };

    struct VehicleDriveStats
    {
        uint32_t frames = 0, snaps = 0, persistentSnaps = 0, underFloorSnaps = 0, blends = 0, refused = 0;
        uint32_t contacts = 0, contactsByFlag = 0, contactsByDecel = 0;   // W9: graces opened, and what opened them
        uint32_t contactsByKick = 0, kickMutes = 0;                       // Sync B: ... by a velocity deviation in any direction; kick budget exhausted
    };

    // ---- sender steering ----

    constexpr float kSteerLockRad = 0.6f;     // front wheel angle at full lock (INFERRED; passenger cars are ~0.6 rad)

    // The wire steering (VehicleUpdate.steer: -1 full left .. +1 full right) the sender infers from its
    // own motion, because GTA offers no steering-angle getter: the kinematic bicycle model gives the
    // front wheel angle delta = atan(wheelbase * yawRate / vFwd), and steer = -delta / lock (positive
    // delta turns left, counter-clockwise from above, which the wire calls negative).
    //   vFwd      speed along the body's forward axis, m/s, signed (negative = reversing; the atan is
    //             of the signed ratio, so a reversing car still reports its wheel angle)
    //   yawRate   rotation about the body's up axis, rad/s, positive = counter-clockwise (left)
    //   wheelbase front axle to rear axle, m
    //   prev      the value returned last time: held while |vFwd| < holdBelowMps (the ratio is noise at
    //             a standstill) and when an input is not finite / the wheelbase is not positive
    // The result is clamped to [-1, 1] and never NaN (a non-finite prev reads as 0). The receiver
    // passes -steer to SET_VEHICLE_STEER_BIAS (-1 right .. +1 left).
    float EstimateSteer(float vFwd, float yawRate, float wheelbase, float prev,
                        float lock = kSteerLockRad, float holdBelowMps = 1.0f);

    // ---- sender cadence (sync plan W6, one controlling client per vehicle) ----

    // Whether a controlled vehicle moves enough to be sent at the full rate: linear speed above 0.2 m/s or
    // angular speed above 0.05 rad/s. Non-finite input reads as still.
    bool VehicleIsMoving(float speedMs, float angSpeedRadS);

    // When one controlled vehicle's VehicleUpdate goes out: every call while it moves (the caller's own
    // send tick is the full rate), immediately when something it carries changed (lights, doors, flags) or
    // when it has just stopped (the receiver must not extrapolate the last moving velocity), and otherwise
    // every stillIntervalMs (5 Hz) as proof of life. The first call after construction or Reset() sends at
    // once (a new controller announces itself). One per controlled vehicle; the vehicle's time base is the
    // caller's monotonic ms.
    class VehicleSendGate
    {
    public:
        explicit VehicleSendGate(double stillIntervalMs = 200.0) : stillMs_(stillIntervalMs) {}
        bool Due(double nowMs, bool moving, bool changed);
        void Reset() { started_ = false; wasMoving_ = false; }
    private:
        double stillMs_;
        bool started_ = false, wasMoving_ = false;
        double lastMs_ = 0;
    };

    class VehicleDriveController
    {
    public:
        explicit VehicleDriveController(const VehicleDriveConfig& cfg = VehicleDriveConfig());
        VehicleDriveOutput Update(const VehicleDriveInput& in);
        void Reset(); // the vehicle was respawned / moved by someone else: next Update snaps
        // T4: the caller handled the contact itself (VehicleInterpolator::ReplaceFrom with the copy's own response): end the soft
        // correction the contact opened, and look for the next contact only after cooldownMs. No snap, no other state touched.
        void EndGrace(double cooldownMs);
        float OffAccumulator() const { return offAcc_; }
        const VehicleDriveStats& Stats() const { return stats_; }

    private:
        VehicleDriveConfig cfg_;
        bool started_ = false;
        float offAcc_ = 0;
        double underFloorMs_ = 0;
        VehicleDriveStats stats_;
        // contact grace (W9 item 6)
        bool graceOn_ = false;
        double graceAgeMs_ = 0, cooldownMs_ = 0;
        double clockMs_ = 0, kickAtMs_[8] = {}, kickMuteMs_ = 0;   // Sync B: the kick budget (sum of dt, the last kick times, the mute left)
        int kickN_ = 0;
        bool haveCmd_ = false;      // lastCmdVel_ is the previous frame's velocity command (not a snap)
        DriveVec lastCmdVel_;
    };

    // ---- present-time prediction (W9 item 6) ----

    struct PresentLeadConfig
    {
        float fullWithinM = 30.0f;     // full lead within this distance of the local player
        float zeroBeyondM = 45.0f;     // no lead beyond this (the car is drawn in the past, as before)
        double maxLeadMs = 250.0;      // cap of the lead: extrapolation past the newest snapshot is an estimate
        double slewMsPerMs = 0.5;      // the lead changes at most this fast (a car crossing the band does not jump)
    };

    // How far past the interpolation time a remote car is drawn. The interpolation time is the render clock minus
    // the car's class extra (about 110 ms behind the present); the lead moves it forward by `wantMs` (that delay,
    // so the car is drawn where it is NOW on the sender's side, extrapolated from its newest snapshots), weighted by
    // the distance to the local player: a car beside you is what you collide with and what you look at, a car 100 m
    // away is not worth the extrapolation error. One per remote car; Update once per frame.
    class PresentLead
    {
    public:
        explicit PresentLead(const PresentLeadConfig& cfg = PresentLeadConfig()) : cfg_(cfg) {}
        // 1 within fullWithinM, 0 beyond zeroBeyondM, linear between; a non-finite distance is 0.
        static double Weight(const PresentLeadConfig& cfg, double distM);
        // The lead (ms) to add to the interpolation time this frame. The first call (and the first after Reset)
        // takes the target at once. dtMs is clamped to [0, 1000]; a non-finite or negative wantMs is 0.
        double Update(double distM, double wantMs, double dtMs);
        double LeadMs() const { return leadMs_; }
        void Reset() { have_ = false; leadMs_ = 0; }

    private:
        PresentLeadConfig cfg_;
        bool have_ = false;
        double leadMs_ = 0;
    };

}
