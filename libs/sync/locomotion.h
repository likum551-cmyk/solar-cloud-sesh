// Remote-ped locomotion: turns the smoothed pose of one remote player (RemoteSync::
// Sample) into the GTA ped commands that make the local copy of that ped walk, run
// and sprint after it, once per game frame:
//
//   LocoInput in;                          // fill from Pose + GET_ENTITY_COORDS/HEADING + IS_PED_*
//   const LocoOutput o = loco.Update(in);
//   if (o.snap)          SET_ENTITY_COORDS_NO_OFFSET(ped, o.snapPos)       (then heading below)
//   if (o.ragdollStart)  SET_PED_TO_RAGDOLL ...    if (o.ragdollEnd) clear the ragdoll
//   if (o.jumpStart)     TASK_JUMP
//   switch (o.task)      StandStill: TASK_STAND_STILL(o.standStillMs)
//                        GoStraight: TASK_GO_STRAIGHT_TO_COORD(o.target, o.moveBlendRatio, -1, 0, 0)
//                                    + SET_PED_DESIRED_MOVE_BLEND_RATIO(o.moveBlendRatio)
//                        GoToAnchor: TASK_GO_TO_ENTITY(ped, anchor, -1, 0.1, o.moveBlendRatio, 0, 0)
//                                    + SET_PED_DESIRED_MOVE_BLEND_RATIO (W11, item 8 below)
//   if (o.setBlend)      SET_PED_DESIRED_MOVE_BLEND_RATIO(o.moveBlendRatio) on the running task (W11)
//   if (o.moveAnchor)    SET_ENTITY_COORDS_NO_OFFSET(anchor, o.anchorPos) EVERY frame (W11)
//   if (o.setMoveRate)   SET_PED_MOVE_RATE_OVERRIDE(o.moveRate)   EVERY frame (it only holds while called);
//                        if the call fails set LocoInput::moveRateAvailable = false from then on
//   if (o.setHeading)    SET_ENTITY_HEADING(o.heading)                      (degrees, [0, 360))
//   if (o.setVelocity)   SET_ENTITY_VELOCITY(o.velocity; z = GET_ENTITY_VELOCITY().z
//                                            when o.velocityZFromPhysics)
//   if (o.paused)        Pause() is running (leaving a vehicle): do nothing else this frame
//
// Pure C++17, no OS or game headers, no allocation; single-threaded; one instance
// per remote ped. Every rule is time-based (dtMs), so 30, 60 and 144 fps behave
// alike. Modelled on GTA Network (Client/Sync/Entity/OnFoot.cs WalkAnimation,
// UpdatePosition; SyncPed.cs StuckDetection) and RAGECOOP-V (SyncedPed.cs WalkTo,
// SmoothTransition; SyncedEntity.cs IsOff), with their per-frame constants turned
// into per-second ones.
//
// HOW IT WORKS (constants are LocoConfig defaults; sources in the table below)
//
//  1. Snap. SET_ENTITY_COORDS to the pose (with its heading and velocity) on the
//     first frame, on Pose::teleported, after ResetHard(), when the ped is more than
//     snapBaseM + snapPerSpeedS * |v| (5 + 5|v| m, GTAN StuckDetection) from the
//     pose, or when a moderate error persists: the excess of the distance over
//     offToleranceM (3 m) is INTEGRATED OVER TIME (m*s), clamped at 0, and a snap
//     fires when it reaches offLimitMs (0.5 m*s). RAGECOOP's IsOff sums the same
//     excess per FRAME (limit 30), which snaps twice as fast at 120 fps as at 60;
//     0.5 m*s is its 60 fps value. A snap re-issues the gait task. Reset() (the same
//     ped was re-tasked by someone else) re-issues the gait task but does NOT snap:
//     the pose is the smoother's delayed one, delay * speed behind where the ped is.
//
//  2. Gait. The speed's gait with hysteresis (a gait is entered at its "up" speed
//     and left below its "down" speed) is the base:
//        still -> walk 0.50 / walk -> still 0.25 m/s
//        walk  -> run  3.10 / run  -> walk  2.10 m/s   (W11, was 2.90 / 2.40)
//        run -> sprint 6.10 / sprint -> run 5.30 m/s   (W11, was 5.90 / 5.60)
//     GTA on foot walks at ~1.2-1.8 m/s, runs at ~3.5-5.5 and sprints at ~6-7.5:
//     each band sits in the gap between two gaits, so ordinary speed noise of a
//     gait never crosses a band, and noise at a band edge cannot flap. The sender's
//     moveState (protocol v3, 0..3) is only a HINT: clamp(moveState, inferred,
//     min(Sprint, inferred + 1)). The sender reports 0 for swimming, climbing, cover
//     and strafing, which used to stand a moving ped still while the velocity steer
//     slid it along in its idle pose. Going Still needs the speed under idleDown for
//     stillHoldMs (150 ms; nothing is issued meanwhile), so a one-sample flap cannot
//     fire StandStill then GoStraight. A change between Walk / Run / Sprint has to
//     hold for gaitDebounceMs (400 ms, W11); leaving Still is immediate..
//
//  3. Task. Moving gaits use TASK_GO_STRAIGHT_TO_COORD towards a point ahead,
//     target = pose + (pose - ped) + direction * lead (GTAN WalkAnimation: Position +
//     (Position - Character.Position) + PedVelocity), direction = the low-passed
//     velocity's (the heading when hardly moving), lead = leadS (3 s) of travel
//     clamped to leadMinM..leadMaxM (3..16 m): GTAN's 1 s lead was "reached" after half
//     a second, ~2 task restarts per second. The task's blend ratio is the plain gait
//     (1 walk, 2 run, 3 sprint; the native's scale is 0 still, 1 walk, 2 run, 3 sprint).
//     It is re-issued:
//       - at once when the last task was not a gait task (first move, after a stand-still), or after a snap /
//         ragdoll end / jump end / Reset() / Pause(). A change between Walk / Run / Sprint is NOT a re-issue
//         since W11: setBlend carries it (item 8);
//       - otherwise at most every minReissueMs (500 ms), when the velocity direction
//         turned more than turnReissueDeg (25) since the last issue, the target's
//         anchor (pose + ped error) drifted driftM (1 m) from where the last one has
//         moved to (its velocity * elapsed; and at least driftReissueMs, 1 s, since
//         the last issue), the ped has come within max(arriveMinM 1 m, arriveFrac
//         0.12 * lead) of the last target (so it never reaches it and stops), or it
//         stalled: the ped's own gait (IS_PED_*) read Still for stallMs while a
//         moving one is commanded (the task ended or was cancelled; at least
//         stallRetryMs, 2.5 s, since the last issue, so a re-issue that does not cure
//         the stall does not repeat every 500 ms).
//       - SoftReset() evaluates the turn / drift / arrival rules at once and re-issues
//         only when one of them fires (W8 item 3); Reset() always re-issues.
//     Every issue records its cause (LocoOutput::why, LocoStats::why*).
//     Animation speed: SET_PED_MOVE_RATE_OVERRIDE(moveRate), moveRate = speed /
//     nominal(gait) clamped to 0.6..1.6 (nominalWalk/Run/Sprint 1.5 / 4.5 / 7 m/s,
//     INFERRED), set every frame a gait task is wanted. If the engine reports the
//     native unavailable (moveRateAvailable = false) the blend ratio carries the speed
//     instead: piecewise-linear through the nominal speeds, kept within half a step of
//     the gait, quantised to blendStep (0.25) and re-issued at most every
//     blendReissueMs (400 ms) when it moves a step.
//     Going still issues TASK_STAND_STILL(standStillMs = 2000, GTAN / RAGECOOP)
//     once; while still nothing more is issued and the heading is steered.
//
//  4. Velocity, every frame. The pose velocity first goes through a one-pole low-pass
//     (velTauMs 120 ms; the sender's raw GET_ENTITY_VELOCITY carries the stride ripple)
//     for the steer, the lead direction and the move rate; seeded on the first frame,
//     after a teleport / snap / ResetHard, and not applied to a stop (raw speed under
//     idleDown), which must not coast on; vz is not filtered.
//     v = low-passed pose v + correction,
//     correction = (corrGainBase + corrGainPerM * |e|) * e on the horizontal error
//     e = pose - ped: 2/s near the pose (GTAN: target + 2 * (posTarget - pos)),
//     rising by 5/s per metre of error (RAGECOOP: v + 5 * dist * (pred - pos)),
//     capped at corrMaxHMs (3 m/s) so a large error cannot fling the ped (beyond
//     the cap the persistent-error snap takes over), and none at all while |e| is
//     under steerDeadbandM (0.08 m). Vertical: ground physics owns
//     z; the pose's vz is passed through and a correction zGain * (|dz| - zDeadbandM)
//     (capped at zCorrMaxMs) is added only when |dz| > zDeadbandM (0.5 m). While
//     jumping there is no vertical correction and z comes from physics
//     (velocityZFromPhysics). While ragdolling the same horizontal velocity is set
//     (the ragdoll follows the sender instead of falling in place), the whole vector
//     capped at ragdollMaxMs (8 m/s) and z from physics.
//
//  5. Heading, every frame (not during a ragdoll): approaches the pose's heading on
//     the shortest arc with time constant headingTauMs (50 ms), framerate
//     independent (1 - exp(-dt/tau)), wrapped across 0/360, output in [0, 360).
//
//  6. Jump and ragdoll (protocol v3; LocoInput defaults make them inert for v2).
//     A change of jumpSeq (after the first value seen) starts one jump; for
//     jumpGraceMs (700 ms, a GTA jump's airtime) or while `jumping` is set, no
//     task is issued (it would cancel the jump) and z comes from physics. The
//     ragdollEnd; in between, no task or heading is set (NaturalMotion owns the
//     ped), only snaps and the velocity above. The engine counts a fall (kPfFalling)
//     as `jumping`.
//
//  7. Pause(ms): the ped is leaving a vehicle (TASK_LEAVE_VEHICLE): for that long
//     Update sets nothing at all (not even a snap), then re-issues the gait task.
//
//  8. W11 (docs/CLOUD-HANDOFF.md item 0b): the live 0.4.3 test still showed 8-12 go-straight per 10 s of running.
//     (a) A change between Walk / Run / Sprint never re-issues the running task: LocoOutput::setBlend asks for
//         SET_PED_DESIRED_MOVE_BLEND_RATIO(moveBlendRatio) on it (every frame the commanded gait differs from the
//         one the task was issued with; the move rate override follows the gait as before). The Walk/Run and Run/Sprint
//         bands are wider (3.1 / 2.1 and 6.1 / 5.3 m/s) and a change holds gaitDebounceMs = 400 ms. The drift rule
//         counts only the lateral part of the target anchor's drift: a Run <-> Sprint speed change moves the anchor
//         along the track faster than the last velocity predicted, which re-issued the task 4 times in 10 s.
//     (b) LocoMode::Anchor: the ped runs TASK_GO_TO_ENTITY(ped, anchor) ONCE; the engine moves an invisible,
//         collision-less anchor entity to LocoOutput::anchorPos every frame (pose + direction * lead, lead =
//         anchorLeadS (0.8 s) of travel clamped to 1.5..5 m). Turns, drift and arrival cannot re-issue a task
//         that has no fixed target: only a lost task does (the stall rule), a snap/Reset, or Still <-> moving.
//         A stall in anchor mode drops the ped back to LocoMode::Straight for anchorFallbackMs (the anchor path may be
//         unusable there: no navmesh, interior). LocoInput::noAnchor (anchor entity missing, aim task) forces Straight
//         for that frame. LocoConfig::mode defaults to Straight here; the engine picks Anchor (loco_mode).
//     (c) Residual push: with LocoInput::pedVel (GET_ENTITY_VELOCITY: the animation's own displacement) the velocity
//         set is pedVel + a low-passed (residualTauMs 150) correction of the position error, capped at
//         residualMaxMs (2 m/s); nothing at all inside the dead band, so the stride carries the ped instead of the
//         pose velocity dragging it. A hard snap stays beyond the teleport threshold. Jumps, ragdoll, swimming and
//         a missing pedVel keep the pose-velocity push of item 4.
//
// CONSTANTS (LocoConfig defaults)
//   idleUp/Down        0.50 / 0.25 m/s  old GTAN GetPedSpeed: walk from 0.5; band below
//   walkRunUp/Down     3.10 / 2.10 m/s  W11: was 2.90 / 2.40; around 2.6, walk max 1.8 .. run min 3.5
//   runSprintUp/Down   6.10 / 5.30 m/s  W11: was 5.90 / 5.60; run max 5.5 .. sprint min 6.0 (old GTAN 6.2)
//   stillHoldMs        150 ms           docs/sync-specs/onfoot.md item 1
//   gaitDebounceMs     400 ms           W11: was 200 (ours; the spec says "debounced")
//   leadS, leadMin/MaxM 3 s, 3..16 m    onfoot.md item 2 (GTAN: 1 s); was 2.5 s, 3..12 m: W8 item 3, arrival re-issues came every 1.9 s
//   turnReissueDeg     25 deg           onfoot.md item 2
//   driftM             1.0 m            onfoot.md item 2 (GTAN: 0.5-0.87 m, squared 0.25-0.75)
//   minReissueMs       500 ms           onfoot.md item 2 (GTAN re-issued every frame)
//   arriveMinM/Frac    1 m / 0.12       onfoot.md item 2: keep the target ahead of the ped; was 0.25 (a run re-issued every 1.9 s, now 2.6 s)
//   stallMs            500 ms           ours: GTAN re-issues on !IsWalking, only the "no gait at all" case is kept
//   nominalWalk/Run/Sprint 1.5/4.5/7    middle of the ranges above; INFERRED, tune against a live ped
//   moveRateMin/Max    0.6 / 1.6        was 0.8 / 1.25 (onfoot.md item 2); W8 item 3 (c): the gait bands are wide
//   driftReissueMs     1000 ms          W8 item 3: a drift re-issue at most once a second
//   stallRetryMs       2500 ms          W8 item 3: a stall re-issue at most once per 2.5 s
//   blendStep/ReissueMs 0.25 / 400 ms   onfoot.md item 2 (fallback)
//   standStillMs       2000 ms          GTAN / RAGECOOP StandStill(2000)
//   corrGainBase       2 /s             GTAN OnFoot: target + 2 * (posTarget - pos)
//   corrGainPerM       5 /(m*s)         RAGECOOP SmoothTransition: 5 * dist
//   corrMaxHMs         3 m/s            ours: anti-fling cap (sprint 7.5 + 3)
//   zDeadbandM/zGain/zCorrMaxMs 0.5 m / 2 /s / 2 m/s   ours (GTAN forces z past 0.1 m)
//   velTauMs           120 ms           onfoot.md item 4
//   steerDeadbandM     0.08 m           onfoot.md item 4
//   ragdollMaxMs       8 m/s            onfoot.md item 4
//   snapBaseM/PerSpeedS 5 m / 5 s       GTAN StuckDetection 5 + 5 * |v|
//   offToleranceM      3 m              RAGECOOP IsOff tolerance
//   offLimitMs         0.5 m*s          RAGECOOP IsOff limit 30 m*frames at 60 fps
//   headingTauMs       50 ms            between RAGECOOP diff/2 per frame (24 ms at
//                                       60 fps) and GTAN lerp 0.1 per frame (158 ms)
//   jumpGraceMs        700 ms           ours: GTA jump airtime estimate
#pragma once

