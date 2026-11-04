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
//  9. Sync A (docs/sync-specs/onfoot.md "Sync A"; found with the simulated ped RealPed in tests/locomotion_test.cpp):
//     (a) the animation rate is a closed loop: moveRate = (pose speed + along-track error / rateTcS) / (nominal x the LEARNED
//         scale of the gait); the scale is the ped's own measured speed over the rate that made it (one-pole, steady frames only);
//     (b) the push is the last resort: a moving ped is pushed only beyond residualDeadM (0.30 m, the ramp starts at the edge), a
//         standing one beyond stillDeadM (0.20 m), and not at all in the first startGraceMs (1.2 s) of a movement while the ped
//         is under startGraceMaxM (2.5 m) behind (the start animation is still getting up to speed);
//     (c) the push base is the ped's animation velocity, never our own last push: a GET_ENTITY_VELOCITY read equal to the last push
//         (0.05 m/s) is "contaminated" and the last clean read (300 ms) or a model of the animation is used instead;
//     (d) a stop is walked out: while the ped is more than settleM (0.35 m) from a stopped pose it keeps a Walk to the pose (the anchor
//         IS the pose, the straight target is re-issued once) for up to settleMaxMs (1.5 s), then stands; the anchor leads by the pose
//         speed with no floor (the 1.5 m floor ran the ped through the pose and the push pulled it back);
//     (e) a change UP in gait that the speed itself asks for is followed after upDebounceMs (150 ms), at once in the first
//         startWindowMs (400 ms) of a movement; a gait the sender's hint lifted still holds gaitDebounceMs;
//     (f) the first stall (a lost task) is noticed stallFirstMs (0.9 s) after its issue; stallRetryMs only after a stall re-issue; the
//         anchor fallback doubles for each stall in a row (20, 40, 80 ... s, cap 300 s) and the streak ends after 90 s without one.
//     LocoDiag (above) is the slide metric of the 10 s "engine: loco slide" line.
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

