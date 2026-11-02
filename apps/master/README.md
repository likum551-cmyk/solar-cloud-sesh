# GAGE-MP master server list

Game servers announce themselves to this HTTP service; the GAGE launcher fetches the list.
Phase P2 of `docs/ENGINE-PLAN.md`. TypeScript (strict), Hono on Node, zod, vitest. State is in
memory behind a `ServerStore` interface so Postgres can replace it later.

```
game server --POST /announce (every ~30 s)--> master <--GET /servers-- launcher
                      \--- optional: master sends one UDP query to ip:port (PROTOCOL.md) ---/
```

## Run

Node 22.12 or newer.

```bash
cd master
npm install            # .npmrc sets legacy-peer-deps (npm 10.9 crashes on vitest's optional peers)
npm run dev            # tsx watch; reads ./.env if present
npm test               # vitest run
npx tsc --noEmit       # typecheck src + test
npm run build          # tsc -> dist/
npm start              # node dist/index.js; reads ./.env if present
```

Local smoke test without a reverse proxy (loopback announces need `ALLOW_PRIVATE=1`):

```bash
ALLOW_PRIVATE=1 npm run dev
curl -s -X POST localhost:8080/announce -H 'content-type: application/json' \
  -d '{"port":24800,"name":"Test","gamemode":"freeroam","lang":"en","players":1,"maxPlayers":32,"protocol":1}'
curl -s localhost:8080/servers
```

## Configuration

Environment variables; unset or empty means the default; an invalid value stops the process at
start with exit code 1 and a log line naming every bad variable. There is no secret in this
service. Save the block below as `.env` next to `package.json` (it is git-ignored) to override
defaults. (A committed `.env.example` with this content was not created: the tool that wrote
these files is not allowed to write `.env*` paths. Copy it by hand.)

| Variable | Default | Meaning |
|---|---|---|
| `HOST` | `127.0.0.1` | listen address. Keep loopback behind a proxy; `0.0.0.0` or `::` only for direct exposure |
| `PORT` | `8080` | listen port (`0` = ephemeral, for tests) |
| `TTL_SECONDS` | `90` | an entry disappears this long after its last heartbeat |
| `TRUST_PROXY` | `0` | `1` = client address is the first hop of `X-Forwarded-For` (see Deploying) |
| `ALLOW_PRIVATE` | `0` | `1` = accept private, loopback and other non-public source addresses (local testing) |
| `VERIFY_UDP` | `0` | `1` = probe new entries with the UDP query first (PROTOCOL.md); the C++ server answers it (`docs/api/server.md`, section 8), so the game's UDP port must be reachable from the master |
| `PROBE_TIMEOUT_MS` | `2000` | overall deadline of one probe |
| `PROBE_ATTEMPTS` | `3` | datagrams sent per probe, spread over the deadline |
| `MAX_INFLIGHT_PROBES` | `64` | concurrent probes; beyond it `503 probe_busy` |
| `MAX_PER_IP` | `8` | live entries per client address (IPv6: per /64) |
| `MAX_ENTRIES` | `2000` | live entries in total (the launcher reads at most 2000) |
| `RATE_LIMIT_BURST` | `20` | token bucket size per client address on `POST /announce` |
| `RATE_LIMIT_PER_MINUTE` | `60` | token bucket refill per client address |
| `SWEEP_INTERVAL_SECONDS` | `10` | expiry sweeper period |
| `SHUTDOWN_GRACE_SECONDS` | `10` | on SIGINT/SIGTERM, how long open requests may finish before they are dropped |
| `LOG_LEVEL` | `info` | `debug`, `info`, `warn`, `error`; logs are JSON lines on stdout |

Flags accept exactly `0` or `1`.

```dotenv
HOST=127.0.0.1
PORT=8080
TTL_SECONDS=90
TRUST_PROXY=0
ALLOW_PRIVATE=0
VERIFY_UDP=0
PROBE_TIMEOUT_MS=2000
PROBE_ATTEMPTS=3
MAX_INFLIGHT_PROBES=64
MAX_PER_IP=8
MAX_ENTRIES=2000
RATE_LIMIT_BURST=20
RATE_LIMIT_PER_MINUTE=60
SWEEP_INTERVAL_SECONDS=10
SHUTDOWN_GRACE_SECONDS=10
LOG_LEVEL=info
```

## API

### `POST /announce`

`Content-Type: application/json` (anything else is `415`), body at most 4096 bytes.

```json
{ "port": 24800, "name": "My Server", "gamemode": "freeroam", "lang": "en",
  "url": "https://example.org", "players": 3, "maxPlayers": 32, "protocol": 1 }
```

