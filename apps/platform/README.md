# GAGE-MP platform

The backend behind the GAGE-MP launcher and the official game servers: accounts and login (password and Discord), roles and bans with an audit log, one-time join tokens for game servers, the official server list with live player counts, the shop (orders, payments, grants, wallet) and per-account launcher settings.

TypeScript (strict), Hono, PostgreSQL through drizzle-orm. Tests run on PGlite (Postgres compiled to WASM), so they need no database server.

- [docs/api.md](docs/api.md): every endpoint (method, path, auth, request, response, errors, rate limits, example), every table and its invariants, every environment variable.
- [docs/game-server-contract.md](docs/game-server-contract.md): what the C++ game server calls and how to retry.
- [docs/launcher-discord-flow.md](docs/launcher-discord-flow.md): what the launcher (native side) must do for Discord login and linking: the `gage-mp://` URL scheme and the claim.

## Requirements

- Node.js 22.12 or newer and npm 10 (`engines` in `package.json`).
- Production: PostgreSQL 15 or newer. Development and tests need nothing else (`DATABASE_URL` unset means in-memory PGlite).

## Run it locally

```bash
cd platform
npm ci
NODE_ENV=development ENABLE_DEV_ROUTES=1 EMAIL_DRIVER=console npm run dev
```

The server refuses to start when `NODE_ENV` is unset (an unset value used to mean "development", which would have mounted `/v1/dev/*`, the "pay any order for free" routes, on a box that merely forgot to set it). Development is something you say: `NODE_ENV=development`, and `ENABLE_DEV_ROUTES=1` for the dev routes and the built-in fake-provider webhook secret (without it, set `FAKE_WEBHOOK_SECRET` to 16+ characters). Put them in a git-ignored `.env`. The CLI commands (`db:migrate`, `admin:grant-role`, `admin:create-server`) load the same configuration and need the same variables.

With that the server uses an in-memory PGlite database (gone on exit), applies the migrations at boot, and listens on two loopback ports: the public API on `127.0.0.1:8787` and the game-server API on `127.0.0.1:8788`.

```bash
curl http://127.0.0.1:8787/health
curl http://127.0.0.1:8787/v1/openapi.json
```

With `NODE_ENV=development`, `EMAIL_DRIVER=console` prints each outgoing mail (verification and reset links with their tokens) in the log, which is how you read them in development, and the default `outbox` driver logs the link the same way. Outside development neither driver keeps or logs a working token (the outbox stores `[redacted]` in its place), so a production log or database dump cannot take over an account. To keep development data between runs use `DATABASE_URL=pglite:./.pglite` (git-ignored).

`.env` files are loaded when present (`--env-file-if-exists=.env`) and are git-ignored. A real environment variable always wins over the file.

### Commands

| command | what it does |
| --- | --- |
| `npm run dev` | server with restart on change |
| `npm test` | the whole suite (vitest on PGlite) |
| `npm run typecheck` | `tsc --noEmit` over `src`, `test` and `scripts` |
| `npm run build` | compile to `dist/` (the entry point is `dist/src/index.js`) |
| `npm start` | run the compiled server |
| `npm run db:generate` | write a new SQL migration from `src/db/schema.ts` into `drizzle/` |
| `npm run db:migrate` | apply pending migrations to `DATABASE_URL` |
| `npm run admin:grant-role` | give or remove a role from the shell (first admin) |
| `npm run admin:create-server` | register a game server and print its secret once |
| `npm run admin:quests` | list, add, set the URL or reward of, and switch on or off the quests that pay coins |
| `npm run docs:reference -- --write` | regenerate the reference block of `docs/api.md` from the code |
| `npm run docs:check` | fail when the docs and the code disagree (also part of `npm test`) |

## Database and migrations

Schema source: `src/db/schema.ts`. Migrations are plain SQL in `drizzle/`, applied in order and recorded in drizzle's `__drizzle_migrations` table, so applying is idempotent.