#include <cmath>
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

    // T2 (desync round 2): how the position error of a walking / running puppet is closed.
    //   Velocity: SET_ENTITY_VELOCITY (animation velocity + a correction). Its effect on a ped under a task is a MODEL (replaces the
    //             animation's velocity / adds to it / is ignored: unverified live); it is kept for jumps, ragdolls, swimming and the
    //             legacy tests.
    //   Nudge:    SET_ENTITY_COORDS_NO_OFFSET of a few millimetres per frame toward the pose (LocoOutput::nudge). Whatever the game does
    //             with velocities, a coordinate write is exact, so the error is bounded in every model of the game; the price is
    //             foot skate by the nudge speed, which the closed-loop move rate keeps small.
    enum class PushMode : uint8_t { Velocity, Nudge };

    // T2: the degradation ladder (LocoOutput::level). Each step down trades looks for certainty:
    //   Full      the configured mode (anchor), move rate, nudge
    //   Straight  TASK_GO_STRAIGHT_TO_COORD (the anchor task was lost / does not move the ped / is dropped)
    //   NoRate    + the move blend ratio carries the speed (SET_PED_MOVE_RATE_OVERRIDE does not scale the stride)
    //   Placed    + the pose is followed almost rigidly (the task does not move the ped at all): looks like a slide, never a drift
    enum class LocoLevel : uint8_t { Full = 0, Straight = 1, NoRate = 2, Placed = 3 };
    // Why the level changed (LocoOutput::levelWhy), for the engine's log line.
    enum class LocoLevelWhy : uint8_t
    {
        None,
        AnchorLost,       // the ped showed no gait while a moving one was commanded (the go-to-entity task is not running)
        AnchorStuck,      // the ped's own speed stayed under stuckRatio of the pose's for stuckMs: the task runs but does not carry it
        AnchorDragging,   // the nudge did more than nudgeShareMax of the distance for nudgeShareWindowMs: the task does not follow the anchor
        RateIneffective,  // the move rate sat at its limit with the ped still off in speed: the native does not scale the stride
        StraightStuck,    // a straight task re-issued after a stall stalled again: nothing moves the ped, follow the pose rigidly
        Recovered,        // a fallback timer ran out: the better level is tried again
        Shared,           // another puppet's controller found the same failure (LevelWitness): imposed here without waiting to see it
    };

    // R3 T2 (desync round 3): what the controller has learned about a coordinate write on this puppet. The live log (0.4.7) showed
    // "the task ran, the ped's own speed 0.00 m/s" under TWO different task types: either every coordinate write cancels the walk
    // (Harm), or the task does not move the ped at all (Harmless: the writes are not the cause). Unknown until a failure is seen with
    // writes flowing: a QUIET window (no coordinate write for quietWindowMs) then tells which.
    enum class WriteVerdict : uint8_t { Unknown, Harmless, Harm };
    // R3 T2: something the ENGINE has to do to the ped because no level of the ladder moved it (LocoOutput::repair). One per request,
    // the engine reports the number done back in LocoInput::repairsDone.
    //   ClearTasks   CLEAR_PED_TASKS_IMMEDIATELY and the flags that could pin a ped (freeze, collision, ragdoll, keep-task) put to defaults
    //   RespawnLean  the puppet is deleted and created again with the LEAN recipe (only invincible + heading + non-temporary events blocked)
    //   RespawnAlt   ... with the ALT recipe (a civilian ped type, no script-host flag, lean flags)
    enum class LocoRepair : uint8_t { None, ClearTasks, RespawnLean, RespawnAlt };

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
        float anchorLeadS = 0.8f, anchorLeadMinM = 0.0f, anchorLeadMaxM = 5.0f;   // Sync A: no floor (was 1.5 m: a stop was run through)
        float anchorFallbackMs = 20000.0f; // a stall in anchor mode: Straight for this long, then Anchor again;
                                           // Sync A: doubled for each stall in a row (cap anchorFallbackMaxMs), the streak ends after anchorStreakResetMs without one
        float anchorFallbackMaxMs = 300000.0f, anchorStreakResetMs = 90000.0f;

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
        // Sync A (docs/sync-specs/onfoot.md "Sync A"): what the simulated ped (tests/locomotion_test.cpp, RealPed) showed to be wrong.
        //  - rateLoop: the animation speed is a CLOSED loop. moveRate = (pose speed + along-track error / rateTcS) / the gait's
        //    nominal speed, and the nominal is LEARNED from the ped's own velocity (nominal x scale, scale = measured speed / rate,
        //    one-pole learnTauMs, only from steady frames: the guessed nominals were INFERRED and a wrong one was carried by the
        //    velocity push, i.e. by sliding, for the whole run).
        //  - residualDeadM / stillDeadM: while a moving gait is shown (a standing ped: stillDeadM) the push starts only beyond this error (the rate loop closes the rest);
        //    inside startGraceMs of leaving Still no push below startGraceMaxM (the start animation needs ~1 s to get up to speed:
        //    dragging it at +2 m/s over a half-speed stride was ~40% of that second).
        //  - settle: a stop does not stand the ped still while it is more than settleM from the pose (it walks the rest, up to
        //    settleMaxMs, until it is within settleDoneM); the anchor leads by the pose's speed with no floor, so a stop is not
        //    run THROUGH (the old 1.5 m floor overshot ~0.9 m and then pushed the ped back ~1 s in an idle pose).
        //  - upDebounceMs / startWindowMs: a change UP in gait is followed after upDebounceMs, at once within startWindowMs of
        //    leaving Still (the first frames of a start read a walking speed and held a Walk for 400 ms).
        //  - stallFirstMs: a lost task is noticed this long after its issue (stallRetryMs only after a stall re-issue).
        bool rateLoop = true;
        float rateTcS = 0.7f, rateErrMinM = -1.0f, rateErrMaxM = 2.5f;
        float learnTauMs = 1200.0f, learnSteadyMs = 600.0f, learnMinScale = 0.55f, learnMaxScale = 1.6f;
        float residualDeadM = 0.30f, stillDeadM = 0.20f;   // moving gait / standing ped: the push ramps up from beyond this error
        float startGraceMs = 1200.0f, startGraceMaxM = 2.5f;
        float settleM = 0.35f, settleDoneM = 0.20f, settleMaxMs = 1500.0f;
        float upDebounceMs = 150.0f, startWindowMs = 400.0f;
        float stallFirstMs = 900.0f;

        // T2: position nudge + self-checks (see PushMode / LocoLevel). Defaults keep the legacy behaviour (velocity push, no
        // self-checks) so the older tests stay what they were; the engine uses ShippedLocoConfig() (below).
        PushMode push = PushMode::Velocity;
        float nudgeDeadM = 0.12f;          // a moving ped is nudged only beyond this horizontal error
        float nudgeStillDeadM = 0.25f;     // a standing one (inside the dead band an idle ped is left alone)
        float nudgeTauMs = 250.0f;         // the excess over the dead band closes with this time constant
        float nudgeMaxMs = 3.0f;           // cap of the nudge speed (anti-fling; beyond it the persistent-error snap takes over)
        float nudgeStillMaxMs = 1.0f;      // cap while commanded Still (an idle pose slid sideways is the most visible skate)
        float nudgeStartGraceM = 1.0f;     // the first startGraceMs of a movement tolerate this much error (the start clip is still getting up to speed)
        float nudgeZBandM = 0.5f;          // |dz| beyond this is nudged too (capped zCorrMaxMs); inside it ground physics owns z
        bool  selfCheck = false;           // the degradation ladder (LocoLevel): measured with the ped's own displacement
        float stuckRatio = 0.35f, stuckMs = 1200.0f, stuckMinPoseMs = 1.5f;     // own speed under ratio * pose speed (pose over stuckMinPoseMs)
        float stuckSettleMs = 900.0f;      // ... and only this long after the movement, task or gait began
        float nudgeShareMax = 0.10f, nudgeShareWindowMs = 3000.0f;   // nudged distance / travelled distance over this window of STEADY motion
        float steadyAccMs2 = 1.5f;         // the pose's low-passed acceleration under this is steady motion (a start, stop or reversal is not)
        int   snapStormN = 3;              // this many failure snaps within 6 s are a stall (nothing carries the ped)
        float ratePinMs = 2000.0f;                                   // the regression must show "no effect" this long
        float rateSlopeMin = 0.25f, rateVarMin = 0.004f, rateProbeTauMs = 5000.0f, rateProbeMinMs = 1500.0f, rateProbeAccMs2 = 3.0f;   // regression of own speed on the rate
        float rateOffMs = 60000.0f, rateOffMaxMs = 600000.0f;        // NoRate for this long (doubling per failure in a row), then tried again
        float placedMs = 30000.0f, placedDurMaxMs = 300000.0f;       // Placed for this long (doubling), then tried again
        float placedTauMs = 60.0f, placedNudgeMaxMs = 8.0f, placedDeadM = 0.03f;   // the rigid follow of the Placed level

        float fastDownFrac = 0.0f, fastDownMs = 100.0f;   // T2: a pose under this fraction of the gait's down edge drops the gait after fastDownMs (0 = off)

        // R3 T2 (desync round 3), all of it only with selfCheck. Why: the 0.4.7 live log had a puppet whose tasks never gave a gait,
        // a ladder that changed level 7 times in 6.5 minutes, and 13 task issues per 10 s.
        //  - quiet window: a failure (lost / stuck / snap storm) seen while coordinate writes were flowing may be the WRITES cancelling the
        //    walk. Before a level is given up, the writes stop for quietWindowMs; if the ped then shows a gait (or moves on its own for
        //    quietNeedGoodMs) the writes are the cause: WriteVerdict::Harm, and the position error is then closed by single PULSES
        //    (one write per pulseMs, only beyond pulseMinErrM) instead of a write every frame. If it still shows nothing the writes are
        //    cleared (Harmless) and the ladder goes on. A working game proves the writes harmless silently: harmlessEvidMs of gait read
        //    and own speed >= stuckRatio of the pose's while writes flow.
        //  - the lost-task trigger needs the ped's own speed to agree (a ped that moves with its gait natives reading Still is not lost).
        //  - ladder: a level change holds levelHoldMs before the next change that is not a deeper step of the same failure; at most
        //    maxLevelChanges per puppet and session (then the level is frozen and a line says so).
        //  - re-issues: rule-based re-issues (turn / drift / arrive / stall) are spaced reissueMinMs, a big turn (turnBigDeg) only
        //    turnBigMinMs, none inside firstGaitWindowMs of an issue (the previous task has not had its chance to show a gait yet), and a
        //    forced one (snap, Reset) at least issueGapMs after the last of the same kind. Still <-> moving is never delayed.
        //  - repairs: a straight task that stalled twice with the writes cleared is asked of the ENGINE to repair (LocoRepair), up to
        //    maxRepairs per puppet; then the final Placed holds deadPlacedMs before a probation.
        bool  quietTrial = true;
        float quietRecentMs = 600.0f, quietWindowMs = 1300.0f, quietNeedGoodMs = 350.0f, quietAbortErrM = 3.0f;
        float harmlessEvidMs = 1500.0f;
        float quietInitialMs = 1550.0f;   // the puppet's FIRST moving task runs without a coordinate write this long (firstGaitWindowMs + a frame)
        float harmSettleMs = 1200.0f;     // with the verdict Harm a write's effect on the walk is given this long before the ped is judged
        float pulseMs = 1500.0f, pulseMinErrM = 0.9f, pulseMaxM = 1.5f;
        bool  lostNeedsLowSpeed = true;
        float levelHoldMs = 8000.0f;
        int   maxLevelChanges = 12;
        float reissueMinMs = 1000.0f, turnBigDeg = 70.0f, turnBigMinMs = 500.0f, firstGaitWindowMs = 1500.0f, issueGapMs = 600.0f;
        int   maxRepairs = 3;
        float deadPlacedMs = 120000.0f, deadPlacedMaxMs = 600000.0f;

        float velTauMs = 120.0f;       // one-pole low-pass of the pose velocity (W1 item 4)
        float steerDeadbandM = 0.08f;  // no steering under this horizontal error
        float ragdollMaxMs = 8.0f;     // cap of the whole velocity while ragdolling

        float snapBaseM = 5.0f, snapPerSpeedS = 5.0f;
        float offToleranceM = 3.0f, offLimitMs = 0.5f; // m, m*s

        float headingTauMs = 50.0f;
        float jumpGraceMs = 700.0f;
    };

    // T2: the config the ENGINE uses (apps/engine LocoCfg) and the tests of tests/t2_onfoot_tests.h measure: the position nudge, the
    // self-checks and the degradation ladder on. `anchor` picks the first level (Anchor or Straight). One definition, so the
    // numbers measured are the numbers shipped.
    LocoConfig ShippedLocoConfig(bool anchor);
    // R3 T2: `c` with every round-3 rule off = the round-2 (0.4.7) controller: no quiet window / write verdict, the lost task trusted on the
    // gait natives alone, 500 ms re-issue spacing, no first-gait window or forced-issue gap, no hold or cap on the ladder, no repair, the
    // short dead timers. The registry switch loco_r3=0 (engine) and the before / after tables of the tests use this one definition.
    LocoConfig Round2Behaviour(LocoConfig c);

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
        // R3 T2: LocoRepair steps the engine has already done on this puppet (the next one is asked for after this many).
        int repairsDone = 0;
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

        // T2. nudge: SET_ENTITY_COORDS_NO_OFFSET(ped, pedPos + nudgeDelta) once this frame (PushMode::Nudge); pedPos is the position
        // LocoInput carried. level / levelWhy: the degradation ladder; levelChanged is true on the frame it moved.
        bool nudge = false;
        LocoVec nudgeDelta;
        LocoLevel level = LocoLevel::Full;
        bool levelChanged = false;
        LocoLevelWhy levelWhy = LocoLevelWhy::None;
        bool rateClamped = false;   // with setMoveRate: the rate sits at moveRateMin / moveRateMax
        // R3 T2. repair: do this to the ped now (see LocoRepair). quiet: a quiet window is open (no coordinate write this frame).
        // writeVerdict / verdictChanged: what the controller knows about coordinate writes on this puppet. capHit: the per-puppet cap
        // of level changes was reached on this frame (the ladder is frozen from now on).
        LocoRepair repair = LocoRepair::None;
        bool quiet = false;
        WriteVerdict writeVerdict = WriteVerdict::Unknown;
        bool verdictChanged = false;
        bool capHit = false;
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
        // Sync A: stops that walked the rest to the pose before standing still, pushes skipped by the start grace, frames
        // whose velocity read was our own last push fed back (GET_ENTITY_VELOCITY returned what we set)
        uint32_t settles = 0, graceFrames = 0, contaminated = 0;
        // T2: nudge frames, level changes (and by LocoLevelWhy)
        uint32_t nudgeFrames = 0, levelChanges = 0, levelWhy[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        double nudgedM = 0, travelledM = 0;   // metres written by nudges / metres the ped moved while a moving gait was shown (cumulative)
        // R3 T2: quiet windows opened / ended with the ped showing a gait (writes harm) / ended without one; pulses written; repairs
        // asked; re-issues the spacing rules held back (or deferred); level changes the hold or the cap refused; frames the engine
        // read a gait (any) while a moving gait was commanded / the commanded moving frames (the gait read rate); task-to-first-gait
        // latency (sum of ms, count) of the issues that did get a gait
        uint32_t quietWindows = 0, quietHarm = 0, quietClear = 0, pulses = 0, repairs = 0, issuesHeld = 0, changesRefused = 0;
        uint32_t gaitReadFrames = 0, commandedFrames = 0, latencyN = 0;
        double latencySumMs = 0;
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
        // T2: pass on a level another puppet's controller found (the game is one game): the anchor task dropped (Straight) or the
        // move rate ignored (NoRate) for `ms`. Reported through the next Update's levelChanged like a level it found itself.
        void Degrade(LocoLevel to, LocoLevelWhy why, double ms);

        Gait CurrentGait() const { return gait_; }
        // The task the controller last issued (None until the first): what the ped is latched on, for the 10 s log line.
        LocoTask LastTask() const { return lastTask_; }
        float OffAccumulator() const { return offAcc_; } // m*s toward a persistent-error snap
        const LocoStats& Stats() const { return stats_; }
        // Sync A: the learned animation speed of a gait as a factor of its nominal (1 = as guessed), and the last move rate asked for
        float NominalScale(Gait g) const { return g == Gait::Walk ? nScale_[1] : g == Gait::Run ? nScale_[2] : g == Gait::Sprint ? nScale_[3] : 1.0f; }
        float LastMoveRate() const { return lastRate_; }
        // T2: the level now, and the ped's own speed (m/s, from its position differences net of our nudges; -1 until measured)
        LocoLevel Level() const { return level_; }
        // R3 T2: what is known about coordinate writes on this puppet, whether a quiet window is open, level changes so far (the cap),
        // the ladder frozen by the cap, and the mean time from a task issue to the first gait the ped showed (-1 = none measured)
        WriteVerdict Verdict() const { return writeVerdict_; }
        bool QuietOpen() const { return quietLeftMs_ > 0.0; }
        int LevelChangesTotal() const { return levelChangesTotal_; }
        bool LadderFrozen() const { return ladderFrozen_; }
        float MeanLatencyMs() const { return stats_.latencyN ? static_cast<float>(stats_.latencySumMs / stats_.latencyN) : -1.0f; }
        float SelfSpeed() const { return haveSelf_ ? static_cast<float>(std::sqrt(double(selfV_.x) * selfV_.x + double(selfV_.y) * selfV_.y)) : -1.0f; }

        // Gait inference with hysteresis: the gait after `current` sees `speed` (m/s).
        static Gait InferGait(Gait current, float speed, const LocoConfig& cfg);
        // The sender's gait (LocoInput::moveState) as a hint to the speed's: clamp(moveState,
        // inferred, min(Sprint, inferred + 1)); anything outside 0..3 leaves `inferred`.
        static Gait ApplyAuthority(Gait inferred, int moveState);

    private:
        LocoOutput UpdateImpl(const LocoInput& in);
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
        int anchorFailStreak_ = 0;           // Sync A: stalls in anchor mode in a row
        double sinceAnchorStallMs_ = 1e9;
        Gait gaitShown_ = Gait::Still;       // the moving gait at the last task evaluation (counts gaitKept)
        float corrFx_ = 0, corrFy_ = 0;      // low-passed residual correction (m/s)

        // Sync A
        float nScale_[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // learned nominal speed factor per gait
        float lastRate_ = 1.0f;              // the move rate asked for last frame
        double rateEmaNum_ = 1.0;            // low-passed applied rate (~ the ped's animation lag), the divisor of a learning sample
        double steadyMs_ = 0;                // time since the gait, the task or the settle state last changed
        double sinceMoveStartMs_ = 1e9;      // time since the shown gait left Still
        bool settling_ = false;
        Gait gaitPrev_ = Gait::Still;
        bool pushedLast_ = false;
        LocoVec lastPush_;                   // the velocity set last frame (when pushedLast_)
        LocoVec animEst_;                    // the ped's animation velocity: its own read, or a model of it when the read is our push
        bool lastIssueWasStall_ = false;
        double sinceCleanMs_ = 1e9;          // time since animEst_ was a clean read

        // T2: the ped's own displacement, the self-checks and the ladder
        bool havePrevPed_ = false;
        LocoVec prevPedAfter_;               // where the ped was after last frame's snap / nudge (the baseline of its own movement)
        bool haveSelf_ = false;
        LocoVec selfV_;                      // low-passed own velocity (position differences net of what we wrote)
        double lagMs_ = 0;                   // how long the ped's own speed has been far under the pose's
        double ratePinnedMs_ = 0;            // how long the move rate has sat at a limit with the speed still off
        double rateOffLeftMs_ = 0, placedLeftMs_ = 0;
        int rateFailStreak_ = 0, placedFailStreak_ = 0;
        double sinceRateFailMs_ = 1e9, sincePlacedMs_ = 1e9;
        double winNudgedM_ = 0, winTravelM_ = 0, winMs_ = 0;   // the nudge-share window
        LocoLevel level_ = LocoLevel::Full;
        double healthyMs_ = 0;               // moving frames without a stall / stuck event (ends a failure streak)
        LocoLevelWhy pendingWhy_ = LocoLevelWhy::None;
        // exponentially weighted regression of (own speed / gait nominal) on the applied move rate: slope ~ the learned scale
        // when the native scales the stride, ~ 0 when it is ignored (only a decision when the rate has varied)
        double rpW_ = 0, rpX_ = 0, rpY_ = 0, rpXX_ = 0, rpXY_ = 0;
        double rateIneffMs_ = 0;
        LocoVec vfPrev_; bool haveVfPrev_ = false; double accLp_ = 0;   // the pose's low-passed acceleration (m/s^2)
        int stallEvents_ = 0; double sinceStallEventMs_ = 1e9;           // straight-task stalls in a row (Placed from the second)

        // R3 T2
        WriteVerdict writeVerdict_ = WriteVerdict::Unknown;
        double sinceWriteMs_ = 1e9;          // time since the last coordinate write this controller asked for (nudge / snap / pulse)
        double quietLeftMs_ = 0, quietElapsedMs_ = 0, quietGoodMs_ = 0;
        bool quietActive_ = false, quietInitial_ = false, quietInitialDone_ = false, repairRespawn_ = false;
        double quietAbortErr_ = 0, quietInitialLeftMs_ = 0;
        int quietTries_ = 0;
        double harmlessEvidMs_ = 0;
        double pulseSinceMs_ = 1e9;
        double levelHoldLeftMs_ = 0;
        int levelChangesTotal_ = 0;
        bool ladderFrozen_ = false, capHitPending_ = false, verdictChangedPending_ = false;
        LocoRepair repairPending_ = LocoRepair::None;
        double sinceIssueGaitMs_ = -1;       // < 0: no issue waiting for its first gait; else ms since that issue
        double sinceForcedIssueMs_ = 1e9;
        LocoVec lastIssueDir_;               // the pose velocity direction of the last issue (a big turn is the one thing that may not wait)
        bool haveIssueDir_ = false;
        bool LadderMayChange(bool deeper);   // the hold and the cap (counts a refusal)
        int failSnaps_ = 0; double sinceFailSnapMs_ = 1e9; bool snapStorm_ = false;

        bool ragdollActive_ = false;
        bool haveJumpSeq_ = false;
        int32_t lastJumpSeq_ = 0;
        double sinceJumpMs_ = 1e9;
        bool wasJumping_ = false;

        LocoStats stats_;
    };

    // Sync A (docs/sync-specs/onfoot.md "Sync A"): the slide metric the next live log needs. One instance per remote ped,
    // fed once per DrivePeer after LocomotionController::Update, summarised and Reset() every 10 s for the "engine: loco
    // slide" line. Pure: the same code runs in locomotion_test against a simulated ped, so the number the log prints is the
    // number the tests measured.
    //   skate     = the distance our own velocity pushes moved the ped (|pushVel - pedVel| * dt, the part of the displacement
    //               the animation did not produce) / the distance the ped moved in total, over frames where the pose moves
    //               (> 1 m/s); 0 = the stride carries the ped, 1 = it is dragged
    //   idleSlide = metres pushed while the ped is commanded Still (a ped sliding in its idle pose)
    //   stops     = stop-starts: the ped's position-derived speed under 0.3 m/s for 200 ms while the pose moves over 1 m/s
    //   derivedSpeed / poseSpeed = mean over the frames the pose moves (the ped's speed from its positions vs the pose's)
    struct LocoDiagFrame
    {
        double dtMs = 0;
        double nowMs = 0;            // a monotonic clock: the ped's speed from positions is measured between two observations on it
                                     // (frames the controller did not run in, a gap over 250 ms, are not bridged)
        LocoVec pose, pedPos;        // the pose and the ped's position as read this frame (before the output is applied)
        float poseSpeed = 0;         // |pose velocity| horizontal, m/s
        Gait gait = Gait::Still;     // LocoOutput::gait
        bool pushed = false;         // LocoOutput::setVelocity
        LocoVec push;                // the velocity set (LocoOutput::velocity)
        LocoVec pedVel;              // the ped's own velocity read this frame
        bool havePedVel = false;
        bool anchorMode = false;
        LocoVec anchor;              // LocoOutput::anchorPos, valid while anchorMode
        // T2
        bool nudged = false;         // LocoOutput::nudge
        LocoVec nudge;               // LocoOutput::nudgeDelta (metres this frame)
        bool rateSet = false;        // LocoOutput::setMoveRate
        float rate = 1.0f;           // LocoOutput::moveRate
        bool rateClamped = false;    // LocoOutput::rateClamped
        float selfSpeed = -1.0f;     // LocomotionController::SelfSpeed() (the ped's own speed, m/s)
    };

    struct LocoDiagSummary
    {
        uint32_t frames = 0, movingFrames = 0, stops = 0, pushFrames = 0;
        double meanErrM = 0, maxErrM = 0;          // |pose - ped| horizontal
        double meanPushMs = 0, maxPushMs = 0;      // |pushVel - pedVel| m/s, over the frames a push was set
        double skate = 0;                          // see above, 0..1
        double idleSlideM = 0;                     // metres pushed while commanded Still
        double derivedSpeedMs = 0, poseSpeedMs = 0;
        double meanAnchorM = 0, maxAnchorM = 0;    // ped -> anchor distance (anchor mode frames only)
        uint32_t anchorFrames = 0;
        // T2: nudges (m/s of the position written), the move rate applied (min / mean / max, share of frames at a limit)
        uint32_t nudgeFrames = 0;
        double nudgedM = 0, meanNudgeMs = 0, maxNudgeMs = 0;
        uint32_t rateFrames = 0;
        double rateMin = 0, rateMean = 0, rateMax = 0, rateClampFrac = 0;
        double selfSpeedMs = 0;                    // mean own speed over the moving frames (net of nudges)
    };

    class LocoDiag
    {
    public:
        void Observe(const LocoDiagFrame& f);
        LocoDiagSummary Summary() const;
        void Reset() { *this = LocoDiag(); }

    private:
        uint32_t frames_ = 0, moving_ = 0, stops_ = 0, pushFrames_ = 0, anchorFrames_ = 0;
        double errSum_ = 0, errMax_ = 0, pushSum_ = 0, pushMax_ = 0;
        double pushDist_ = 0, totalDist_ = 0, idleSlide_ = 0;
        double derivedSum_ = 0, poseSum_ = 0, anchorSum_ = 0, anchorMax_ = 0;
        uint32_t nudgeFrames_ = 0, rateFrames_ = 0, rateClamped_ = 0, selfN_ = 0;
        double nudgeDist_ = 0, nudgeSpeedSum_ = 0, nudgeSpeedMax_ = 0, rateSum_ = 0, rateMin_ = 0, rateMax_ = 0, selfSum_ = 0;
        bool havePrev_ = false;
        LocoVec prevPed_;
        double prevNowMs_ = 0;
        double stuckMs_ = 0;
        bool stuckCounted_ = false;
    };

    // R3 T2 (desync round 3): the gait DIAGNOSTIC. "The task ran but the ped showed no gait" was all the 0.4.7 log could say; this puts
    // what the game reports about the puppet next to the pose, per puppet, per 10 s: which gait the IS_PED_* natives read while a moving
    // one is commanded, the ped's speed three ways (from its positions including our writes, GET_ENTITY_SPEED, own displacement net of
    // our writes), the move blend ratio set, the coordinate writes, the task issues. Pure: the engine feeds one frame per DrivePeer and
    // prints the summary; the same code runs in tests/r3_t2_tests.h.
    struct GaitDiagFrame
    {
        double dtMs = 0, nowMs = 0;
        Gait commanded = Gait::Still;   // LocoOutput::gait (what the controller shows)
        Gait read = Gait::Still;        // World::PedGait (IS_PED_SPRINTING / RUNNING / WALKING)
        float poseSpeed = 0;            // horizontal m/s
        float ownSpeed = -1;            // LocomotionController::SelfSpeed() (net of our writes), < 0 = none yet
        float entitySpeed = -1;         // GET_ENTITY_SPEED, < 0 = unreadable
        LocoVec pedPos;                 // GET_ENTITY_COORDS this frame (speed from positions, between two frames)
        bool wrote = false;             // a coordinate write went out on the ped this frame (nudge, snap, pulse)
        bool issued = false;            // a gait task was issued this frame
        bool blendSet = false;          // SET_PED_DESIRED_MOVE_BLEND_RATIO this frame
        float moveBlend = 0;
        int pedStill = -1;              // IS_PED_STILL: 1 / 0, -1 = not asked
        bool ragdoll = false;           // IS_PED_RAGDOLL
    };

    struct GaitDiagSummary
    {
        uint32_t frames = 0, commandedFrames = 0;            // frames observed / frames a moving gait was commanded
        uint32_t readStill = 0, readWalk = 0, readRun = 0, readSprint = 0;   // the IS_PED_* gait on the commanded frames
        uint32_t stillTrue = 0, stillAsked = 0, ragdollFrames = 0, writeFrames = 0, issues = 0, blendFrames = 0;
        double poseMs = 0, ownMs = 0, entityMs = 0, posMs = 0;   // means over the commanded frames that have the value
        uint32_t ownN = 0, entityN = 0, posN = 0;
        double blendMean = 0;
        double commandedShare() const { return frames ? double(commandedFrames) / frames : 0.0; }
        double readShare(uint32_t n) const { return commandedFrames ? double(n) / commandedFrames : 0.0; }
    };

    class GaitDiag
    {
    public:
        void Observe(const GaitDiagFrame& f);
        GaitDiagSummary Summary() const;
        void Reset() { *this = GaitDiag(); }

    private:
        GaitDiagSummary s_;
        double poseSum_ = 0, ownSum_ = 0, entitySum_ = 0, posSum_ = 0, blendSum_ = 0;
        bool havePrev_ = false;
        LocoVec prevPos_;
        double prevNowMs_ = 0;
    };

    // T2: the game is one game for every puppet. A failure two DIFFERENT puppets' controllers report within witnessMs (the anchor
    // task that does not carry the ped, the move rate that does not scale the stride) is machine-wide: it is imposed on every
    // puppet (LocomotionController::Degrade) for imposeMs, so the next puppet does not spend its own seconds finding out. One
    // puppet alone (stuck in geometry, a bad spawn) never degrades the others. Pure; the engine owns one instance.
    class LevelWitness
    {
    public:
        // true when this report turned the finding machine-wide
        bool Report(LocoLevelWhy why, int id, double nowMs);
        // ms left of an imposed level (Straight or NoRate), 0 = none
        double ActiveMs(LocoLevel level, double nowMs) const;
        void Reset() { *this = LevelWitness(); }
        uint32_t Globals() const { return globals_; }
        static constexpr double kWitnessMs = 60000.0, kImposeStraightMs = 45000.0, kImposeNoRateMs = 120000.0;
    private:
        struct Slot { bool have = false; int id = -1; double atMs = 0, untilMs = 0; };
        Slot slot_[2];   // 0 = anchor failures (-> Straight), 1 = rate ineffective (-> NoRate)
        uint32_t globals_ = 0;
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
