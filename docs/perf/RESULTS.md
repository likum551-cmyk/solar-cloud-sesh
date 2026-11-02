# Netcode results: GAGE server with interest management, send rates, budget and protocol v2

The same load-test matrix as [BASELINE.md](BASELINE.md) (protocol v1, everybody sent to everybody), run on
the finished scaling work: spatial interest management with hysteresis and speed-aware radii, per-pair send
rates, a per-client byte budget, a ~1 Hz coarse channel for everyone out of range, and protocol v2 (quantised
fields, delta entries against acknowledged baselines). Design: [docs/api/server.md](../api/server.md)
(replication section), wire format: [docs/api/protocol.md](../api/protocol.md).

## Machine and method

Identical to BASELINE.md: AMD Ryzen 7 7800X3D (8 cores), 31.7 GB, Windows 11 Pro build 26300, WinLibs gcc 13.3
`-O2`, loopback only (127.0.0.1), no loss, ~0 ms latency, 2026-10-29. One process (`gage-loadtest`): the
server on its own thread (the same single-threaded `Server::Poll()` loop `gage-server` runs), N bots on 2 to 4
worker threads, one ENet host and UDP socket per bot.

```
gage-loadtest --bots N --scenario S --seconds 15 --warmup 5 --ramp 100
```

N in 50, 200, 500, 1000 and S in `spread`, `mixed`, `hotspot`, one run after the other with 3 s between runs,
default server settings (`ReplConfig` defaults, see the constants below), window = 15.x s after a 5 s warm-up
that begins when the ramp is done. The v1 numbers are the ones in BASELINE.md, taken on the v1 build; the v2
numbers are from a `gage-loadtest` built from the finished code: the binary rebuilt at the end of the work
differs from the measured one in 4 bytes (the PE timestamp and checksum, checked with `cmp -l`). The rebuilt
binary was also run once more on the heaviest case (1000 hotspot): tick work avg 30.7 ms, 1 missed tick, 77.8 % CPU,
16.5 KB/s per client, 0 refused frames (matrix: 31.6 ms, 7, 80.3 %, 16.6 KB/s, 0).

Columns as in BASELINE.md. New ones: `WorldState packet avg (entries avg)` is the average WorldState packet in
bytes (entries in it); `client gets world pkts/s` is how many WorldState packets per second the average bot
received (the server ticks at 20; a bot with nobody in range is sent no empty packets, so it is lower by design);
`coarse bytes/s` is the all-clients payload spent on the coarse channel (out of range players, ~1 Hz);
`deltas` is the share of WorldState entries sent as deltas; `refused frames` is what the bots' strict decoder
rejected (every run: 0, and 0 malformed, 0 stale).

## Results (v2)

| bots | scenario | joined | tick work avg | p99 | max | missed ticks | server CPU (of 1 core) | out total (payload) | out per client avg / p99 | WorldState packet avg (entries avg) | client gets world pkts/s (of 20) | coarse bytes/s (all clients) | deltas | refused frames |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 50 | spread | 50 | 0.32 ms | 0.67 ms | 0.78 ms | 0 | 2.1 % | 0.02 MB/s | 0.4 / 0.7 KB/s | 16 B (1.1) | 3.5 | 0.02 MB/s | 99.2 % | 0 |
| 50 | mixed | 50 | 0.48 ms | 0.86 ms | 1.65 ms | 0 | 1.6 % | 0.03 MB/s | 0.6 / 1.3 KB/s | 31 B (3.6) | 9.2 | 0.01 MB/s | 100.0 % | 0 |
| 50 | hotspot | 50 | 0.71 ms | 0.93 ms | 1.06 ms | 0 | 1.8 % | 0.05 MB/s | 1.0 / 1.7 KB/s | 42 B (5.2) | 18.2 | 0.01 MB/s | 100.0 % | 0 |
| 200 | spread | 200 | 1.72 ms | 2.69 ms | 2.85 ms | 0 | 1.6 % | 0.28 MB/s | 1.4 / 1.9 KB/s | 18 B (1.4) | 8.6 | 0.24 MB/s | 99.4 % | 0 |
| 200 | mixed | 200 | 2.22 ms | 2.94 ms | 3.02 ms | 0 | 7.2 % | 0.41 MB/s | 2.1 / 4.4 KB/s | 73 B (9.5) | 13.1 | 0.21 MB/s | 99.9 % | 0 |
| 200 | hotspot | 200 | 3.09 ms | 3.71 ms | 3.86 ms | 0 | 6.4 % | 0.76 MB/s | 3.8 / 6.9 KB/s | 148 B (20.5) | 19.5 | 0.16 MB/s | 99.9 % | 0 |
| 500 | spread | 500 | 5.78 ms | 7.42 ms | 7.36 ms | 0 | 23.4 % | 1.74 MB/s | 3.5 / 4.4 KB/s | 24 B (2.1) | 14.6 | 1.51 MB/s | 99.5 % | 0 |
| 500 | mixed | 500 | 6.94 ms | 10.8 ms | 14.7 ms | 0 | 24.7 % | 2.42 MB/s | 4.8 / 9.7 KB/s | 135 B (18.4) | 15.4 | 1.33 MB/s | 99.9 % | 0 |
| 500 | hotspot | 500 | 9.70 ms | 12.8 ms | 15.1 ms | 0 | 31.1 % | 4.38 MB/s | 8.8 / 15.9 KB/s | 333 B (47.8) | 19.8 | 1.00 MB/s | 99.9 % | 0 |
| 1000 | spread | 1000 | 15.3 ms | 23.6 ms | 27.2 ms | 2 | 53.0 % | 5.04 MB/s | 5.0 / 5.9 KB/s | 35 B (3.4) | 17.8 | 4.16 MB/s | 99.5 % | 0 |
| 1000 | mixed | 1000 | 18.6 ms | 25.6 ms | 51.7 ms | 1 | 51.8 % | 8.34 MB/s | 8.3 / 19.5 KB/s | 223 B (31.6) | 17.5 | 4.18 MB/s | 99.9 % | 0 |
| 1000 | hotspot | 1000 | 31.6 ms | 47.1 ms | 88.8 ms | 7 | 80.3 % | 16.41 MB/s | 16.6 / 25.6 KB/s | 648 B (94.2) | 19.3 | 3.61 MB/s | 99.9 % | 0 |