- `drizzle/0000_init.sql` creates the tables.
- `drizzle/0001_invariants.sql` is hand-written: the append-only triggers on `wallet_ledger` and `audit_log`, the wallet balance trigger (no overdraw except a `revoke`), the order state machine trigger and the immutable `order_items`. The server's guarantees rest on these.
- `drizzle/0002_security_hardening.sql` binds a grant's ack to the server it was delivered to (`grants.delivering_server`, checked by `grants_ack_matches_binding`) and adds the Discord flow's claim (`oauth_flows.claim_hash`, status `link_ready`).
- `drizzle/0003_quests.sql` adds the quests (`quests`, `quest_progress`), the ledger reason `quest` and a seed: "Link your Discord" on at 500 coins; the social quests (YouTube, Facebook, Instagram, TikTok, X) off with no URL; "Join the Solar RP Discord" off (not available yet).

```bash
# change src/db/schema.ts, then:
npm run db:generate                          # writes drizzle/00NN_<name>.sql and a snapshot
# triggers and other hand-written SQL go in an empty custom migration:
npx drizzle-kit generate --custom --name=my_change
# read the SQL, commit it, then apply it:
DATABASE_URL=postgres://... npm run db:migrate
```

Rules:

- A server process never migrates Postgres by itself. Run `db:migrate` at deploy time, before starting the new version (the PGlite development database is the one exception).
- Never edit a migration that was applied anywhere: add a new one.
- After a schema change update `scripts/docs-notes.ts` (a note per table) and run `npm run docs:reference -- --write`; `npm run docs:check` fails otherwise.
- The tests apply the same SQL files to PGlite, but PGlite is not Postgres: run the suite once against a real Postgres before a risky migration (see "What is verified" below).

## First admin and game servers

There is no public way to become an admin. Register the account through the launcher or the API, then on the server:

```bash
npm run admin:grant-role -- --user <username-or-email> --role admin
npm run admin:create-server -- --id eu-1 --name "GAGE EU 1" --region eu \
    --host 203.0.113.10 --port 24800 --allowed-ip 203.0.113.10
```

