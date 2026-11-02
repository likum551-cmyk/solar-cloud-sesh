# Netcode baseline: GAGE server before interest management (protocol v1)

Measured on the code as it was before the scaling work (protocol v1, every player sent a full
`WorldState` of everybody else on every tick), plus read-only instrumentation (`Server::metrics()`,
the `stats` command) that does not change what is sent. The v1 build is kept in the scratch
folder of the run as `baseline_gage-server.exe` / `baseline_gage-loadtest.exe`; the results after
the scaling work are in [RESULTS.md](RESULTS.md).

## Machine

| | |
|---|---|
| CPU | AMD Ryzen 7 7800X3D, 8 cores / 8 logical processors reported by Windows |
| RAM | 31.7 GB |
| OS | Windows 11 Pro, build 26300 |
| Toolchain | WinLibs gcc 13.3 (mingw-w64, msvcrt), `-O2` Release, ENet vendored in `third_party/enet` |
| Network | loopback only (127.0.0.1); no packet loss, ~0 ms latency |
| Date | 2026-10-29 |

Everything runs in one process (`gage-loadtest`): the server on its own thread (it is the same
single-threaded `Server::Poll()` loop that `gage-server` runs), N bots spread over 2 to 4 worker
threads, one `ClientSession` (one ENet host, one UDP socket) per bot. The bots, the server thread
and the OS share the 8 cores. At 1000 bots the whole process used 1.5 to 1.9 cores of 8, so the
box was not the limit: the server thread was (CPU 95 to 97 % of one core, the `cpu_pct` column).

## Exact command lines

```
gage-loadtest --bots N --scenario S --seconds 15 --warmup 5 --ramp 100
```

for N in 50, 200, 500, 1000 and S in `spread`, `mixed`, `hotspot`, run one after the other (never in
parallel) with 3 s between runs: `apps/loadtest/run_matrix.sh baseline <gage-loadtest.exe> <outdir>`. Defaults:
20 Hz tick and client send rate, seed 1, 100 joins per second, a chat line about every 120 s per bot.
The report covers a 15 s window after a 5 s warm-up that begins when the ramp is done (the log of
each run prints it as `window 15.x s`). The server ran with `maxPlayers = max(N, 32)`, the default
`ReplConfig`-less v1 behaviour, `maxConnectionsPerIp` unlimited, loopback bind.

Scenarios (the positions are the only difference):

* `spread`: bots uniformly over an 8 x 8 km square. Nearly nobody is near anybody.
* `hotspot`: all bots in 6 clusters of 70 to 150 m radius (a worst case for interest management:
  everyone near many others).
* `mixed`: half in hotspots, half spread.

Roles: spread 60 % walkers (1 to 5 m/s, wandering), 25 % drivers (30 to 60 m/s,
straight runs with turns), 15 % idlers (stand still); inside hotspots 50 % walkers, 15 % drivers (kept near their hotspot), 35 % idlers.

## Results (v1)

`tick work` = time one server tick spends working (receive drain, simulation, building and
sending the world), microseconds converted to ms. A tick that has to start while the previous one is
still running is late; `missed` counts the ticks that did not happen on schedule (20 per second
are expected). `out` is payload bytes the server hands ENet, per second, all clients together;
`per client` is the same per connected client. Window means of the per-second samples; "p99" is
over ticks (tick work) or over the per-client per-second samples (bytes).

| bots | scenario | tick work avg | p99 | max | missed ticks (per s) | server CPU (of 1 core) | out total | out per client avg / p99 | WorldState packet avg | client gets world pkts/s (of 20) |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 50 | spread | 1.46 ms | 1.86 ms | 1.94 ms | 0 (0) | 5.0 % | 1.48 MB/s | 29.6 / 29.7 KB/s | 1477 B (49 entries) | 19.9 |
| 50 | mixed | 1.44 ms | 1.86 ms | 2.45 ms | 0 (0) | 4.7 % | 1.48 MB/s | 29.6 / 31.7 KB/s | 1477 B (49) | 19.8 |
| 50 | hotspot | 1.44 ms | 1.86 ms | 2.07 ms | 0 (0) | 2.2 % | 1.48 MB/s | 29.5 / 31.7 KB/s | 1477 B (49) | 19.9 |
| 200 | spread | 14.9 ms | 31.7 ms | 66.1 ms | 0 (0) | 33.7 % | 23.9 MB/s | 119.8 / 127.0 KB/s | 5977 B (199) | 19.8 |
| 200 | mixed | 15.0 ms | 23.6 ms | 38.2 ms | 0 (0) | 30.0 % | 23.9 MB/s | 119.6 / 127.0 KB/s | 5977 B (199) | 20.0 |
| 200 | hotspot | 14.5 ms | 23.6 ms | 33.7 ms | 0 (0) | 31.2 % | 23.9 MB/s | 119.3 / 127.0 KB/s | 5977 B (199) | 19.8 |
| 500 | spread | 81.3 ms | 110.6 ms | 145.0 ms | 97 (6.2) | 95.7 % | 87.7 MB/s | 175.5 / 188.4 KB/s | 14977 B (499) | 11.6 |
| 500 | mixed | 84.6 ms | 127.0 ms | 148.5 ms | 96 (6.2) | 95.6 % | 84.0 MB/s | 168.1 / 188.4 KB/s | 14977 B (499) | 11.2 |
| 500 | hotspot | 84.3 ms | 110.6 ms | 117.3 ms | 96 (6.2) | 92.9 % | 84.4 MB/s | 168.9 / 172.0 KB/s | 14977 B (499) | 11.2 |
| 1000 | spread | 244 ms | 377 ms | 473 ms | 234 (14.5) | 95.0 % | 70.9 MB/s | 88.6 / 102.4 KB/s | 25181 B (839) | 2.9 |
| 1000 | mixed | 329 ms | 377 ms | 374 ms | 267 (16.3) | 96.8 % | 76.3 MB/s | 77.2 / 86.0 KB/s | 29857 B (995) | 2.6 |
| 1000 | hotspot | 299 ms | 377 ms | 390 ms | 255 (16.0) | 95.1 % | 72.6 MB/s | 77.8 / 94.2 KB/s | 28089 B (936) | 2.6 |

