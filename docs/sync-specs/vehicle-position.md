# Vehicle position and contact on the machine that sees it (R3 T5, desync overhaul round 3), 2026-11-04

GI, live (0.4.7, only his `gage.log` exists): "car collisions are non existent: p2 hits me on his screen, for me he is about 2-3 meters away just staggering."
`[03:42:53] pipe clock: 601 world/vehicle frames per 10 s (arrival gap mean 17 p95 36 max 36 ms), 58 delay rises; shared delay 84 ms (target 84)`;
`[03:43:33] vehicle impacts (T4): copies whose response was adopted 0 (contacts left to the grace 2); velocity jumps our cars felt 2; server impacts in 2: applied 2`.
Evidence levels: **simulated** = the offline simulations (tests/vehsim.h one car, tests/vehsim_2m.h two machines) with the real controllers, interpolator, NetClock, PresentLead,
arbiter and tracker; **measured live** = a number in GI's log; **compiled only** = the engine glue. Nothing below was run in the game.

## 1. Root cause: the lead closes the render DELAY, not the network LATENCY (simulated; consistent with the live numbers)

* Round 2's lead (`PresentLead`, 30 / 45 m) draws a near copy at `RenderT - extra + lead` with `lead = shared delay + extra`: "now on the server's timeline". That is applied (it
  is not skipped for ctrl'd cars, it works on `dispT`, the cap 250 ms is not reached at 84 ms). The 84 ms delay is NOT the 2.1 m.
* What is left is two network legs that no timestamp carries. The server's `SampleClock` places a sample at `sender time + min(recv - sender)`: the fastest uplink is absorbed
  into the offset, so a vehicle stamp is the sample time PLUS the controller's uplink. And a frame reaches the receiver the fastest downlink after its tick, which the
  `NetClock` baseline (minimum offset) subtracts. "Now on the timeline" is therefore the owner's position half the controller's round trip + half ours in the past.
  On 40 ms each way that is 80 ms = 2.05 m at 25 m/s, 3.3 m at 40 m/s (`tests/r3_t5_tests.h` check 1, 2).