#include <cstdint>

namespace gage::sync {

    enum class Gait : uint8_t { Still = 0, Walk = 1, Run = 2, Sprint = 3 };

    enum class LocoTask : uint8_t
    {
        None,       // issue nothing this frame
        StandStill, // TASK_STAND_STILL(standStillMs)
        GoStraight, // TASK_GO_STRAIGHT_TO_COORD(target, moveBlendRatio) + SET_PED_DESIRED_MOVE_BLEND_RATIO
        GoToAnchor, // W11: TASK_GO_TO_ENTITY(ped, anchor, -1, 0.1, moveBlendRatio, 0, 0) + SET_PED_DESIRED_MOVE_BLEND_RATIO
    };

    // W11: how a moving gait is tasked. Straight: TASK_GO_STRAIGHT_TO_COORD at a far point, re-issued on turn / drift /
    // arrival. Anchor: TASK_GO_TO_ENTITY once at an invisible entity the engine moves every frame (LocoOutput::anchorPos).
    enum class LocoMode : uint8_t { Straight, Anchor };

    struct LocoVec { float x = 0, y = 0, z = 0; };

    struct LocoConfig
    {
        float idleUp = 0.50f, idleDown = 0.25f;           // m/s, horizontal
        float walkRunUp = 3.10f, walkRunDown = 2.10f;     // W11: dead zone 1.0 m/s (was 2.90 / 2.40)
        float runSprintUp = 6.10f, runSprintDown = 5.30f; // W11: dead zone 0.8 m/s (was 5.90 / 5.60)

