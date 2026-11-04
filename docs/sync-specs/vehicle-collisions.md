# Vehicle collisions (T4, desync overhaul round 2), 2026-11-03

GI, live: "Huge desync in car-to-car collisions ('not even close')". Code and simulation only: nothing here was run in the game. Everything the engine does with the
game's physics is INFERRED and labelled live-unverified at the end.

> **R3 T5 (2026-11-04) correction.** The simulation numbers below stamp every sample at its TRUE time, but the server's `SampleClock` stamps it at its sample time PLUS the controller's
> fastest uplink (and the receiver's clock is the fastest downlink behind the server's tick). With the real stamp round 2's present-time lead leaves a copy half the controller's and
> half our round trip behind its owner (2.05 m at 25 m/s on 40 ms each way), the two screens' contacts land up to 370 ms apart, and the arbiter's two tracks sit at two times (it
> missed a T-bone whose controllers had 15 ms and 60 ms uplinks 4 times out of 4). See docs/sync-specs/vehicle-position.md for the measurement and the fix (near-car clock + link lag,
> `LinkInfo`, the arbiter's half-round-trip correction, the proximity contact).

## Why two cars hitting each other look nothing alike (first-hand, from the code and the two-machine simulation)

Every vehicle is simulated by exactly one client (its controller; `VehicleState.ctrl`). On every other machine it is a copy: a solid body whose velocity, angular velocity and pose
the drive controller overwrites each frame from a 126-193 ms old snapshot (`PresentLead` draws it at the present only within 30 m). A contact between two controllers' cars is therefore
two unrelated physics events, one per machine, and `tests/vehsim_2m.h` (new: N machines, the real controllers / interpolators / NetClock / PresentLead behind the real relay, planar rigid-body
physics, a reference world with one physics instance) measured what they do:

1. The copy is a WALL that keeps coming. At the next frame it is re-commanded to the stream's pre-impact velocity, so the local car is hit again every frame until the other controller's
   post-impact samples arrive (the network latency, 100-300 ms): each car is thrown back far harder than momentum allows. Head-on 20+20 m/s: the real cars are 11 m / 11 m/s off the
   reference one second later (18 m / 20 m/s on a 90 ms link), where a correct pair would leave each other at 2-3 m/s.
2. Where and when it touches differs per screen (up to the one-way latency plus the render delay's remainder: 1-130 ms spread in the runs), and the two real cars overlap by up to 2 m
   at the same instant (`trueOverlap`): the two screens' physics do not describe one event.
3. A parked or empty car is a copy for everybody except its last driver (`vehicles.h`: the controller keeps it out to 250 m). The rammer hits an immovable wall; the car moves only
   when the owner's screen has heard of the hit and its stream arrives (4-9 m / 3-8 m/s off the reference, in every run).
4. The W9/Sync B contact grace softened only the copy on the hitter's screen and made a third pose of the car for 400 ms. It helped (every grace-less variant was worse in the simulation)
   but it pulls the copy back to the stale stream.
5. The speed cap: a `VehicleUpdate` over `vehicleMaxSpeed` (150 m/s) was refused (`BadArg`): a 139 m/s hyper car's overshoot froze it for everyone else.

## The candidates, measured

Same scenes (seeded; 4-6 seeds per cell). The candidate table is the 25 ms-each-way link (30 ms downlink jitter, 3% loss); the per-link tables below add machine 1 on 90 ms with 5% loss and 120 ms each way with 60 ms jitter and 5% loss. `pos 1 s` / `vel 1 s` = the worst real car's
position / velocity error against the one-physics-instance reference one second after its first contact. P was dropped for the T-bone and bike regressions and the model-dependence of its own-car write; S alone cannot reach the copy.

| candidate | what it is | head-on 11 m | rear-end 5.3 m | into parked 4.1 m | two into a third (3 s) 32 m | sustained push 5.1 m | T-bone 1.4 m | bike into truck 0.2 m |
|---|---|---|---|---|---|---|---|---|
| today | copies are walls; grace on | 11.0 | 5.3 | 4.1 | 31.8 (3 s) | 5.1 | 1.4 | 0.2 |
| S: server arbiter only | ContactArbiter + VehicleImpact, applied as a correction | 9.7 .. 14.8 | 4.4 .. 9.1 | 4.1 .. 5.5 | 31.8 (3 s) | 4.0 .. 8.1 | 1.4 | 0.2 |
| P: client prediction | the same solver run locally the moment the car's physics feels the hit, the copy changed at once | 0.7 | 0.3 | 0.15 | 0.5 (3 s: 1.6) | 0.3 | 6.1 | 1.2 |
| A: adopt the copy's own response | the copy's physics response becomes its trajectory; the stale stream is held back until it shows the impact | 1.2 | 1.1 (3 s: 4.7) | 1.4 | 1.3 (3 s: 1.8) | 0.2 | 4.4 | 0.4 |
| **AS: A + the server's resolution as a residual** | | **0.9** | **0.3 (3 s: 0.7)** | **0.3** | 0.5 (3 s: 1.75) | 0.7 | 1.1 | 1.4 |
| **ASC: AS + the empty-car claim** | | 0.9 | 0.3 | **0.0** | **0.7 (3 s: 1.5)** | 0.7 | 1.1 | 1.4 |

(P, S, A at 25 ms; the full per-link tables for today / AS / ASC are below.) Not built, rejected on design grounds, with the reasons in the report: a single "host" client simulating both
cars of a contact (it would need the other driver's inputs, which only exist on his machine, so the guest's car would be steered by a 100+ ms old copy of his own input, or rubber-band
to the host's result; GTA's physics cannot be rolled back), and lowering the 250 m stickiness alone (the claim replaces it where it matters; ctrlStickyMeters is unchanged).

### The chosen design: server-arbitrated contact resolution + copy adoption + empty-car claim

1. COPY ADOPTION (client, `libs/sync/vehicle_drive.*`, `vehicle_impact.h`, engine `TickVehicles`). When a copy takes an impulse inside a frame (the drive controller's contact trigger:
   collision flag, speed loss or velocity kick), that is the OTHER car's response to the hit from the game's own two-body physics. `DecideAdopt` bounds it (>= 2 m/s, <= 30 m/s, yaw <= 8 rad/s),
   `VehicleInterpolator::ReplaceFrom` stores it as the copy's newest snapshot, the stale stream is refused until a pushed sample shows the impact (its velocity is at least half the
   impulse away from the pre-impact one) or 300 ms have passed, `VehicleDriveController::EndGrace` ends the contact's soft correction, and the frame's commands are not written. Without the
   confirmation the result depends on the guard length (T-bone 1.4 .. 13 m for 40 .. 300 ms); with it the result is the same for 40 / 120 / 300 ms.
2. SERVER ARBITER (`ContactArbiter`, `libs/sync/vehicle_impact.h`, wired in `apps/server/server.cpp` BroadcastImpacts). Keeps the last 8 accepted samples of every vehicle on the server's time
   base. For each pair with different controllers within 14 m it scans the common window at 4 ms for the first instant the boxes (by mass class, grown 0.3 m each) touch, bisects it, takes the
   velocities held from before the instant (a bounce is already in the samples when the server looks: both controllers' physics stop their cars at each other's copy, so the tracks never
   overlap), solves one planar impulse (restitution 0.25, friction 0.3) and sends each controller `VehicleImpact` (message 40): its change of velocity and yaw rate, the contact time on its own
   sample clock, the normal, closing speed and depth. Once per contact (re-armed after 120 ms apart), none for a graze (the true boxes must overlap by 0.1 m along the face), a closing
   speed under 1.2 m/s, the same controller or an untrusted one; |dv| <= 30 m/s.
3. APPLY AS A CORRECTION (client, `ImpactTracker` + `DecideImpactApply`, engine `DrainImpacts`). The tracker measures the velocity jumps no driver produces (> max(1.5 m/s, 60 m/s^2 * dt)
   per frame) on cars this machine controls; the message's change MINUS what those jumps already gave is written onto the car (velocity and yaw rate). A contact the car's physics did not feel
   is applied only if the server's true boxes overlapped >= 0.1 m and our copy of the other car is within 7 m; a local jump pointing the other way is not subtracted; messages older
   than 1.5 s, duplicates and changes under 0.6 m/s are ignored.
4. EMPTY-CAR CLAIM (`apps/server/vehicles.h`, `VehicleStore::FindToucher`). A driven vehicle (trusted driver, > 3 m/s, closing > 1 m/s) within 10 m + 0.5 s of travel of a driverless vehicle takes
   its control: the rammer's own physics then resolves the contact natively (same controller = no arbitration). Holds 1.5 s. This is the answer for parked cars: 4.1-8.9 m / 3.3-8.2 m/s off
   today, 0.00 with the claim on every link, overlap 0.01 m.
5. SPEED CAP (`VehicleStore::Update`). Over `vehicleMaxSpeed` and under twice it: accepted with the velocity clamped to the cap (counted); above: `BadArg` as before. The hyper Jesko's
   139 m/s target leaves 11 m/s headroom; a 160 m/s overshoot used to freeze it for every other client.

### Trade-off (one paragraph, for the report)

Resolving every contact on ONE machine (a host simulating both cars) would give the most physical result but is not available here: the other driver's inputs live on his machine, GTA's physics
cannot be rolled back, and the host would show the guest's car steered by a copy 100+ ms stale. The design therefore keeps each controller's own physics for its own car (so the player's car never
rubber-bands under his hands), and removes the three sources of divergence that can be removed: the copy no longer behaves as a wall (its own response is adopted, so each machine simulates a closed
two-body system from nearly identical states), the server computes the one momentum-conserving result from both exact tracks on one clock and corrects each controller by the difference to what
its game did (a bounded, cheat-resistant residual rather than an authority: the controller can only drive its own track, the server caps |dv|, refuses untrusted controllers and never moves a
car it did not see touch), and a driverless vehicle moves to the rammer's machine before the hit so the pair is one physics instance. What it costs: a model (planar boxes by mass class) that is
not the game's solver, so a hit the model gets wrong (a side hit, a very light bike into a very heavy truck: 1.2-1.4 m in the simulation, worse than today's 0.2 m) is corrected the wrong way by up to its
error; at 120 ms latency a T-bone gets worse (10.9 / 41 m at 3 s vs 10.7 / 13 m) because the planar simulation's interlocked bodies are chaotic and the adopted response amplifies it; and the
whole thing lives on assumptions about the game (below) that only a live run can check, with `impact_sync=0` as the switch back.