* The T4 simulation stamped samples at their TRUE time (`SampleClock is idealised`), which hid exactly this: with the real stamp round 2's lead leaves 2.05 m where the idealised
  stamp showed 1.05 m (the receiver's own downlink). The old numbers in vehicle-collisions.md were optimistic by the uplink.
* On GI's machine P2's copy was therefore drawn ~2 m behind where P2's screen had it. P2's car, on P2's screen, hit GI's copy -- a GHOST 2 m behind GI's real car -- and was
  stopped, bounced and wobbled by a wall that is not where GI is; GI sees the result of that bounce streamed back: a car 2-3 m off "just staggering". On GI's own screen the two
  real cars never touched, so no flag, speed loss or kick opened the grace or the adoption ("adopted 0, contacts left to the grace 2"), and the server's arbiter -- which compares
  two tracks each placed at ITS controller's own `sample time + uplink` -- saw the boxes meet only if the two uplinks happened to be equal (a 45 ms difference is 1.8 m at 40 m/s
  closing, more than the arbiter's whole margin): "server impacts in 2" is that arbiter finding two of them (check 11: with 15 ms and 60 ms controllers it finds ZERO of four).

## 2. The stagger: NOT the delay (simulated with the live rhythm)

58 delay rises per 10 s: in a simulation with the live rhythm (30 Hz sender into a 30 Hz tick, WorldState and VehicleWorld 0.5 ms apart -> 601 frames per 10 s, clean link) the
clock makes 43-48 "rises" per 10 s with a MEAN of 0.1 ms and a 3 ms band (84..85 ms) after warm-up; the single start-up rise is 12 ms. The counter increments whenever the 95th
percentile of 180-256 samples ticks 1 ms above a delay that is decaying toward it: ripple, not a flapping target. A delay step does matter when it is real (jitter 30 ms, a
400 ms hole: 110 ms and a 37 ms jump), but round 2's lead then pushed the copy ahead for 500 ms (the render clock follows a delay change in 500 ms, the lead in 40 ms); the
near-car clock removes that coupling. The `pipe clock` line now prints the rises over 5 ms, their mean and the largest, so the next log shows it directly.
The stagger GI SAW is the other machine's: P2's real car bounced off the ghost of GI's car (above). It cannot be measured from GI's log; the controller-side companion of the
new position line (speeds of the cars we send) and the T4 lines on P2's machine will show it.

## 3. The fix

1. **Near-car clock** (`veh_near_clock`, default on). `NetClock::NearT(now)` = the timeline time of the frame arriving now over the fastest path (`now - baseline`), steered like
   `RenderT` (+-10%, resync 500 ms) but WITHOUT the delay. A copy near the local player is drawn at `NearT + linkLag`; `NearLeadWantMs` hands the difference to the delayed clock
   to the existing `PresentLead` slew, so neither the shared delay nor the class extra (hold 1.5 s / 8 s) moves a near car, and the two missing network legs are added.
   `linkLag = LinkLagMs(our RTT, controller's RTT)` = half each (each half capped at 150 ms); our RTT is ENet's (`ClientSession::lowRttMs`), the controller's is the server's
   `LinkInfo` (below). Ranges: full lead within 40 m, none beyond 70 m (a closing speed of 50 m/s covers the 30 m fade in 0.6 s; the slew is 0.5 ms/ms), cap 450 ms.
   The interpolator's extrapolation cap grows with the lead (`SetExtrapolateMaxMs(500 + lead)`), or a 400 ms hole froze a lead-ed car.
   Result (simulated, check 2): the copy is within 0.2 m (p95) of the owner's position now at 3 / 10 / 25 / 40 m/s (round 2: 0.27 / 0.83 / 2.05 / 3.27 m means).
   Cost: dead reckoning over 150-200 ms is wrong for a moment at a steering or braking onset (lane change p95 0.35 m, hard brake p95 0.41 m, against round 2's steady 2 m); the
   commanded-velocity spike at a lane-change onset doubled (148 -> 263 m/s^2, one frame).
2. **`Msg::LinkInfo` (44)**, S->C, unreliable, ~1 Hz, [count][id, rttMs] for the vehicle controllers: the only way a receiver can know the OWNER's uplink. No existing layout changes.
   Without it (never received) the lag uses our RTT for both halves.
3. **Contact detection on the machine that sees it.**
   a. *Server*: each controller's samples enter the arbiter at `SampleClock time - half its round trip` (`contactLagCapMs`, default 150, 0 = off; slewed by
      `SlewArbCorrMs` so the track keeps increasing). With 15 ms and 60 ms controllers the arbiter finds 0 of 4 T-bones before and 4 of 4 after (check 11, 12).
   b. *Client*: **proximity contact** (`veh_prox_contact`, default on). Each frame, for a copy within 40 m of us, `sync::impact::PredictOverlap` steps the copy's DRAWN pose (with its
      target velocity) and OUR vehicle's pose forward 120 ms at constant velocity and yaw rate as the same oriented boxes the arbiter uses (mass class by vehicle class, grown 0.35 m),
      and reports an overlap that is still ahead (boxes apart now) and closing faster than 1.5 m/s. That opens the drive controller's contact grace (`contactOpened` 4) a few frames
      BEFORE the game's flag, so the copy yields to the hit instead of being a wall; the first real evidence inside that grace (flag, speed loss, kick) is reported as a contact
      (`contactOpened` 1..3, counted as an upgrade) so the T4 adoption still sees the impulse. A pair already touching, passing in the next lane or drifting apart opens nothing.
      Simulated (check 16): never worse than 0.25 m in head-on / rear-end / T-bone / sustained push, 1.7-4 m better in the T-bone at 40 ms each way.
   c. A controller-side contact report (a NEW message) was considered and NOT built: with the tracks on one time base the server's boxes touch whenever the cars did.

## 4. Simulation tables (4 seeds, 1 s after the reference contact, the real stamp; "today" = round 2 as shipped)