        // Gait authority and debounce (W1 item 1 and 2)
        float stillHoldMs = 150.0f;    // speed under idleDown this long before a moving gait goes Still
        float gaitDebounceMs = 400.0f; // a change between Walk / Run / Sprint holds this long (Still <-> moving: at once); W11: was 200

        // Task target and re-issue (W1 item 2)
        float leadS = 3.0f;                           // target = leadS seconds of travel ahead ...
        float leadMinM = 3.0f, leadMaxM = 16.0f;      // ... clamped to this many metres
        float turnReissueDeg = 25.0f;                 // velocity direction turned more than this since the last issue
        float driftM = 1.0f;                          // the target's anchor (pose + ped error) moved more than this from where the last one has gone to
        float minReissueMs = 500.0f;                  // between re-issues for turn / drift / arrival / stall
        float driftReissueMs = 1000.0f;               // ... and at least this between two drift re-issues (W8 item 3)
        float stallRetryMs = 2500.0f;                 // ... and this between two stall re-issues: a stall re-issue that does not cure
                                                      //     the stall must not repeat every minReissueMs (live test 0.4.2: 10-21 / 10 s)
        float arriveMinM = 1.0f, arriveFrac = 0.12f;  // ped within max(arriveMinM, arriveFrac * lead) of the target
        float stallMs = 500.0f;                       // ped shows no gait while a moving one is commanded
        int   standStillMs = 2000;