| Field | Rule |
|---|---|
| `port` | integer 1 to 65535; the UDP game port |
| `name` | 1 to 64 characters after trimming; no control, bidi-override or unpaired-surrogate characters |
| `gamemode` | 1 to 32 UTF-8 bytes (the launcher blanks longer labels), same character rule |
| `lang` | language tag such as `en`, `pt-BR`, `zh-Hans` (at most 8 bytes, the launcher's cap) |
| `url` | optional; empty or an `http(s)` URL without credentials, at most 256 characters |
| `players` | integer 0 to `maxPlayers` |
| `maxPlayers` | integer 1 to 1024 |
| `protocol` | integer 0 to 65535 (`gage::proto::kVersion`) |

The listed address is the IP of the TCP connection plus `port`; the body cannot name another
address. Unknown body fields are ignored.

`200` `{ "status": "ok", "key": "88.99.0.1:24800", "created": true, "ttlSeconds": 90, "heartbeatSeconds": 30 }`
(announce at least every `heartbeatSeconds`). Errors are JSON `{ "error": code, "message": text }`:

| Status | `error` | Cause |
|---|---|---|
| 400 | `invalid_json`, `validation_failed` (with `issues: [{path, message}]`), `bad_forwarded_for` | bad body, or `TRUST_PROXY=1` without a valid `X-Forwarded-For` |
| 403 | `private_address`, `too_many_servers` | non-public source (unless `ALLOW_PRIVATE=1`); `MAX_PER_IP` live entries already |
| 413 | `payload_too_large` | body over 4096 bytes |
| 415 | `unsupported_media_type` | not `application/json` |
| 422 | `unreachable`, `protocol_mismatch` | `VERIFY_UDP=1` only: no valid reply to the UDP query; the reply's protocol differs from the announced one |
| 429 | `rate_limited` | token bucket empty; `Retry-After` is set |
| 503 | `server_list_full`, `probe_busy` | `MAX_ENTRIES` reached; too many probes running (`Retry-After` set) |

### `GET /servers`

`Cache-Control: public, max-age=10`. A JSON object keyed by `ip:port` (IPv6 as `[addr]:port`),
busiest first, in the shape of RAGE:MP's master list:

```json
{ "88.99.0.1:24800": { "name": "My Server", "url": "https://example.org", "lang": "en",
    "gamemode": "freeroam", "players": 3, "maxplayers": 32, "protocol": 1 } }
```

`url` is `""` when the server gave none. An empty list is `{}`.

### `GET /healthz`

`200 { "status": "ok", "servers": <live count>, "uptimeSeconds": n }`, or
`503 { "status": "error" }` when the store cannot be read. Never cached.

## Launcher compatibility

`libs/net/serverfinder.cpp` parses an object keyed `ip:port` whose values carry
`name`, `url`, `lang`, `gamemode`, `players`, `maxplayers`; `GET /servers` emits exactly that, so
the launcher needs only a new endpoint (`kDefaultListUrl` or the `server_endpoint` registry
value). Verified on 2026-10-29 by compiling the launcher's own `ParseServerList`
(`libs/net/serverfinder.cpp`, unmodified, into a throwaway harness) and feeding it the output of this
service: host, port, name, url, counts, mode and lang came back as announced for IPv4 and IPv6
entries. The tests use a line-for-line TypeScript port of that parser as oracle
(`test/launcher-parser.ts`).

Added field: `protocol`, which today's launcher ignores. Constraints the parser imposes, enforced
at announce so nothing vanishes silently: `gamemode` at most 32 bytes and `lang` at most 8 bytes
(longer labels are blanked), and at most 2000 entries are read. Behaviour to know: an empty
list makes the launcher's `ParseServerList` fail with "no servers in list" (it reports an error,
not an empty list).

## Deploying (documentation only; nothing here is deployed)

Layout assumed: code in `/opt/gage-master` (`npm ci && npm run build` there), settings in
`/etc/gage-master.env` (the `.env` block above, with `TRUST_PROXY=1`), a proxy in front on 443.
These snippets were written from memory and have not been run: validate them with
`systemd-analyze verify`, `caddy validate` or `nginx -t` on the host.

`/etc/systemd/system/gage-master.service`:

```ini
[Unit]
Description=GAGE-MP master server list
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=gage-master
Group=gage-master
WorkingDirectory=/opt/gage-master
EnvironmentFile=/etc/gage-master.env
ExecStart=/usr/bin/node /opt/gage-master/dist/index.js
Restart=on-failure
RestartSec=2
# SIGTERM starts the graceful shutdown; SHUTDOWN_GRACE_SECONDS (10) plus margin
TimeoutStopSec=20
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
PrivateDevices=true
ProtectKernelTunables=true
ProtectControlGroups=true
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
CapabilityBoundingSet=
MemoryMax=256M

[Install]
WantedBy=multi-user.target
```

