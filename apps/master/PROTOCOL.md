# GAGE UDP query (proposal for the C++ server and launcher)

Status: proposal. The master side is implemented (`src/probe.ts`, `src/query-protocol.ts`,
behind `VERIFY_UDP=1`, default 0). The C++ server answers the query (`apps/server/server.cpp`,
codec in `libs/protocol/query.{h,cpp}`; see `docs/api/server.md`, section 8). The reference
server-side implementation and its verification against the vendored ENet 1.3.18 are in
`tools/enet-query-check.cpp`.

## What it is for

1. **Master reachability check.** Before listing a newly announced `ip:port`, the master sends
   one datagram to that UDP port and lists the server only if a GAGE server answers. This
   catches closed firewalls and typos, and stops `POST /announce` from listing addresses where
   nothing runs.
2. **Launcher ping and live counts.** The same datagram gives the launcher a round-trip time
   and the current player count over UDP. `libs/net/ping.cpp` measures TCP connect time,
   which cannot work for a server that only listens on UDP.

What it does **not** prove: only that something at that address speaks this query. A hostile
host can fake the reply. Abuse control is the TCP-verified source address (the master never
accepts an address from the request body), the per-address entry cap and the announce rate
limit (README). The query is a liveness check, not authentication.

## Datagrams

Sent to the game port (the same UDP port ENet listens on). Both are exactly 24 bytes. Integers
are little-endian, like the game protocol.

Request (master or launcher to server):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `47 41 47 45` (ASCII "GAGE") |
| 4 | 1 | kind = `0x01` |
| 5 | 1 | query version = `1` |
| 6 | 2 | zero |
| 8 | 8 | nonce, random per probe, opaque to the server |
| 16 | 8 | zero (padding, so the reply is not larger than the request) |