        // Animation speed (W1 item 2): move rate = speed / nominal(gait) clamped; fallback = quantised blend ratio
        float nominalWalk = 1.5f, nominalRun = 4.5f, nominalSprint = 7.0f; // m/s of an unscaled gait (INFERRED, live check)
        // W8 item 3 (c): 0.6 .. 1.6 (was 0.8 .. 1.25). The gait bands are wide (a run is entered at 2.9 m/s and left at
        // 5.9, nominal 4.5): with the old clamp the animation speed was off by up to ~2x at the edges of a band, the
        // ped slid. 0.6 .. 1.6 covers a run (0.64 .. 1.31) and a sprint completely and most of a walk. INFERRED: a live
        // look decides the final window.
        float moveRateMin = 0.6f, moveRateMax = 1.6f;
        float blendStep = 0.25f, blendReissueMs = 400.0f;

        // W11 item 2: the continuously steered target. mode defaults to Straight (the library's old behaviour); the engine
        // sets Anchor from loco_mode. The anchor leads the pose by anchorLeadS of travel (shorter than the straight path's
        // 3 s: it is moved every frame, so it need not outrun a re-issue; a long lead cuts corners: error ~ lead^2 / 2R), clamped to anchorLeadMinM..anchorLeadMaxM.
        LocoMode mode = LocoMode::Straight;
        float anchorLeadS = 0.8f, anchorLeadMinM = 1.5f, anchorLeadMaxM = 5.0f;
        float anchorFallbackMs = 20000.0f; // a stall in anchor mode: Straight for this long, then Anchor again