All 1000-bot runs joined 1000 of 1000 (v1: 991 to 996, the rest timed out in the overloaded ramp). Round trip
time reported by the bots: 12 to 19 ms in every run except the 1000 hotspot (29 ms); v1: 14 to 17 ms at 50/200,
47 ms at 500, 163 to 369 ms at 1000. Missed ticks are counts over the ~15.5 s window (v1 at 1000: 234 to 267).

## Before and after

| bots | scenario | tick work avg v1 -> v2 | missed v1 -> v2 | CPU v1 -> v2 | out total v1 -> v2 | per client avg v1 -> v2 | tick-work factor | bytes factor |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 50 | spread | 1.46 ms -> 0.32 ms | 0 -> 0 | 5.0 % -> 2.1 % | 1.48 MB/s -> 0.02 MB/s | 29.6 -> 0.4 KB/s | 4.5x | 74.8x |
| 50 | mixed | 1.44 ms -> 0.48 ms | 0 -> 0 | 4.7 % -> 1.6 % | 1.48 MB/s -> 0.03 MB/s | 29.6 -> 0.6 KB/s | 3.0x | 50.5x |
| 50 | hotspot | 1.44 ms -> 0.71 ms | 0 -> 0 | 2.2 % -> 1.8 % | 1.48 MB/s -> 0.05 MB/s | 29.5 -> 1.0 KB/s | 2.0x | 30.4x |
| 200 | spread | 14.9 ms -> 1.72 ms | 0 -> 0 | 33.7 % -> 1.6 % | 23.86 MB/s -> 0.28 MB/s | 119.8 -> 1.4 KB/s | 8.7x | 85.4x |
| 200 | mixed | 15.0 ms -> 2.22 ms | 0 -> 0 | 30.0 % -> 7.2 % | 23.91 MB/s -> 0.41 MB/s | 119.6 -> 2.1 KB/s | 6.8x | 58.2x |
| 200 | hotspot | 14.5 ms -> 3.09 ms | 0 -> 0 | 31.2 % -> 6.4 % | 23.91 MB/s -> 0.76 MB/s | 119.3 -> 3.8 KB/s | 4.7x | 31.6x |
| 500 | spread | 81.3 ms -> 5.78 ms | 97 -> 0 | 95.7 % -> 23.4 % | 87.66 MB/s -> 1.74 MB/s | 175.5 -> 3.5 KB/s | 14.1x | 50.5x |
| 500 | mixed | 84.6 ms -> 6.94 ms | 96 -> 0 | 95.6 % -> 24.7 % | 83.95 MB/s -> 2.42 MB/s | 168.1 -> 4.8 KB/s | 12.2x | 34.7x |
| 500 | hotspot | 84.3 ms -> 9.70 ms | 96 -> 0 | 92.9 % -> 31.1 % | 84.38 MB/s -> 4.38 MB/s | 168.9 -> 8.8 KB/s | 8.7x | 19.3x |
| 1000 | spread | 244 ms -> 15.3 ms | 234 -> 2 | 95.0 % -> 53.0 % | 70.90 MB/s -> 5.04 MB/s | 88.6 -> 5.0 KB/s | 16.0x | 14.1x |
| 1000 | mixed | 329 ms -> 18.6 ms | 267 -> 1 | 96.8 % -> 51.8 % | 76.34 MB/s -> 8.34 MB/s | 77.2 -> 8.3 KB/s | 17.6x | 9.2x |
| 1000 | hotspot | 299 ms -> 31.6 ms | 255 -> 7 | 95.1 % -> 80.3 % | 72.59 MB/s -> 16.41 MB/s | 77.8 -> 16.6 KB/s | 9.4x | 4.4x |