Reply (server to the request's source address and port):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `47 41 47 45` |
| 4 | 1 | kind = `0x81` |
| 5 | 1 | query version = `1` |
| 6 | 2 | `u16` game protocol version (`gage::proto::kVersion`) |
| 8 | 8 | the request's nonce, copied unchanged |
| 16 | 2 | `u16` current players |
| 18 | 2 | `u16` max players |
| 20 | 4 | zero (reserved; receivers ignore it) |

Test vectors (nonce `01 02 03 04 05 06 07 08`, protocol 1, players 3, max 32; the Node tests pin
these bytes, the C++ side should assert the same):

```
request 474147450101000001020304050607080000000000000000
reply   474147458101010001020304050607080300200000000000
```

Reply field by field: `47414745 81 01 0100 0102030405060708 0300 2000 00000000`.

Query format v2 (2026-11-02, "update ping"): a request whose `u16` at offset 6 is exactly `2` (the master always sends
`0`, as above) is answered with a reply that also carries the latest client version in bytes 20..22 and `02` in byte 5.
The master never asks for it and always gets the v1 reply described here; layouts and rules:
`docs/api/protocol.md`, "The UDP query (formats v1 and v2)".

Server rules:

- Answer only a datagram that is exactly 24 bytes with the full prefix `GAGE 01 01`. Anything
  else is not ours: leave it to ENet. Never reply to a malformed query.
- Reply to the source address and port of the request, from the game socket (`host->socket`).
- Rate-limit replies (suggested: 20 per second per source address, 2000 per second in total,
  silently dropping the rest). The reply is not larger than the request, so the server is no
  amplifier, but an unlimited responder is still a free packet source.
- `protocol` is the version the server enforces in `Hello`; `players` and `maxPlayers` are the
  live values. The master rejects an announce whose `protocol` differs from the reply's.

Master rules (implemented): new entries are probed, heartbeats for a live entry are not;
the request is sent `PROBE_ATTEMPTS` times (default 3) spread over `PROBE_TIMEOUT_MS` (default
2000); a datagram from any other source address or port is ignored; a datagram from the target
that is not a valid reply to this nonce fails the probe at once (`bad-reply`); silence is
`timeout` (also what a closed port looks like). Failure is `422 unreachable`.

## Why ENet cannot mistake this for its own traffic

The query shares the game's UDP socket, so two things must hold: ENet must never act on a
query, and no ENet datagram may be taken for a query. Both follow from ENet's header layout
(`third_party/enet/protocol.c`, `include/enet/protocol.h`).

Every ENet datagram starts with `ENetProtocolHeader.peerID`, a big-endian `u16`: bit 15 =
sent-time flag, bit 14 = compressed flag, bits 13..12 = session id, bits 11..0 = peer id.

The magic's first two bytes `47 41` read as `0x4741`: sent-time flag clear, **compressed flag
set**, session 0, **peer id 1857**.

- **Without the intercept callback** (older servers) ENet drops the query.
  `enet_protocol_handle_incoming_commands` (protocol.c 1026 to 1039) strips the flags and
  looks up the peer: id 1857 is neither `0xFFF` (the "new connection" slot) nor below
  `host->peerCount`, so it executes `return 0` at line 1039, before reading any command and
  before touching any state. GAGE builds the host with `peerCount = kMaxPlayers = 1024`;
  any `peerCount` up to 1857 behaves the same (1857 would be an index past the table).
  `tools/enet-query-check.cpp` check A shows it on a real host: the datagram is received,
  no event, no peer, no reply. A master with `VERIFY_UDP=1` then sees a timeout, which is the
  honest answer for a server that does not speak the query.
- **With the intercept callback** (the proposal) `enet_protocol_receive_incoming_commands`
  calls `host->intercept` for every received datagram before parsing it (protocol.c 1264 to 1280);
  returning 1 makes ENet `continue` to the next datagram, so ENet never sees the query. This is
  the standard ENet hook for raw packets (ENet 1.3 ChangeLog).
- **The other direction.** A datagram a client sends to the server carries, in that field,
  either the server-side peer index of the connection (below `peerCount`, so below 1024) or
  `0xFFF` for the first CONNECT, with at most the sent-time flag, compressed flag and a session id
  added. A peer id of 1857 can occur in none of them, and the callback additionally requires
  the exact size and the six-byte prefix `GAGE 01 01`. The observed first two bytes on real ENet
  traffic in check D are `8fff` (CONNECT: sent-time flag, peer `0xFFF`) and `8000` (peer 0).
- **Why this stays true.** It depends on the server's ENet `peerCount` staying at or below
  1856. Assert it where the host is created:
  `static_assert(gage::proto::kMaxPlayers <= 0x740)`. `test/query-protocol.test.ts` pins the
  magic's decoded fields and checks the peer id against 1024.
- ENet's optional checksum and range-coder compression are not used by GAGE; neither would
  change the above, because the peer lookup rejects the datagram first.

## Reference server-side code

`tools/enet-query-check.cpp` contains `QueryIntercept(ENetHost*, ENetEvent*)`, about 30 lines,
verified on ENet 1.3.18. Wiring it is one line after the host is created:

```cpp
host->intercept = QueryIntercept;
```

and adding the rate limit. The C++ server reads `players`, `maxPlayers` and the protocol
version from its own state instead of the constants in the harness.

## Checking the claims against ENet

Build the harness by hand (it is not in the CMake build). From the repository root, with the
WinLibs toolchain from `CLAUDE.md` on `PATH`:

```bash
E=third_party/enet
mkdir -p /tmp/enetcheck && cd /tmp/enetcheck
gcc -O1 -I ../../$E/include -c ../../$E/{callbacks,compress,host,list,packet,peer,protocol,win32}.c
g++ -std=c++17 -O1 -I ../../$E/include ../../apps/master/tools/enet-query-check.cpp *.o \
    -o enet-query-check.exe -lws2_32 -lwinmm
./enet-query-check.exe             # all checks A to D, prints ALL CHECKS PASSED
./enet-query-check.exe --serve 8   # only the responder, prints "PORT <n>", exits after 8 s
```

Adjust the relative paths to wherever you build. On 2026-10-29 all checks passed and the master's
`probeServer` accepted the C++ reply from `--serve` (cross-implementation check).
