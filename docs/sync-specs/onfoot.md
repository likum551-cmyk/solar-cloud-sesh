# On-foot movement and animation sync spec, 2026-11-02

Design agent output (read-only; line numbers as of 2026-11-02). INFERRED = verify. Order by player impact. Items 1-4 are libs/sync only (no protocol change): ship first.

## 1. Remote peds slide in an idle pose (gait authority)

Cause: sender `move = PedGait()` (engine.cpp:3827; hook.cpp:4205-4210: sprint, else run, else walk, else 0); swim / climb / ladder / cover / crouch / aim-strafe likely report 0 (INFERRED). Receiver passes it as moveState (engine.cpp:2562); locomotion.cpp:84 trusts it; :180-193 issues TASK_STAND_STILL while :143-172 pushes the ped at pose speed. A one-sample flap of `move` fires StandStill then GoStraight 50 ms later.

Fix (LocomotionController::Update): `inferred = InferGait(speed)`, `gait = clamp(auth, inferred, min(3, inferred+1))`; downgrade to Still only after speed < idleDown for `LocoConfig::stillHoldMs` (150 ms). engine.cpp:2564: set `in.jumping` when kPfFalling is set (sent, never read).

Test (locomotion_test.cpp TestAuthoritativeGait): (a) moveState 0, vel 1.5 m/s, 2 s at 60 fps -> standStill == 0, goStraight >= 1, gait Walk (today standStill 1, goStraight 0); (b) moveState 2 at 4.5 m/s with one 0 frame -> no StandStill.

## 2. Task churn (~2 restarts per second)

Cause: locomotion.cpp:213 `arrive = max(0.5, 0.5*lead)`, lead = |v| * 1 s: target "reached" after ~0.5 s at any speed; with the 200 ms floor ~2 re-issues/s (INFERRED ~20 per 10 s straight running). Gait flips re-issue at once (:204), gait mismatch every 500 ms (:215). Fixed move blend ratio 1/2/3 (:31) while real speed within a gait varies ~1.5x -> wrong anim speed, error grows, persistent snaps (INFERRED). Existing test accepts 5..30 issues per 10 s (locomotion_test.cpp:229).

Fix: leadS 2.5 s clamped 3..12 m; arrive = 0.25 * lead; minReissueMs 500; re-issue only on direction change > 25 deg, drift > 1.0 m, debounced gait class change, or remaining < 1 m. Match anim speed with SET_PED_MOVE_RATE_OVERRIDE (speed / nominal(gait) clamped 0.8..1.25; hash needed); fallback quantised move blend ratio in 0.25 steps re-issued at most every 400 ms.

Test: straight 4.5 m/s for 10 s at 30/60/144 fps -> goStraight <= 4 (today ~20), counts within 2 across frame rates.

## 3. Reset() snaps the ped onto the delayed pose

Cause: Reset() clears started_ (locomotion.cpp:37-44) so :112 always snaps; called on aim end (engine.cpp:2541-2545), emote end (:2457), leaving a vehicle (:2532, :2939), drive-mode change (:2492): pop of delay*speed (~0.6 m at a 4 m/s run).

Fix: Reset() = re-issue only (keeps started_, sets forceIssue_); new ResetHard() = old behaviour, for a new ped. The distance snap at :112 still covers real divergence.

Test: after Reset() with the pose 0.6 m ahead -> !o.snap && o.task == GoStraight (edit the existing check at locomotion_test.cpp:279).

## 4. Velocity noise, deadband, ragdoll follow

Cause: sender velocity is raw GET_ENTITY_VELOCITY (engine.cpp:4017), stride ripple INFERRED; feeds the steer (locomotion.cpp:158) and lead target (:198) unfiltered. Ragdoll gets no velocity (:143 skips) -> falls in place, then persistent-error snap.

Fix: one-pole low-pass of in.vel (tau 120 ms); no steer when |e| < 0.08 m; while ragdolling output velocity v + gain*e capped at 8 m/s, no tasks, no heading.

Tests: +-1 m/s ripple at 2 Hz -> RMS of o.velocity vs mean < 0.35 m/s (today ~0.7 INFERRED); ragdoll with pose v 6 m/s -> setVelocity with vx ~6 (today false).

## 5. Pipeline lag: sample time = server receive time

Cause: sends on frame boundaries (engine.cpp:4000-4004); the worker takes a single-slot state (join.cpp:173-176) but sleeps in Poll(10) (join.cpp:230): 0-10 ms uplink delay per sample. Server timestamps at processing (server.cpp:874), keeps only the newest sample (replication.cpp:210-216), derives ageMs from receive time (:309). A 30 Hz sender into a 20 Hz tick gives alternating 33/67 ms placed spacing; TypicalGapMs (smoother.cpp:147-154) takes the upper median 67 ms: delay = 67 + p95 (30) + 25, matching the observed 115-160 ms. Existing tests feed ideal tick-aligned samples (sync_test.cpp:127-148).

