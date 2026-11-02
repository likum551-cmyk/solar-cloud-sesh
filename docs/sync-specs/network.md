# Network timing spec, 2026-11-02

Design agent output (read-only; line numbers as of 2026-11-02). INFERRED = verify.

## Findings

1. **Uneven WorldState counts** come from the idle gate, not budget, relevance or acks. A subject whose quantised sample (1 cm pos, 1/32 m/s vel, heading, PedState) is bit-identical for idleAfterMs 500 drops to rateIdleHz 1 (replication.cpp:302-304, 379; replconfig.h:41-42). The compare is bit-exact: one LSB of velocity or z noise restarts a 500 ms near-rate burst (INFERRED cause of the 106-174 per 10 s windows). A stalled sender looks like a standing one (age saturates at 255, :310). 2 players cost ~0.7 kB/s against a 24 kB/s budget.
2. **Aliasing.** 30 Hz send into a 20 Hz tick forwards only the newest sample per tick (server.cpp:874-876, replication.cpp:215): one third never leave the server, spacing alternates 33/67 ms, age is a 0-33 ms saw-tooth measured from server receipt (replication.cpp:309; smoother.h:117-121). Frame quantisation (16.7 ms), the mailbox + Poll(10) wait (join.cpp:213-230) and uplink jitter go straight into sample times: +-10 ms = 6 cm at 6 m/s, 30 cm at 30 m/s. Repeats 10-20% are mostly 1 Hz idle refreshes (by design) plus INFERRED 2-5% true duplicates; "holding" = standing peds past the 250 ms cap; bridge resets = idle-to-move after > 500 ms. Benign.
3. **Delay** = gap (>= 50) + p95(offset + ageMs) + 25 (smoother.cpp:141, 299). The age saw-tooth (p95 ~32 ms) is counted as jitter; 50+32+15+25 matches the logged 115-207 ms. The delay falls per packet (smoother.cpp:143: 2.5 s at 20 Hz, 50 s for a 1 Hz pair).
4. **Two clocks.** Peds: per-player baseline, adaptive 75-300 ms, slewed render clock, samples at tick*tickMs - ageMs. Vehicles: one global decaying-max offset, fixed 120 ms, no slew, tMs = tick*tickMs with NO age (engine.cpp:916-925, 950, 3177-3180; protocol.cpp:349-376; vehicles.h:451-545): a 10 Hz wobble of 0.2 m at 25 m/s, angular velocity off by up to 33%; a re-sent unchanged state is a new snapshot (vehicle_drive.cpp:210). Ped vs car offset reaches ~90 ms.

## Changes (by player impact)

### N1. Vehicle timeline (v5)
- VehicleWorld entry + u8 ageMs (age of V.posMs, vehicles.h:326, at the tick).
- Header value 2 = motion-only entry: id, hdr, age, 22 B motion, flags = 26 B vs 40 B. Static fields (model, health, fuel, colours, seats) only on change, enter, and 1/s.
- Engine: tMs = tick*tickMs - age (engine.cpp:950). VehicleInterpolator::Push returns VehPush::Repeat for bit-identical motion; age 255 counts as Repeat; a driver change resets.
- Test: vehicle_drive_test Push(s,1000) then Push(identical,1050) == Repeat and Count()==1 (today Accepted, 2). netcode_test VehicleNetworkTests: re-sent VehicleUpdate seq decodes to equal tick*50-ageMs.

### N2. Sender-time stamping (v5, peds and vehicles)
- PlayerState + u16 sampleMs (37 -> 39 B; aiming 43 -> 45). VehicleUpdate + u16 (33 -> 35 B).
- Set from NowMs() at the read (engine.cpp:3981), carried through the mailbox (join.cpp:40-41, 406-416) into SendState.
- Server: SenderClock per Conn (new apps/server/senderclock.h; or the shared pure libs/sync/sampleclock.h of onfoot.md 5b, one implementation): unwrap by signed delta, offset = recv - unwrapped, 256-sample windowed min, sample = clamp(unwrapped + minOffset, recv - 300 ms, recv). Pass as Replicator::SetState's nowUs (server.cpp:874-875) and as V.posMs.
- Ages against the nominal tick time (server.cpp:551 passes actual NowUs(), so tick lateness leaks into placement).
- Poll(10) -> Poll(2) (join.cpp:230).
- Edges: first 8 samples use recv; gap > 30 s re-anchors; liars clamped.
- Test: interest_test TimelineTests: samples every 22.2 ms, receipt jitter +-12 ms -> stddev(tick*50 - ageMs - true sample time) <= 1.5 ms (today ~7). kVersion == 5, PlayerState 39 B (netcode_test.cpp:249, server_test.cpp:742).

