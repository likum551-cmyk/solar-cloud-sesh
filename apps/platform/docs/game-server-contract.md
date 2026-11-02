# Game server contract

What the official GAGE-MP game server must do against the platform, and what it can rely on. Requests, responses and error codes of every route are listed in full, with examples, in [api.md](api.md#game-server-api-internal-listener); this document is the behaviour: the order of calls, what to retry, what to treat as fatal.

Routes used by a game server (all on the **internal listener**, default `127.0.0.1:8788`, never on the public one):

| call | when |
| --- | --- |
| `POST /internal/game/verify` | a client connects and presents its join token |
| `POST /internal/game/session-end` | a client leaves, is kicked, or the server shuts down |
| `GET /internal/game/grants` (alias `POST /internal/game/grants/pull`) | after verify, and whenever the server wants to check for new purchases |
| `POST /internal/game/grants/:id/ack` | after a grant or revoke has been applied and saved |
| `GET /internal/game/wallet` (alias `POST /internal/game/wallet/balance`) | to show or check a balance |
| `POST /internal/game/wallet/spend` | the player buys something in game with platform currency |

## 1. Credentials

Every internal call carries two headers:

```
X-Gage-Server-Id: eu-1
X-Gage-Server-Secret: gss_...
```

- The server row is created by an admin (`npm run admin:create-server -- --id eu-1 --name "GAGE EU 1" --region eu --host 203.0.113.10 --port 24800 --allowed-ip 203.0.113.10` or `POST /v1/admin/servers`). The secret is shown **once** at creation or rotation and only its SHA-256 is stored; keep it in the game server's environment or a root-only config file, never in the repository or the logs.
- The secret is compared in constant time. A wrong secret and an unknown id give the same answer: HTTP 401 `invalid_server_credentials`.
- The call must come from an IP in the server's allowlist, else HTTP 403 `ip_not_allowed`. The address is the TCP peer, never `X-Forwarded-For`. A server with an empty allowlist cannot call at all.
- Rotating the secret (`POST /v1/admin/servers/:id/rotate-secret`) invalidates the old one at once, so there is a short window in which the game server fails closed: rotate and restart the game server with the new secret together.
- Both 401 and 403 are configuration errors, not transient: log them loudly, do not retry in a tight loop, and refuse new players until fixed (fail closed).
- Do not log the headers.

**Integration note for the C++ server.** `HttpPost` in `apps/server/httpclient.h` sends a fixed header set (`Host`, `User-Agent`, `Accept`, `Content-Type`, `Content-Length`, `Connection`) and has no parameter for extra headers, and there is no GET. To call the platform it needs one addition: an optional list of extra request headers that it writes verbatim (and rejects when a value contains CR or LF). Until then no internal call can authenticate. That is the reason every GET route has a POST alias (`POST /internal/game/grants/pull`, `POST /internal/game/wallet/balance`) that takes the same parameters in a JSON body. The platform deliberately does not accept the secret in the body or the query string, which are logged far more often than headers.

All calls take and return JSON (`Content-Type: application/json` on POST bodies, 415 otherwise), with bodies under 64 KiB. Every response has `X-Request-Id`: log it next to your own error so an operator can find the platform's log line.

## 2. A player connects: verify

The launcher obtains a join token (`POST /v1/game/join`, 60 seconds, bound to the account and to this server id) and hands it to the game, which sends it to the game server in its connect packet. The game server must not let the player in until verify says `ok: true`.

```
POST /internal/game/verify
{ "token": "gmj_..." }
```

**Every verdict is HTTP 200.** The body decides:

| body | meaning | game server action |
| --- | --- | --- |
| `{ "ok": true, "accountId", "username", "roles", "emailVerified", "sessionId", "ban": null }` | token valid, now consumed | let the player in, use `username` as the in-game name, keep `accountId` and `sessionId` |
| `{ "ok": false, "reason": "invalid_token" }` | unknown, malformed, or issued for another server | refuse the connection |
| `{ "ok": false, "reason": "expired" }` | older than 60 s | refuse; the player joins again from the launcher |
| `{ "ok": false, "reason": "used" }` | already redeemed (replay, or a second connect attempt) | refuse |
| `{ "ok": false, "reason": "banned", "accountId", "username", "ban": { "reason", "expiresAt" } }` | the account is banned now (a token issued before the ban no longer exists, a ban deletes the account's unused tokens, so that case is `invalid_token`; `banned` is what a token that raced the ban gets) | refuse and show the ban reason; `expiresAt` null means permanent |

Rules the game server can rely on:

- **Single use.** The token is consumed by the first verify that reaches it, whatever the outcome that follows. A network timeout on the verify call is therefore ambiguous: the token may have been consumed. Do not retry verify with the same token to "make sure"; refuse the connection and let the player rejoin from the launcher with a new token (they get one in a second). The one exception is a transport failure that definitely never reached the platform (connection refused): the token is still good for the remainder of its 60 seconds.
- **Server binding.** A token minted for `eu-1` is `invalid_token` on `eu-2`.
- **One session per account.** A successful verify opens the account's game session on this server and closes any older open session of that account (on this or another server) as `superseded`. The same account cannot be in two places. The platform does not tell the old server: it learns of it only because that player's grant and wallet calls start answering 409 `no_active_session`. Treat that answer for a player you believe is connected as "this player has joined elsewhere" and disconnect them (or, at least, stop selling to them).
- **Roles** come from the database at verify time. A role change while the player is connected is not pushed; read it again at the next verify.
- **Username** is unique case-insensitively and is the in-game name. This API version has no rename, but key your own persistent data by `accountId`, never by name, so a future rename cannot orphan it.
- `emailVerified` is informational; the platform already refuses to issue join tokens to unverified accounts while `REQUIRE_VERIFIED_EMAIL=1`.

## 3. A player leaves: session-end

```
POST /internal/game/session-end
{ "accountId": "<uuid>", "reason": "quit" }   // sessionId or accountId (or both) is required
```

`reason` is one of `quit`, `timeout`, `kicked`, `shutdown`, `crash` (default `quit`). The answer is `{ "ended": n }`: `0` means nothing was open (already ended, superseded, or never verified), which is not an error. The call is idempotent, so retry it on a transport failure. Send it on every disconnect path, including a server shutdown for each connected player (a session left open is closed by the platform's housekeeping after `GAME_SESSION_MAX_HOURS`, but until then purchases are still delivered to a server the player has left).

Grant and wallet calls need an **open session of that account on this server**; after session-end they answer 409 `no_active_session`.

**A ban ends the session.** When a moderator bans an account the platform closes its open game session at once (end reason `banned`) and deletes its unused join tokens. The game server is not told; it finds out at its next call for that account: `wallet/spend` and the grant pull answer **403 `account_banned`** (with `details.reason` and `details.expiresAt`), checked before the session, so this is what a banned player gets even though the session is already closed (a spend with a key that was already applied is refused too). Treat it as "disconnect this player, show the reason". Poll for grants every 30 to 60 seconds while a player is connected, so a ban is noticed within that time.

## 4. Purchases: grants

A shop purchase produces ledger entries the game server applies to the player. Wallet credits (in-game currency) are applied by the platform itself and never appear here; the game server sees only **item** and **entitlement** grants and the **revokes** of them (refund or chargeback).

```
GET /internal/game/grants?accountId=<uuid>
-> { "grants": [ { "id", "seq", "kind": "grant" | "revoke", "revokesGrantId", "type": "item" | "entitlement", "sku", "payload", "quantity", "orderId", "createdAt" } ] }
```

Loop:

1. Pull. Entries are the account's pending ones in `seq` order (oldest first), at most 100 per call: after acking a full page, pull again until the list is empty. Reading does **not** consume anything, but it **binds the returned entries to your server** (see "Delivery binding" below). A banned account is refused (403 `account_banned`).
2. For each entry in order: apply it to the player's data and **persist** it.
3. Only then `POST /internal/game/grants/:id/ack` for that entry (no body). The answer is `{ "id", "alreadyAcked" }`. Once acknowledged an entry is never delivered again, also across platform restarts: the state is in the database.

**Delivery binding.** Pulling records the pulling server as the entry's deliverer (`grants.delivering_server`), and only that server's ack counts. An ack from any other server, and an ack of an entry nobody pulled, is **409 `grant_not_pulled`** and changes nothing (the database refuses such an ack too). Consequences for the game server:

- Always pull before you ack, on the server that holds the player's session. Acking something you did not pull is a bug.
- An account has one open session, so the server holding it is the deliverer. If the player moves to another server, that server's pull takes the pending entries over; your later ack for them is 409 `grant_not_pulled`: the new server delivers them (apply idempotently by `id`, as below, so a double application is harmless). Treat that 409 as "not mine any more", drop it, do not retry.
- A repeat ack of an entry **you** acknowledged is still 200 with `alreadyAcked: true`. Another server never gets that answer for an entry that is not its own.

What an entry means:

| `kind` | `type` | `payload` | action |
| --- | --- | --- | --- |
| `grant` | `item` | `{ "type": "item", "key": "hat.red", "quantity": 1 }` | give `quantity` of the item `key`, times the entry's `quantity` |
| `grant` | `entitlement` | `{ "type": "entitlement", "key": "vip", "durationDays": 30 }` | set the status `key`; with `durationDays` it runs that long from now (extend an existing one), without it is permanent |
| `revoke` | same as the grant it undoes | the original payload | undo the grant named by `revokesGrantId`: take the item(s) back, end the entitlement |

Delivery guarantee: **at least once**. If the game server crashes after applying an entry and before the ack, the entry is delivered again. Therefore:

- **Apply idempotently by `id`**: store the ids of applied grants with the player's data (or make the change a no-op the second time) and skip an id you already applied, but still ack it.
- Ack **after** the change is saved, never before: acking first and crashing loses the item for good, which is the one failure the platform cannot repair.
- A revoke is delivered after the grant it undoes (higher `seq`). If the player never received the grant (it was still pending when the order was refunded) the pair is still delivered in order; applying the grant then the revoke leaves the player with nothing, which is correct.
- Entries stay pending while the server is down or the player is offline. Pull again after each verify. A periodic pull while the player is connected (every 30 to 60 seconds is plenty) delivers purchases made during play.
- Acking an id that does not exist gives 404 `grant_not_found`; acking twice is fine (`alreadyAcked: true`). An ack is not tied to an open session (the server may finish applying after the player left, as long as it is still the entry's deliverer) and the platform records which server acknowledged each entry (`grants.acked_by`, always equal to `delivering_server`). One server cannot ack another server's entries, whether or not it knows their ids.

An order becomes `fulfilled` when its last pending grant is acknowledged. A game server that never acks leaves orders `paid` and the entries pending forever; that is visible in the admin order view.

## 5. In-game currency: wallet

```
GET  /internal/game/wallet?accountId=<uuid>            -> { "balances": [ { "currency": "coins", "balance": 1500 } ] }
POST /internal/game/wallet/spend
{ "accountId", "currency": "coins", "amount": 500, "idempotencyKey": "eu-1:purchase:8841", "memo": "Sultan RS" }
-> 200 { "replayed": false, "balance": 1000 }
```

- `amount` is a positive integer of the currency's smallest unit. The platform has no "add" call for game servers: currency is credited only by paid orders, so a compromised game server can spend a connected player's balance but cannot mint any.
- **`idempotencyKey`** (8 to 64 characters of `A-Za-z0-9_.:-`) is chosen by the game server **once per purchase attempt**, before the first send, and reused on every retry of that attempt (derive it from your own purchase id, not from a clock or a random number generated per send). The same key with the same parameters returns the original result with `replayed: true` and charges nothing more; the same key with different parameters is 409 `idempotency_mismatch`. Keys are scoped to the server id, so two servers cannot collide.
- A balance too low gives 409 `insufficient_funds` with `details.balance`, and nothing is charged. A balance never goes negative through a spend (the database refuses it); only a refund's clawback can push it below zero, after which spends fail until purchases pay the debt off.
- Resend on a timeout or a 5xx with the **same key**: either the spend was applied (you get `replayed: true`) or it was not (it is applied now). Never generate a new key for a retry.
- Spending is allowed only while the account has an open session on this server (409 `no_active_session` otherwise) and the account is not banned (403 `account_banned`, checked first, also for a replayed key; the ban is re-checked under a row lock, so a spend cannot slip in while a ban is being applied).
- Deliver the thing the player bought only after a 200. If the game server crashes between the 200 and delivering it, the player has paid for nothing: record the intent (key, item) locally before the call and finish delivery from that record on restart, replaying the same key to learn whether it was charged.

## 6. Failures and what to do

| HTTP | code | meaning | game server action |
| --- | --- | --- | --- |
| 200 | (verify only) `ok: false` | a verdict, not a failure | follow the table in section 2 |
| 400 | `invalid_request`, `invalid_json` | your request is malformed | a bug on the game server: log with the request id, do not retry |
| 401 | `invalid_server_credentials` | wrong id or secret | configuration error, fail closed, alert |
| 403 | `ip_not_allowed` | the call came from an address outside the allowlist | configuration error, fail closed, alert |
| 403 | `account_banned` | spend or grant pull for a banned account | disconnect the player, show `details.reason`; do not retry |
| 404 | `grant_not_found` | no such grant | log; do not retry |
| 409 | `no_active_session` | the account has no open session on this server | the player is not (or no longer) connected here: drop the work |
| 409 | `grant_not_pulled` | ack of an entry this server did not pull, or that another server now delivers | drop it; pull first, ack only your own |
| 409 | `insufficient_funds`, `idempotency_mismatch` | see section 5 | per section 5 |
| 413, 415 | `payload_too_large`, `unsupported_media_type` | malformed request | a bug on the game server |
| 429 | `rate_limited` | too many requests | wait `Retry-After` seconds |
| 500, 502, 503 or no answer | `internal_error`, network failure | the platform is down or in trouble | **infrastructure failure**, not a verdict |

Any non-200 answer to **verify** and any transport failure means "could not tell". The game server must **refuse the player** (fail closed): admitting players when the platform cannot say who they are would let a banned or unauthenticated client in. Use a short timeout (2 to 3 seconds), at most one quick retry for a connect error (see the single-use note in section 2), and tell the player to try again.

For grants, wallet and session-end, retry transient failures (timeouts, 5xx, 429) with backoff and jitter, bounded; all of these calls are safe to repeat (session-end is idempotent, ack is idempotent, pull does not consume, spend is idempotent by key).

Never call the platform from the game tick loop: use a worker thread or an asynchronous request, as the existing HTTP client expects.

## 7. Operating notes

- The game server's identity is its id plus secret plus source IP. Run the game server on the address in the allowlist; behind NAT or a floating IP put the egress address in it.
- Platform downtime does not stop players who are already connected. It stops new joins (verify fails closed), purchases keep accumulating as pending grants and are delivered after the next pull.
- The platform logs every internal call with the server id, the route and the `requestId`. Join and spend identifiers appear in logs; tokens, secrets and passwords never do.
- Time: expiry decisions (join token, ban) are made by the platform's clock. The game server never has to compare timestamps itself.