## Per link: today vs the chosen design (average of 6 seeds)

| scene | link | today: pos 1 s / 3 s, vel 1 s | new: pos 1 s / 3 s, vel 1 s | overlap today -> new (max m, ms) |
|---|---|---|---|---|
| head-on 20+20 | sym25 | 11.0 / 20.4, 11.2 | 0.9 / 0.9, 0.0 | 0.63, 46 -> 0.63, 46 |
| head-on 20+20 | asym25/90 | 18.3 / 42.9, 19.7 | 1.1 / 1.1, 0.0 | 1.08, 106 -> 1.08, 132 |
| head-on 20+20 | sym120 | 12.3 / 26.8, 14.5 | 1.8 / 1.8, 0.1 | 2.00, 195 -> 2.00, 283 |
| t-bone 15/12 | sym25 | 1.4 / 1.4, 0.5 | 1.1 / 1.2, 0.6 | 0.27, 0 -> 0.27, 0 |
| t-bone 15/12 | asym25/90 | 4.2 / 4.5, 2.0 | 4.5 / 5.9, 3.2 | 0.96, 150 -> 1.00, 205 |
| t-bone 15/12 | sym120 | 10.7 / 13.3, 7.1 | 10.9 / 41.3, 19.0 | 2.00, 714 -> 2.00, 1516 |
| rear-end 12/24 | sym25 | 5.3 / 8.8, 5.3 | 0.3 / 0.7, 0.7 | 0.11, 0 -> 0.11, 0 |
| rear-end 12/24 | asym25/90 | 7.5 / 11.6, 6.9 | 3.1 / 8.7, 6.2 | 0.00, 0 -> 0.43, 31 |
| rear-end 12/24 | sym120 | 9.1 / 16.9, 7.7 | 5.2 / 14.5, 6.8 | 0.00, 0 -> 1.07, 347 |
| side-swipe 20/20 | sym25 | 0.2 / 0.0, 0.3 | 0.2 / 0.0, 0.3 | 0.00, 0 -> 0.00, 0 |
| side-swipe 20/20 | asym25/90 | 0.8 / 0.0, 1.1 | 0.8 / 0.0, 1.1 | 0.00, 0 -> 0.00, 0 |
| side-swipe 20/20 | sym120 | 1.2 / 0.0, 2.1 | 1.2 / 0.0, 2.1 | 0.22, 0 -> 0.22, 0 |
| into parked 15 | sym25 | 4.1 / 4.5, 3.3 | 0.0 / 0.0, 0.0 | 0.16, 0 -> 0.01, 0 |
| into parked 15 | asym25/90 | 5.0 / 5.3, 4.1 | 0.0 / 0.0, 0.0 | 0.56, 78 -> 0.01, 0 |
| into parked 15 | sym120 | 5.5 / 8.9, 8.2 | 0.0 / 0.0, 0.0 | 0.68, 131 -> 0.01, 0 |
| two into a third | sym25 | 3.3 / 31.8, 10.8 | 0.7 / 1.5, 3.5 | 0.13, 0 -> 0.48, 50 |
| two into a third | asym25/90 | 4.2 / 31.8, 10.8 | 0.7 / 1.6, 2.6 | 0.45, 58 -> 0.38, 27 |
| two into a third | sym120 | 5.2 / 31.8, 10.8 | 0.7 / 2.3, 3.8 | 0.58, 102 -> 1.50, 452 |
| sustained push | sym25 | 5.1 / 3.5, 2.0 | 0.7 / 2.4, 1.6 | 0.20, 0 -> 0.36, 73 |
| sustained push | asym25/90 | 7.5 / 4.9, 4.9 | 0.9 / 4.7, 4.0 | 0.76, 176 -> 0.97, 1223 |
| sustained push | sym120 | 8.4 / 4.3, 5.1 | 2.6 / 9.8, 5.0 | 0.22, 0 -> 2.00, 692 |
| bike into truck | sym25 | 0.2 / 0.2, 0.2 | 1.4 / 1.4, 0.2 | 0.07, 0 -> 0.07, 0 |
| bike into truck | asym25/90 | 0.6 / 0.7, 0.8 | 1.3 / 1.3, 0.8 | 0.07, 0 -> 0.07, 0 |
| bike into truck | sym120 | 0.8 / 0.9, 0.8 | 1.2 / 1.2, 0.9 | 0.06, 0 -> 0.06, 0 |