        // W11 item 3: with LocoInput::pedVel the velocity push is pedVel + a low-passed, capped position correction.
        float residualTauMs = 150.0f, residualMaxMs = 2.0f;

        float corrGainBase = 2.0f;   // 1/s
        float corrGainPerM = 5.0f;   // 1/(m*s)
        float corrMaxHMs = 3.0f;     // m/s
        float zDeadbandM = 0.5f, zGain = 2.0f, zCorrMaxMs = 2.0f;
        // W8 item 4: the same correction tuned for water (LocoInput::swimming). A swimmer has no ground to stand on:
        // the surface moves and a diver is legitimately below the pose's land-like z band, so the vertical dead band
        // is wider and its gain gentler (z is the sample's, underwater allowed); the horizontal pull is scaled and
        // capped lower (water drag answers a velocity push slowly: the land gain overshoots and buzzes). INFERRED.
        float swimZDeadbandM = 0.9f, swimZGain = 1.0f;
        float swimCorrScale = 0.6f, swimCorrMaxHMs = 1.5f;
        float velTauMs = 120.0f;       // one-pole low-pass of the pose velocity (W1 item 4)
        float steerDeadbandM = 0.08f;  // no steering under this horizontal error
        float ragdollMaxMs = 8.0f;     // cap of the whole velocity while ragdolling