(The 1000-bot rows ran with 991 to 996 joined bots: the other 4 to 9 had their Hello unanswered for 5 s during the 100
joins/s ramp because the server was already overloaded; the server log shows `refuse HelloTimeout`.)

Round trip time reported by the bots: 14 to 17 ms at 50/200 bots, 47 ms at 500, 163 to 369 ms at 1000.

## What the numbers say

* The cost is O(N^2) in both CPU and bytes, independent of where the players are: spread and
  hotspot are indistinguishable, because v1 sends everybody to everybody. A WorldState entry is
  30 bytes, so the packet is `3 + 30 * (N - 1)` bytes: 1477 B at 50 players, 5977 B at 200, 14977 B at
  500, 29857 B at 1000. Anything above the ~1400 B MTU is fragmented by ENet (about 5 fragments at 200,
  11 at 500, 22 at 1000: size / 1400 rounded up, ENet splits at the peer MTU).
* 50 bots work (1.4 ms of a 50 ms tick, 30 KB/s per client). 200 bots still hold the tick rate but
  burn a third of a core and 120 KB/s per client (a mobile or home uplink budget is typically
  under 50 KB/s). **500 bots are already past the limit**: tick work 81 ms against a 50 ms slot,
  a third of the ticks are missed and clients see 11 instead of 20 world updates a second, with
  175 KB/s per client.
* At 1000 bots the server is overloaded by a factor of six (tick work 244 to 329 ms; only 13 to 17 %
  of the expected ticks run; each client receives 2.6 to 2.9 world updates a second and has a
  200 to 370 ms RTT). Per client bytes then look better (77 KB/s) only because the server cannot
  produce more. That is a collapse, not a capacity.
* Server CPU is the bottleneck, not the machine: the server is one thread (95 to 97 % of one core at
  500 and 1000 bots); the process as a whole used 1.5 to 1.9 of the 8 cores.
* Per-client uplink of the clients (bots to server) was constant: 20 PlayerState packets per second,
  about 33 B of payload per packet, 0.6 to 0.7 KB/s each.

## How to reproduce

```bash
export PATH="$PWD/.toolchain/winlibs-gcc13.3/mingw64/bin:$PATH"
cmake --build build/win --target gage-loadtest
build/win/tools/gage-loadtest.exe --bots 500 --scenario mixed --seconds 15 --warmup 5 --ramp 100
# whole matrix:
bash apps/loadtest/run_matrix.sh baseline build/win/tools/gage-loadtest.exe <outdir>
```

Each run prints a once-a-second table, a summary, and one `RESULT key=value ...` line that
`run_matrix.sh` collects. `--csv out.csv` writes the per-second rows. The server side numbers
come from `Server::metrics()`; a running `gage-server` prints the same with its `stats` console command
(`stats json` for a machine-readable form).

## Limits of these measurements

* One machine, one process: bots and server share cores; at 1000 bots the 4 client worker threads used up to
  ~1 core. Client-side receive rates are therefore a lower bound on what a real client would see
  when the server is healthy and an exact observation when it is not.
* Loopback: no latency, loss or MTU effects beyond ENet's own fragmentation; no real NIC cost.
* Windows `GetThreadTimes` is sampled by the scheduler quantum (~15.6 ms), so CPU % of a single
  second is coarse (the 50-bot hotspot reads 2.2 % where its siblings read 5 %); only windows of 15 s or
  more are quoted.
* Windows timer granularity makes ticks start about 7 ms late on average even when idle (the
  `tick lateness` row of each log); the 20 Hz rate itself is held (`100 % of expected ticks` at 50
  bots).
* Every number is one 15 s run; run-to-run variation at 200 bots (tick avg 14.5 to 15.0 ms across the three scenarios) is
  within 3 %. The 1000-bot rows are an overloaded system and vary more (244 to 329 ms).