| scene, link | today pos 1 s | contact spread today | overlap today | fix pos 1 s | spread | overlap |
|---|---|---|---|---|---|---|
| head-on, 40 / 40 ms | 1.37 | 0 | 1.64 m | 0.77 | 0 | 0.10 m |
| rear-end, 40 / 40 ms | 6.62 (3 s: 11.0) | 372 ms | 1.72 m | 0.98 (3 s: 2.7) | 5 ms | 0.00 m |
| T-bone, controllers 15 / 60 ms | 11.9, arbiter finds 0 of 4 | 54 ms | 1.7 m | 2.36, finds 4 of 4 | 2 ms | 0.03 m |
| T-bone, 40 / 40 ms | 7.6 | 54 ms | 1.5 m | 7.2 (5.5 with proximity) | 2 ms | 0.04 m |

The T-bone is chaotic in the planar model (interlocked boxes: single seeds range 1.3 .. 24 m after the fix); the sim clamps bodies launched past 80 m/s and counts them
(`Result::blowups`). Side-swipes are unchanged. Sustained push at 3 s: 8.8 -> 5.4 m.

## 5. Diagnostics (10 s, rate-limited) and what to grep

Client: `engine: vehicle position (R3 T5), last 10 s: near clock on|OFF, proximity contact on|OFF; our round trip X ms, controllers' (server's view) [id:rtt ms, ...], N LinkInfo in,
owner's round trip unknown on N frames; link lag used mean/max; shared delay; copies: speed B m/s: F frames, drawn vs the owner's report now mean/p95/max m (aim, tracking), aim N ms
behind, lead N ms; ... ; under 15 m / 15-45 m / over 45 m: mean/p95/max; drive snaps N; proximity: frames predicted / graces opened / upgraded; as controller: updates sent (mean speed;
bands)`. "drawn vs the owner's report now" = the entity's position before the frame's commands against the owner's snapshots sampled at (now + link lag): the best estimate of
where the owner really is. `aim` = how far the interpolation target is from it (render delay not closed, lead slew, weight < 1); `tracking` = the drive controller's own error
(correction cap, blending, physics). **Reading it**: a mean of metres at 15-30 m/s with `aim N ms behind` near 0 -> the controller's tracking; `aim` large -> the lead is not closing
(is `near clock OFF`? is the owner's round trip unknown?); both small but the cars still look apart -> the other machine (compare its line at the same time).
`engine: pipe clock: ... N delay rises (M over 5 ms, mean X ms, largest Y ms)`. Server `gage.log`: unchanged `contact:` line; the new send count is in `stats` as `linkInfoSent`.

## 6. Switches

* `veh_near_clock=0` (HKCU\Software\GAGE-MP, read at Welcome): round 2's lead (shared delay + extra, 30 / 45 m, 250 ms, no link lag, normal extrapolation cap).
* `veh_prox_contact=0`: the flag / speed loss / kick triggers only.
* `impact_sync=0` (T4) unchanged. Server: `contactLagCapMs: 0` in server.json = arbiter times as the SampleClock placed them.

## 7. Weak points (honest)

* The lag assumes each leg is half its round trip. An asymmetric path (20 up / 70 down) is exact in the simulation only because the sim's legs are what the RTT says; on a real
  path with a congested uplink the half is wrong by the asymmetry (check 3: 50 ms short = 1.2 m). `lowRttMs` / the server's measure is ENet's smoothed value, not the minimum.
* Extrapolating 150-200 ms: wrong at a steering / braking onset (above). The contact itself is the owner's physics against a copy that is where the owner will be; if the owner
  brakes in the last 150 ms the copy is ahead of him briefly.
* Nothing here reaches inside GTA: the proximity predicate, the engine glue and the diagnostics are compiled, not run. Whether `GET_ENTITY_VELOCITY` of OUR car is what the game
  integrates (the predicate uses it), and the mass class by vehicle class, are the T4 premises again.
* The T-bone remains chaotic in the model; the T4 adoption amplifies interlocked bodies. Not changed.
* The ambient side check is in the report, not here.