        float snapBaseM = 5.0f, snapPerSpeedS = 5.0f;
        float offToleranceM = 3.0f, offLimitMs = 0.5f; // m, m*s

        float headingTauMs = 50.0f;
        float jumpGraceMs = 700.0f;
    };

    struct LocoInput
    {
        // The remote player (RemoteSync::Sample -> Pose): root position as the sender's
        // GET_ENTITY_COORDS, velocity m/s, heading degrees (any range), Pose::teleported.
        LocoVec pos, vel;
        float heading = 0;
        bool teleported = false;

        // The local ped that shows it: GET_ENTITY_COORDS, GET_ENTITY_HEADING and its
        // current gait from IS_PED_WALKING / RUNNING / SPRINTING (Still when none). Only
        // a Still reading is used (the stall rule): pass the shown gait while an aim task
        // owns the animation.
        LocoVec pedPos;
        float pedHeading = 0;
        Gait pedGait = Gait::Still;

        double dtMs = 0; // since the previous Update (clamped to [0, 1000])

        // Protocol v3 (defaults = unknown, the v2 behaviour).
        int8_t moveState = -1;  // 0..3 = the sender's gait, a hint (ApplyAuthority); anything else: from speed alone
        bool ragdoll = false;   // the sender is ragdolling
        bool jumping = false;   // the sender is in a jump
        int32_t jumpSeq = -1;   // increments per jump; < 0 = not sent

        // false once SET_PED_MOVE_RATE_OVERRIDE proved unavailable (World::SetPedMoveRateOverride
        // returned false): the controller then matches animation speed with a quantised move blend ratio.
        bool moveRateAvailable = true;

        // Sync plan W6 (protocol v5 kPfSwimming): the sender is swimming. The game reports no land gait for a
        // swimmer, so pedGait reads Still: that is not a stall (no re-issue of the go-straight every
        // minReissueMs), and the land animation speed override is not set (nothing it scales is playing).
        // The pose is still followed (velocity and snap rules unchanged).
        bool swimming = false;

        // W11 item 3: the ped's own velocity now (GET_ENTITY_VELOCITY: what the animation moves it by). With havePedVel
        // the controller pushes only the residual error on top of it (LocoConfig::residualTauMs / residualMaxMs).
        LocoVec pedVel;
        bool havePedVel = false;
        // W11 item 2: no anchor entity this frame (not created yet, lost, or an aim task owns the walk): the controller
        // acts as LocoMode::Straight for it. A switch between the modes re-issues the task once.
        bool noAnchor = false;
    };

    // Why a GoStraight was issued (LocoOutput::why; the per-trigger counters of LocoStats). W8 item 3: the live test
    // showed 10-21 re-issues per 10 s of straight running and nothing said which rule fired.
    enum class LocoWhy : uint8_t
    {
        None,     // no GoStraight this frame
        Gait,     // first task of a gait: the commanded (debounced) gait changed, or the last task was a stand-still
        Reset,    // Reset(): the task was taken away (an aim task, an emote, a cleared task list, ...)
        Forced,   // a snap, a landing, a ragdoll end or the end of a Pause
        Blend,    // the quantised blend ratio moved a step (only without SET_PED_MOVE_RATE_OVERRIDE)
        Turn,     // the velocity direction turned more than turnReissueDeg
        Drift,    // the target's anchor drifted more than driftM
        Arrive,   // the ped came within max(arriveMinM, arriveFrac * lead) of the target
        Stall,    // the ped showed no gait for stallMs while a moving one is commanded
    };
    // (Anchor mode: Gait = the first task after a stand-still; Turn / Drift / Arrive / Blend never fire.)

    struct LocoOutput
    {
        bool snap = false;          // SET_ENTITY_COORDS_NO_OFFSET(snapPos), heading/velocity below
        LocoVec snapPos;

        LocoTask task = LocoTask::None;
        LocoVec target;             // GoStraight
        float moveBlendRatio = 0;   // GoStraight: 1 walk, 2 run, 3 sprint (task speed and blend ratio); with
                                    // LocoInput::moveRateAvailable false, quantised to blendStep instead
        int standStillMs = 0;       // StandStill

