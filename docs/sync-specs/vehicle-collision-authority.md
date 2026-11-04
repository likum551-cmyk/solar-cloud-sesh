# Car-to-car collision authority and convergence (R4 T2, desync overhaul round 4), 2026-11-05

GI, live (0.4.8, one car-to-car approach in his log, the friend's log does not exist): "car to car collision is not even close and very desync".
Evidence levels: **simulated** = `tests/vehsim_2m.h` (two or three machines, the REAL interpolator / drive controller / NetClock / PresentLead / arbiter / tracker behind the
real stamp semantics: the server places a sample at its sample time + the controller's fastest uplink, the receiver's baseline hides the fastest downlink); **measured live** =
a number in GI's log; **compiled only** = the engine glue. Nothing here ran in the game.

## 0. What the live number "drawn vs the owner's report now" measures (and what it cannot)

`engine: vehicle position (R3 T5)` computes, per frame and per copy within 120 m: `total = |entity position before this frame's commands - ideal|` where
`ideal = interp.Sample(NearT + linkLag)`: the owner's newest snapshots, extrapolated by the SAME interpolator to the instant "now + the link lag". So:

* It IS lag compensated, but with the SAME lag the aim uses (`LinkLagMs(our ENet RTT, the server's LinkInfo RTT of the owner)`). A wrong lag moves the drawn copy and the
  ideal together, so **the metric is blind to a wrong link lag** (ENet's smoothed RTT includes up to a tick of service delay; a congested uplink makes the halves
  asymmetric). Simulated: the lag 30 ms too long leaves the metric at 0.05 m while the copy is 0.45 m (15 m/s) away from where the owner really is (R4 T2 check 1).
* It is also blind to the extrapolation error: both sides use the same dead reckoning, so a car that brakes or steers inside the 100-200 ms horizon reads "ideal" while the
  owner's real car is somewhere else (simulated: 0.14 m at 9 m/s^2 braking, `target-real` column of the trace).
* What it does measure is `aim` (the interpolation time is not where the lead wants it: delay not closed, slew, lead weight < 1, distance fade) plus `tracking` (the drive
  controller's own error: correction cap, physics, a grace).
* The speed bands MIX distances: a car over 30 m/s spends most of its frames in the 40-120 m ring where `PresentLead` fades to zero (full within 40 m, none beyond 70 m, cap 450 ms).
  "aim 72 ms behind, lead 104 ms" at over 30 m/s against 176 ms in the other bands is that arithmetic (a weight average), **not a cap and not a range gate on the lead
  itself**. R4 T2 adds the same numbers for the frames inside the full-lead range only (`within 40 m: aim N ms behind, lead N ms`).
* 905 of 980 drive frames EXTRAPOLATING is by design: the near clock draws a copy at NearT + link lag, 100-200 ms past the newest snapshot, so almost every frame extrapolates.
  The cost of that is the extrapolation error under acceleration (section 2).

## 1. The contact rule: only a contact with a car THIS machine controls softens a copy

Today (round 2/3) the collision flag (`HAS_ENTITY_COLLIDED_WITH_ANYTHING`), a speed loss or a velocity kick on a copy opens the 400 ms soft correction (and the T4
adoption), whatever the copy touched. In GI's log 28-77 flag-true reads per window had the nearest own car 5-12 m away or none, with 4-8 rising edges: the copy is touching
the world (kerb, pole, prop, the ground, another copy). A world touch of a COPY is not an event of the owner's world: the owner's car did not touch it, the owner's stream is
the truth, and a soft correction there only lets the copy drift off the stream for 400 ms (weight 0.15 rising on a smoothstep, no angular correction before weight 0.5).

**Rule.** A copy opens the contact grace / the adoption only when a vehicle this machine controls (the local player's vehicle, or an empty car the server gave us) is within
`contactNearM` (10 m, origin to origin) of it, or the proximity predictor says it is about to overlap one. Every other touch (world, ped, another copy) is **counted and
classified** (`engine: vehicle contacts (R4 T2)`) and the copy keeps following its owner's stream at full weight. `veh_contact_gate=0` restores the old behaviour.
Why this loses nothing: a copy-vs-copy touch is two owners' physics, each machine resolves its own (the server arbiter handles different controllers); a copy-vs-world touch
at the owner's stream position cannot be real if the owner's car passed through it.

## 2. Convergence: the copy returns to the owner's stream after every contact, and braking does not leave an offset

* Section 3 of `vehicle-collisions.md` (adoption + the server residual) is unchanged: the hit copy adopts its own response, the controlled car takes the arbiter's difference.
* **Every** contact episode is watched (`ContactWatch`, `libs/sync/vehicle_position.h`): when a grace opens (flag / speed loss / kick / proximity) on a copy, when a server
  impact is applied (the other car's copy is watched), and when an adoption happens, the copy's distance to the owner's report (the ideal above) is sampled at +0.5 / +1 / +2 s
  and logged (`copy vs the owner's report after a contact: ...`). T4's check ran only after an adoption and compared 0 times in every window of GI's log.
* **Acceleration-aware extrapolation** (`veh_accel_extrap`, default on). The simulation shows a steady offset of 0.5-0.65 m for as long as a car brakes (9 m/s^2, 92 ms RTT):
  the drive controller adds a velocity correction to the target's velocity, and the target's velocity is the newest snapshot's carried 100-200 ms forward at constant speed,
  i.e. `a * L` too fast; a proportional correction settles where `g * e = a * L` (`e = a L / g` = 0.5 m for a = 9, L = 0.2 s, g = 3.5/s). Cars brake before a crash and
  after one, so this sits exactly where the collision is judged. The interpolator now carries the tangential acceleration (from the speed change over the last >= 90 ms,
  clamped to +-10 m/s^2, a stop never reverses) into both the extrapolated position and velocity.

## 3. Switches

`veh_contact_gate=0` (HKCU\Software\GAGE-MP, read at Welcome): any contact opens the grace as before. `veh_accel_extrap=0`: constant-velocity extrapolation as before.
`impact_sync=0`, `veh_prox_contact=0`, `veh_near_clock=0`, server `contactLagCapMs: 0` unchanged.

## 4. Measurements (simulated, `tests/vehsim_2m.h`, RTT 92 ms = 46/46, jitter 15 ms, 30 samples/s, real stamp, 8 seeds, means)

The number is the divergence between the two machines' VIEWS of one car at the same global instant: the owner's real car on its own screen against its copy on the other
machine (`Result::viewDiv`, per car, worst of the other machines), at the reference's first contact and +0.5 / +1 / +2 s. "base" = round 3 as shipped (constant-velocity
extrapolation, every contact opens the grace); "new" = acceleration-aware extrapolation + the contact gate. Car A / car B, metres.

| scene (model 0: the body honours the velocity command) | base 0 s | +0.5 s | +1 s | +2 s | new 0 s | +0.5 s | +1 s | +2 s |
|---|---|---|---|---|---|---|---|---|
| head-on 15 + 15 (closing 30) | 0.04 / 0.04 | 0.29 / 0.30 | 0.03 / 0.03 | 0.03 / 0.02 | 0.04 / 0.04 | 0.16 / 0.19 | 0.03 / 0.03 | 0.03 / 0.03 |
| side-swipe 20 (closing 12 along the side) | 0.04 / 0.06 | 0.28 / 0.31 | **0.64 / 0.72** | **0.65 / 0.71** | 0.04 / 0.06 | 0.24 / 0.26 | 0.11 / 0.18 | 0.03 / 0.07 |
| rear-end 25 (closing 13) | 0.01 / 0.03 | 0.43 / 0.23 | **0.56 / 0.64** | **0.65 / 0.64** | 0.01 / 0.03 | 0.13 / 0.26 | 0.13 / 0.13 | 0.03 / 0.03 |
| stationary car hit at 15 (empty car claimed) | 0.03 / 0.00 | 0.17 / 0.50 | **0.60 / 0.61** | 0.03 / 0.03 | 0.03 / 0.00 | 0.12 / 0.14 | 0.09 / 0.10 | 0.03 / 0.03 |
| head-on, both brake from 18 m | **0.62 / 0.61** | 0.06 / 0.06 | 0.02 / 0.02 | 0.02 / 0.02 | 0.14 / 0.15 | 0.22 / 0.09 | 0.03 / 0.03 | 0.03 / 0.03 |
| rear-end, both brake from 18 m | 0.25 / 0.37 | 0.17 / 0.40 | 0.39 / 0.22 | 0.05 / 0.03 | 0.04 / 0.10 | 0.17 / 0.16 | 0.04 / 0.03 | 0.03 / 0.03 |

Targets (<= 1.0 m at +1 s, <= 0.5 m at +2 s, no velocity change beyond what the closing speed allows, no snaps) are met by the new logic in every scene under every model; round 3
already met them in a clean head-on and failed the 0.5 m target at +2 s in the side-swipe and the rear-end (0.65-0.72 m, persistent until the cars stand still).
The largest one-step velocity change of a real car: head-on 17.7 m/s (bound 22.6), rear-end 7.8 (10.3), stationary 8.9 (11.8), side-swipe 0.4 (9.7): the server residual
never doubles a hit. Models 1-3 (the body takes half a command per frame; the world flags a copy 0.5 times a second, with and without a 3 m/s bump) are in the test (mean
+1 s <= 0.40 m, +2 s <= 0.29 m for the new logic) and in the report; with world touches and the gate off, one seed's copy is 2.9 m off its owner at +2 s after a head-on.

**What the simulation did NOT reproduce.** GI's 1.4-1.8 m mean (tracking) under 15 m and 3.2 m over 30 m/s are not in the model with a clean pipeline (<= 0.15 m while the cars
are at constant speed), so a cause not in the model is at work in the game (what the copy's body does with a velocity command; the owner's real manoeuvres). The ground truth
twin of the live metric, the touch classification and the watch after every contact exist so the next log can say which. That is a hypothesis, not a diagnosis.