Quests (coins for linking Discord and for following the Solar RP channels; [docs/api.md](docs/api.md#quests)). A social quest goes live once it has its channel URL:

```bash
npm run admin:quests -- list
npm run admin:quests -- set-url --slug youtube_subscribe --url https://www.youtube.com/@<channel> --dry-run
npm run admin:quests -- set-url --slug youtube_subscribe --url https://www.youtube.com/@<channel>
npm run admin:quests -- activate --slug youtube_subscribe
npm run admin:quests -- set-reward --slug link_discord --amount 500
npm run admin:quests -- add --slug discord_event --kind visit --title "..." --url https://... --reward 100 --sort 70
# release build: node --env-file=/opt/gage/platform.env dist/scripts/quests.js <command> [flags]
```

`admin:create-server` prints the server secret once (only a hash is stored). Put it in the game server's environment. All three commands accept `--dry-run`, are audited as `cli:<os user>`, and need `DATABASE_URL`. Roles are `player`, `mod`, `admin`; the last admin cannot be removed.

## Configuration

Everything comes from environment variables, validated at start (a bad value stops the process and lists every problem; secrets are never echoed). The full table with defaults and meaning is in [docs/api.md](docs/api.md#environment-variables); the ones you will touch:

| variable | note |
| --- | --- |
| `NODE_ENV` | required (unset is refused); `production` refuses an in-memory database, a missing `DATABASE_URL`, `ENABLE_DEV_ROUTES=1` and the fake payment provider |
| `ENABLE_DEV_ROUTES` | `1` mounts `/v1/dev/*` and allows the public built-in fake webhook secret; development machines only |
| `ARGON2_MAX_CONCURRENT`, `ARGON2_MAX_QUEUE` | process-wide cap on password hashing (default 2 running, 64 waiting, then 503 `busy`) |
| `LOGIN_ACCOUNT_IP_MAX_FAILURES`, `LOGIN_ACCOUNT_MAX_FAILURES` | login lockout per identifier and address block (5) and per identifier overall (100), see the Login lockout section of the API doc |
| `DATABASE_URL` | `postgres://user:pass@host:5432/gage_platform` (required in production) |
| `PUBLIC_API_URL` | the public HTTPS URL; used for checkout and Discord redirects |
| `CORS_ORIGINS` | the launcher's WebView2 origin (default `https://app.gage`); no wildcard |
| `TRUST_PROXY` | `1` behind a reverse proxy that appends `X-Forwarded-For` |
| `HOST`, `PORT` | public listener, default `127.0.0.1:8787` |
| `INTERNAL_HOST`, `INTERNAL_PORT` | game-server listener, default `127.0.0.1:8788` |
| `DISCORD_CLIENT_ID`, `DISCORD_CLIENT_SECRET` | enable Discord login (both or neither) |
| `FAKE_WEBHOOK_SECRET` | HMAC secret of the fake payment provider, at least 16 characters; required unless `ENABLE_DEV_ROUTES=1` |

### Example environment file

`.env*` files are not written by the tooling that produced this repository (they are owner-managed and git-ignored), so this block is documentation only: copy it to `.env` (development) or `/etc/gage/platform.env` (VPS, mode 600) and replace every placeholder. It contains no real secret.

```ini
# --- required in production ---------------------------------------------------------------
NODE_ENV=production
DATABASE_URL=postgres://gage_platform:CHANGE_ME@127.0.0.1:5432/gage_platform
PUBLIC_API_URL=https://api.example.invalid

# --- network -------------------------------------------------------------------------------
HOST=127.0.0.1
PORT=8787
INTERNAL_HOST=127.0.0.1
INTERNAL_PORT=8788
TRUST_PROXY=1
CORS_ORIGINS=https://app.gage

# --- accounts ------------------------------------------------------------------------------
REQUIRE_VERIFIED_EMAIL=1
# Real delivery through Resend: verify the sending domain in Resend first, then
EMAIL_DRIVER=resend
RESEND_API_KEY=re_xxxxxxxx
EMAIL_FROM=Solar RP <no-reply@example.com>
EMAIL_LINK_BASE_URL=gage-mp://auth
# Discord login (optional; register the redirect URI <PUBLIC_API_URL>/v1/auth/discord/callback)
#DISCORD_CLIENT_ID=
#DISCORD_CLIENT_SECRET=

# --- shop ----------------------------------------------------------------------------------
# Deliberately empty: this example is a PRODUCTION environment, and the only payment provider
# that ships is the fake one, which grants items without charging anyone and which production
# refuses. Register a real PaymentProvider in buildDeps() first (see "Before launch"); until
# then the server will not start in production, on purpose. Never put PAYMENT_PROVIDER=fake or
# ALLOW_FAKE_PAYMENTS_IN_PRODUCTION=1 here (that pair is for a staging box only, with its own
# FAKE_WEBHOOK_SECRET). ENABLE_DEV_ROUTES must stay unset: startup refuses it in production.

# --- operations ----------------------------------------------------------------------------
LOG_LEVEL=info
SHUTDOWN_GRACE_SECONDS=10
```

A staging box that has to exercise the shop before a real provider exists differs in exactly these lines (and nowhere else):

```ini
NODE_ENV=production
PAYMENT_PROVIDER=fake
ALLOW_FAKE_PAYMENTS_IN_PRODUCTION=1
FAKE_WEBHOOK_SECRET=CHANGE_ME_at_least_16_chars
```

Before launch:

- Implement `EmailSender` (`src/email.ts`) with a real mail provider and pass it to `buildDeps({ email })`. The shipped `outbox` and `console` drivers do not deliver anything, and the server logs a warning while one of them is used in production.
- Implement `PaymentProvider` (`src/shop/provider.ts`) for the real payment processor and register it in `buildDeps()`. Remove `ALLOW_FAKE_PAYMENTS_IN_PRODUCTION`. Until then production refuses to start (the fake provider grants items for free).
- The launcher side of Discord login (the `gage-mp://` URL scheme and passing the claim on) is in [docs/launcher-discord-flow.md](docs/launcher-discord-flow.md); without it a Discord login cannot finish.
- Put a breached-password check in front of `checkPassword` (`src/auth/password.ts`): the bundled common-password list is short.

## Deploy on the VPS

The Solar RP VPS is deployed by `tools/deploy/deploy-platform.sh` (run it with `--dry-run` first; `docs/OPS.md`, section "Platform API"): PostgreSQL on the same host, settings in `/opt/gage/platform.env` (template `tools/deploy/platform.env.example`), unit `tools/deploy/gage-platform.service`, and Caddy's `https://solar-rp.com/api/*` route to the public listener. The notes below are the general shape it follows.

The repository's older `deploy/` folder holds the systemd unit (`deploy/systemd/gage-platform.service`), the Caddy site (`deploy/caddy/Caddyfile`) and `deploy/scripts/release.sh`, which builds, backs up, migrates and switches a release. The shape it expects:

1. PostgreSQL 15+ on the same host or a private network; a database and a role for the platform (`createdb gage_platform`, a role with `LOGIN` that owns it). Back it up with `pg_dump` (`deploy/scripts/backup-db.sh`).
2. `/etc/gage/platform.env` (mode 600, owned by the service user) with the variables above and `NODE_ENV=production`.
3. Build on the box or in CI: `npm ci && npm run build && npm prune --omit=dev`. The compiled entry point is `dist/src/index.js` and the compiled migration runner is `dist/scripts/migrate.js` (production installs have no `tsx`).
4. Migrate before switching: `node --env-file=/etc/gage/platform.env dist/scripts/migrate.js`.
5. Start with systemd. Behind Caddy (or nginx) terminate TLS and proxy only the **public** listener (`127.0.0.1:8787`), set `TRUST_PROXY=1`, and make sure the proxy overwrites or appends `X-Forwarded-For` (the platform reads the last entry).
6. **Never proxy the internal listener** (`127.0.0.1:8788`). Game servers on the same host call it on loopback; remote game servers need a private network or VPN and a firewall rule that admits only their addresses. Each game server must also be in its own IP allowlist. `INTERNAL_HOST=0.0.0.0` is never right without a firewall.
7. Check: `curl -s https://api.example.invalid/health`, then read the journal for the boot line (`journalctl -u gage-platform -n 50`). `GET /health` answers 503 when the database is unreachable.
8. First admin and first game server: the two `admin:*` commands above (with `node --env-file=... dist/scripts/<name>.js` in place of `npm run`, same flags).
9. Housekeeping runs inside the process (`MAINTENANCE_INTERVAL_SECONDS`), safe with more than one process. Several processes behind a proxy are fine (rate limits and locks live in the database); each needs its own `PORT`.
10. Stop with SIGTERM: in-flight requests finish within `SHUTDOWN_GRACE_SECONDS`.

## Layout

```
src/
  config.ts        environment schema and cross-variable production rules
  platform.ts      buildDeps() / startPlatform(): wiring, listeners, shutdown, maintenance timer
  index.ts         process entry
  deps.ts          the injected dependency record (db, clock, email, payments, discord, serverStatus)
  email.ts         EmailSender interface, outbox and console senders
  auth/            accounts, passwords, tokens, sessions, login throttle, Discord
  admin/           roles, bans, audit log, orders and products administration
  servers/         server table, credentials, UDP query probe and status cache
  game/            join tokens, verify, game sessions
  shop/            products, orders, state machine, providers, webhooks, grants, wallet
  quests/          quests that pay coins (player service, admin operations)
  settings.ts      per-account launcher settings
  maintenance.ts   housekeeping
  http/            router and middleware, route files per area, OpenAPI generation
  db/              schema, connection (pg and PGlite), migrator
  util/            errors, logger, tokens, IP handling
drizzle/           SQL migrations (0001 holds the database-enforced invariants)
scripts/           admin CLIs, migration runner, documentation generator and drift check
test/              vitest suites
docs/              api.md, game-server-contract.md, launcher-discord-flow.md
```

## What is verified, and what is not

- The suite runs on PGlite through drizzle's PGlite driver. The production driver (`pg`) and a real Postgres server have not been run against it. The SQL is standard Postgres (plpgsql triggers, partial indexes, `FOR UPDATE`, `pg_trigger_depth()`), but run the suite and `db:migrate` against a real Postgres 15+ before the first deploy.
- Discord is mocked (no request ever goes to Discord), the email senders deliver nothing, and the only payment provider is the fake one.
- The UDP server query is tested against a local responder that follows `apps/master/PROTOCOL.md`, not against the C++ server.
- The game-server contract is documented and tested through HTTP; the C++ side is not wired. The C++ HTTP client needs support for custom request headers first (see the contract).
- Bans take effect at once (the open game session is closed, unused join tokens are deleted) and are enforced when joining, verifying a join token, checking out, and on the game server's spend and grant pull. A banned mod or admin loses every moderation route. Bans do not block logging in, so a banned player can still read the reason.
- Everything in `drizzle/0002_security_hardening.sql` (the grant delivery binding, the Discord claim) ran on PGlite only. Run the suite and `db:migrate` against a real Postgres 15+ before deploying it.