The factors are v1 divided by v2 on the same row. The 1000-bot v1 rows are an overloaded server (it produced
only 2.6 to 2.9 world updates a second per client), so their byte figures understate what v1 would send if it
kept up; the byte factors at 1000 are therefore a floor.

## What the numbers say

* **The O(N^2) is gone from the common case.** Tick work per player in `spread` is 6.4, 8.6, 11.6 and 15.3 microseconds
  at 50/200/500/1000 (tick work 0.32, 1.72, 5.78, 15.3 ms: slightly superlinear), against 29, 75, 163 and 244 in v1
  (1.46, 14.9, 81, 244 ms). 500 players no longer miss
  a tick in any scenario (v1: 96 to 97 missed in 15 s, a third of the ticks), and 1000 players run at 52 to 53 % of
  one core in `spread` and `mixed`.
* **Bytes per client are set by what is near, not by how many are online.** A player in a 1000-player spread world
  receives 5.0 KB/s (v1: 88.6), in the mixed world 8.3 KB/s (v1: 77.2). A WorldState packet is 16 to 35 B when
  one to three players are in range and never fragments (largest packet seen 1097 B, cap 1200 B).
  Entry encoding: 99.2 to 100 % of entries are deltas; a delta entry is 10.8 B for a walker, 12.6 B for a driver, an
  absolute one 19 B (`quant_test`), against 30 B in v1.
* **The worst case still fits at 1000, narrowly.** All 1000 players in six 70 to 150 m clusters (about 212
  relevant players each, 211,571 relevant pairs) take 31.6 ms of the 50 ms slot, 80 % of the core, 7 missed ticks in
  15 s (0.45 per second) and 16.6 KB/s per client. This is the only row where the byte budget binds (474,420
  entry-sends per second deferred, `budget_deferred_per_s`): the busiest clients are held to the 24 KB/s budget and
  see far players at lower rates (no client received fewer than 18.4 world packets a second, avg 19.3). I did
  not measure past 1000 players, so I make no claim about the ceiling.
* **Server CPU is now per-packet cost more than per-byte cost.** At 1000 spread the bytes dropped 14x but the server
  still sends 29,861 datagrams a second and receives 20,007 packets a second (v1: 64,314 datagrams out, fragments
  included), so CPU fell
  only 1.8x (95 % to 53 %). I did not profile; the arithmetic points at fixed cost per datagram, and the next lever
  would be fewer, fuller datagrams (for example folding the coarse page into the WorldState packet).
* **The coarse channel is most of the bytes when little is near.** At 1000 spread it is 4.16 of 5.04 MB/s (83 %), about
  4.2 KB/s per client, 87 % of its 4.8 KB/s cap (20 % of the budget in at most 4 pages a second). At 1000 hotspot it is
  3.6 of 16.4 MB/s. That is the price of every client seeing every online player as a blip without an interest radius
  (about 7 B per blip, a moving player at most about once a second). It grows with N per client; the 20 % cap bounds it
  and stretches the refresh beyond 1 s when it binds (by design, not measured above 1000 players);
  `rateCoarseHz` and `coarseSharePercent` are the knobs.
* **Fidelity was not bought with the bandwidth.** The pop-in test (`interest_test`) has players approach at
  60 + 0, 0 + 60, 60 + 60 and 300 + 300 m/s: the other player's first update arrives at 455, 455, 518 and 590 m, always
  outside the 400 m the client streams, and a moving driver gets at least 14 updates before it is within 300 m. With
  15 % packet loss and 600 players 4,695,230 entries were decoded with 0 wrong values and 0 refused frames.