Fix: (a) Poll(2) while joined (no protocol change). (b) v5: PlayerState.sampleMs u16 (+2 B, +60 B/s up per client); new pure libs/sync/sampleclock.h unwraps it and keeps off = min(recv - sample) over a 2 s window; server ageMs = tickMs - (sample + off), clamped 0..255 and within 100 ms of the receive-time placement. Reset the estimator on a gap > 30 s. (c) server.json tickHz 30 option (gap 33 ms): evaluate in the harness first.

Tests (sync_test.cpp MakeSenderStream + TestSenderPipeline): 100 fps frame-quantised 30 Hz sender, worker wait U[0,10] ms, uplink jitter, 20 Hz server with ages, downlink jitter, 2% loss; ground truth = sample creation time. p95 placement error <= 2 ms (today >= 5 INFERRED), speed ripple (stddev/mean of per-frame step) <= 3%. Direct SampleClock unit test. Calibrate thresholds from the first printed run.

## 6. Missing animation states

| State | Sender today | Receiver today |
|---|---|---|
| Entering a vehicle | never sent (engine.cpp:3810) | move >= 4 = placement only (:2520-2530) |
| Exiting | none | CLEAR_PED_TASKS_IMMEDIATELY pop (:2932) |
| Climb/vault, falling, parachute | flags sent (:3838-3840) | never read |
| Ladder, reload, cover, stealth, swim, melee | not sent | not read |
| Ragdoll | sent | held, no follow (item 4) |
| Aim, weapon, shots, emotes | sent | work |

Tier A (no new natives, no wire change): falling = jumping (item 1); while kPfClimbing / kPfParachute pause the controller, PlacePeer, collision off (SET_ENTITY_COLLISION); on unseat TASK_LEAVE_VEHICLE flags 0 with a 1.5 s loco pause instead of CLEAR_PED_TASKS_IMMEDIATELY.

Tier B (v5, 0 extra bytes): kPfSwimming = 1<<13, kPfMelee = 1<<14 (bit 15 reserved, kPfKnown = (1<<15)-1); kPfReloading, kPfStealth, kPfCover already exist. Entering/exiting: move 5/6 plus optional u8 vehId (+1 B only when 5 or 6), extension bit kExtVeh = 64 (worldcodec.h:37-38). Entering detected with GET_VEHICLE_PED_IS_TRYING_TO_ENTER; receiver TASK_ENTER_VEHICLE (both in the table); exiting set from the link's Exit command. Seat warp stays as the fallback after 2.5 s. Update qwire.h:53-56, protocol.cpp, worldcodec, quant_test, netcode_test, docs/api/protocol.md.

Natives, hash needed (native:: constant + kCrossmap3889 row + GAGE_NEW_NATIVE): IS_PED_RELOADING + TASK_RELOAD_WEAPON (finite clip, not infinite), GET/SET_PED_STEALTH_MOVEMENT, IS_PED_SWIMMING, IS_PED_IN_COVER + TASK_PUT_PED_DIRECTLY_INTO_COVER, IS_PED_PERFORMING_MELEE_ACTION, SET_PED_MOVE_RATE_OVERRIDE. Melee / climb / vault clips via TASK_PLAY_ANIM: dictionary + clip names from a game asset dump only, never invented.

Tests (gameplay_test.cpp): round trip of every new bit and field; decoder rejects reserved bits.

## 7. Aim / walk-aim restarts

Aim task re-issued every 500 ms or on any loco task (engine.cpp:2599). Re-issue only when the aim point moves > 1 m or the gait class changes; never over a jump or fall.

## 8. Measurable targets (asserted in tests/)

- Smoother sender pipeline (6 m/s circle, 100 fps sender, 2% loss, uplink jitter <= 8 ms, downlink <= 10 ms): p95 placement error <= 2 ms; speed ripple <= 3%; p95 position error vs truth <= 0.10 m; max frozen interval <= 100 ms (frozen = < 1 cm/frame while truth speed > 1 m/s).
- Loss stress (new Metrics.maxFrozenMs): 10% loss, +-15 ms jitter -> <= 150 ms frozen; 30% loss -> <= 300 ms.
- Loco closed loop (SimPed anim-driven mode): after 2 s on a 4.5 m/s circle p95 distance <= 0.5 m, max <= 1.0 m; 0 persistent snaps in 60 s; GoStraight <= 0.4/s straight, <= 1.0/s circle; 0 StandStill above 0.5 m/s; 0 snaps on Reset().
- Latency: shown-pose age behind sample time <= 200 ms at jitter 10 ms; with tickHz 30, gap 33 ms and delay <= 100 ms.

## Sequencing

Items 1-4 (libs/sync only) first; then 5(a)+(c) and 6 Tier A; then the v5 bump with 5(b) and Tier B. Server and client redeploy together (docs/api/protocol.md:751).
