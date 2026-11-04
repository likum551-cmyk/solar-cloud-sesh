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

## W11 (0.4.4): no re-issue on gait changes, anchor steering, residual push

Live 0.4.3 still issued 8-12 TASK_GO_STRAIGHT_TO_COORD per 10 s (first of a gait 6-8, turn 4-6, drift 3-5). Implemented in libs/sync/locomotion.* (header section 8), tests in tests/locomotion_test.cpp (TestW11):
- A Walk / Run / Sprint change is carried by SET_PED_DESIRED_MOVE_BLEND_RATIO on the running task (`LocoOutput::setBlend`), never a re-issue; bands 3.1 / 2.1 and 6.1 / 5.3 m/s, hold 400 ms; the straight path's drift rule counts only the lateral drift (a speed change moved the anchor along the track and re-issued).
- `loco_mode=anchor` (default, HKCU value like peer_drive, read at Welcome): TASK_GO_TO_ENTITY once at an invisible, collision-less, frozen, non-networked helper ped moved to pose + direction * lead (0.8 s of travel, 1.5..5 m) every frame; only a lost task re-issues (stall rule); a stall drops that ped to the straight path for 20 s; no anchor entity or an aim task acts as straight for the frame. `loco_mode=straight` keeps the old path.
- Velocity push: with the ped's own velocity (GET_ENTITY_VELOCITY) only the residual error is pushed on top of it, low-passed (150 ms) and capped at 2 m/s, nothing inside the 0.08 m dead band. Jumps, ragdoll and swimming keep the pose-velocity push.
- INFERRED, to confirm live: that TASK_GO_TO_ENTITY on a frozen invisible ped follows a moved target without pathing detours, and that SET_PED_DESIRED_MOVE_BLEND_RATIO on a running task changes its speed. The 10 s `engine: loco` line shows anchor on/off, go-to-entity issues, gait changes carried, stall fallbacks.

## Sync A (0.4.6): why the receiver still slid, and what changed

GI after 0.4.4/0.4.5: "players slide". W11 was never run in a game, so this is a code audit plus a simulation (`tests/locomotion_test.cpp`: `RealPed`, `RunSlide`, `SlideScenarios`; only what the code lets a simulator model: a lagged animation speed with a stride ripple, limited turn rate, go-to arrival, true nominal speeds the controller only guesses, three readings of SET_ENTITY_VELOCITY, a plain 30 Hz sample interpolator for the pose). Not modelled: real GTA clips, foot plants, navmesh, collision, the real smoother.

Slide metric (`LocoDiag`, the same code runs in the test and in the engine): skate = metres our velocity pushes moved the ped / metres it moved while the pose moves; idle slide = metres pushed while the ped is told to stand; stop-starts; pose-ped error; ped-anchor distance.

Findings (old logic, simulated): (1) a stop ran the ped THROUGH the pose (anchor lead floor 1.5 m, the ped runs at full gait until the stand-still) and the residual push then dragged it back ~1 s in an idle pose: 1.5 m of idle slide per stop of a run, 4.6-7.4 m in a 20 s stop-and-go; (2) the animation speed was open loop on INFERRED nominals: a ped 15% off was carried by the push for the whole run (skate 14-16%); (3) the push base is GET_ENTITY_VELOCITY: if the game keeps the velocity we set, the push integrates every frame (ped 2-2.7x the pose speed, 6 m off, a snap a second); (4) a start was dragged at +2 m/s over a half-speed stride (~40% skate for the first 1.5 s) and held a Walk for 400 ms because the first frames of a start read a walking speed; (5) a lost go-to-entity task (frozen invisible anchor not followed) dragged the ped at 2 m/s in an idle pose for 2.5 s at every start, then every 20 s again.