`pos`: the worst real car's distance from the one-physics-instance reference at 1 s / 3 s after its first contact; `vel`: its velocity error at 1 s; `overlap`: the two real cars' interpenetration
at the same global instant (max m, milliseconds over 0.3 m). Parked scenes use the claim (ASC), the others AS. Honest reading: head-on, rear-end (good link), ramming a parked car and three
cars are far better on every link; side-swipes are untouched; a T-bone, a bike into a truck and a long push at 120 ms are not better and sometimes worse (see WEAK in the report).

## Diagnostics (rate-limited, every 10 s) and what to grep for

Client `gage.log`:
* `engine: vehicle impacts (T4), last 10 s: impact_sync on; copies whose response was adopted N (contacts left to the grace N, stream pushes held back N); velocity jumps our cars felt N; server
  impacts in N: applied N (change written X m/s total, largest Y), no local contact N, too small N, too old N, duplicate N, car not ours N, queue overflow N; copy vs controller's stream 500 ms
  after the adoption: N compared, mean error X m, worst Y m`. This is the divergence estimate between the two screens: the adopted copy against its controller's real stream half a second
  after the contact. Mean under 1 m = the two machines agree; a mean of several metres = the adopted response was wrong.
* `engine: vehicle id=N: contact on the remote copy -- its own response (X m/s change, yaw rate Y) adopted as its trajectory` (first 12) and `engine: vehicle id=N: server impact #S with vehicle M ...`
  (first 12: what the server computed, what the game had done, what was written).