```bash
useradd --system --no-create-home --shell /usr/sbin/nologin gage-master
systemctl daemon-reload && systemctl enable --now gage-master
journalctl -u gage-master -f      # JSON lines
```

**TRUST_PROXY.** With `TRUST_PROXY=1` the service believes the first hop of `X-Forwarded-For`
as the client address. That is only safe when the service listens on loopback and the proxy
**replaces** the header with the real peer address, never appends to a client-supplied one. Both
snippets below do that. The service logs a warning at start if `TRUST_PROXY=1` and `HOST` is not
loopback.

Caddy (`/etc/caddy/Caddyfile`):

```caddyfile
master.example.org {
    encode zstd gzip
    request_body {
        max_size 8KB
    }
    reverse_proxy 127.0.0.1:8080 {
        header_up X-Forwarded-For {remote_host}
    }
}
```

nginx (inside the `server` block of the TLS vhost; certificates e.g. from certbot):

```nginx
limit_req_zone $binary_remote_addr zone=gage_list:10m rate=5r/s;

server {
    listen 443 ssl;
    server_name master.example.org;
    client_max_body_size 8k;

    location = /servers {
        limit_req zone=gage_list burst=20 nodelay;
        proxy_pass http://127.0.0.1:8080;
        proxy_set_header X-Forwarded-For $remote_addr;   # overwrite; not $proxy_add_x_forwarded_for
        proxy_set_header Host $host;
    }
    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header X-Forwarded-For $remote_addr;   # overwrite; not $proxy_add_x_forwarded_for
        proxy_set_header Host $host;
    }
}
```

Firewall: TCP 443 inbound to the proxy; if `VERIFY_UDP=1`, outbound UDP from the host must be
allowed (the probe goes to the announcing server's game port). `GET /servers` is not rate limited
by the service (it is a cached read); limit it at the proxy as in the nginx example.

## Behaviour and limits

- **Memory only.** A restart empties the list; servers reappear within one heartbeat
  (30 s with the default TTL). Run a single instance: the entry caps and rate limits are
  per process. `ServerStore` (`src/store.ts`) is the seam for Postgres; its methods already
  carry the invariants (per-address cap, total cap, expiry) that a SQL version must enforce
  atomically.
- **Per-address limits** use the IPv4 address or the IPv6 /64.
- **Expiry** is exact: an entry is listed while `now < lastHeartbeat + TTL`. The sweeper only
  reclaims memory; listing and caps never count expired entries.
- **Graceful shutdown** on SIGINT and SIGTERM: stop accepting, let in-flight requests finish for
  up to `SHUTDOWN_GRACE_SECONDS`, then close the rest. Windows delivers only SIGINT.
- **Logs** are one JSON object per line on stdout: `ts`, `level`, `msg`, plus fields. Rejections
  log their reason (`announce rejected`, `reason`); listing a server logs `server listed`.

## Tests

`npm test` runs vitest over `test/`: validation rejects, TTL expiry (fake timers), per-address
cap, rate limit, private-address rejection and the `ALLOW_PRIVATE` override, `TRUST_PROXY`,
list shape against the launcher parser port, the UDP probe against a fake responder (success,
wrong reply, timeout, stranger source), `VERIFY_UDP` through `POST /announce`, the sweeper,
and the real HTTP server (real connection address, graceful close, port in use).

`tools/mutation/` holds the mutation sweep used to check that each rule has a test that fails
without it (`node tools/mutation/run-mutations.mjs <masterDir> tools/mutation/mutations.json <workDir> <summary.json>`;
it edits `src/` one mutation at a time and restores every file, hash-checked, at the end: do not
edit `src/` while it runs).

## Layout

```
src/config.ts        env -> Config (zod)            src/store.ts          ServerStore + MemoryStore
src/app.ts           Hono routes and middleware     src/probe.ts          UDP probe
src/schema.ts        announce body validation       src/query-protocol.ts UDP query wire format
src/ip.ts            address normalisation/ranges   src/sweeper.ts        expiry timer
src/rate-limit.ts    token bucket                   src/server.ts         wiring, listener, shutdown
src/logger.ts        JSON logs                      src/index.ts          process entry
test/                vitest                         tools/                ENet check (C++), mutation sweep
PROTOCOL.md          UDP query proposal for the C++ side
```