        bool setVelocity = false;   // SET_ENTITY_VELOCITY(velocity)
        LocoVec velocity;
        bool velocityZFromPhysics = false; // use the ped's own current z velocity instead of velocity.z

        bool setHeading = false;    // SET_ENTITY_HEADING(heading)
        float heading = 0;          // degrees, [0, 360)

        bool jumpStart = false;     // TASK_JUMP
        bool ragdollStart = false;  // put the ped into ragdoll
        bool ragdollEnd = false;    // take it out again

        Gait gait = Gait::Still;    // the gait being shown (diagnostics)
        LocoWhy why = LocoWhy::None; // task == GoStraight: the rule that issued it (diagnostics)

        bool setMoveRate = false;   // SET_PED_MOVE_RATE_OVERRIDE(moveRate), EVERY frame this is set (the native
                                    // only holds while it is called); set while a gait task is wanted
        float moveRate = 1.0f;      // speed / nominal(gait), clamped moveRateMin..moveRateMax
        bool paused = false;        // Pause() is in force: nothing else is set this frame

        // W11. setBlend: SET_PED_DESIRED_MOVE_BLEND_RATIO(moveBlendRatio) on the RUNNING task, every frame this is set
        // (the commanded gait differs from the one the task was issued with: a Walk / Run / Sprint change is not a re-issue).
        bool setBlend = false;
        // Anchor mode: move the anchor entity to anchorPos (SET_ENTITY_COORDS_NO_OFFSET) EVERY frame moveAnchor is set; the
        // engine creates it there before the first GoToAnchor. anchorMode says the controller acts as Anchor now
        // (cfg.mode == Anchor, not noAnchor, not in a stall fallback), also on frames that issue no task.
        bool anchorMode = false;
        bool moveAnchor = false;
        LocoVec anchorPos;
    };

    struct LocoStats
    {
        uint32_t frames = 0, snaps = 0, persistentSnaps = 0, goStraight = 0, standStill = 0, jumps = 0;
        // goStraight by cause (they sum to goStraight)
        uint32_t whyGait = 0, whyReset = 0, whyForced = 0, whyBlend = 0, whyTurn = 0, whyDrift = 0, whyArrive = 0, whyStall = 0;
        // Reset() calls, SoftReset() calls and how those ended: the running task kept, or re-issued
        uint32_t resets = 0, softResets = 0, softKept = 0, softReissued = 0;
        // W11: GoToAnchor issues (also counted in goStraight and by cause), Walk / Run / Sprint changes carried by setBlend
        // without a re-issue, stalls that dropped anchor mode to Straight, frames spent in anchor mode
        uint32_t anchorIssues = 0, gaitKept = 0, anchorFallbacks = 0, anchorFrames = 0;
    };

    class LocomotionController
    {
    public:
        explicit LocomotionController(const LocoConfig& cfg = LocoConfig());

        LocoOutput Update(const LocoInput& in);

        // The same ped was re-tasked by someone else (an aim task ended, an emote ended, it left a
        // vehicle, the drive mode changed): the next Update re-issues the gait task at once. The ped is
        // NOT snapped (the pose is the smoother's delayed one, up to delay * speed behind where the ped
        // is: a snap there is a visible pop); a real divergence still snaps by the distance and
        // persistent-error rules. The jump / ragdoll edge state and the stats are kept.
        void Reset();
        // The ped was touched but its gait task may still be running (a flinch that did not clear the task list, a
        // hold that ended): re-evaluate now, ignoring minReissueMs, and re-issue ONLY when the turn, drift or arrival
        // rule would (the running task still points within turnReissueDeg of where the pose is going and has not been
        // reached): otherwise the task is kept. W8 item 3 (b): Reset() re-issued at once from ~12 call sites.
        // Use Reset() when the task is known gone (ClearPedTasks, an aim task or an emote replaced it).
        void SoftReset();
        // The ped must be put onto the pose: the next Update snaps. For a ped whose position is not
        // known to be close to the pose (it could not be read). Edge state and stats are kept.
        void ResetHard();
        // A NEW ped (spawned or respawned): ResetHard() plus the jump / ragdoll edge state.
        // The next jumpSeq is a baseline again (no jump on the first frame), and a
        // remote ragdoll still in progress asks for ragdollStart on the new ped.
        void ResetForNewPed();
        // The ped is busy with something else for `ms` (leaving a vehicle): Update returns an
        // empty output (paused = true) until it has run out, then re-issues the gait task. A pause
        // replaces the previous one; Pause(0) cancels it.
        void Pause(double ms);