Changes (`libs/sync/locomotion.*`, see the header's item 9): closed-loop move rate with learned nominals; push dead band 0.30 m (0.20 m standing) and a 1.2 s start grace; contaminated-read guard; settle (a stop is walked out) and an anchor lead without a floor; faster up-debounce / start window; first stall after 0.9 s and a doubling anchor fallback.

Simulated result (27 runs: 9 scenarios x animation x1 / x0.85 / x1.15): worst skate 6.4% (old 25%), idle slide of a stop-and-go 0.05-1.2 m (old 4.6-7.4 m); with a persisting velocity read the ped stays on the pose (old: 2.0-2.7x speed, 12-17 snaps); with a dead go-to-entity task p95 error 0.06 m (old 4.4 m). The reversal and teleport scenarios (a discontinuous pose) are not improved and not asserted.

Next live test: the new 10 s line `engine: loco slide id=... skate X%, idle slide Y m, speed from positions A vs pose B, ..., learned animation speed x walk / run / sprint, move rate`. Healthy: skate under ~5%, idle slide under ~0.5 m, speed from positions within 5% of the pose speed, stop-starts 0, learned factors steady (a run near 1.0 means the guessed nominals were right), contaminated reads 0 (if > 0 the game keeps our velocity: the guard works but tell us), tasks 1 per 10 s, anchor stalls 0.

## Sequencing

Items 1-4 (libs/sync only) first; then 5(a)+(c) and 6 Tier A; then the v5 bump with 5(b) and Tier B. Server and client redeploy together (docs/api/protocol.md:751).

## T2 (desync round 2, 0.4.7 candidate): on-foot sliding and puppet animations, designed for a game nobody has measured

GI after 0.4.6: "Peds slide; animations are not in place on other players' screens". There is still no live log after 0.4.3. Everything 0.4.5/0.4.6 added (go-to-entity on a frozen invisible anchor, the closed-loop move rate, the velocity push on top of the ped's own velocity) was verified against a MODEL of the game (T0 sec 9). T2 does not add a fourth model; it makes the controller correct in every model it can imagine, and makes it find out which one is true while it runs.

### What was unknown (the axes of tests/t2_onfoot_tests.h)
| axis | values |
|---|---|
| TASK_GO_TO_ENTITY on the moved anchor | followed / followed at 60% of the commanded speed / never moves the ped / no task moves the ped |
| SET_ENTITY_VELOCITY on a ped under a task | replaces the animation's displacement / adds to it / is ignored |
| SET_PED_MOVE_RATE_OVERRIDE | scales the stride / is ignored |
| turning | arcs round at speed / pivots on the spot |
| true stride vs the 1.5 / 4.5 / 7 m/s guess | x1, x0.85, x1.15 |
40 models (120 with the stride factors) x 12 scenarios (steady walk/run/sprint, gait changes, circle, S-curve, run 3 s / stop 2 s, walk 2 s / stop 2 s, ramp and curb, **a realistic reversal (stop, turn on the spot, go) at a run and a walk, a zigzag**, plus the stress case "instant 180 flip at full speed"). `T2_EXPLORE=1 T2_SWEEP=1 locomotion_test` prints the class table below; `T2_BYMODEL=1` one line per model.

The old "reversal" scenario flipped the direction at FULL speed (its speed dip was centred on t = 4k, the flip on t = 4k+2: a 9 m/s velocity step no player can make). It stays as the stress row; the 20% skate / 4 m error that was quoted for "reversals" is that row. A real stop-turn-go at a run: 0.4.6 29% skate / 2.2 m error under the "velocity adds" model, 8.9% / 1.09 m under "replace"; T2: 8% / 0.55 m.

### Findings (simulated; live-unverified)
1. The velocity push is a model. Under "velocity adds to the animation" the ped runs at 1.2-1.7x the pose speed (gait changes: skate 13.8%, p95 1.37 m; instant flip: 4.1 m); under "ignored" it does nothing and the error grows to 3-5 m (rate also ignored). 0.4.6 is right only if the game replaces the velocity. A coordinate write (SET_ENTITY_COORDS_NO_OFFSET, the call the snap and the anchor already use) is exact in every model.
2. Stopping and reversing: the Run -> Walk debounce kept the faster gait 400 ms after the pose had stopped (the smallest Run stride, 2.7 m/s, ran 1.1 m past it).
3. Nothing in 0.4.6 noticed a go-to-entity task that "runs" but carries nothing (the ped reads Run, travels 40%), a stride that does not respond to the rate, or a ped that has to be snapped again and again.

### What T2 does (libs/sync/locomotion.*, no wire change)
- **Position nudge** (`PushMode::Nudge`, the shipped default): the horizontal error beyond a 0.12 m dead band (0.25 m for a standing ped) closes with a 250 ms time constant, capped at 3 m/s (1 m/s standing), as `LocoOutput::nudge` = SET_ENTITY_COORDS_NO_OFFSET(ped, pedPos + delta). No velocity write on foot (jumps, ragdoll and swimming keep the velocity path). The first 1.2 s of a movement tolerate 1 m (the start clip). `loco_push=velocity` restores the 0.4.6 push.
- **Own speed**: the ped's displacement net of what we wrote (`SelfSpeed()`), the one measurement that needs no model of GET_ENTITY_VELOCITY. It feeds the learned stride scale and the checks below.
- **Degradation ladder** (`LocoLevel`, `selfCheck`, `loco_selfcheck=0` turns it off; every step is logged as `engine: loco level`): 0 full (anchor task + move rate + nudge) -> 1 straight task (`AnchorLost`: the ped reads no gait; `AnchorStuck`: own speed under 35% of the pose's for 1.2 s, or three snaps in 6 s; `AnchorDragging`: the nudges had to carry more than 10% of the distance in 3 s of steady motion) -> 2 speed carried by the blend ratio on the running task, no re-issue (`RateIneffective`: the regression of own speed / gait nominal on the applied rate has a slope under 0.25 for 2 s while the rate varied) -> 3 rigid follow (`StraightStuck`: the straight task stalled twice in 15 s; nudge tau 60 ms, cap 8 m/s). Each fallback has a timer that doubles per failure in a row and then tries the better level again (`Recovered`). Steady motion (pose acceleration under 1.5 m/s^2, 1.2 s after a start) is the only evidence for the drag test, so starts, stops and reversals never degrade anything.
- **Witness** (`LevelWitness`): two DIFFERENT puppets reporting the same anchor or rate failure within 60 s impose the level on every puppet (45 s / 120 s), so the third player does not spend his own seconds finding out; one puppet alone never degrades the others.
- **Fast down-shift**: a pose under 60% of the gait's down edge drops Run -> Walk / Sprint -> Run after 100 ms instead of 400 ms.
- **Diagnostics**: `engine: loco slide` gets a `| T2:` tail (level, nudge frames / metres / mean / max m/s / share of the distance, move rate applied min / mean / max and the share of frames at a limit, own speed vs pose speed, level changes by reason, push mode); `engine: loco level` on every ladder step (60 per session); `engine: anims onfoot (T2)` per 10 s.

### Result (sim, mean of the scored scenarios; skate / error p95 in m)
| design | game works as assumed | stride ignored | go-to follows at 60% | go-to never moves the ped |
|---|---|---|---|---|
| 0.4.6 anchor + velocity push | 2.9% / 0.46 | 4.5% / 1.37 | 11.6% / 1.68 | 11.6% / 1.66 |
| 0.4.6 `loco_mode=straight` | 6.5% / 0.83 | 5.3% / 1.67 | 5.9% / 1.25 | 5.9% / 1.25 |
| **T2 anchor + nudge + ladder (default)** | **1.5% / 0.27** | 3.8% / 0.34 | 13.9% / 0.64 | 11.1% / 0.42 |
| T2 straight + nudge + ladder | 5.8% / 0.38 | 7.6% / 0.44 | 6.7% / 0.41 | 6.7% / 0.41 |
The last two columns of the anchor row are mostly the seconds before the ladder knows (a 20 s run, ~2-4 s of detection); the witness removes that for the second and later puppets. The straight task has no model risk but it has the task churn (6-9 re-issues per 10 s) that the skate figure does not count. The default is therefore the anchor with the ladder; `loco_mode=straight` is one registry value away.
Where the game works as 0.4.6 assumed: T2 error p95 0.28 m (0.39), skate 1.8% (1.3%): the same. Where it does not (30 models): mean error p95 0.48 m (1.48 m). No level change in 228 runs of a working game.

### Live A/B recipe for GI (HKCU\Software\GAGE-MP string values, read at every Welcome: rejoin to apply)
1. Default (`loco_mode` unset, `loco_push` unset): two players, walk / run / sprint / stop-and-go / U-turns for 60 s. grep `engine: loco slide` and `engine: loco level`.
2. `loco_mode=straight`, same walk. 3. `loco_push=velocity`, same walk. 4. (only if 1 shows a problem) `loco_selfcheck=0`.
Decision numbers (per puppet, 10 s line): healthy = `level 0`, skate < 5%, nudge share < 5% of the distance, own speed within 10% of pose speed, error mean < 0.3 m / max < 1 m, move rate at a limit < 30% of frames, level changes 0, ped-anchor distance about the lead (0.8 s of travel: 1.2 m walk, 3.6 m run, 5 m sprint; much more = the ped is not following).
- `level changes` with `anchor-lost` / `stuck` / `dragging` on the FIRST seconds of every puppet: TASK_GO_TO_ENTITY on the frozen anchor does not work on that build: ship `loco_mode=straight` as the default.
- `rate-ineffective`: SET_PED_MOVE_RATE_OVERRIDE does not scale the stride: drop the rate loop.
- `own speed` near 0 for a walking puppet under the default but normal with `loco_push=velocity`: the per-frame coordinate write breaks the walk animation: ship `loco_push=velocity`.
- skate high under 2 but fine under 1: the anchor is better than straight on that build; the reverse: straight.

### Animations (no per-ped animation id on the wire; none added)
| state | sender | wire | receiver | verdict |
|---|---|---|---|---|
| walk / run / sprint, start, stop | IS_PED_WALKING.. | `move` (a hint) + velocity | gait from the smoothed speed, move rate / blend ratio, settle at stops | T2 above; start / stop / turn clips are the game's |
| jump | IS_PED_JUMPING edge | jumpSeq + kPfJumping | TASK_JUMP on the step; falling = jumping (no task) | audited (`jump`) |
| land | none | none | the jump window ends, the gait task is re-issued | ok |
| reload | IS_PED_RELOADING | kPfReloading | TASK_RELOAD_WEAPON once | audited |
| crouch | GET_PED_STEALTH_MOVEMENT | kPfStealth | SET_PED_STEALTH_MOVEMENT | audited |
| swim | IS_PED_SWIMMING | kPfSwimming | no swim task: the game swims a ped in deep water; velocity path | audited (`swim`, 2.5 s window) |
| ragdoll | IS_PED_RAGDOLL | kPfRagdoll | SET_PED_TO_RAGDOLL | audited |
| climb / vault | IS_PED_CLIMBING / VAULTING | kPfClimbing / kPfVaulting | pre-place on the sender's pose, TASK_CLIMB (0d counters) | unchanged |
| cover | IS_PED_IN_COVER | kPfCover | TASK_PUT_PED_DIRECTLY_INTO_COVER (0d counters) | unchanged |
| aim / strafe / shoot | aim flags | kPfAiming + aim point | aim tasks | unchanged |
| melee, emotes | | kPfMelee / PlayAnim | swing clips, TASK_PLAY_ANIM | sync batch E |
| **ladder** | none | kPfLadder exists (bit 8), never set | never read | **missing: the puppet slides up the ladder in a stand pose**. Needs natives (below); the wire bit already exists, so no wire change |
| **parachute** | IS_PED_IN_PARACHUTE_FREE_FALL | kPfParachute | placed, collision off, no parachute task | missing: needs natives |
| enter / exit a vehicle | | move 5 / 6 | | T1 |
No state found that needs a new wire field AND can be played with a native we have: a wire change would only carry what no puppet can show. `engine: anims onfoot (T2)` prints per 10 s, for jump / reload / stealth / swim / ragdoll: asked, SHOWN (the game's own readback said yes within the kind's window, average ms), NOT shown, unreadable. A kind that is asked but never shown is an animation the other players do not see.

## R3 T2 (desync round 3, client 0.4.8 candidate): "the task ran, the puppet showed no gait, own speed 0.00"

Live (0.4.7, GI's `gage.log`, puppet id=2): `TASK_GO_TO_ENTITY` on the anchor, then `TASK_GO_STRAIGHT_TO_COORD`, then a rigid follow: the ped read no gait and its own speed (net of our writes) was 0.00-0.48 m/s against a pose of 1.05-5.64 m/s; 13 go-tasks per 10 s, skate 90%, 7 level changes in 6.5 minutes. `TASK_CLIMB` did not start either.

### What reading the code ruled out (evidence level: code only; nothing here was seen in a game)
- `World::SpawnRemotePed` (hook.cpp, `SpawnRemotePed`) creates the puppet unfrozen (`FreezeEntityPosition(ped, false)`), collision on, invincible, events blocked, ragdoll off, keep-task, config flag 188. 188 is `PED_FLAG_DISABLE_HURT` (alt:V ped flag list); none of these is documented to stop scripted tasks. Only the anchor is frozen (`SpawnPed`), and the straight task, which has no anchor, failed the same way.
- The per-frame coordinate write on the ped (`engine.cpp` DrivePeer `if (o.nudge && !o.snap)`) is the one thing applied to the puppet every frame in the 0.4.7 default. But 0.4.3-0.4.6 had no coordinate write (a velocity push) and GI already reported "players slide in an idle pose", and the first 0.4.7 event (03:39:51) is the puppet's first ladder event (no earlier one in the log), probably in the first seconds of a movement, where the start grace holds the nudge back. So it is a candidate (W), not the likeliest.
- All natives run inside `run_script_threads` with the TLS script thread set to the `startup` thread when the stock scripts are off (hook.cpp `scriptTickBody`): the puppets and their tasks belong to a script handler that never ticks. Untestable offline; it is a candidate for the D / F family below.

### The candidates, and what the 0.4.7 log can and cannot tell apart (`tests/r3_t2_tests.h`, `R3T2_TABLE=1`)
| cause | model | 0.4.7 10 s numbers (60 s run, pose 4.5 m/s) |
|---|---|---|
| W | every coordinate write cancels the walk for 400 ms | gait read 0%, own speed 0.01 m/s, 3569 nudge frames |
| D | no task moves the ped | the same, to the digit |
| F | a ped flag/state blocks tasks until the ENGINE changes the ped | the same |
| L | a task needs 1.2 s to show a gait, a re-issue restarts the wait | gait read 43%, own 1.8 m/s |
| B | the IS_PED_* natives read Still, the ped moves fine | gait 0%, **own speed 4.5 m/s** |
W, D and F cannot be told apart by anything round 2 logged. B can (own speed), and 0.4.7 mislabelled it "lost". The round-3 controller tells W from D / F by an experiment, and F from D by what the engine can do to the ped.

### What the controller does now (libs/sync/locomotion.*, only with the ladder on; all of it rolls back with `loco_r3=0`)
- **Quiet window / write verdict.** The puppet's FIRST moving task runs with no coordinate write for `quietInitialMs` (1.55 s). A failure (lost / stuck / snap storm) seen while writes were flowing opens a quiet window (no write at all, 0.7-1.3 s, as long as the error it may cost allows). The ped reading a gait (or moving on its own) for 350 ms in it = **Harm**: every write cancels the walk; from then on the position error is closed by single pulses (one write per 1.5 s beyond 0.9 m), the nudge-share / rate / stuck detectors ignore the pulses' effect. Nothing in it = **Harmless**, the ladder goes on at once. A working game proves Harmless silently (gait read and own speed agree while writes flow for 1.5 s).
- **Lost needs the ped's own speed to agree** (B is not lost).
- **Ladder converges.** Monotone down within one failure; a level change holds `levelHoldMs` (8 s) before an UP step; at most `maxLevelChanges` (12) per puppet and session, then the level is frozen and one line says so; the fallback timers keep doubling.
- **Re-issue limits.** Rule-based re-issues (turn / drift / arrival / stall) at most once per second (`reissueMinMs`); only a turn of more than 70 degrees may come after 500 ms; a first gait gets `firstGaitWindowMs` (1.5 s; the live cadence was 0.77 s) before it is called lost; a forced re-issue (snap, Reset) waits `issueGapMs` (600 ms; the whole window while the previous issue still waits for its first gait). Still <-> moving is never held.
- **Repairs.** A straight task that stalled twice with the writes cleared makes the controller ask the ENGINE (`LocoOutput::repair`): 1 `ClearTasks` (CLEAR_PED_TASKS_IMMEDIATELY, freeze off, collision on), 2 `RespawnLean` (creation recipe 1: invincible, heading, events blocked and nothing else), 3 `RespawnAlt` (recipe 2: CIVMALE ped type, no script-host flag). After each the straight task is tried first. After the third the rigid follow holds 120 s (then 240, ... 600 s) before a probation.

### Live steps (GI)
Default config first, 60 s with a friend walking / running. `grep gage.log`:
- `engine: gait id=` (every 10 s per puppet, plus `engine: gait local player`): what the game READS (IS_PED_*), the speed three ways, blend ratio set, writes, issues, task-to-first-gait latency, write verdict, ladder level, **active task types**. Compare the puppet's list with the local player's.
- `engine: loco write verdict id=`: Harm -> the per-frame nudge was the cause (fixed by pulses). Harmless -> not the writes.
- `engine: loco repair id=` and `engine: spawned remote peer ... creation recipe N`: which repair (if any) made the puppet walk: the line after it says whether the gait came (`engine: gait` reads walking/running).
- `engine: loco level id=`: now at most a handful per session; `the cap of level changes ... FROZEN` if the backstop was hit.
Switches (HKCU\Software\GAGE-MP, read at every Welcome): `loco_r3=0` (all round-3 rules off = 0.4.7), `loco_repair=0` (no repairs), plus the older `loco_mode`, `loco_push`, `loco_selfcheck`.
Decision: gait `READS still` ~0% AND own speed ~0 in every line, no verdict Harm, repairs 1-3 asked and the gait never comes -> tasks do not run on a puppet whatever its flags: next suspects are the dormant script host (the `startup` thread) and the call context; the active-task-type list says whether the game has any locomotion task on the ped. Gait reads > 0 but own speed ~0 -> the task runs and physics does not move the ped (collision around the puppet, `HAS_COLLISION_LOADED_AROUND_ENTITY`).

## R4 T1 (desync round 4, client 0.4.10 candidate): BLEND, a velocity-driven walking controller

Live (GI, 2026-11-04), two walking variants of the round-3 code. 0.4.8 (anchor + nudge): the anchor task "ran but showed no gait", own speed 0.00-0.30 m/s against a pose of 1.0-3.8. 0.4.9 (straight + velocity push): the straight task DOES move the puppet, but too fast (speed from positions 7.7 / 10.7 / 8.9 m/s against a pose of 4.5 / 6.1 / 3.9), the velocity push peaked at 30.7-33.6 m/s, the pose-ped error reached 6.0-7.8 m (= the persistent-error snap = the "legit teleports while running"), 11-40 velocity reads per 10 s equalled our own write. Evidence level of everything below the log numbers: **simulated** (`tests/r4_t1_blend_tests.h`) or **compiled only** (engine glue); nothing here has run in a game.

### Why 0.4.9 teleported (read in the code, reproduced in the sim; the game's side is a MODEL)
- The 0.4.9 push is `GET_ENTITY_VELOCITY read + a correction` (`LocoConfig::residualMaxMs` 2 m/s caps only the CORRECTION). With a task that moves the ped faster than the pose and a velocity write that is only partly honoured, the read IS the task's velocity, the push can never slow the ped by more than 2 m/s, the error grows ahead of the pose until the persistent-error snap (`offLimitMs`) teleports it: 2-6 hard snaps per 20 s run in the sim (B1.5x task, half-honoured writes), 12 with the writes ignored. Any spike in the read is written straight back (a +25 m/s read gave +26 m/s above the pose velocity in the unit test; GI's 30 m/s is NOT reproduced by an ordinary run, only by a read that spikes: **an assumption**).
- "contaminated reads" (read == our last write to 0.05 m/s) on 3-8% of the frames are what a write that survives a frame looks like when the task re-asserts its own velocity most of the time: consistent with "partly overwritten", not proof.
- "last applied: move 3 / move 4" in `engine: loco` is `pe.lastPed.move` = the SENDER's PedState.move (0..3 gait, `proto::kMoveInVehicle` = 4), NOT a blend ratio. The ratio the controller sends is `BlendRatio(gait)` 1..3 or `QuantisedBlend` clamped to 3 (asserted over every scenario: max 3.00).

### The animation source (decided from what the code already proves)
| candidate | proven by the code? | what the models predict (position never depends on the task here, so these are the A rows of the table) |
|---|---|---|
| **straight task to a far point on the pose's line = the gait source; position by velocity writes (shipped)** | the task DOES move a puppet and show a gait (0.4.9 live); velocity writes are partly seen (contaminated reads) | D: p95 0.03-0.26 m, exact position, gait exists in B/S worlds (1.0x speed with the writes half honoured via the trim; B/D: the clip plays 1.5-2x the pose speed, position exact). C: p95 <= 0.5. E: rigid follow after ~1.4 s. A: no gait in any case. |
| no movement task, `TASK_PLAY_ANIM` loop with root motion off | only upper-body 16\|32 clips were seen playing (0.4.8 melee); no verified walk/run clip name, no way to set the clip's rate (SET_ENTITY_ANIM_SPEED not in hook.h) | position as the A rows (nothing else moves the ped); the gait would be the clip's own speed against the pose's: skating by the mismatch; not chosen (unverifiable names, no rate) |
| task kept alive with its target at the ped | no: TASK_GO_STRAIGHT arrives at once (< 0.25 m) and ends | no gait: the A rows plus a re-issue churn; not chosen |
So: candidate 1, the least sensitive to A-D for the POSITION (every model passes or falls to the rigid follow), and the only one that gives a gait where the game has one. Its risk is the task's own displacement fighting the write; four things answer it: the correction is capped (pose velocity + 2.5 m/s, 3.0 absolute); the move blend ratio is a continuous speed knob trimmed by what the writes had to correct (a task that runs 1.5x / 2x too fast is slowed to 1.0x); the target is on the pose's line (never aimed past it by the ped's error: a transient error at the moment of an issue became a sideways aim for the whole life of a 12-40 m lead) and aims where a turn is going; and the self-check below.

### What Blend does (`LocoMode::Blend`, `ShippedLocoConfig(LocoMode)`; `push` is ignored)
- **Velocity, every frame** (FiveM's `CNetBlenderPed` in spirit): `v = low-passed pose velocity + clamp((pose - ped) / blendSmoothS 0.25 s, blendMaxDiffMs 2.5)`, then `|v - pose velocity| <= blendHardCapMs 3.0` whatever the base and the correction add up to. z: the game's own (`velocityZFromPhysics`) unless the pose is more than 0.5 m above / below. No write inside 0.03 m (moving) / 0.12 m (standing; a standing ped is slid in at most 1.5 m/s).
- **Coordinate writes: the hard snap only**, beyond `blendSnapM` 6 m (3D) or on `Pose::teleported`; the persistent-error snap (the 0.4.9 teleport) is off in Blend. The rigid follow (below) writes coordinate steps capped at the pose speed + 3 m/s (the old Placed level stepped 8 m/s whatever the pose did and was ~0.4 m late at a sprint; it now also adds the part of the pose velocity the ped does not carry itself).
- **Gait**: speed bands with hysteresis as before, `upDebounceMs` = `gaitDebounceMs` = 400, `gaitMinHoldMs` 400 (a moving gait, once shown, holds 400 ms: a pose speed shuffling across the still / walk band flapped 0.4.9 three times in 20 s, Blend 0). Move blend ratio = the nominal speeds' piecewise-linear inverse of the pose speed (1.5 / 4.5 / 7 m/s = 1 / 2 / 3), + `blendTrim_` (range -1.2 .. +0.6, driven by the mean correction along the track: negative = the writes brake a ped that runs ahead), clamped 0.5 .. 3.0, sent to the running task with SET_PED_DESIRED_MOVE_BLEND_RATIO when it moved 0.08. No SET_PED_MOVE_RATE_OVERRIDE (it would scale the speed a second time). The task: far lead (8 s of travel, 12..40 m), re-issued by the turn rule (25 degrees / 70 degrees big) and arrival; no drift rule; a stall (the ped reads Still while a gait is commanded) re-issues up to `blendStallLimit` 3 times, then the gait source is called dead (`NoGaitSource`) and retried every 15 s: the position is carried by the velocity either way, so nothing is repaired and no level is given up.
- **Self-check, from READ-BACKS** (`BlendWatch`): every frame after a velocity write, (a) GET_ENTITY_VELOCITY and (b) the ped's own displacement (positions) are compared with the velocity written the frame before; either differing by > 1 m/s is a "fight" frame (leaky share over ~0.6 s; counters `fightVelFrames` / `fightPosFrames`). The decision is on the EFFECT: an error above 1.2 m that closes slower than 0.5 m/s for 1 s, or above 0.7 m with > 75% fight frames for 0.5 s, or a snap storm, sends the puppet to the **rigid follow** (`LocoLevel::Placed`, reason `BlendFighting` / `BlendNoTrack`, logged once as `engine: loco level`) for 30 s, doubling per failure in a row, instead of pushing harder or teleporting; two different puppets finding it within 60 s impose it on every puppet for 45 s (`LevelWitness`, reason `shared`). A read that returns our own write (0 read-back fights) does not hide it: the positions decide.
- **swingActive** (task 3's interface, all modes): `LocoInput::swingActive` (the engine sets `pe.swingProbe.Active()`) holds every task issue (gait and stand-still) and every repair while it is set and `swingHoldMs` (700 ms) after, because the probe goes false the moment the clip is seen playing while the clip plays on; pending issues go out afterwards; the velocity drive and the snap are unaffected.

### Results (sim, 30 samples/s, 80 ms jitter, 2% loss, 130 ms render delay, 11 scenarios: walk / run / sprint straight, gait changes, 90 degree turns every 3 s, run 3 s / stop 5 s, circle, 1 s outage, 5 s outage, teleport 30 m, ramp and curb; each cell is 11 runs; `R4T1_TABLE=1 R4T1_BOTH=1` prints the whole matrix)
| model (gait source / our velocity) | 0.4.9 worst p95 / max / hard snaps | Blend worst p95 / max / hard snaps / falls to the rigid follow |
|---|---|---|
| A none / D honoured | 3.30 / 4.12 / 0 | 0.26 / 0.32 / 0 / 0 of 11 |
| A none / C half | 5.08 / 5.35 / 129 | 0.39 / 1.23 / 0 / 10 of 11 |
| A none / E ignored | 5.41 / 7.58 / 178 | 0.11 / 0.20 / 0 / 11 of 11 |
| B 1.5x / D | 3.77 / 4.33 / 3 | 0.26 / 0.30 / 0 / 0 (gait plays 1.5x the pose speed: the kinematics cannot show it) |
| B 1.5x / C | 3.97 / 5.00 / 22 | 0.50 / 0.77 / 0 / 2 (gait 1.02-1.08x) |
| B 1.5x / E | 4.49 / 5.17 / 70 | 0.14 / 0.35 / 0 / 11 |
| B 2x / D | 3.43 / 4.55 / 4 | 0.26 / 0.30 / 0 / 0 (gait 2x) |
| B 2x / C | 4.89 / 5.71 / 96 | 0.49 / 1.47 / 0 / 9 (gait 1.01-1.12x) |
| B 2x / E | 5.19 / 6.06 / 121 | 0.12 / 0.37 / 0 / 11 |
| S scaled / D | 3.51 / 4.34 / 4 | 0.26 / 0.30 / 0 / 0 |
| S scaled / C | 2.78 / 3.74 / 1 | 0.47 / 0.68 / 0 / 2 |
| S scaled / E | 2.19 / 2.30 / 1 | 0.66 / 1.42 / 0 / 11 |
Everywhere: speed from positions within 0.3% of the pose on the straight runs, the velocity written never above the pose velocity + 3.0 m/s, snaps = the first frame + the real teleports, no moving gait held < 400 ms.

### Live A/B (HKCU\Software\GAGE-MP, read at every Welcome: rejoin to apply)
`loco_mode=blend` (the build default of 0.4.10) | `straight` (0.4.9: straight task + velocity push) | `anchor` (0.4.8: anchor task + nudge); `loco_push=nudge|velocity` still overrides the push of the last two; `loco_selfcheck=0`, `loco_r3=0`, `loco_repair=0` as before. The Welcome lines say `(loco_mode, build default|REGISTRY override)`. Per puppet and 10 s: `engine: loco slide` ends with `| R4 T1: mode, speed from positions vs pose (ratio), largest per-frame correction (m/s above the pose velocity), hard snaps, pose-ped error p50 / p95 / max, trim, gait source, task issues held by a swing; verdict: blend ok | blend fighting the task: N frames the velocity read back differs from the write by > 1 m/s | blend DEGRADED to the rigid follow`; `engine: gait` ends with the same verdict. Healthy: ratio 0.95-1.05, largest correction <= 3.0, hard snaps 0, error p95 <= 0.5 m / max <= 1.5 m, verdict `blend ok`. `blend fighting` with a stable error = the trim is carrying it (writes partly honoured); `DEGRADED` = the writes are not honoured on that build: the rigid follow keeps the error small, the answer is then `loco_mode=anchor` or a different animation source (see the natives list in the report).