* The existing Sync B / W9 lines stay (`vehicle collision (Sync B)`, `contact graces opened ...`): with `impact_sync` on, graces opened should drop to the contacts adoption refused.
Server log: `contact: N impact(s) in the last 10 s (T total, M messages), pairs examined, soft touches, skipped untrusted, dv capped, max closing; empty-car claims C, velocity clamps K`.

## GI's live test (two players, both on 0.4.7+ and the server redeployed together)

1. Both drive at each other head-on at ~100 km/h on a straight road, from 100+ m. Expect: both cars stop and separate at a few m/s (not fly apart), the other car does not plough
   through yours a second time. Look for `server impact #...` lines on BOTH machines with similar numbers (`authoritative change` about -24 / +24 m/s for equal cars), `copies whose response was adopted` >= 1.
2. Ram a parked car (one that someone left) at ~60 km/h: it must move on BOTH screens and look alike. Server: `empty-car claims` +1 per ram; client of the rammer: `car not ours` 0.
3. Drive a Jesko past 150 m/s (downhill / jump): the other player must still see it move; server `velocity clamps` > 0 and no `outside the world box or over twice` line.
4. T-bone and a bike into a truck: report if it looks wrong; set `impact_sync` to `0` (HKCU\Software\GAGE-MP, string) and reconnect to compare with the old grace.

## Live-unverified (assumptions about the game this rests on)

* A copy that is hit moves by the game's physics within one frame and `GET_ENTITY_VELOCITY` / `GET_ENTITY_ROTATION_VELOCITY` read it before the next command (the Sync B premise).
* `SET_ENTITY_VELOCITY` / `SET_ENTITY_ANGULAR_VELOCITY` on the controlled car takes effect (the yaw correction uses the world-frame setter like the drive controller: right for a level car).
* The game gives the controlled car a velocity jump of at least 1.5 m/s in a frame when it hits a copy (the tracker's threshold) and no such jump from driving or braking.
* Mass classes by model name (bikes 250 kg, trucks / buses 7 t, the rest 1.5 t) and boxes 4.6 x 2.0 m: the server has no dimensions or mass natives.