## What did not move, or got worse

* Tick lateness is unchanged (a Windows timer artefact): 5.7 to 7.5 ms average at 50 to 500 bots, p99 13.8 to 14.8 ms;
  at 1000 it reaches p99 35 to 70 ms and max 81 to 193 ms. Windows `GetThreadTimes` is quantum-sampled, so CPU of a
  single second is coarse (the 50-bot rows read 1.6 to 2.1 %); only 15 s windows are quoted.
* The 1000 hotspot tick work p99 is 47 ms and max 88.8 ms: single ticks exceed the 50 ms slot, the average does not.
  Denser clusters or a slower CPU will miss ticks first in exactly this case.
* Per-second samples of one client's bytes can exceed the budget: p99 25.6 KB/s and max 30.9 KB/s against 24 KB/s
  at 1000 hotspot. Over a simulated 10 s the budget is exact (`interest_test`: 8 kB/s gives 68 to 82 kB, doubling it
  doubles the bytes). I did not isolate why single seconds overshoot in the live run (candidates: the bucket's
  burst allowance, catch-up ticks after a late tick, sample alignment).
* Bot uplink (PlayerState, client to server) is unchanged in rate, 20 packets/s, but a v2 PlayerState is 29 B
  against 33 B in v1.

## Constants (defaults, all in `server.json`, validated at startup)

| setting | value |
|---|---|
| tick / client send rate | 20 Hz / 20 Hz |
| enter radius | 400 m + 1.0 s lookahead x (speed A + speed B), capped at 600 m (`relevanceMaxRadius`) |
| leave radius | enter + 50 m (hysteresis; `relevanceLeaveRadius` 450 for a standing pair) |
| spatial grid cell / candidate scan | 128 m / every 100 ms, staggered by player id |
| send tiers, by distance minus 0.5 s x combined speed | near < 120 m: 20 Hz; mid < 250 m: 10 Hz; far: 5 Hz; unchanged for >= 500 ms: 1 Hz (wakes on movement) |
| always-relevant players | `SetAlwaysRelevant(subject, receiver\|everyone, on)`, sent at the far rate, never leave |
| priority accumulator | gains rate / tick rate per tick, cap 8, due at >= 0.999; over budget the pairs are picked by accumulator, then oldest last send, then nearest |
| per-client budget | 24,000 B/s token bucket (`clientBudgetBytesPerSec`), bucket capacity max(1200 B, two ticks of budget) = 2400 B |
| WorldState packet cap | 1200 B (never fragments) |
| coarse channel | 1 Hz per player, at most 4 pages/s, at most 20 % of the budget, entry due on a 0.5 m cell change or after 5 s |
| leave notices | repeated in 3 consecutive frames |
| protocol | v2 (`kVersion = 2`): 1 cm position, 65536-step heading, 1/32 m/s velocity, entry age in ms, deltas against an acknowledged baseline (32-tick history), v1 clients refused with VersionMismatch |

## Reproduce

```bash
export PATH="$PWD/.toolchain/winlibs-gcc13.3/mingw64/bin:$PATH"
cmake --build build/win --target gage-loadtest
build/win/tools/gage-loadtest.exe --bots 1000 --scenario hotspot --seconds 15 --warmup 5 --ramp 100
# whole matrix (about 9 minutes), then the tables of this file:
bash apps/loadtest/run_matrix.sh final build/win/tools/gage-loadtest.exe <outdir>
node apps/loadtest/results_tables.mjs <baseline_results.txt> <outdir>/final_results.txt
```

`results_tables.mjs` needs two result files with the same 12 rows; the v1 rows are the `RESULT` lines of the
baseline run (the same matrix on the v1 build, `run_matrix.sh baseline <v1 gage-loadtest.exe> <outdir>`). A running
`gage-server` prints the same server-side numbers with `stats` (`stats json` for machines).

## Limits of these measurements

* One machine, one process, loopback: bots and server share cores (at 1000 bots the process used 0.9 to 1.4 cores of 8
  in v2), no latency, jitter or NIC cost. Loss and reordering are covered only by `interest_test` (15 % loss, 600
  players, a simulation with no sockets), not by the load test.
* Each cell is one 15 s run. Repeat variation of the heaviest case: 30.7 vs 31.6 ms tick work, 1 vs 7 missed ticks,
  77.8 vs 80.3 % CPU, so single-digit differences in missed ticks are noise.
* Windows only. The Linux server build has not been run here.
* The bots do not render; "no pop-in" is established by the distance at which the first update for an approaching
  player arrives (`interest_test`), not by watching a client.