        Gait CurrentGait() const { return gait_; }
        // The task the controller last issued (None until the first): what the ped is latched on, for the 10 s log line.
        LocoTask LastTask() const { return lastTask_; }
        float OffAccumulator() const { return offAcc_; } // m*s toward a persistent-error snap
        const LocoStats& Stats() const { return stats_; }

        // Gait inference with hysteresis: the gait after `current` sees `speed` (m/s).
        static Gait InferGait(Gait current, float speed, const LocoConfig& cfg);
        // The sender's gait (LocoInput::moveState) as a hint to the speed's: clamp(moveState,
        // inferred, min(Sprint, inferred + 1)); anything outside 0..3 leaves `inferred`.
        static Gait ApplyAuthority(Gait inferred, int moveState);

    private:
        LocoConfig cfg_;
        bool started_ = false;
        Gait gait_ = Gait::Still;           // the gait being shown (debounced)
        Gait inferred_ = Gait::Still;       // the speed's gait with hysteresis
        double stillMs_ = 0;                // how long the speed has asked for Still while gait_ is moving
        Gait pendingGait_ = Gait::Still;    // a candidate moving gait waiting out gaitDebounceMs
        double pendingMs_ = 0;
        bool haveVf_ = false;
        LocoVec vf_;                        // low-passed pose velocity (z not filtered)
        double pauseMs_ = 0;

        LocoTask lastTask_ = LocoTask::None;
        Gait lastTaskGait_ = Gait::Still;
        LocoVec lastTarget_, lastAnchor_, lastTargetVel_;
        float lastLeadM_ = 0, lastBlend_ = 0;
        float blendSet_ = 0;                // W11: the blend ratio last sent to the running task (issue or setBlend)
        double sinceIssueMs_ = 0;
        double mismatchMs_ = 0;             // how long the ped has shown no gait while a moving one is commanded
        bool forceIssue_ = true;
        bool resetForced_ = false;          // forceIssue_ came from Reset() (a task lost), not from a snap / landing
        bool reevaluate_ = false;           // SoftReset(): the next evaluation ignores minReissueMs

        float offAcc_ = 0;

        // W11
        double anchorFallbackLeftMs_ = 0;    // > 0: a stall dropped anchor mode to Straight
        Gait gaitShown_ = Gait::Still;       // the moving gait at the last task evaluation (counts gaitKept)
        float corrFx_ = 0, corrFy_ = 0;      // low-passed residual correction (m/s)

        bool ragdollActive_ = false;
        bool haveJumpSeq_ = false;
        int32_t lastJumpSeq_ = 0;
        double sinceJumpMs_ = 1e9;
        bool wasJumping_ = false;

        LocoStats stats_;
    };

    // When a remote ped's aim task (TASK_AIM_GUN_AT_COORD / TASK_GO_TO_COORD_WHILE_AIMING_AT_COORD)
    // is (re)issued. Every issue restarts the aim blend, so it is done only when needed:
    //   - no aim task runs (the first frame of an aim, or something cancelled it);
    //   - the aim point moved more than max(moveM, distFrac * its distance from the ped) from the
    //     one last issued (a far point, e.g. the camera ray's 200 m end that travels with the
    //     sender, needs a long move before the angle is off);
    //   - the shown gait changed (the walk-aim variant carries the gait target and speed);
    //   - the controller issued a GoStraight this frame (the walk-aim task carries that target, so
    //     it must follow it or the ped stops at the old one);
    //   - never while airborne (a jump or fall: a task would cancel it).
    // Pure, single-threaded, one instance per remote ped.
    class AimGate
    {
    public:
        explicit AimGate(float moveM = 1.0f, float distFrac = 0.05f) : moveM_(moveM), distFrac_(distFrac) {}

        // true = issue the aim task now (and the gate remembers `aim` and `gait` as issued).
        bool ShouldIssue(const LocoVec& aim, const LocoVec& ped, Gait gait, bool haveTask, bool locoIssuedGoStraight,
                         bool airborne);

    private:
        float moveM_, distFrac_;
        LocoVec lastAim_;
        Gait lastGait_ = Gait::Still;
    };

}