### N3. Rates
- tickHz 30 (config.h:33); rateNear/Mid/Far 30/15/10 (replconfig.h:37-39); budget 36000 (:49); vehicle tiers follow tickHz (server.cpp:305-309); engine send stays 1.5x tick = 45 Hz (engine.cpp:4000).
- Delay floor 71+j ms vs 107+j today. 60 Hz gains only ~14 ms more for 2x cost: skip.
- Bandwidth, 32 players within 120 m: per client 8.7 -> 13.2 kB/s; server egress 278 -> 422 kB/s (3.4 Mbit/s); all-absolute fallback 27 kB/s (hence 36000); vehicles (10 in view, motion-only) 7.8 kB/s per client; uplink ~5 kB/s per player.
- kWorldHistory 32 ticks = 1.07 s at 30 Hz; RTT > ~900 ms degrades to absolute.
- Test: interest_test at tickHz 30: moving player at 50/200/350 m gets 148..150 / 73..77 / 48..52 entries in 5 s (today 98..100 / 48..52 / 23..27). 32-player CircleRun <= 14 kB/s per client, >= 90% delta.

### N4. Idle dead-band (replication.cpp:302-304)
- Subject.refQ. Quiet when |dpos| <= 2 cm, |dv| <= 2 LSB, |dheading| <= 36 units, ped equal except aim <= 3 dm. While quiet replicate refQ (byte-identical idle samples). Leaving the band sets refQ and wakes; creep escapes after ~2 s; weapon change still wakes.
- Test: interest_test RateTests: still player with vz toggled +-1/32 every 700 ms -> <= 25 entries in 10 s (today ~150). Existing wake test stays green.

### N5. One render clock
- New libs/sync/netclock.{h,cpp}, per connection, fed by every WorldState and VehicleWorld (offset = recv - tick*tickMs). Baseline = 5 s wall-time min; delay = tick + p95(age) + p95(excess) + 0.5 tick; rises at once, falls per second.
- PlayerSmoother and vehicles read renderT from it with a per-class extra = class gap - tick. Remove g_vehClock* / kVehDelayMs (engine.cpp:193-195, 916-925, 3180).
- Test: sync_test: one +-30 ms jittered stream into ped and vehicle paths -> |renderT difference| <= 5 ms after 5 s (today up to ~60).

### N6. Threads, stalls, ENet
- (a) TickPeers/TickVehicles hold g_peersMtx across natives (engine.cpp:2686-2856, 3081-3260); OnWorld stamps after taking the lock (:840). Copy poses under the lock, call natives outside; stamp before the lock.
- (b) A game-thread stall stops acks, baselines die after 32 ticks (replication.cpp:401): worker sends a keep-alive ack after 100 ms without a state.
- (c) Server tick_ ignores wall time after a stall (server.cpp:492-504): derive from elapsed time and shift Pair.mask by the tick delta (replication.cpp:363).
- (d) INFERRED: enet_peer_throttle_configure(peer, 5000, 32, 0) at session.cpp:185 and server.cpp:608 (RTT jitter can make ENet drop unreliable state, peer.c:61-90, protocol.c:1524-1527). Gate on the new ENet counters first.
- Test: server_test: pause Poll 500 ms -> receiver's WorldState tick delta >= 9 (today 1).

## New 10 s counters

Client (join.cpp:145 beat + engine sync line): frames decoded/stale/rejected/malformed; WorldState inter-arrival p50/p95/max; per remote: entries, accepted, repeats split idle/duplicate/order, placed-gap p50/p95/max, ageMs p50/p95 and count at 255, network-excess p95 separate from age p95, mode % (interp/extrap/hold/buffer), longest extrapolation, delay min/max, resyncs; worker loop gap max, g_peersMtx wait max, mailbox overwrites, post-to-wire ms; PlayerState send interval p50/p95/max; vehicles frames/full/removed, accepted/stale/dup/bridge, mode %, clock-offset steps > 5 ms; ENet rtt, rttVar, packetThrottle, packetLoss.

Server (total and per receiver): per pair-tick outcome (sent, idle-suppressed, rate-not-due, budget-deferred, packet-cap, no-state); wakes and noise-wakes; samples received / forwarded / never-forwarded / forwarded-twice; PlayerState rate, inter-arrival p95, seq gaps per sender; age at forward p50/p95; ack lag; absolute-because-no-baseline; tickLate p95/max, ticksMissed; vehicle frames full vs motion-only, bytes, VehicleUpdate inter-arrival; ENet throttle and loss per peer.

Critical files: apps/server/replication.cpp, apps/server/server.cpp, apps/engine/engine.cpp, apps/engine/join.cpp, libs/protocol/protocol.cpp.
