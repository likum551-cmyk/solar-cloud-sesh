# GAGE-MP platform API

The platform is the account, shop and server-list backend behind the GAGE-MP launcher and the official game servers. This document describes every endpoint, table, invariant and environment variable. The parts marked as generated are produced from the code (`npm run docs:reference -- --write`) and checked by `npm run docs:check` and `test/docs.test.ts`, so they cannot drift from what the server does. The game server's own contract is in [game-server-contract.md](game-server-contract.md).

Contents

1. [Overview](#overview)
2. [Conventions](#conventions)
3. [Accounts, passwords and sessions](#accounts-passwords-and-sessions)
4. [Discord](#discord-login-and-linking)
5. [Roles, bans and the audit log](#roles-bans-and-the-audit-log)
6. [Joining a game server](#joining-a-game-server)
7. [The server list](#the-server-list)
8. [Shop, payments and delivery](#shop-payments-and-delivery)
9. [Wallet](#wallet)
10. [Settings rules](#settings-rules)
11. [Housekeeping, logging and shutdown](#housekeeping-logging-and-shutdown)
12. Generated reference: [endpoints](#registration-login-and-sessions), [error codes](#error-codes), [tables](#tables), [environment variables](#environment-variables)

## Overview

One Node process (Hono on `@hono/node-server`, PostgreSQL through drizzle-orm) with two HTTP listeners:

| listener | default | who calls it | routes |
| --- | --- | --- | --- |
| public | `127.0.0.1:8787` (`HOST`, `PORT`) | the launcher (WebView2), behind a TLS reverse proxy | `/v1/*`, `/health` |
| internal | `127.0.0.1:8788` (`INTERNAL_HOST`, `INTERNAL_PORT`) | the official game servers | `/internal/*`, `/health`, `/v1/openapi.json` |

`/internal/*` is never mounted on the public listener (unless `INTERNAL_ON_PUBLIC_LISTENER=1`), so it cannot be reached through the public reverse proxy. In addition every internal call must carry the server's id and secret and come from an IP in that server's allowlist.

Money is always an integer in minor units (cents) with an ISO 4217 code. Ids are UUIDs, except server ids (slugs such as `eu-1`). Timestamps are ISO 8601 UTC strings.

## Conventions

- **Requests.** Bodies are JSON with `Content-Type: application/json` (415 otherwise). An empty body is "no body". Unknown fields are ignored unless a schema says otherwise. Bodies larger than the route's limit (default `BODY_LIMIT_BYTES`, 64 KiB) are refused with 413 before they are parsed.
- **Authentication.** Player routes take `Authorization: Bearer <session token>`; game servers send `X-Gage-Server-Id` and `X-Gage-Server-Secret`; payment webhooks are authenticated by the provider's signature over the raw body. Each route in the reference names its auth kind.
- **Errors.** Every failure is `{ "error": { "code", "message", "requestId", "details"? } }` with a matching HTTP status. Switch on `code` (stable), show `message` to humans (may change). Generic codes any route can return: `invalid_request` (400, `details.issues` lists `{ path, message }`), `invalid_json` (400), `unsupported_media_type` (415), `payload_too_large` (413), `unauthorized` (401), `forbidden` (403), `not_found` (404), `rate_limited` (429) and `internal_error` (500). An unexpected exception is always a generic 500 that leaks nothing; the cause is in the log under the same `requestId`. The full table of codes is under [Error codes](#error-codes).
- **Request ids.** Every response has `X-Request-Id`. A sane inbound `X-Request-Id` (8 to 64 characters of `[A-Za-z0-9_-]`) is echoed, anything else is replaced by a fresh UUID. The id is in every log line of that request and in error bodies.
- **Rate limits.** Fixed windows per client IP (IPv6 clients are grouped by /64) or per account, kept in the database, so they survive restarts and apply across processes. A refused request is 429 `rate_limited` with `Retry-After` (seconds). The numbers are in each route's entry. Login failures have their own lockout policy, described under [Accounts](#accounts-passwords-and-sessions). Behind a reverse proxy set `TRUST_PROXY=1` so the client address is read from the last `X-Forwarded-For` entry (the one your proxy appended; entries the client wrote earlier in the header are ignored). `/internal/*` always uses the socket address.
- **CORS.** Only the origins in `CORS_ORIGINS` (the launcher's WebView2 origin, default `https://app.gage`) get `Access-Control-Allow-Origin`; every other origin gets no CORS headers, so a web page elsewhere cannot read responses. Credentials are never used (bearer tokens). The internal listener sends no CORS headers at all.
- **Headers on every response.** `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`, `X-Request-Id`.
- **Idempotency.** `POST /v1/shop/checkout` takes an `Idempotency-Key` header, `POST /internal/game/wallet/spend` an `idempotencyKey` field, and webhooks are deduplicated by `(provider, event id)`. See the sections below.
- **Pagination.** Lists that can grow take `limit` and a `before=<last id>` cursor and return newest first.
- **Reference.** `GET /v1/openapi.json` returns an OpenAPI 3.1 document of the routes the listener serves, generated from the same schemas that validate requests.

## Accounts, passwords and sessions

**Registration** (`POST /v1/auth/register`): email, username (3 to 20 characters of `[A-Za-z0-9_]`, unique case-insensitively, a few names such as `admin` are reserved) and a password. The username is the in-game name; display case is kept. The answer is the same whether or not the email already has an account (no enumeration): an existing owner gets a mail saying so instead. Registering sends a verification email and does not log in.

**Passwords.**

- 10 to 128 characters, counted as Unicode code points after NFKC normalisation (so the same password typed on different systems matches).
- Rejected when it is on the bundled common-password list (also with digits or punctuation appended and with leetspeak undone), contains the username (3+ characters) or contains the email name (4+ characters). `details.problem` names the reason: `too_short`, `too_long`, `common`, `contains_username`, `contains_email`.
- The bundled list is deliberately short. For real coverage put a breached-password check (for example the HIBP range API, or a local list) in front of `checkPassword` in `src/auth/password.ts` before launch.
- Stored as argon2id (`@node-rs/argon2`) with a random salt in a PHC string. Defaults are OWASP's: `m=19456` KiB, `t=2`, `p=1` (`ARGON2_*`). A hash made with weaker parameters is replaced after the next successful login. A malformed stored hash never throws: it just does not verify.
- **Load limit.** Every hash and verify in the process (login, register, reset, password change) goes through one limiter: at most `ARGON2_MAX_CONCURRENT` (2) run at once and `ARGON2_MAX_QUEUE` (64) wait; the next one is refused immediately with 503 `busy` and `Retry-After`, so a burst of password attempts cannot exhaust memory or the thread pool. A refused request is not counted against the caller (it is not a wrong password) and does not spend a reset or verification token. `POST /v1/me/password` is additionally limited to 10 calls per hour per account.

**Email verification and password reset.** Both use random 256-bit tokens of which only the SHA-256 is stored. A token is single use (consumption is one conditional UPDATE, so concurrent attempts cannot both succeed), expires (`EMAIL_VERIFY_TTL_HOURS`, `PASSWORD_RESET_TTL_MINUTES`), is of one kind only, and a newer token voids the older. A verification token is void if the account's email changed after it was issued. Tokens are redeemed by **POST** so mail scanners that open links cannot consume them; the mail links to `EMAIL_LINK_BASE_URL` (by default the launcher's `gage-mp://auth`). A rejected weak password during reset leaves the token usable. A successful reset revokes every session, marks the email verified and clears the login lockout; **if the email was not verified before, every Discord identity attached to the account is deleted in the same transaction** (whoever registered an address they do not own must not keep a way in) and the owner is mailed. Changing the password with `POST /v1/me/password` voids every outstanding reset token. `forgot-password` always answers 202 and is limited to 10 requests per hour per client IP (`forgot-password`, checked first) and 3 per hour per email address, in that order: a client that is already limited cannot spend the address's budget and so cannot keep its owner from getting a reset mail. Verification gates joining and buying (`REQUIRE_VERIFIED_EMAIL`), never logging in. **Setting a first password needs a verified email** (403 `email_not_verified`; a Discord-only account's email is verified or absent in practice). Linking and unlinking Discord re-authenticate with the current password instead (see [Discord](#discord-login-and-linking)). Linking or unlinking a Discord identity mails the owner (`discord_linked`, `discord_unlinked`).

**Mail delivery.** The platform talks to an `EmailSender` interface (`src/email.ts`). The shipped implementations are the dev outbox (rows in `email_outbox`) and a console logger. Neither delivers mail, and neither keeps a working token: the outbox stores the body, subject and meta with every live token replaced by `[redacted]`, and the full link is logged only when `NODE_ENV=development`. Implement the interface with your provider and pass it to `buildDeps({ email })` before launch; a real sender receives the whole message, tokens included (`EmailMessage.secrets` lists them). The server logs a warning at start in production while this is not done.

**Sessions.** Login returns an opaque bearer token (`gms_` + 256 random bits). Only its SHA-256 is stored. A session is valid for `SESSION_TTL_DAYS` (30) of inactivity: each use pushes `expires_at` forward, never past the absolute cap `SESSION_MAX_DAYS` (180). `GET /v1/me/sessions` lists the live devices (the current one flagged), `DELETE /v1/me/sessions/:id` signs one out, `DELETE /v1/me/sessions` all others. **Changing the password (and resetting it) revokes every other session**, and the account is mailed.

**Login lockout.** Failures are counted on the identifier **as typed** (the username or the email, lower-cased and hashed), never on the account it resolves to, with three counters per attempt:

| counter | key | locks after | default |
| --- | --- | --- | --- |
| address | the client IP group (IPv6: /64), any identifier | `LOGIN_IP_MAX_FAILURES` | 20 |
| identifier from one block | the identifier + the IPv4 /24 or IPv6 /64 it was typed from | `LOGIN_ACCOUNT_IP_MAX_FAILURES` | 5 |
| identifier from anywhere | the identifier | `LOGIN_ACCOUNT_MAX_FAILURES` | 100 |

When a counter reaches its threshold the key is locked for `LOGIN_LOCKOUT_BASE_SECONDS` (30), doubling with each further failure up to `LOGIN_LOCKOUT_MAX_SECONDS` (3600). While any key of the attempt is locked the password is not even checked (a correct password during a lockout is refused too) and the answer is 429 `rate_limited` with `Retry-After`. A counter is forgotten after `LOGIN_FAILURE_WINDOW_SECONDS` (900) of calm, measured from the later of the last failure and the end of the lock; a successful login clears that identifier's counters (never the address-wide one), and so does a password reset or change. The counters are in `auth_throttle`, so a restart does not reset them.

What this gives, deliberately:

- **No oracle.** A lock is a function of what the caller itself typed and where from, so a 429 says nothing about accounts. Typing the email of a locked username answers exactly like typing any other email (and the other way round); an unknown identifier is counted and answered like a real one. An account has two identifiers (username, email), each with its own budget.
- **No lockout denial of service from one place.** One address block can lock only itself out of an identifier (5 failures), never the owner on another network: the real owner logs in normally while an attacker is being refused.
- **A block of addresses is not a multiplier.** Addresses in one IPv4 /24 (IPv6 /64) share the 5-failure budget for an identifier.
- **Distributed guessing still stops.** 100 failures on one identifier from at least 20 address blocks lock it for everyone (a 429 until the lock ends or a password reset clears it); fewer than that never lock the owner out.

## Discord login and linking

Authorization code flow with PKCE (S256) for a launcher that cannot receive a redirect itself:

1. The launcher calls `POST /v1/auth/discord/start` and gets `flowId`, `pollSecret` and `authorizeUrl`.
2. The player opens `authorizeUrl` in the browser and approves.
3. Discord redirects the browser to `GET /v1/auth/discord/callback`. The platform checks the `state`, redeems the code with the PKCE verifier and the client secret, reads the Discord profile (`identify email`), records the outcome on the flow and puts a random **claim** on it (32 random bytes, base64url, only its SHA-256 is stored). The page it returns redirects the browser to `gage-mp://auth/discord?flow=<flowId>&claim=<claim>` and shows a "Return to GAGE" button for the same URL; the claim is never displayed.
4. The operating system hands that URL to the launcher (the `gage-mp` scheme is registered per user), which polls `POST /v1/auth/discord/poll` with `flowId`, `pollSecret` and `claim` until the status is not `pending`. A new player gets `needs_username` and finishes with `POST /v1/auth/discord/complete` (also with the claim; the username is the in-game name and must be free). An existing player gets `complete` with a session.

What the launcher has to do (URL scheme, forwarding to the running instance, what to send) is in [launcher-discord-flow.md](launcher-discord-flow.md).

Rules:

- **State and PKCE.** The `state` is random, stored only as a hash, valid `DISCORD_FLOW_TTL_SECONDS` (600) and consumed by one conditional UPDATE, so a forged, expired or replayed callback matches nothing and causes no request to Discord. The PKCE verifier never leaves the server and is erased as soon as the callback finished.
- **Two secrets, two channels (no login phishing).** The poll secret never leaves the launcher that started the flow; the claim only ever reaches the browser of whoever approved on Discord, and from there the launcher on the same machine. Without the claim, `poll` answers `{"status":"pending"}` exactly as it does for a flow nobody approved, and `complete` is refused (400 without a `claim` field, 404 `flow_not_found` with a wrong one), so someone who starts a flow and sends the authorize URL to a victim cannot read or take the victim's approval: the victim's browser holds the claim, the attacker's launcher holds the poll secret, and only the same machine has both. The claim is compared in constant time, is single use (cleared when the flow is consumed, so a repeat is 410 `flow_used`) and dies with the flow (`flow_expired`). Results (`complete`, `linked`) are delivered once. A wrong poll secret looks exactly like an unknown flow. A refused sign-in (`access_denied`, `email_in_use`, a Discord error) also carries a claim in the handoff URL, so the launcher can show the reason and nobody else can read it.
- **Linking is two-sided too, and re-authenticated.** `POST /v1/me/discord/link` takes the account's current `password` (Solar RP sends no email, so a verified email cannot be the proof; wrong passwords share one lockout with `POST /v1/me/password` and `DELETE /v1/me/discord`, keyed on the account and the client address like a login: 401 `invalid_credentials`, then 429 `rate_limited`) and starts the same flow with the signed-in account on it. The callback attaches nothing: the identity is attached when the party that started the flow presents the claim, under a lock on the account row (the one a password reset takes), and is refused (`failed`, `password_changed`) when the password changed after the start, so approving someone else's link flow can never put your Discord on their account or theirs on yours, and the real owner's reset voids a squatter's link in flight (the reset also deletes identities from an account whose email was never verified). The owner is mailed (`discord_linked`; `discord_unlinked` on `DELETE /v1/me/discord`, which re-authenticates the same way).
- **Linking from the website** (`handoff: "web"`): the account page keeps `pollSecret` in the tab's sessionStorage and navigates to `authorizeUrl`. The callback answers 303 to `<SITE_ORIGIN>/account/#discord_flow=<flowId>&discord_claim=<claim>`: the origin is configuration (never from the request, so no open redirect) and the claim rides in the fragment, which browsers never send to a server, so no access log holds it. The page finishes with `POST /v1/me/discord/link/finish { flowId, pollSecret, claim }` under the session of the account that started the flow; any other combination is 404 `flow_not_found`. A web flow is not readable through `poll`. Someone who starts a web link and sends the authorize URL to a victim gets the claim delivered to the victim's browser, which has neither the poll secret nor the attacker's session: nothing is linked.
- **Identity** is the immutable Discord user id, never the email or name.
- **No auto-merge.** If the Discord email, verified or not, belongs to an existing GAGE account, login fails with `email_in_use`. That player logs in with their password and links Discord from the settings (`POST /v1/me/discord/link`), which is the only way to attach Discord to an existing account.
- **Unverified Discord emails are never trusted:** they are not stored on the account and do not count as verified. A verified one is stored and counts as verified.
- A Discord-only account has no password. It may set one with `POST /v1/me/password` (without `currentPassword`, needs an email) or through the emailed reset. `DELETE /v1/me/discord` is refused (`last_login_method`) while Discord is its only way in.
- The callback page is static text with `Content-Security-Policy: default-src 'none'`, `Referrer-Policy: no-referrer` and `Cache-Control: no-store`; nothing from the request is echoed. A callback whose state is invalid, used or expired is 400 and carries no `gage-mp://` URL.
- Discord is mocked in the tests; nothing contacts Discord unless `DISCORD_CLIENT_ID` and `DISCORD_CLIENT_SECRET` are set.

## Roles, bans and the audit log

**Roles** are `player` (everyone), `mod` and `admin`, read from the database on every request, so a change applies to existing sessions at once. Mods may search accounts and ban or unban players; admins may also change roles, manage servers, products and orders, and read the audit log. The last admin cannot lose the role (`last_admin`; concurrent demotions are serialised), so the platform cannot lock itself out. The first admin is created from the command line: `npm run admin:grant-role -- --user <username-or-email> --role admin`.

**Bans** carry a reason (1 to 500 characters), an optional expiry (at least 60 seconds; omit for permanent), who issued them and, when lifted, who lifted them and why. At most one ban per account is in force (`already_banned`; an expired ban is closed as `expired` when the next one is issued). Mods may ban players; admins may also ban mods; nobody may ban an admin or themselves. **Lifting a ban mirrors that:** nobody may lift their own ban, and only an admin may lift the ban of a mod or an admin (403 `forbidden_target`; every refusal is audited as `account.unban_refused`). **A banned mod or admin keeps the account but none of the powers:** every `mod` and `admin` route answers 403 `account_banned` for them, whatever roles they still hold. A ban does not stop logging in (so the player can read the reason in `GET /v1/me`).

**A ban takes effect at once**, in one transaction under a lock on the account row: the account's open game session is closed with end reason `banned`, its unused join tokens are deleted, and from then on `POST /v1/game/join` and checkout are refused, the game server's `wallet/spend` and `grants` pull answer 403 `account_banned` (also for a replayed spend key) and `POST /internal/game/verify` answers `reason: "banned"` for a token that slipped past the ban (a token issued before it is plain `invalid_token`, it no longer exists). Spends and token redemptions hold a share of the same row lock, so none can slip in between the ban being decided and the session being closed. The game server learns of the ban at its next call for the account; kicking the connection is its job.

**The audit log** records every admin and moderator action: ban, unban, role change, server create/update/rotate/delete, product create/update/delete and order refund. Each row is written in the same transaction as the change (no action without a record, no record of a refused action), holds the actor (account id, or `cli:<os user>` for the command line), target, details, client address and request id, and the table is append-only: a trigger rejects UPDATE and DELETE. Secrets never appear in it. `GET /v1/admin/audit` pages it newest first and filters by action and target.

## Joining a game server

1. The launcher calls `POST /v1/game/join { serverId }` with the player's session. The platform refuses unverified emails, banned accounts, unknown or hidden servers and servers in maintenance (staff may still join), and answers with a **join token** (`gmj_` + 256 random bits), valid `JOIN_TOKEN_TTL_SECONDS` (60), bound to that account and server. An account has at most one live token: asking again voids the previous one. At most 10 per minute per account.
2. The launcher starts the game with the token (the native client passes it to the game server in its connect packet).
3. The game server calls `POST /internal/game/verify { token }` with its credentials. The token is **single use**: it is consumed by one conditional UPDATE inside the same transaction that opens the player's game session, whatever the outcome after that check (a banned player's token cannot be retried; a ban also deletes the account's unused tokens). The answer is `ok: true` with the account id, username, roles and email state, or `ok: false` with `invalid_token`, `expired`, `used` or `banned`.
4. When the player leaves, the game server calls `POST /internal/game/session-end`.

An account has at most one open game session (a new verify supersedes the old); sessions open longer than `GAME_SESSION_MAX_HOURS` are closed as `stale`. Details for the game server are in [game-server-contract.md](game-server-contract.md).

Game server credentials. Each server has an id, a secret (`gss_` + random bits, stored only as a SHA-256, shown once at creation or rotation) and an IP allowlist. The secret is compared in constant time, an unknown id costs the same as a wrong secret, and a server with an empty allowlist cannot call at all (fail closed). Create servers with `npm run admin:create-server` or `POST /v1/admin/servers`.

## The server list

`GET /v1/servers` lists the visible official servers in `sort` order with live state from the GAGE UDP query (`apps/master/PROTOCOL.md`: a magic, a nonce and a reply with protocol version, players and max players). Each server is probed at most once per `SERVER_STATUS_TTL_MS` (5 s) however many launchers ask: one probe in flight per server, and an entry past its TTL is returned immediately while a refresh runs in the background (stale-while-revalidate). The request never blocks on a slow server: a server that has never been probed gets at most `SERVER_STATUS_WAIT_MS` (400 ms) and is reported `state: "unknown"`; a server that does not answer within `PROBE_TIMEOUT_MS` is `offline` with the reason in `detail`. A reply that is not a valid answer to our nonce (spoofed, truncated, wrong version) counts as offline. Host names are resolved and cached; the list shows `maintenance.enabled` and the message set by an admin.

## Shop, payments and delivery

**Products** have a SKU, name, description, price in minor units with a currency, a kind (`currency_pack`, `vip`, `cosmetic`, `bundle`), an active flag, a sort order, an optional HTTPS image URL, a per-order `maxQuantity` and a **grant payload**: `{ "grants": [...] }` with 1 to 20 entries of

| entry | meaning | delivered by |
| --- | --- | --- |
| `{ "type": "wallet_credit", "currency": "coins", "amount": 1000 }` | in-game currency, credited to the wallet ledger (amount x line quantity) | the platform, when the order is paid |
| `{ "type": "entitlement", "key": "vip", "durationDays": 30 }` | a named status for the game server to apply; no `durationDays` = permanent | the game server, through the grants queue |
| `{ "type": "item", "key": "hat.red", "quantity": 1 }` | an item or cosmetic (x line quantity) | the game server, through the grants queue |

A currency pack may only contain wallet credits, a VIP only entitlements, a cosmetic only items; a bundle may mix. The payload is validated when a product is created or edited, and **copied onto the order line at checkout**: later edits or deactivation never change what an existing order delivers, and a product with orders cannot be deleted.

**Orders** move through a state machine that the database enforces with a trigger:

| from | to | when |
| --- | --- | --- |
| `pending` | `paid` | a verified `payment.succeeded` for the right amount and currency |
| `pending` | `failed` | `payment.failed`, an amount mismatch, a provider error at checkout, or the order expired (`ORDER_TTL_MINUTES`) |
| `paid` | `fulfilled` | every item/entitlement grant was acknowledged (at once if there are none) |
| `paid`, `fulfilled` | `refunded` | an admin refund or a provider `refund.succeeded` |
| `paid`, `fulfilled` | `chargeback` | a provider `chargeback.opened` |
| `failed` | `paid` | **only** a verified, correct payment arriving after the order failed as `expired` or `payment_failed` (money received means goods delivered). `amount_mismatch` and checkout errors never revive: a person decides |

`refunded` and `chargeback` are terminal. A payment of the wrong amount fails the order, delivers nothing and logs an error naming the order so the operator can refund at the provider.

**Checkout.** `POST /v1/shop/checkout { items: [{ sku, quantity }] }` creates a pending order and a checkout session at the provider and returns `checkoutUrl`. The order row is committed before the provider is called, so a crash leaves a pending order that expires, never a payment without an order. With an `Idempotency-Key` header (8 to 64 characters of `[A-Za-z0-9_-]`) a retry returns the original order; the same key with a different basket is 422 `idempotency_mismatch`; simultaneous requests with one key create one order. The launcher opens `checkoutUrl` in the browser and polls `GET /v1/shop/orders/:id`.

**Payment providers.** The shop talks to a `PaymentProvider` interface (`createCheckout`, `verifyWebhook`, `refund`, all idempotent; `src/shop/provider.ts` lists the contract). `FakeProvider` implements it for development and tests: "paying" is a correctly signed webhook (`t=<unix>,v1=<HMAC-SHA256 of "<t>.<raw body>">` with `FAKE_WEBHOOK_SECRET`, 5 minute tolerance). In development the launcher can use `/v1/dev/fake-checkout/:orderId`; those routes are opt-in (`ENABLE_DEV_ROUTES=1`, which startup refuses when `NODE_ENV=production`) and never mounted otherwise, so an unset or mistyped `NODE_ENV` cannot expose them (an unset `NODE_ENV` is itself refused at startup). The built-in public webhook secret is only used with `ENABLE_DEV_ROUTES=1`; otherwise `FAKE_WEBHOOK_SECRET` (16+ characters) is required whenever the fake provider is configured. Production refuses the fake provider unless `ALLOW_FAKE_PAYMENTS_IN_PRODUCTION=1` (a staging box). A real provider (Tebex, Paddle, Stripe, a crypto processor) is one more implementation registered in `buildDeps()`; nothing else changes.

**Webhooks** (`POST /v1/webhooks/:provider`). The signature is verified over the raw body before anything in it is trusted (400 `invalid_webhook` otherwise). A verified event is answered 200 whatever the outcome, so the provider does not retry forever: `applied`, `ignored` (the order's state did not allow it), `unmatched` (no such order), `rejected` (for example an amount mismatch) or `duplicate`. **Idempotency:** `(provider, event id)` is inserted into `payment_events` under a unique constraint first, inside the same transaction as the effects; a redelivery finds the row and does nothing, and simultaneous deliveries of one event apply it once. The order row is locked and every transition is a conditional UPDATE.

**Delivery (grants).** Paying creates one `grants` row per grant entry, unique per `(order, line, entry, kind)`. Wallet credits are applied by the platform in the same transaction (the grant is born acknowledged by `platform` with a ledger credit keyed `grant:<id>`). Item and entitlement grants wait as `pending`. The game server, while the player has an open session on it, pulls them with `GET /internal/game/grants?accountId=` in `seq` order, applies them and acknowledges each with `POST /internal/game/grants/:id/ack`. Pulling does not consume, but it **binds** each returned entry to the pulling server (`grants.delivering_server`): only that server's ack counts. Any other server, and an ack of an entry nobody pulled, is 409 `grant_not_pulled` and changes nothing (the database refuses it too, `grants_ack_matches_binding`), so a server cannot mark another server's delivery as done. A player has one open session, so the server holding it is the deliverer: when the player moves, the next server's pull takes the pending entries over and the previous server's ack is refused. An ack is idempotent for the acking server and conditional (`pending` to `acked`, once); an acknowledged entry is never delivered again, also across restarts, because the state is in the database. A banned account's pull and spend are refused (403 `account_banned`). Delivery before the ack is **at-least-once** (a crash between apply and ack re-delivers), so the game server applies a grant idempotently by its `id`. The order becomes `fulfilled` when its last pending grant is acknowledged.

**Refunds and chargebacks** create a `revoke` entry for every grant of the order (unique per revoked grant, so it happens once). A revoked wallet credit is clawed back by a negative ledger entry keyed `revoke:<grantId>` that may take the balance below zero (a debt that later purchases pay off; spending is blocked meanwhile). Item and entitlement revokes are delivered and acknowledged through the same queue, after the grant they undo. `POST /v1/admin/orders/:id/refund` asks the provider first (idempotent per order); if the provider fails, nothing changes (503 `refund_failed`); the provider's later `refund.succeeded` finds the order already refunded and is ignored. Partial refunds are not supported (they are rejected and logged for manual handling).

## Wallet

In-game currency is a **ledger**, not a mutable number. `wallet_ledger` is append-only (a trigger rejects UPDATE and DELETE); each entry is a signed `delta` with a reason (`purchase`, `quest`, `spend`, `revoke`) and a unique idempotency key. `wallet_balances` is derived: a trigger on the ledger updates it in the same statement, a guard trigger rejects every other write to it, and an insert that would take a balance below zero is rejected by the database (`insufficient_funds`), except a `revoke`. Every balance therefore equals the sum of its ledger entries, and two concurrent spends can never overdraw.

The game server spends with `POST /internal/game/wallet/spend` (needs an open session of the account on that server). The `idempotencyKey` is chosen by the game server per spend and scoped to the server: resending the same request after a timeout is applied once (`replayed: true`), the same key with different parameters is 409 `idempotency_mismatch`. Players read their balances and history with `GET /v1/me/wallet` and `/v1/me/wallet/ledger`.

## Quests

Quests pay coins into the wallet (ledger reason `quest`). `GET /v1/me/quests` lists the active ones with this account's status; `POST /v1/me/quests/:slug/start` starts a visit quest and `POST /v1/me/quests/:slug/claim` pays. Kinds (tables `quests` and `quest_progress`):

- `discord_link` pays when the account has a linked Discord identity (seeded on, 500 coins).
- `visit` (YouTube, Facebook, Instagram, TikTok, X ...) cannot be verified by any API, so it is an **honor system gated by Discord**: the player starts it (the client opens its `url`), and may claim it 30 s later (`visitMinSeconds`), only with a linked Discord account. Seeded off and without a URL; the owner sets each URL and switches it on with `npm run admin:quests`.
- `discord_guild` (membership of the Solar RP Discord server) is **not available yet**: it lists as `locked` / `not_available`, a claim is 409 `quest_unavailable` and the admin script refuses to switch it on. Checking membership needs a Discord authorization with the `guilds.members.read` scope at claim time (the platform keeps no Discord tokens), which needs a Discord flow a browser can finish; today only the launcher's `gage-mp://` claim finishes one, and the launcher does not handle that link yet.

Each quest pays once per account and once per Discord account: the ledger entry's idempotency key is `quest:<questId>:<accountId>`, `quest_progress` has one row per (account, quest) and one claim per (Discord user id, quest), and the credit and the claim are one transaction, so concurrent or repeated claims pay once and unlinking Discord to link it elsewhere pays nothing more. A banned account cannot claim.

## Settings rules

`GET/PUT /v1/me/settings` store one JSON object per account for the launcher (game folder, language, start minimised ...). The platform does not interpret it but bounds it: at most `SETTINGS_MAX_BYTES` of UTF-8 (16 KiB; counted in bytes, not characters), 6 levels deep, 200 keys per object, 200 items per array, strings up to 2048 characters, keys matching `[A-Za-z0-9_.:-]{1,64}`, and no `__proto__`, `constructor` or `prototype` keys (refused, never silently dropped). A write replaces the whole object. `version` counts writes; send `expectedVersion` (0 = nothing stored yet) to make the write conditional, so two PCs cannot silently overwrite each other (409 `version_conflict` with `details.currentVersion`). At most 120 writes per account per 10 minutes.

## Housekeeping, logging and shutdown

- **Housekeeping** runs every `MAINTENANCE_INTERVAL_SECONDS` (5 min) and can run from several processes: it deletes sessions expired or revoked more than a week ago, auth tokens a week past expiry or use, rate-limit windows and quiet throttle rows older than a day, join tokens a day past expiry and Discord flows a day past expiry; closes game sessions open longer than `GAME_SESSION_MAX_HOURS`; and fails pending orders past `ORDER_TTL_MINUTES` as `expired`. It never touches orders, grants, the wallet ledger, payment events or the audit log.
- **Logging.** One JSON object per line on stdout: `ts`, `level`, `msg` and fields. Field values are JSON-encoded, so client-controlled text cannot forge a line (a field named `msg`, `level` or `ts` is renamed `field_msg` and so on). Each request logs `requestId`, method, path, status, duration, client IP and, when known, the account or server id. Tokens, passwords and secrets are never logged. `GET /health` is logged at debug level.
- **Health.** `GET /health` answers `{ status: "ok", uptimeSeconds, version }`, or 503 `database_unavailable` (without the cause) when a `SELECT 1` does not answer within 3 s.
- **Graceful shutdown.** On SIGTERM or SIGINT the listeners stop accepting, in-flight requests finish (up to `SHUTDOWN_GRACE_SECONDS`, then connections are cut), the maintenance timer stops and the database closes. A second signal exits at once. Slow clients are bounded: headers within 15 s, a whole request within 30 s.
- **Migrations** are never run by the server process against Postgres: run `npm run db:migrate` at deploy time (the README has the exact commands). The in-process PGlite development database is migrated at boot.

<!-- reference:start (generated by npm run docs:reference; do not edit by hand) -->

## Meta

### GET /health

Liveness and database check.

Used by the process supervisor and load balancer. 503 when the database does not answer.

- **Auth:** none (public)
- **Listener:** public and internal
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { status: "ok", uptimeSeconds, version }
- **Errors:** `database_unavailable`

**Example**

```http
GET /health
```

```http
HTTP 200

{
  "status": "ok",
  "uptimeSeconds": 86400,
  "version": "0.1.0"
}
```

### GET /v1/openapi.json

Machine-readable reference of the routes this listener serves.

OpenAPI 3.1, generated from the same schemas the handlers validate with.

- **Auth:** none (public)
- **Listener:** public and internal
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, OpenAPI document
- **Errors:** only the generic ones

**Example**

```http
GET /v1/openapi.json
```

```http
HTTP 200

{
  "openapi": "3.1.0",
  "info": {
    "title": "GAGE-MP platform API",
    "version": "0.1.0"
  },
  "paths": {
    "/health": {
      "get": {
        "summary": "Liveness and database check"
      }
    }
  }
}
```

Abbreviated: the real document lists every mounted route with its request schema.

## Registration, login and sessions

### POST /v1/auth/register

Create an account.

Sends a verification email. Does not log in. The answer is the same whether or not the email already has an account.

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** `register` per client IP (IPv6: /64): 10 per 1 h
- **Success:** HTTP 201, { verificationRequired: boolean }
- **Errors:** `weak_password`, `username_taken`, `busy`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `email` | string | yes | 0..254 chars |
| `username` | string | yes | matches `^[A-Za-z0-9_]{3,20}$` |
| `password` | string | yes | 1..1024 chars |

**Example**

```http
POST /v1/auth/register
Content-Type: application/json

{
  "email": "speedy@example.com",
  "username": "Speedy_Gonzo",
  "password": "a-long-unusual-passphrase"
}
```

```http
HTTP 201

{
  "verificationRequired": true
}
```

### POST /v1/auth/login

Log in with email or username and password.

Failed attempts are counted per client address and per typed identifier (never per account, so a 429 reveals nothing about which accounts exist); a locked key is refused with 429 before the password is checked. See "Login lockout" above.

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** `login` per client IP (IPv6: /64): 120 per 10 min
- **Success:** HTTP 200, { token, expiresAt, sessionId, account }
- **Errors:** `invalid_credentials`, `busy`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `identifier` | string | yes | 1..254 chars |
| `password` | string | yes | 1..1024 chars |
| `deviceName` | string | no | 1..64 chars |

**Example**

```http
POST /v1/auth/login
Content-Type: application/json

{
  "identifier": "Speedy_Gonzo",
  "password": "a-long-unusual-passphrase",
  "deviceName": "Gaming PC"
}
```

```http
HTTP 200

{
  "token": "gms_Qm9uZUpvdXJub3NoZ2FtZXBsYXRmb3JtMDEyMzQ1Ng",
  "expiresAt": "2026-11-28T12:00:00.000Z",
  "sessionId": "3f2a8c1e-91b4-4d7a-b6c5-2e8f1a0d9c33",
  "account": {
    "id": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
    "username": "Speedy_Gonzo",
    "email": "speedy@example.com",
    "emailVerified": true,
    "roles": [
      "player"
    ],
    "hasPassword": true,
    "discord": null,
    "ban": null,
    "createdAt": "2026-10-01T09:30:00.000Z"
  }
}
```

### POST /v1/auth/logout

Revoke the current session.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 204, (no body)
- **Errors:** only the generic ones

**Example**

```http
POST /v1/auth/logout
Authorization: Bearer <session token>
```

```http
HTTP 204
```

### POST /v1/auth/verify-email

Confirm an email address with the emailed token.

The token is single use and expires (EMAIL_VERIFY_TTL_HOURS). POST, not GET, so mail scanners that open links cannot consume it.

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** `verify-email` per client IP (IPv6: /64): 20 per 10 min
- **Success:** HTTP 200, { account }
- **Errors:** `invalid_token`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `token` | string | yes | 16..200 chars |

**Example**

```http
POST /v1/auth/verify-email
Content-Type: application/json

{
  "token": "dmVyaWZ5X3Rva2VuX3NhbXBsZV9ub3RfcmVhbF8wMTIzNDU2"
}
```

```http
HTTP 200

{
  "account": {
    "id": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
    "username": "Speedy_Gonzo",
    "email": "speedy@example.com",
    "emailVerified": true,
    "roles": [
      "player"
    ],
    "hasPassword": true,
    "discord": null,
    "ban": null,
    "createdAt": "2026-10-01T09:30:00.000Z"
  }
}
```

### POST /v1/auth/resend-verification

Send a new verification email.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 202, { sent: true }
- **Errors:** `already_verified`, `no_email`

**Example**

```http
POST /v1/auth/resend-verification
Authorization: Bearer <session token>
```

```http
HTTP 202

{
  "sent": true
}
```

### POST /v1/auth/forgot-password

Email a password reset token.

Always 202, whether or not the address has an account. Limited per client IP (checked first) and per email address (3 per hour): a client that is already limited cannot spend the address's budget.

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** `forgot-password` per client IP (IPv6: /64): 10 per 1 h
- **Success:** HTTP 202, { sent: true }
- **Errors:** only the generic ones

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `email` | string | yes | 0..254 chars |

**Example**

```http
POST /v1/auth/forgot-password
Content-Type: application/json

{
  "email": "speedy@example.com"
}
```

```http
HTTP 202

{
  "sent": true
}
```

### POST /v1/auth/reset-password

Set a new password with a reset token.

Single-use token. Revokes every session of the account, verifies the email and clears the login lockout. If the email was not verified before, any Discord account attached to the account is removed (and the owner is emailed).

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** `reset-password` per client IP (IPv6: /64): 20 per 10 min
- **Success:** HTTP 200, { ok: true }
- **Errors:** `invalid_token`, `weak_password`, `busy`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `token` | string | yes | 16..200 chars |
| `password` | string | yes | 1..1024 chars |

**Example**

```http
POST /v1/auth/reset-password
Content-Type: application/json

{
  "token": "cmVzZXRfdG9rZW5fc2FtcGxlX25vdF9yZWFsXzAxMjM0NTY",
  "password": "another-long-unusual-passphrase"
}
```

```http
HTTP 200

{
  "ok": true
}
```

### GET /v1/me

The signed-in account.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { account }
- **Errors:** only the generic ones

**Example**

```http
GET /v1/me
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "account": {
    "id": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
    "username": "Speedy_Gonzo",
    "email": "speedy@example.com",
    "emailVerified": true,
    "roles": [
      "player"
    ],
    "hasPassword": true,
    "discord": null,
    "ban": null,
    "createdAt": "2026-10-01T09:30:00.000Z"
  }
}
```

### POST /v1/me/password

Change or set the password.

Revokes every other session and voids outstanding password-reset links. A Discord-only account may omit currentPassword to set its first password (it needs a verified email). Limited to 10 calls per hour per account.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** `change-password` per account: 10 per 1 h
- **Success:** HTTP 200, { revokedSessions }
- **Errors:** `invalid_credentials`, `weak_password`, `current_password_required`, `email_required`, `email_not_verified`, `busy`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `currentPassword` | string | no | 1..1024 chars |
| `newPassword` | string | yes | 1..1024 chars |

**Example**

```http
POST /v1/me/password
Authorization: Bearer <session token>
Content-Type: application/json

{
  "currentPassword": "a-long-unusual-passphrase",
  "newPassword": "another-long-unusual-passphrase"
}
```

```http
HTTP 200

{
  "revokedSessions": 2
}
```

### GET /v1/me/sessions

List signed-in devices.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { sessions: [{ id, deviceName, userAgent, ip, createdAt, lastSeenAt, expiresAt, current }] }
- **Errors:** only the generic ones

**Example**

```http
GET /v1/me/sessions
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "sessions": [
    {
      "id": "3f2a8c1e-91b4-4d7a-b6c5-2e8f1a0d9c33",
      "deviceName": "Gaming PC",
      "userAgent": "GAGE-MP-Launcher/0.2",
      "ip": "203.0.113.50",
      "createdAt": "2026-10-29T12:00:00.000Z",
      "lastSeenAt": "2026-10-29T12:00:00.000Z",
      "expiresAt": "2026-11-28T12:00:00.000Z",
      "current": true
    }
  ]
}
```

### DELETE /v1/me/sessions/:id

Sign out one device.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 204, (no body)
- **Errors:** `session_not_found`

**Path parameters**

- `id`: UUID

**Example**

```http
DELETE /v1/me/sessions/3f2a8c1e-91b4-4d7a-b6c5-2e8f1a0d9c33
Authorization: Bearer <session token>
```

```http
HTTP 204
```

### DELETE /v1/me/sessions

Sign out every other device.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { revoked }
- **Errors:** only the generic ones

**Example**

```http
DELETE /v1/me/sessions
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "revoked": 2
}
```

## Discord

### POST /v1/auth/discord/start

Start a Discord login.

Returns the URL to open in a browser and the secret to poll with. The poll secret stays in the launcher; the result is only readable with the claim the callback page hands to the launcher (see docs/launcher-discord-flow.md).

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** `discord-start` per client IP (IPv6: /64): 20 per 10 min
- **Success:** HTTP 200, { flowId, authorizeUrl, pollSecret, expiresAt }
- **Errors:** `discord_disabled`

**Request body**

A JSON object (see the description above).

**Example**

```http
POST /v1/auth/discord/start
```

```http
HTTP 200

{
  "flowId": "a8d3f5b2-7e61-4c09-9b24-1d6e8f0a3c57",
  "authorizeUrl": "https://discord.com/oauth2/authorize?response_type=code&client_id=123456789012345678&scope=identify+email&redirect_uri=https%3A%2F%2Fapi.gage-mp.example%2Fv1%2Fauth%2Fdiscord%2Fcallback&state=...&code_challenge=...&code_challenge_method=S256&prompt=consent",
  "pollSecret": "cG9sbF9zZWNyZXRfc2FtcGxlX25vdF9yZWFsXzAxMjM0NTY",
  "expiresAt": "2026-10-29T12:10:00.000Z"
}
```

### GET /v1/auth/discord/callback

Discord redirect target (browser page).

Discord sends the player's browser here. Validates the single-use state, redeems the code with the PKCE verifier, records the outcome and puts a random claim on the flow (only its hash is stored). A launcher flow gets a small HTML page (200 when the sign-in was approved, 400 otherwise) that redirects to gage-mp://auth/discord?flow=<flowId>&claim=<claim> and shows a "Return to GAGE" button for the same URL. A web link flow (started with handoff "web") gets 303 to <SITE_ORIGIN>/account/#discord_flow=<flowId>&discord_claim=<claim>, whatever the outcome (the account page reads why it failed); the target origin is configuration, never taken from the request, and the claim is in the fragment so no server log sees it. The claim is never displayed. A callback whose state is invalid, used or expired is 400 with no handoff.

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** `discord-callback` per client IP (IPv6: /64): 60 per 10 min
- **Success:** HTTP 200, text/html | 303 Location: <SITE_ORIGIN>/account/#discord_flow&discord_claim
- **Errors:** only the generic ones

**Query string**

| field | type | required | rules |
| --- | --- | --- | --- |
| `code` | string | no | 0..512 chars |
| `state` | string | no | 0..200 chars |
| `error` | string | no | 0..100 chars |

**Example**

```http
GET /v1/auth/discord/callback?code=<code from Discord>&state=<state from the authorize URL>
```

```http
HTTP 200

<!doctype html><html lang="en"><head>...<meta http-equiv="refresh" content="0;url=gage-mp://auth/discord?flow=<flowId>&amp;claim=<claim>">...</head><body><main><h1>Approved</h1><p>Return to GAGE to finish signing in. You can close this window afterwards.</p><p><a class="btn" href="gage-mp://auth/discord?flow=<flowId>&amp;claim=<claim>">Return to GAGE</a></p></main></body></html>
```

The page redirects to the launcher's `gage-mp://auth/discord?flow=<flowId>&claim=<claim>` URL and offers the same URL as a "Return to GAGE" button; the claim (32 random bytes, base64url, 43 characters) is only in that URL, never in visible text. HTTP 400 with the heading "Discord sign-in did not finish" when Discord refused (the URL is still handed over, so the launcher can read why); HTTP 400 without any URL when the state is unknown, used or expired. The page never repeats anything from the request, and is sent with `Content-Security-Policy: default-src 'none'`, `Referrer-Policy: no-referrer` and `Cache-Control: no-store`.

### POST /v1/auth/discord/poll

Poll a Discord flow.

status pending | failed {code} | needs_username {suggestedUsername, discordUsername} | linked | complete {token, account}. Send the claim from the gage-mp:// URL: without it (or with a wrong one) the answer is always {status: "pending"}, exactly as for a flow nobody approved. complete and linked are returned once and spend the flow and its claim; a link is attached to the account only by this call. A web link flow (handoff "web") is not readable here (404 flow_not_found): the account page finishes it with POST /v1/me/discord/link/finish.

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** `discord-poll` per client IP (IPv6: /64): 300 per 10 min
- **Success:** HTTP 200, { status, ... }
- **Errors:** `flow_not_found`, `flow_expired`, `flow_used`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `flowId` | string (uuid) | yes | matches `^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$` |
| `pollSecret` | string | yes | 16..200 chars |
| `claim` | string | no | 16..200 chars |

**Example**

```http
POST /v1/auth/discord/poll
Content-Type: application/json

{
  "flowId": "a8d3f5b2-7e61-4c09-9b24-1d6e8f0a3c57",
  "pollSecret": "cG9sbF9zZWNyZXRfc2FtcGxlX25vdF9yZWFsXzAxMjM0NTY",
  "claim": "Y2xhaW1fc2FtcGxlX25vdF9yZWFsX2Zvcl9kb2NzXzA"
}
```

```http
HTTP 200

{
  "status": "needs_username",
  "suggestedUsername": "cool_guy_99",
  "discordUsername": "cool.guy-99"
}
```

`claim` comes from the `gage-mp://` URL; without it, or with a wrong one, the answer is always `{ "status": "pending" }`. Other results: `{ "status": "failed", "code": "email_in_use" }` (also `access_denied`, `discord_error`, `invalid_request`; for a link: `discord_already_linked`, `already_has_discord`, `password_changed`), `{ "status": "linked" }` (link flows) and `{ "status": "complete", "token", "expiresAt", "sessionId", "account" }`. `complete` and `linked` are returned once and spend the claim.

### POST /v1/auth/discord/complete

Choose a username to finish a first Discord login.

Needs the claim as well as the poll secret (a wrong claim is 404 flow_not_found). Spends the flow and its claim.

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** `discord-complete` per client IP (IPv6: /64): 20 per 10 min
- **Success:** HTTP 200, { token, expiresAt, sessionId, account }
- **Errors:** `flow_not_found`, `flow_expired`, `flow_used`, `flow_state`, `username_taken`, `email_in_use`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `flowId` | string (uuid) | yes | matches `^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$` |
| `pollSecret` | string | yes | 16..200 chars |
| `claim` | string | yes | 16..200 chars |
| `username` | string | yes | matches `^[A-Za-z0-9_]{3,20}$` |

**Example**

```http
POST /v1/auth/discord/complete
Content-Type: application/json

{
  "flowId": "a8d3f5b2-7e61-4c09-9b24-1d6e8f0a3c57",
  "pollSecret": "cG9sbF9zZWNyZXRfc2FtcGxlX25vdF9yZWFsXzAxMjM0NTY",
  "claim": "Y2xhaW1fc2FtcGxlX25vdF9yZWFsX2Zvcl9kb2NzXzA",
  "username": "CoolGuy99"
}
```

```http
HTTP 200

{
  "token": "gms_Qm9uZUpvdXJub3NoZ2FtZXBsYXRmb3JtMDEyMzQ1Ng",
  "expiresAt": "2026-11-28T12:00:00.000Z",
  "sessionId": "3f2a8c1e-91b4-4d7a-b6c5-2e8f1a0d9c33",
  "account": {
    "id": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
    "username": "CoolGuy99",
    "email": "speedy@example.com",
    "emailVerified": true,
    "roles": [
      "player"
    ],
    "hasPassword": false,
    "discord": {
      "id": "123456789012345678",
      "username": "cool.guy-99"
    },
    "ban": null,
    "createdAt": "2026-10-01T09:30:00.000Z"
  }
}
```

### POST /v1/me/discord/link

Start linking Discord to the signed-in account.

Re-authenticates with the account's current password (`password`; wrong passwords count towards the same lockout as a password change, keyed on the account and the client address: 401 invalid_credentials, then 429 rate_limited). `handoff` "launcher" (default) finishes like a Discord login (callback, gage-mp:// handoff, claimed poll answering "linked"); "web" makes the callback redirect to the website's account page, which finishes with POST /v1/me/discord/link/finish. The identity is attached only by that last step, and is refused there (failed, code password_changed) when the password changed after the start. The owner is emailed.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** `discord-link` per account: 10 per 10 min
- **Success:** HTTP 200, { flowId, authorizeUrl, pollSecret, expiresAt }
- **Errors:** `discord_disabled`, `already_has_discord`, `password_required`, `invalid_credentials`, `rate_limited`, `busy`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `password` | string | yes | 1..1024 chars |
| `handoff` | "launcher" \| "web" | no | default `"launcher"` |

**Example**

```http
POST /v1/me/discord/link
Authorization: Bearer <session token>
Content-Type: application/json

{
  "password": "correct-horse-battery-staple",
  "handoff": "web"
}
```

```http
HTTP 200

{
  "flowId": "a8d3f5b2-7e61-4c09-9b24-1d6e8f0a3c57",
  "authorizeUrl": "https://discord.com/oauth2/authorize?...",
  "pollSecret": "cG9sbF9zZWNyZXRfc2FtcGxlX25vdF9yZWFsXzAxMjM0NTY",
  "expiresAt": "2026-10-29T12:10:00.000Z"
}
```

With `handoff: "launcher"` (the default) finish exactly like a login flow (callback, `gage-mp://` handoff, then poll with the claim; the poll answers `{ "status": "linked" }`). With `handoff: "web"` the website keeps `pollSecret` in the tab (sessionStorage), navigates to `authorizeUrl`, and the callback redirects to `<SITE_ORIGIN>/account/#discord_flow=<flowId>&discord_claim=<claim>`; the page then calls `POST /v1/me/discord/link/finish`. Only that last step attaches the identity. The owner is emailed when the link is made.

### POST /v1/me/discord/link/finish

Finish a web Discord link on the account page.

For a flow started with handoff "web": `flowId` and `claim` come from the account page's URL fragment (#discord_flow&discord_claim), `pollSecret` from the tab that started the flow. The session must belong to the account that started it. Any mismatch (wrong or missing secret, claim of another flow, another account's session, a launcher flow) is 404 flow_not_found. Answers { status: "linked" } once (the identity is attached by this call; the same pair again is a harmless no-op) or { status: "failed", code } (access_denied, discord_error, invalid_request, discord_already_linked, already_has_discord, password_changed).

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** `discord-link-finish` per account: 30 per 10 min
- **Success:** HTTP 200, { status: "linked" } | { status: "failed", code }
- **Errors:** `flow_not_found`, `flow_expired`, `flow_used`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `flowId` | string (uuid) | yes | matches `^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$` |
| `pollSecret` | string | yes | 16..200 chars |
| `claim` | string | yes | 16..200 chars |

**Example**

```http
POST /v1/me/discord/link/finish
Authorization: Bearer <session token>
Content-Type: application/json

{
  "flowId": "a8d3f5b2-7e61-4c09-9b24-1d6e8f0a3c57",
  "pollSecret": "cG9sbF9zZWNyZXRfc2FtcGxlX25vdF9yZWFsXzAxMjM0NTY",
  "claim": "Y2xhaW1fc2FtcGxlX25vdF9yZWFsX2Zvcl9kb2NzXzA"
}
```

```http
HTTP 200

{
  "status": "linked"
}
```

A refusal reads `{ "status": "failed", "code": "discord_already_linked" }` (also `access_denied`, `discord_error`, `invalid_request`, `already_has_discord`, `password_changed`).

### DELETE /v1/me/discord

Unlink Discord.

Refused (409 last_login_method) when Discord is the account's only way in (no password). Re-authenticates with the current password (`password`; the same lockout as linking and changing the password). The owner is emailed.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** `discord-unlink` per account: 10 per 10 min
- **Success:** HTTP 204, (no body)
- **Errors:** `last_login_method`, `not_linked`, `invalid_credentials`, `rate_limited`, `busy`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `password` | string | yes | 1..1024 chars |

**Example**

```http
DELETE /v1/me/discord
Authorization: Bearer <session token>
Content-Type: application/json

{
  "password": "correct-horse-battery-staple"
}
```

```http
HTTP 204
```

## Settings

### GET /v1/me/settings

Read the account's client settings.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { settings: object, version: number, updatedAt: string|null }
- **Errors:** only the generic ones

**Example**

```http
GET /v1/me/settings
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "settings": {
    "language": "ka",
    "gameFolder": "D:\\Games\\GTA V"
  },
  "version": 3,
  "updatedAt": "2026-10-29T12:00:00.000Z"
}
```

### PUT /v1/me/settings

Replace the account's client settings.

A JSON object (at most SETTINGS_MAX_BYTES, default 16384, 6 levels deep, keys [A-Za-z0-9_.:-]{1,64}). With expectedVersion the write fails with 409 if another device wrote first.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** `settings-write` per account: 120 per 10 min
- **Success:** HTTP 200, { settings, version, updatedAt }
- **Body limit:** 17408 bytes
- **Errors:** `invalid_settings`, `version_conflict`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `settings` | object | yes | The settings object (see "Settings" in docs/api.md for the limits) |
| `expectedVersion` | integer | no | 0..* |

**Example**

```http
PUT /v1/me/settings
Authorization: Bearer <session token>
Content-Type: application/json

{
  "settings": {
    "language": "en",
    "gameFolder": "D:\\Games\\GTA V",
    "startMinimised": true
  },
  "expectedVersion": 3
}
```

```http
HTTP 200

{
  "settings": {
    "language": "en",
    "gameFolder": "D:\\Games\\GTA V",
    "startMinimised": true
  },
  "version": 4,
  "updatedAt": "2026-10-29T12:00:00.000Z"
}
```

## Servers and joining

### GET /v1/servers

The official servers with live player counts.

Live state comes from the GAGE UDP query (apps/master/PROTOCOL.md), cached about 5 s per server. The request never waits on a slow server: a server not probed yet may report state "unknown".

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { servers: [{ id, name, region, host, port, tags, sort, maintenance: { enabled, message }, status: { state, players, maxPlayers, pingMs, protocol, checkedAt } }] }
- **Errors:** only the generic ones

**Example**

```http
GET /v1/servers
```

```http
HTTP 200

{
  "servers": [
    {
      "id": "eu-1",
      "name": "GAGE EU 1",
      "region": "eu",
      "host": "eu1.gage-mp.example",
      "port": 24800,
      "tags": [
        "freeroam"
      ],
      "sort": 0,
      "maintenance": {
        "enabled": false,
        "message": null
      },
      "status": {
        "state": "online",
        "players": 12,
        "maxPlayers": 64,
        "pingMs": 31,
        "protocol": 1,
        "checkedAt": "2026-10-29T12:00:00.000Z"
      }
    }
  ]
}
```

### POST /v1/game/join

Get a single-use join token for a server.

The token is valid JOIN_TOKEN_TTL_SECONDS (default 60), bound to this account and server, and consumed by the game server's POST /internal/game/verify.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { token, expiresAt, server: { id, name, host, port } }
- **Errors:** `email_not_verified`, `account_banned`, `server_not_found`, `server_maintenance`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `serverId` | string | yes | matches `^[a-z0-9][a-z0-9-]{1,31}$` |

**Example**

```http
POST /v1/game/join
Authorization: Bearer <session token>
Content-Type: application/json

{
  "serverId": "eu-1"
}
```

```http
HTTP 200

{
  "token": "gmj_dGhpc19pc19hX3NhbXBsZV9qb2luX3Rva2VuXzEyMzQ1Ng",
  "expiresAt": "2026-10-29T12:01:00.000Z",
  "server": {
    "id": "eu-1",
    "name": "GAGE EU 1",
    "host": "eu1.gage-mp.example",
    "port": 24800
  }
}
```

## Shop, orders, wallet and items

### GET /v1/me/wallet

In-game currency balances.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { balances: [{ currency, balance }] }
- **Errors:** only the generic ones

**Example**

```http
GET /v1/me/wallet
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "balances": [
    {
      "currency": "coins",
      "balance": 1500
    }
  ]
}
```

### GET /v1/me/wallet/ledger

Wallet history, newest first.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { entries: [{ id, currency, delta, reason, memo, createdAt }] } (page with ?before=<last id>)
- **Errors:** only the generic ones

**Query string**

| field | type | required | rules |
| --- | --- | --- | --- |
| `limit` | integer | no | 1..200; default `50` |
| `before` | integer | no | *..* |

**Example**

```http
GET /v1/me/wallet/ledger?limit=2
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "entries": [
    {
      "id": 17,
      "currency": "coins",
      "delta": -500,
      "reason": "spend",
      "memo": "Sultan RS",
      "createdAt": "2026-10-29T12:00:00.000Z"
    },
    {
      "id": 12,
      "currency": "coins",
      "delta": 2000,
      "reason": "purchase",
      "memo": null,
      "createdAt": "2026-10-28T18:00:00.000Z"
    }
  ]
}
```

Next page: `?limit=2&before=12` (the last id received).

### GET /v1/me/items

Items and entitlements the account owns.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { items: [{ id, sku, type, payload, quantity, orderId, delivered, createdAt }] }
- **Errors:** only the generic ones

**Example**

```http
GET /v1/me/items
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "items": [
    {
      "id": "e2c4a7f1-6b38-4d95-8a10-7f3b5c9d2e84",
      "sku": "vip-30",
      "type": "entitlement",
      "payload": {
        "type": "entitlement",
        "key": "vip",
        "durationDays": 30
      },
      "quantity": 1,
      "orderId": "b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
      "delivered": true,
      "createdAt": "2026-10-29T12:00:00.000Z"
    }
  ]
}
```

### GET /v1/shop/products

Products on sale.

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { products: [{ id, sku, name, description, priceMinor, currency, kind, grants, sort, imageUrl, maxQuantity }] }
- **Errors:** only the generic ones

**Example**

```http
GET /v1/shop/products
```

```http
HTTP 200

{
  "products": [
    {
      "id": "9d1f6a52-3c47-4b88-a1e0-5c2d7b9e4f60",
      "sku": "vip-30",
      "name": "VIP, 30 days",
      "description": "Priority queue and a gold name tag.",
      "priceMinor": 999,
      "currency": "USD",
      "kind": "vip",
      "grants": [
        {
          "type": "entitlement",
          "key": "vip",
          "durationDays": 30
        }
      ],
      "active": true,
      "sort": 10,
      "imageUrl": "https://cdn.example.com/shop/vip.png",
      "maxQuantity": 1
    }
  ]
}
```

### POST /v1/shop/checkout

Create an order and a checkout at the payment provider.

Send an Idempotency-Key header (8-64 chars of [A-Za-z0-9_-]) so a retry returns the same order. Open checkoutUrl in the browser; the order becomes paid when the provider's webhook arrives.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** `checkout-ip` per client IP (IPv6: /64): 60 per 1 h
- **Success:** HTTP 201, { order }
- **Errors:** `email_not_verified`, `account_banned`, `product_not_found`, `product_inactive`, `quantity_exceeds_limit`, `mixed_currency`, `idempotency_mismatch`, `payment_unavailable`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `items` | array of { sku: string, quantity: integer } | yes | 1..10 items |

**Example**

```http
POST /v1/shop/checkout
Authorization: Bearer <session token>
Idempotency-Key: checkout-7f3a9c21-0001
Content-Type: application/json

{
  "items": [
    {
      "sku": "coins-1000",
      "quantity": 2
    },
    {
      "sku": "hat-red",
      "quantity": 1
    }
  ]
}
```

```http
HTTP 201

{
  "order": {
    "id": "b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
    "status": "pending",
    "currency": "USD",
    "totalMinor": 1498,
    "provider": "fake",
    "checkoutUrl": "http://127.0.0.1:8787/v1/dev/fake-checkout/b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
    "failureReason": null,
    "createdAt": "2026-10-29T12:00:00.000Z",
    "expiresAt": "2026-10-30T12:00:00.000Z",
    "paidAt": null,
    "fulfilledAt": null,
    "refundedAt": null,
    "items": [
      {
        "lineNo": 0,
        "sku": "coins-1000",
        "name": "1000 coins",
        "kind": "currency_pack",
        "quantity": 2,
        "unitPriceMinor": 499
      },
      {
        "lineNo": 1,
        "sku": "hat-red",
        "name": "Red hat",
        "kind": "cosmetic",
        "quantity": 1,
        "unitPriceMinor": 500
      }
    ]
  }
}
```

### GET /v1/shop/orders

The account's orders, newest first.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { orders: [order] }
- **Errors:** only the generic ones

**Query string**

| field | type | required | rules |
| --- | --- | --- | --- |
| `limit` | integer | no | 1..100; default `50` |

**Example**

```http
GET /v1/shop/orders?limit=20
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "orders": [
    {
      "id": "b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
      "status": "pending",
      "currency": "USD",
      "totalMinor": 1498,
      "provider": "fake",
      "checkoutUrl": "http://127.0.0.1:8787/v1/dev/fake-checkout/b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
      "failureReason": null,
      "createdAt": "2026-10-29T12:00:00.000Z",
      "expiresAt": "2026-10-30T12:00:00.000Z",
      "paidAt": null,
      "fulfilledAt": null,
      "refundedAt": null,
      "items": [
        {
          "lineNo": 0,
          "sku": "coins-1000",
          "name": "1000 coins",
          "kind": "currency_pack",
          "quantity": 2,
          "unitPriceMinor": 499
        },
        {
          "lineNo": 1,
          "sku": "hat-red",
          "name": "Red hat",
          "kind": "cosmetic",
          "quantity": 1,
          "unitPriceMinor": 500
        }
      ]
    }
  ]
}
```

### GET /v1/shop/orders/:id

One of the account's orders (poll it after checkout).

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { order }
- **Errors:** `order_not_found`

**Path parameters**

- `id`: UUID

**Example**

```http
GET /v1/shop/orders/b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "order": {
    "id": "b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
    "status": "paid",
    "currency": "USD",
    "totalMinor": 1498,
    "provider": "fake",
    "checkoutUrl": null,
    "failureReason": null,
    "createdAt": "2026-10-29T12:00:00.000Z",
    "expiresAt": "2026-10-30T12:00:00.000Z",
    "paidAt": "2026-10-29T12:03:10.000Z",
    "fulfilledAt": null,
    "refundedAt": null,
    "items": [
      {
        "lineNo": 0,
        "sku": "coins-1000",
        "name": "1000 coins",
        "kind": "currency_pack",
        "quantity": 2,
        "unitPriceMinor": 499
      },
      {
        "lineNo": 1,
        "sku": "hat-red",
        "name": "Red hat",
        "kind": "cosmetic",
        "quantity": 1,
        "unitPriceMinor": 500
      }
    ]
  }
}
```

## Quests

### GET /v1/me/quests

Quests that pay coins, with this account's status.

Active quests in display order. status: available (a visit quest not started yet), started (waiting: waitSeconds left), claimable, claimed, locked (lockReason discord_required = link Discord first; discord_already_claimed = this Discord account already claimed it on another Solar RP account; not_available = cannot be completed yet). A visit quest is claimable 30 s after its start and only with a linked Discord account; each quest pays once per account and once per Discord account.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { discordLinked, visitMinSeconds, quests: [{ slug, kind, title, description, url, reward: { currency, amount }, sort, status, lockReason, startedAt, claimableAt, waitSeconds, claimedAt }] }
- **Errors:** only the generic ones

**Example**

```http
GET /v1/me/quests
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "discordLinked": true,
  "visitMinSeconds": 30,
  "quests": [
    {
      "slug": "link_discord",
      "kind": "discord_link",
      "title": "Link your Discord",
      "description": "Connect your Discord account to your Solar RP account.",
      "url": null,
      "reward": {
        "currency": "coins",
        "amount": 500
      },
      "sort": 0,
      "status": "claimed",
      "lockReason": null,
      "startedAt": null,
      "claimableAt": null,
      "waitSeconds": null,
      "claimedAt": "2026-11-06T11:58:40.000Z"
    },
    {
      "slug": "youtube_subscribe",
      "kind": "visit",
      "title": "Subscribe on YouTube",
      "description": "Subscribe to the Solar RP YouTube channel.",
      "url": "https://www.youtube.example/@solar-rp",
      "reward": {
        "currency": "coins",
        "amount": 100
      },
      "sort": 20,
      "status": "started",
      "lockReason": null,
      "startedAt": "2026-11-06T11:59:50.000Z",
      "claimableAt": "2026-10-29T12:00:00.000Z",
      "waitSeconds": 12,
      "claimedAt": null
    }
  ]
}
```

Without a linked Discord account a visit quest that was started reads `status: "locked", lockReason: "discord_required"`, and so does the Discord link quest.

### POST /v1/me/quests/:slug/start

Start a visit quest (then open its url).

Records when the quest was started; the claim opens 30 s later. Idempotent: a repeated start keeps the first time. Returns the quest with its url for the client to open.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** `quest-start` per account: 60 per 10 min
- **Success:** HTTP 200, { quest }
- **Errors:** `quest_not_found`, `quest_inactive`, `not_a_visit_quest`

**Path parameters**

- `slug`: UUID

**Example**

```http
POST /v1/me/quests/youtube_subscribe/start
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "quest": {
    "slug": "youtube_subscribe",
    "kind": "visit",
    "title": "Subscribe on YouTube",
    "description": "Subscribe to the Solar RP YouTube channel.",
    "url": "https://www.youtube.example/@solar-rp",
    "reward": {
      "currency": "coins",
      "amount": 100
    },
    "sort": 20,
    "status": "started",
    "lockReason": null,
    "startedAt": "2026-10-29T12:00:00.000Z",
    "claimableAt": "2026-11-06T12:00:30.000Z",
    "waitSeconds": 30,
    "claimedAt": null
  }
}
```

Open `quest.url` after this call. Starting again keeps the first `startedAt`.

### POST /v1/me/quests/:slug/claim

Claim a quest's reward into the wallet.

Pays the reward once (ledger reason quest). The credit and the claim are one transaction, and the database refuses a second credit for the same account or Discord account, so concurrent claims pay once.

- **Auth:** session (`Authorization: Bearer <session token>`)
- **Listener:** public
- **Rate limits:** `quest-claim` per account: 30 per 10 min
- **Success:** HTTP 200, { quest, credited: { currency, amount }, balance }
- **Errors:** `quest_not_found`, `quest_inactive`, `already_claimed`, `quest_unavailable`, `quest_not_started`, `discord_required`, `discord_already_claimed`, `quest_wait`, `account_banned`

**Path parameters**

- `slug`: UUID

**Example**

```http
POST /v1/me/quests/link_discord/claim
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "quest": {
    "slug": "link_discord",
    "kind": "discord_link",
    "title": "Link your Discord",
    "description": "Connect your Discord account to your Solar RP account.",
    "url": null,
    "reward": {
      "currency": "coins",
      "amount": 500
    },
    "sort": 0,
    "status": "claimed",
    "lockReason": null,
    "startedAt": null,
    "claimableAt": null,
    "waitSeconds": null,
    "claimedAt": "2026-10-29T12:00:00.000Z"
  },
  "credited": {
    "currency": "coins",
    "amount": 500
  },
  "balance": 500
}
```

A claim too early is 409 `quest_wait` with `Retry-After` and `details: { waitSeconds, claimableAt }`.

## Payment webhooks and the dev checkout

### POST /v1/webhooks/:provider

Payment provider webhook.

Authenticated by the provider's signature over the raw body (provider-specific header). Verified events always get 200 { received, duplicate, result }; (provider, event id) is processed once. A bad signature is 400 invalid_webhook.

- **Auth:** payment provider signature over the raw body
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { received: true, duplicate: boolean, result: applied | ignored | unmatched | rejected | duplicate }
- **Body limit:** 262144 bytes
- **Errors:** `invalid_webhook`, `unknown_provider`

**Path parameters**

- `provider`: payment provider id (for example `fake`)

**Example**

```http
POST /v1/webhooks/fake
X-Fake-Signature: t=1793275390,v1=<hex HMAC-SHA256 of "<t>.<raw body>">
Content-Type: application/json

{
  "id": "evt_5d8e2f",
  "type": "payment.succeeded",
  "orderId": "b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
  "providerRef": "fake_cs_1a2b3c",
  "amountMinor": 1498,
  "currency": "USD"
}
```

```http
HTTP 200

{
  "received": true,
  "duplicate": false,
  "result": "applied"
}
```

The signature header is provider specific; this one is the fake provider's. A redelivery of the same event id answers `{ "received": true, "duplicate": true, "result": "duplicate" }`.

### GET /v1/dev/fake-checkout/:orderId

Dev only: the fake provider's "payment page".

Mounted only with PAYMENT_PROVIDER=fake and ENABLE_DEV_ROUTES=1 (refused in production). Says how to complete the payment.

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { orderId, status, totalMinor, currency, complete: { method, url, body } }
- **Errors:** `order_not_found`

**Path parameters**

- `orderId`: UUID

**Example**

```http
GET /v1/dev/fake-checkout/b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21
```

```http
HTTP 200

{
  "orderId": "b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
  "status": "pending",
  "totalMinor": 1498,
  "currency": "USD",
  "complete": {
    "method": "POST",
    "url": "http://127.0.0.1:8787/v1/dev/fake-checkout/b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21/complete",
    "body": {
      "outcome": "succeeded | failed"
    }
  }
}
```

### POST /v1/dev/fake-checkout/:orderId/complete

Dev only: pay or fail an order through a signed webhook.

Builds a correctly signed fake-provider event and runs it through the same handler as POST /v1/webhooks/fake.

- **Auth:** none (public)
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, webhook outcome
- **Errors:** `order_not_found`

**Path parameters**

- `orderId`: UUID

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `outcome` | "succeeded" \| "failed" | no | default `"succeeded"` |

**Example**

```http
POST /v1/dev/fake-checkout/b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21/complete
Content-Type: application/json

{
  "outcome": "succeeded"
}
```

```http
HTTP 200

{
  "received": true,
  "duplicate": false,
  "result": "applied"
}
```

## Moderation and administration

### GET /v1/admin/accounts

Search accounts by username prefix or exact email.

- **Auth:** session of an account with the `mod` or `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { accounts: [{ id, username, email, emailVerified, roles, ban, createdAt, lastLoginAt }] }
- **Errors:** only the generic ones

**Query string**

| field | type | required | rules |
| --- | --- | --- | --- |
| `q` | string | yes | 1..100 chars |
| `limit` | integer | no | 1..50; default `20` |

**Example**

```http
GET /v1/admin/accounts?q=speedy&limit=10
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "accounts": [
    {
      "id": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
      "username": "Speedy_Gonzo",
      "email": "speedy@example.com",
      "emailVerified": true,
      "roles": [
        "player"
      ],
      "ban": null,
      "createdAt": "2026-10-01T09:30:00.000Z",
      "lastLoginAt": "2026-10-29T12:00:00.000Z"
    }
  ]
}
```

### GET /v1/admin/accounts/:id

One account.

- **Auth:** session of an account with the `mod` or `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { account }
- **Errors:** `account_not_found`

**Path parameters**

- `id`: UUID

**Example**

```http
GET /v1/admin/accounts/7c9e6679-7425-40de-944b-e07fc1f90ae7
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "account": {
    "id": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
    "username": "Speedy_Gonzo",
    "email": "speedy@example.com",
    "emailVerified": true,
    "roles": [
      "player"
    ],
    "ban": {
      "id": "5a1c7e90-2b4d-4f36-8c18-9e0d3a6b1f72",
      "reason": "aimbot in ranked",
      "createdAt": "2026-10-29T12:00:00.000Z",
      "expiresAt": "2026-11-28T12:00:00.000Z"
    },
    "createdAt": "2026-10-01T09:30:00.000Z",
    "lastLoginAt": "2026-10-29T12:00:00.000Z"
  }
}
```

### POST /v1/admin/accounts/:id/ban

Ban an account.

Mods may ban players; admins may also ban mods; admins cannot be banned. Omit durationSeconds for a permanent ban. Takes effect at once: the open game session is closed (end reason banned) and unused join tokens are deleted.

- **Auth:** session of an account with the `mod` or `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 201, { ban }
- **Errors:** `account_not_found`, `forbidden_target`, `already_banned`

**Path parameters**

- `id`: UUID

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `reason` | string | yes | 1..500 chars |
| `durationSeconds` | integer | no | 60..315360000 |

**Example**

```http
POST /v1/admin/accounts/7c9e6679-7425-40de-944b-e07fc1f90ae7/ban
Authorization: Bearer <session token>
Content-Type: application/json

{
  "reason": "aimbot in ranked",
  "durationSeconds": 2592000
}
```

```http
HTTP 201

{
  "ban": {
    "id": "5a1c7e90-2b4d-4f36-8c18-9e0d3a6b1f72",
    "reason": "aimbot in ranked",
    "createdAt": "2026-10-29T12:00:00.000Z",
    "expiresAt": "2026-11-28T12:00:00.000Z"
  }
}
```

### POST /v1/admin/accounts/:id/unban

Lift an account's ban.

You cannot lift your own ban, and only an admin may lift the ban of a mod or an admin (403 forbidden_target); refusals are audited as account.unban_refused. A banned mod or admin cannot call any admin route (403 account_banned).

- **Auth:** session of an account with the `mod` or `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { ok: true }
- **Errors:** `not_banned`, `forbidden_target`

**Path parameters**

- `id`: UUID

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `reason` | string | no | 0..500 chars |

**Example**

```http
POST /v1/admin/accounts/7c9e6679-7425-40de-944b-e07fc1f90ae7/unban
Authorization: Bearer <session token>
Content-Type: application/json

{
  "reason": "appeal accepted"
}
```

```http
HTTP 200

{
  "ok": true
}
```

### PUT /v1/admin/accounts/:id/roles

Set an account's roles.

Replaces the role set (player is always kept). The last admin cannot lose the admin role.

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { roles }
- **Errors:** `account_not_found`, `last_admin`

**Path parameters**

- `id`: UUID

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `roles` | array of "player" \| "mod" \| "admin" | yes | 0..3 items |

**Example**

```http
PUT /v1/admin/accounts/0b6d2e94-5d3c-4c1e-8f0a-6a1c9f2b7d11/roles
Authorization: Bearer <session token>
Content-Type: application/json

{
  "roles": [
    "mod"
  ]
}
```

```http
HTTP 200

{
  "roles": [
    "player",
    "mod"
  ]
}
```

### GET /v1/admin/audit

The audit log, newest first.

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { entries: [{ id, at, actorAccountId, actorLabel, action, targetType, targetId, details, ip, requestId }] }
- **Errors:** only the generic ones

**Query string**

| field | type | required | rules |
| --- | --- | --- | --- |
| `limit` | integer | no | 1..200; default `50` |
| `before` | integer | no | *..* |
| `action` | string | no | 0..64 chars |
| `targetId` | string | no | 0..200 chars |

**Example**

```http
GET /v1/admin/audit?action=account.ban&limit=1
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "entries": [
    {
      "id": 301,
      "at": "2026-10-29T12:00:00.000Z",
      "actorAccountId": "0b6d2e94-5d3c-4c1e-8f0a-6a1c9f2b7d11",
      "actorLabel": "account:0b6d2e94-5d3c-4c1e-8f0a-6a1c9f2b7d11",
      "action": "account.ban",
      "targetType": "account",
      "targetId": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
      "details": {
        "reason": "aimbot in ranked",
        "expiresAt": "2026-11-28T12:00:00.000Z",
        "username": "Speedy_Gonzo"
      },
      "ip": "203.0.113.20",
      "requestId": "4b1f0c6e-9d2a-4e57-8a31-6c0f2e9b7d15"
    }
  ]
}
```

Page back with `before=<id of the last entry>`.

### GET /v1/admin/servers

All servers including hidden ones (no secrets).

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { servers: [...] }
- **Errors:** only the generic ones

**Example**

```http
GET /v1/admin/servers
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "servers": [
    {
      "id": "eu-1",
      "name": "GAGE EU 1",
      "region": "eu",
      "host": "eu1.gage-mp.example",
      "port": 24800,
      "tags": [
        "freeroam"
      ],
      "sort": 0,
      "visible": true,
      "maintenance": false,
      "maintenanceMessage": null,
      "allowedIps": [
        "203.0.113.10"
      ],
      "createdAt": "2026-10-29T12:00:00.000Z",
      "updatedAt": "2026-10-29T12:00:00.000Z"
    }
  ]
}
```

### POST /v1/admin/servers

Create an official server.

The response contains the server's secret exactly once; only its hash is stored. Put it in the game server's config together with the id.

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 201, { server, secret }
- **Errors:** `server_exists`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `id` | string | yes | matches `^[a-z0-9][a-z0-9-]{1,31}$` |
| `name` | string | yes | 1..64 chars |
| `region` | string | yes | 1..32 chars |
| `host` | string | yes | 0..253 chars |
| `port` | integer | yes | 1..65535 |
| `tags` | array of string | no | 0..10 items; default `[]` |
| `sort` | integer | no | -1000000..1000000; default `0` |
| `visible` | boolean | no | default `true` |
| `maintenance` | boolean | no | default `false` |
| `maintenanceMessage` | string \| null | no | default `null` |
| `allowedIps` | array of string | no | 0..16 items |

**Example**

```http
POST /v1/admin/servers
Authorization: Bearer <session token>
Content-Type: application/json

{
  "id": "eu-1",
  "name": "GAGE EU 1",
  "region": "eu",
  "host": "eu1.gage-mp.example",
  "port": 24800,
  "tags": [
    "freeroam"
  ],
  "allowedIps": [
    "203.0.113.10"
  ]
}
```

```http
HTTP 201

{
  "server": {
    "id": "eu-1",
    "name": "GAGE EU 1",
    "region": "eu",
    "host": "eu1.gage-mp.example",
    "port": 24800,
    "tags": [
      "freeroam"
    ],
    "sort": 0,
    "visible": true,
    "maintenance": false,
    "maintenanceMessage": null,
    "allowedIps": [
      "203.0.113.10"
    ],
    "createdAt": "2026-10-29T12:00:00.000Z",
    "updatedAt": "2026-10-29T12:00:00.000Z"
  },
  "secret": "gss_c2FtcGxlX3NlcnZlcl9zZWNyZXRfbm90X3JlYWxfX18"
}
```

The secret is shown once. Only its hash is stored.

### PATCH /v1/admin/servers/:id

Edit a server (name, host, port, tags, sort, visible, maintenance, allowedIps).

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { server }
- **Errors:** `server_not_found`

**Path parameters**

- `id`: server id (`^[a-z0-9][a-z0-9-]{1,31}$`)

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `name` | string | no | 1..64 chars |
| `region` | string | no | 1..32 chars |
| `host` | string | no | 0..253 chars |
| `port` | integer | no | 1..65535 |
| `tags` | array of string | no | 0..10 items |
| `sort` | integer | no | -1000000..1000000 |
| `visible` | boolean | no |  |
| `maintenance` | boolean | no |  |
| `maintenanceMessage` | string \| null | no |  |
| `allowedIps` | array of string | no | 0..16 items |

**Example**

```http
PATCH /v1/admin/servers/eu-1
Authorization: Bearer <session token>
Content-Type: application/json

{
  "maintenance": true,
  "maintenanceMessage": "Back at 14:00 UTC"
}
```

```http
HTTP 200

{
  "server": {
    "id": "eu-1",
    "name": "GAGE EU 1",
    "region": "eu",
    "host": "eu1.gage-mp.example",
    "port": 24800,
    "tags": [
      "freeroam"
    ],
    "sort": 0,
    "visible": true,
    "maintenance": true,
    "maintenanceMessage": "Back at 14:00 UTC",
    "allowedIps": [
      "203.0.113.10"
    ],
    "createdAt": "2026-10-29T12:00:00.000Z",
    "updatedAt": "2026-10-29T12:00:00.000Z"
  }
}
```

### POST /v1/admin/servers/:id/rotate-secret

Issue a new server secret (the old one stops working).

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { secret }
- **Errors:** `server_not_found`

**Path parameters**

- `id`: server id (`^[a-z0-9][a-z0-9-]{1,31}$`)

**Example**

```http
POST /v1/admin/servers/eu-1/rotate-secret
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "secret": "gss_c2FtcGxlX3NlcnZlcl9zZWNyZXRfbm90X3JlYWxfX18"
}
```

### DELETE /v1/admin/servers/:id

Delete a server (and its join tokens and sessions).

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 204, (no body)
- **Errors:** `server_not_found`

**Path parameters**

- `id`: server id (`^[a-z0-9][a-z0-9-]{1,31}$`)

**Example**

```http
DELETE /v1/admin/servers/eu-1
Authorization: Bearer <session token>
```

```http
HTTP 204
```

### GET /v1/admin/products

All products including inactive.

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { products: [...] }
- **Errors:** only the generic ones

**Example**

```http
GET /v1/admin/products
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "products": [
    {
      "id": "9d1f6a52-3c47-4b88-a1e0-5c2d7b9e4f60",
      "sku": "vip-30",
      "name": "VIP, 30 days",
      "description": "Priority queue and a gold name tag.",
      "priceMinor": 999,
      "currency": "USD",
      "kind": "vip",
      "grants": [
        {
          "type": "entitlement",
          "key": "vip",
          "durationDays": 30
        }
      ],
      "active": true,
      "sort": 10,
      "imageUrl": "https://cdn.example.com/shop/vip.png",
      "maxQuantity": 1
    }
  ]
}
```

### POST /v1/admin/products

Create a product.

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 201, { product }
- **Errors:** `sku_exists`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `sku` | string | yes | matches `^[a-z0-9][a-z0-9_.-]{1,62}$` |
| `name` | string | yes | 1..80 chars |
| `description` | string | no | 0..2000 chars; default `""` |
| `priceMinor` | integer | yes | 1..100000000 |
| `currency` | string | yes | matches `^[A-Z]{3}$` |
| `kind` | "currency_pack" \| "vip" \| "cosmetic" \| "bundle" | yes |  |
| `grantPayload` | any | yes |  |
| `active` | boolean | no | default `true` |
| `sort` | integer | no | -1000000..1000000; default `0` |
| `imageUrl` | string (uri) \| null | no | default `null` |
| `maxQuantity` | integer | no | 1..100; default `1` |

**Example**

```http
POST /v1/admin/products
Authorization: Bearer <session token>
Content-Type: application/json

{
  "sku": "vip-30",
  "name": "VIP, 30 days",
  "description": "Priority queue and a gold name tag.",
  "priceMinor": 999,
  "currency": "USD",
  "kind": "vip",
  "grantPayload": {
    "grants": [
      {
        "type": "entitlement",
        "key": "vip",
        "durationDays": 30
      }
    ]
  },
  "sort": 10,
  "imageUrl": "https://cdn.example.com/shop/vip.png"
}
```

```http
HTTP 201

{
  "product": {
    "id": "9d1f6a52-3c47-4b88-a1e0-5c2d7b9e4f60",
    "sku": "vip-30",
    "name": "VIP, 30 days",
    "description": "Priority queue and a gold name tag.",
    "priceMinor": 999,
    "currency": "USD",
    "kind": "vip",
    "grants": [
      {
        "type": "entitlement",
        "key": "vip",
        "durationDays": 30
      }
    ],
    "active": true,
    "sort": 10,
    "imageUrl": "https://cdn.example.com/shop/vip.png",
    "maxQuantity": 1
  }
}
```

### PATCH /v1/admin/products/:id

Edit a product (existing orders keep their snapshot).

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { product }
- **Errors:** `product_not_found`, `sku_exists`

**Path parameters**

- `id`: UUID

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `sku` | string | no | matches `^[a-z0-9][a-z0-9_.-]{1,62}$` |
| `name` | string | no | 1..80 chars |
| `description` | string | no | 0..2000 chars |
| `priceMinor` | integer | no | 1..100000000 |
| `currency` | string | no | matches `^[A-Z]{3}$` |
| `kind` | "currency_pack" \| "vip" \| "cosmetic" \| "bundle" | no |  |
| `grantPayload` | any | no |  |
| `active` | boolean | no |  |
| `sort` | integer | no | -1000000..1000000 |
| `imageUrl` | string (uri) \| null | no |  |
| `maxQuantity` | integer | no | 1..100 |

**Example**

```http
PATCH /v1/admin/products/9d1f6a52-3c47-4b88-a1e0-5c2d7b9e4f60
Authorization: Bearer <session token>
Content-Type: application/json

{
  "priceMinor": 799
}
```

```http
HTTP 200

{
  "product": {
    "id": "9d1f6a52-3c47-4b88-a1e0-5c2d7b9e4f60",
    "sku": "vip-30",
    "name": "VIP, 30 days",
    "description": "Priority queue and a gold name tag.",
    "priceMinor": 799,
    "currency": "USD",
    "kind": "vip",
    "grants": [
      {
        "type": "entitlement",
        "key": "vip",
        "durationDays": 30
      }
    ],
    "active": true,
    "sort": 10,
    "imageUrl": "https://cdn.example.com/shop/vip.png",
    "maxQuantity": 1
  }
}
```

### DELETE /v1/admin/products/:id

Delete a product nobody ordered.

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 204, (no body)
- **Errors:** `product_not_found`, `product_in_use`

**Path parameters**

- `id`: UUID

**Example**

```http
DELETE /v1/admin/products/9d1f6a52-3c47-4b88-a1e0-5c2d7b9e4f60
Authorization: Bearer <session token>
```

```http
HTTP 204
```

### GET /v1/admin/orders

Orders, newest first (filter by status or account).

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { orders: [order + accountId] }
- **Errors:** only the generic ones

**Query string**

| field | type | required | rules |
| --- | --- | --- | --- |
| `status` | "pending" \| "paid" \| "fulfilled" \| "refunded" \| "failed" \| "chargeback" | no |  |
| `accountId` | string (uuid) | no | matches `^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$` |
| `limit` | integer | no | 1..200; default `50` |

**Example**

```http
GET /v1/admin/orders?status=paid&accountId=7c9e6679-7425-40de-944b-e07fc1f90ae7
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "orders": [
    {
      "id": "b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
      "status": "paid",
      "currency": "USD",
      "totalMinor": 1498,
      "provider": "fake",
      "checkoutUrl": null,
      "failureReason": null,
      "createdAt": "2026-10-29T12:00:00.000Z",
      "expiresAt": "2026-10-30T12:00:00.000Z",
      "paidAt": "2026-10-29T12:00:00.000Z",
      "fulfilledAt": null,
      "refundedAt": null,
      "items": [
        {
          "lineNo": 0,
          "sku": "coins-1000",
          "name": "1000 coins",
          "kind": "currency_pack",
          "quantity": 2,
          "unitPriceMinor": 499
        },
        {
          "lineNo": 1,
          "sku": "hat-red",
          "name": "Red hat",
          "kind": "cosmetic",
          "quantity": 1,
          "unitPriceMinor": 500
        }
      ],
      "accountId": "7c9e6679-7425-40de-944b-e07fc1f90ae7"
    }
  ]
}
```

### GET /v1/admin/orders/:id

One order.

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { order }
- **Errors:** `order_not_found`

**Path parameters**

- `id`: UUID

**Example**

```http
GET /v1/admin/orders/b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21
Authorization: Bearer <session token>
```

```http
HTTP 200

{
  "order": {
    "id": "b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
    "status": "fulfilled",
    "currency": "USD",
    "totalMinor": 1498,
    "provider": "fake",
    "checkoutUrl": null,
    "failureReason": null,
    "createdAt": "2026-10-29T12:00:00.000Z",
    "expiresAt": "2026-10-30T12:00:00.000Z",
    "paidAt": "2026-10-29T12:00:00.000Z",
    "fulfilledAt": "2026-10-29T12:00:00.000Z",
    "refundedAt": null,
    "items": [
      {
        "lineNo": 0,
        "sku": "coins-1000",
        "name": "1000 coins",
        "kind": "currency_pack",
        "quantity": 2,
        "unitPriceMinor": 499
      },
      {
        "lineNo": 1,
        "sku": "hat-red",
        "name": "Red hat",
        "kind": "cosmetic",
        "quantity": 1,
        "unitPriceMinor": 500
      }
    ]
  }
}
```

### POST /v1/admin/orders/:id/refund

Refund a paid order and revoke what it granted.

- **Auth:** session of an account with the `admin` role
- **Listener:** public
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { order }
- **Errors:** `order_not_found`, `order_not_refundable`, `refund_failed`

**Path parameters**

- `id`: UUID

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `reason` | string | no | 0..300 chars |

**Example**

```http
POST /v1/admin/orders/b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21/refund
Authorization: Bearer <session token>
Content-Type: application/json

{
  "reason": "customer request"
}
```

```http
HTTP 200

{
  "order": {
    "id": "b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
    "status": "refunded",
    "currency": "USD",
    "totalMinor": 1498,
    "provider": "fake",
    "checkoutUrl": null,
    "failureReason": null,
    "createdAt": "2026-10-29T12:00:00.000Z",
    "expiresAt": "2026-10-30T12:00:00.000Z",
    "paidAt": "2026-10-29T12:00:00.000Z",
    "fulfilledAt": null,
    "refundedAt": "2026-10-29T12:00:00.000Z",
    "items": [
      {
        "lineNo": 0,
        "sku": "coins-1000",
        "name": "1000 coins",
        "kind": "currency_pack",
        "quantity": 2,
        "unitPriceMinor": 499
      },
      {
        "lineNo": 1,
        "sku": "hat-red",
        "name": "Red hat",
        "kind": "cosmetic",
        "quantity": 1,
        "unitPriceMinor": 500
      }
    ]
  }
}
```

## Game server API (internal listener)

### POST /internal/game/verify

Redeem a join token.

Consumes the token (single use) and opens the account's game session on this server. A ban deletes the account's unused tokens, so those come back invalid_token. 200 { ok: true, accountId, username, roles, emailVerified, sessionId, ban: null } or 200 { ok: false, reason: invalid_token | expired | used } or 200 { ok: false, reason: banned, accountId, username, ban: { reason, expiresAt } }.

- **Auth:** game server (`X-Gage-Server-Id` + `X-Gage-Server-Secret`, from an allowlisted IP)
- **Listener:** internal
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, verdict (see description)
- **Errors:** only the generic ones

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `token` | string | yes | 8..200 chars |

**Example**

```http
POST /internal/game/verify
X-Gage-Server-Id: eu-1
X-Gage-Server-Secret: <server secret>
Content-Type: application/json

{
  "token": "gmj_dGhpc19pc19hX3NhbXBsZV9qb2luX3Rva2VuXzEyMzQ1Ng"
}
```

```http
HTTP 200

{
  "ok": true,
  "accountId": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
  "username": "Speedy_Gonzo",
  "roles": [
    "player"
  ],
  "emailVerified": true,
  "sessionId": "3f2a8c1e-91b4-4d7a-b6c5-2e8f1a0d9c33",
  "ban": null
}
```

Refusals are also HTTP 200: `{ "ok": false, "reason": "invalid_token" | "expired" | "used" }`, or `{ "ok": false, "reason": "banned", "accountId", "username", "ban": { "reason", "expiresAt" } }`.

### POST /internal/game/session-end

The player left.

Closes the account's open session on this server. Idempotent (ended: 0 when nothing was open).

- **Auth:** game server (`X-Gage-Server-Id` + `X-Gage-Server-Secret`, from an allowlisted IP)
- **Listener:** internal
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { ended: number }
- **Errors:** only the generic ones

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `sessionId` | string (uuid) | no | matches `^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$` |
| `accountId` | string (uuid) | no | matches `^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$` |
| `reason` | "quit" \| "timeout" \| "kicked" \| "shutdown" \| "crash" | no | default `"quit"` |

**Example**

```http
POST /internal/game/session-end
X-Gage-Server-Id: eu-1
X-Gage-Server-Secret: <server secret>
Content-Type: application/json

{
  "accountId": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
  "reason": "quit"
}
```

```http
HTTP 200

{
  "ended": 1
}
```

### GET /internal/game/grants

Pending item/entitlement grants and revokes of an account.

Apply them in the order returned, then POST .../ack for each. Not consumed by reading. Needs an open session of the account on this server and an account that is not banned. Pulling binds the entries to this server: only its ack counts.

- **Auth:** game server (`X-Gage-Server-Id` + `X-Gage-Server-Secret`, from an allowlisted IP)
- **Listener:** internal
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { grants: [{ id, seq, kind, revokesGrantId, type, sku, payload, quantity, orderId, createdAt }] }
- **Errors:** `no_active_session`, `account_banned`

**Query string**

| field | type | required | rules |
| --- | --- | --- | --- |
| `accountId` | string (uuid) | yes | matches `^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$` |

**Example**

```http
GET /internal/game/grants?accountId=7c9e6679-7425-40de-944b-e07fc1f90ae7
X-Gage-Server-Id: eu-1
X-Gage-Server-Secret: <server secret>
```

```http
HTTP 200

{
  "grants": [
    {
      "id": "e2c4a7f1-6b38-4d95-8a10-7f3b5c9d2e84",
      "seq": 412,
      "kind": "grant",
      "revokesGrantId": null,
      "type": "entitlement",
      "sku": "vip-30",
      "payload": {
        "type": "entitlement",
        "key": "vip",
        "durationDays": 30
      },
      "quantity": 1,
      "orderId": "b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
      "createdAt": "2026-10-29T12:00:00.000Z"
    }
  ]
}
```

### POST /internal/game/grants/pull

Same as GET /internal/game/grants (for clients that only POST).

- **Auth:** game server (`X-Gage-Server-Id` + `X-Gage-Server-Secret`, from an allowlisted IP)
- **Listener:** internal
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { grants: [...] }
- **Errors:** `no_active_session`, `account_banned`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `accountId` | string (uuid) | yes | matches `^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$` |

**Example**

```http
POST /internal/game/grants/pull
X-Gage-Server-Id: eu-1
X-Gage-Server-Secret: <server secret>
Content-Type: application/json

{
  "accountId": "7c9e6679-7425-40de-944b-e07fc1f90ae7"
}
```

```http
HTTP 200

{
  "grants": [
    {
      "id": "e2c4a7f1-6b38-4d95-8a10-7f3b5c9d2e84",
      "seq": 412,
      "kind": "grant",
      "revokesGrantId": null,
      "type": "entitlement",
      "sku": "vip-30",
      "payload": {
        "type": "entitlement",
        "key": "vip",
        "durationDays": 30
      },
      "quantity": 1,
      "orderId": "b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21",
      "createdAt": "2026-10-29T12:00:00.000Z"
    }
  ]
}
```

### POST /internal/game/grants/:id/ack

The grant or revoke was applied.

Only the server the entry was delivered to (the one that pulled it) may ack it; any other server, and any ack of an entry nobody pulled, is 409 grant_not_pulled. Idempotent for that server: a repeated ack returns alreadyAcked: true. After an ack the entry is never delivered again.

- **Auth:** game server (`X-Gage-Server-Id` + `X-Gage-Server-Secret`, from an allowlisted IP)
- **Listener:** internal
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { id, alreadyAcked }
- **Errors:** `grant_not_found`, `grant_not_pulled`

**Path parameters**

- `id`: UUID

**Example**

```http
POST /internal/game/grants/e2c4a7f1-6b38-4d95-8a10-7f3b5c9d2e84/ack
X-Gage-Server-Id: eu-1
X-Gage-Server-Secret: <server secret>
```

```http
HTTP 200

{
  "id": "e2c4a7f1-6b38-4d95-8a10-7f3b5c9d2e84",
  "alreadyAcked": false
}
```

Only the server that pulled the entry may ack it (409 `grant_not_pulled` otherwise).

### GET /internal/game/wallet

Wallet balances of an account.

- **Auth:** game server (`X-Gage-Server-Id` + `X-Gage-Server-Secret`, from an allowlisted IP)
- **Listener:** internal
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { balances: [{ currency, balance }] }
- **Errors:** `no_active_session`

**Query string**

| field | type | required | rules |
| --- | --- | --- | --- |
| `accountId` | string (uuid) | yes | matches `^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$` |

**Example**

```http
GET /internal/game/wallet?accountId=7c9e6679-7425-40de-944b-e07fc1f90ae7
X-Gage-Server-Id: eu-1
X-Gage-Server-Secret: <server secret>
```

```http
HTTP 200

{
  "balances": [
    {
      "currency": "coins",
      "balance": 1500
    }
  ]
}
```

### POST /internal/game/wallet/balance

Same as GET /internal/game/wallet (for clients that only POST).

- **Auth:** game server (`X-Gage-Server-Id` + `X-Gage-Server-Secret`, from an allowlisted IP)
- **Listener:** internal
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { balances: [...] }
- **Errors:** `no_active_session`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `accountId` | string (uuid) | yes | matches `^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$` |

**Example**

```http
POST /internal/game/wallet/balance
X-Gage-Server-Id: eu-1
X-Gage-Server-Secret: <server secret>
Content-Type: application/json

{
  "accountId": "7c9e6679-7425-40de-944b-e07fc1f90ae7"
}
```

```http
HTTP 200

{
  "balances": [
    {
      "currency": "coins",
      "balance": 1500
    }
  ]
}
```

### POST /internal/game/wallet/spend

Spend in-game currency.

Idempotent per (server, idempotencyKey): resend the same request after a timeout and it is applied once. Refused with 409 insufficient_funds when the balance is too low, and with 403 account_banned for a banned account (also for a replayed key).

- **Auth:** game server (`X-Gage-Server-Id` + `X-Gage-Server-Secret`, from an allowlisted IP)
- **Listener:** internal
- **Rate limits:** none beyond the global body limit
- **Success:** HTTP 200, { replayed, balance }
- **Errors:** `no_active_session`, `insufficient_funds`, `idempotency_mismatch`, `account_banned`

**Request body (JSON)**

| field | type | required | rules |
| --- | --- | --- | --- |
| `accountId` | string (uuid) | yes | matches `^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$` |
| `currency` | string | yes | matches `^[a-z][a-z0-9_]{1,15}$` |
| `amount` | integer | yes | 1..1000000000 |
| `idempotencyKey` | string | yes | matches `^[A-Za-z0-9_.:-]{8,64}$` |
| `memo` | string | no | 0..200 chars |

**Example**

```http
POST /internal/game/wallet/spend
X-Gage-Server-Id: eu-1
X-Gage-Server-Secret: <server secret>
Content-Type: application/json

{
  "accountId": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
  "currency": "coins",
  "amount": 500,
  "idempotencyKey": "eu-1:purchase:8841",
  "memo": "Sultan RS"
}
```

```http
HTTP 200

{
  "replayed": false,
  "balance": 1000
}
```

## Error codes

| code | meaning | returned by |
| --- | --- | --- |
| `account_banned` | The account is banned. `details` has `reason` and `expiresAt` (null = permanent). Also returned to a banned mod or admin by every admin route, and to a game server for a banned account on spend and pull. | `POST /v1/me/quests/:slug/claim`<br>`POST /v1/game/join`<br>`POST /v1/shop/checkout`<br>`GET /internal/game/grants`<br>`POST /internal/game/grants/pull`<br>`POST /internal/game/wallet/spend` |
| `account_not_found` | No such account. | `GET /v1/admin/accounts/:id`<br>`POST /v1/admin/accounts/:id/ban`<br>`PUT /v1/admin/accounts/:id/roles` |
| `already_banned` | The account already has a ban in force; unban first to change it. | `POST /v1/admin/accounts/:id/ban` |
| `already_claimed` | This account already claimed the quest (each quest pays once per account). | `POST /v1/me/quests/:slug/claim` |
| `already_has_discord` | This account already has a Discord account linked. Unlink it first. | `POST /v1/me/discord/link` |
| `already_verified` | The email address is already verified. | `POST /v1/auth/resend-verification` |
| `busy` | The server is busy checking passwords (ARGON2_MAX_CONCURRENT running, ARGON2_MAX_QUEUE waiting). Nothing was counted against you; retry after `Retry-After` seconds. | `POST /v1/auth/register`<br>`POST /v1/auth/login`<br>`POST /v1/auth/reset-password`<br>`POST /v1/me/discord/link`<br>`DELETE /v1/me/discord`<br>`POST /v1/me/password` |
| `current_password_required` | The account has a password, so the current one must be sent. | `POST /v1/me/password` |
| `database_unavailable` | The database did not answer (health check). | `GET /health` |
| `discord_already_claimed` | The Discord account linked here already claimed this quest on another Solar RP account (one Discord account, one claim). | `POST /v1/me/quests/:slug/claim` |
| `discord_already_linked` | That Discord account is linked to another GAGE account. | any route (generic) |
| `discord_disabled` | Discord login is not configured on this server. | `POST /v1/auth/discord/start`<br>`POST /v1/me/discord/link` |
| `discord_required` | The account has no linked Discord account. Quests that pay are Discord-gated: link Discord first. | `POST /v1/me/quests/:slug/claim` |
| `email_in_use` | The Discord email belongs to an existing GAGE account. Log in with the password and link Discord from the account settings. | `POST /v1/auth/discord/complete` |
| `email_not_verified` | Verify the email address first: to join or buy (while REQUIRE_VERIFIED_EMAIL=1), and before a Discord-only account sets its first password. (Linking and unlinking Discord re-authenticate with the password instead.) | `POST /v1/me/password`<br>`POST /v1/game/join`<br>`POST /v1/shop/checkout` |
| `email_required` | Add an email address before setting a password. | `POST /v1/me/password` |
| `flow_expired` | The Discord flow is older than DISCORD_FLOW_TTL_SECONDS. | `POST /v1/auth/discord/poll`<br>`POST /v1/auth/discord/complete`<br>`POST /v1/me/discord/link/finish` |
| `flow_not_found` | No such Discord flow, or the poll secret is wrong, or (for `complete` and the web link finish) the claim is wrong, or (web link finish) the session belongs to another account, or the flow was started for the other handoff: all look the same. | `POST /v1/auth/discord/poll`<br>`POST /v1/auth/discord/complete`<br>`POST /v1/me/discord/link/finish` |
| `flow_state` | `complete` was called on a flow that is not waiting for a username. | `POST /v1/auth/discord/complete` |
| `flow_used` | The result of this Discord flow was already delivered. | `POST /v1/auth/discord/poll`<br>`POST /v1/auth/discord/complete`<br>`POST /v1/me/discord/link/finish` |
| `forbidden` | The account lacks the role this route needs. | any route (generic) |
| `forbidden_target` | You may not ban this account (yourself, an admin, or a mod unless you are an admin), or lift this ban (your own, or that of a mod or admin unless you are an admin). Refused unbans are audited. | `POST /v1/admin/accounts/:id/ban`<br>`POST /v1/admin/accounts/:id/unban` |
| `grant_not_found` | No such grant for this server. | `POST /internal/game/grants/:id/ack` |
| `grant_not_pulled` | The grant was not delivered to this server: it was never pulled, or another server holds the delivery (the player moved). Pull again while the account has a session here. | `POST /internal/game/grants/:id/ack` |
| `idempotency_mismatch` | The Idempotency-Key (checkout, 422) or idempotencyKey (spend, 409) was already used for a different request. | `POST /v1/shop/checkout`<br>`POST /internal/game/wallet/spend` |
| `insufficient_funds` | The balance is lower than the amount. `details.balance` has the balance. | `POST /internal/game/wallet/spend` |
| `internal_error` | An unexpected failure on the server (logged with the request id). | any route (generic) |
| `invalid_credentials` | Wrong identifier or password (the same answer for an unknown account). | `POST /v1/auth/login`<br>`POST /v1/me/discord/link`<br>`DELETE /v1/me/discord`<br>`POST /v1/me/password` |
| `invalid_json` | The body is not valid JSON. | any route (generic) |
| `invalid_request` | The body, query string or a path parameter failed validation. `details.issues` lists `{ path, message }` (at most 10). | any route (generic) |
| `invalid_server_credentials` | Unknown server id or wrong secret. | any route (generic) |
| `invalid_settings` | The settings object breaks a rule. `details.problems` lists up to 5 reasons. | `PUT /v1/me/settings` |
| `invalid_token` | The emailed token is unknown, expired, already used or for another purpose. | `POST /v1/auth/verify-email`<br>`POST /v1/auth/reset-password` |
| `invalid_webhook` | The webhook signature or body did not verify. | `POST /v1/webhooks/:provider` |
| `ip_not_allowed` | The call came from an address outside the server's allowlist. | any route (generic) |
| `last_admin` | This is the last admin: promote another account first. | `PUT /v1/admin/accounts/:id/roles` |
| `last_login_method` | The account has no password: Discord is its only way in. Set a password first. | `DELETE /v1/me/discord` |
| `mixed_currency` | The basket mixes currencies. | `POST /v1/shop/checkout` |
| `no_active_session` | The account has no open game session on this server (call verify first). | `GET /internal/game/grants`<br>`POST /internal/game/grants/pull`<br>`GET /internal/game/wallet`<br>`POST /internal/game/wallet/balance`<br>`POST /internal/game/wallet/spend` |
| `no_email` | The account has no email address. | `POST /v1/auth/resend-verification` |
| `not_a_visit_quest` | Only visit quests are started; the other kinds are claimed directly. | `POST /v1/me/quests/:slug/start` |
| `not_banned` | The account has no ban to lift. | `POST /v1/admin/accounts/:id/unban` |
| `not_found` | No such route. | any route (generic) |
| `not_linked` | No Discord account is linked. | `DELETE /v1/me/discord` |
| `order_not_found` | No such order (an order of another account looks the same). | `GET /v1/shop/orders/:id`<br>`GET /v1/dev/fake-checkout/:orderId`<br>`POST /v1/dev/fake-checkout/:orderId/complete`<br>`GET /v1/admin/orders/:id`<br>`POST /v1/admin/orders/:id/refund` |
| `order_not_refundable` | Only paid or fulfilled orders with a provider payment can be refunded. | `POST /v1/admin/orders/:id/refund` |
| `password_required` | The account has no password to re-authenticate with (it signs in through Discord). Set a password first. | `POST /v1/me/discord/link` |
| `payload_too_large` | The body is larger than the route's limit. | any route (generic) |
| `payment_unavailable` | The payment provider could not create the checkout. The order is marked failed; try again. | `POST /v1/shop/checkout` |
| `product_in_use` | Orders reference the product: deactivate it instead of deleting it. | `DELETE /v1/admin/products/:id` |
| `product_inactive` | The product is not on sale. | `POST /v1/shop/checkout` |
| `product_not_found` | No such product. | `POST /v1/shop/checkout`<br>`PATCH /v1/admin/products/:id`<br>`DELETE /v1/admin/products/:id` |
| `quantity_exceeds_limit` | More than the product's `maxQuantity` in one order. | `POST /v1/shop/checkout` |
| `quest_inactive` | The quest exists but is switched off: it cannot be started or claimed. | `POST /v1/me/quests/:slug/start`<br>`POST /v1/me/quests/:slug/claim` |
| `quest_not_found` | No quest has this slug. | `POST /v1/me/quests/:slug/start`<br>`POST /v1/me/quests/:slug/claim` |
| `quest_not_started` | A visit quest must be started (`POST /v1/me/quests/:slug/start`) before it is claimed. | `POST /v1/me/quests/:slug/claim` |
| `quest_unavailable` | The quest cannot be completed yet (the Discord server quest: its membership check is not built). | `POST /v1/me/quests/:slug/claim` |
| `quest_wait` | Too early: a visit quest is claimable `visitMinSeconds` (30) after its start. `Retry-After` and `details.waitSeconds` say how long; `details.claimableAt` says when. | `POST /v1/me/quests/:slug/claim` |
| `rate_limited` | Too many requests or failed attempts. `Retry-After` says how many seconds to wait. | `POST /v1/me/discord/link`<br>`DELETE /v1/me/discord` |
| `refund_failed` | The provider refused or could not be reached; nothing was changed. | `POST /v1/admin/orders/:id/refund` |
| `server_exists` | A server with that id already exists. | `POST /v1/admin/servers` |
| `server_maintenance` | The server is in maintenance; staff may still join. | `POST /v1/game/join` |
| `server_not_found` | No such server (hidden servers do not exist for players). | `POST /v1/game/join`<br>`PATCH /v1/admin/servers/:id`<br>`POST /v1/admin/servers/:id/rotate-secret`<br>`DELETE /v1/admin/servers/:id` |
| `session_not_found` | No such live session on this account. | `DELETE /v1/me/sessions/:id` |
| `sku_exists` | A product with that SKU already exists. | `POST /v1/admin/products`<br>`PATCH /v1/admin/products/:id` |
| `unauthorized` | No valid session token. The response carries `WWW-Authenticate: Bearer`. | any route (generic) |
| `unknown_provider` | No payment provider with that id. | `POST /v1/webhooks/:provider` |
| `unsupported_media_type` | A body was sent without `Content-Type: application/json`. | any route (generic) |
| `username_taken` | The username is taken (case-insensitively) or reserved. | `POST /v1/auth/register`<br>`POST /v1/auth/discord/complete` |
| `version_conflict` | Another device wrote first. `details.currentVersion` is the stored version. | `PUT /v1/me/settings` |
| `weak_password` | The password is too short, too long, too common or contains the username or email name. `details.problem` is one of `too_short`, `too_long`, `common`, `contains_username`, `contains_email`. | `POST /v1/auth/register`<br>`POST /v1/auth/reset-password`<br>`POST /v1/me/password` |

## Tables

Columns, indexes and checks are read from `src/db/schema.ts`; triggers are in `drizzle/0001_invariants.sql`. All timestamps are `timestamptz`.

#### `account_roles`

Roles of an account: `player` (everyone), `mod`, `admin`.

| column | type | null | notes |
| --- | --- | --- | --- |
| `account_id` | uuid | no |  |
| `role` | text | no |  |
| `granted_at` | timestamp with time zone | no | default `now()` |

Constraints and indexes

- primary key (account_id, role)
- foreign key `account_roles_account_id_accounts_id_fk` (account_id) references accounts (id) on delete cascade
- check `account_roles_role`: role in ('player', 'mod', 'admin')

Invariants

- One row per (account, role); only those three values exist.
- Deleting an account removes its roles.

#### `account_settings`

The launcher's per-account settings object.

| column | type | null | notes |
| --- | --- | --- | --- |
| `account_id` | uuid | no | primary key |
| `data` | jsonb | no |  |
| `version` | integer | no | default `1` |
| `updated_at` | timestamp with time zone | no | default `now()` |

Constraints and indexes

- foreign key `account_settings_account_id_accounts_id_fk` (account_id) references accounts (id) on delete cascade
- check `account_settings_object`: jsonb_typeof(data) = 'object'

Invariants

- `data` is a JSON object; `version` counts writes and backs the optimistic `expectedVersion` check.

#### `accounts`

One row per player. `username` is the in-game name.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `email` | text | yes |  |
| `email_verified_at` | timestamp with time zone | yes |  |
| `username` | text | no |  |
| `password_hash` | text | yes |  |
| `password_changed_at` | timestamp with time zone | yes |  |
| `created_at` | timestamp with time zone | no | default `now()` |
| `updated_at` | timestamp with time zone | no | default `now()` |
| `last_login_at` | timestamp with time zone | yes |  |

Constraints and indexes

- unique index `accounts_username_lower_uq` (lower(username))
- unique index `accounts_email_lower_uq` (lower(email)) where email is not null
- check `accounts_username_format`: username ~ '^[A-Za-z0-9_]{3,20}$'
- check `accounts_email_lowercase`: email is null or (email = lower(email) and length(email) <= 254)
- check `accounts_password_needs_email`: password_hash is null or email is not null
- check `accounts_verified_needs_email`: email_verified_at is null or email is not null

Invariants

- Usernames are unique case-insensitively (`accounts_username_lower_uq`) and match `[A-Za-z0-9_]{3,20}`; display case is kept.
- Emails are stored lower-case and are unique case-insensitively when present.
- A password hash or a verified email needs an email address (a Discord-only account may have neither).
- The password column holds an argon2id PHC string, never a password.

#### `audit_log`

Every admin and moderator action.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | bigint | no | primary key; identity |
| `at` | timestamp with time zone | no | default `now()` |
| `actor_account_id` | uuid | yes |  |
| `actor_label` | text | no |  |
| `action` | text | no |  |
| `target_type` | text | no |  |
| `target_id` | text | no |  |
| `details` | jsonb | no | default `{}` |
| `ip` | text | yes |  |
| `request_id` | text | yes |  |

Constraints and indexes

- index `audit_log_at_idx` (at)
- index `audit_log_target_idx` (target_type, target_id)

Invariants

- Append-only: a trigger rejects UPDATE and DELETE.
- Written in the same transaction as the change it records, so there is no admin action without a row.
- `actor_label` says who acted when there is no account (`cli:<os user>`, `system`).

#### `auth_throttle`

Login failure counters with exponential lockout, per client IP group and per TYPED identifier (never per account).

| column | type | null | notes |
| --- | --- | --- | --- |
| `key` | text | no | primary key |
| `failures` | integer | no | default `0` |
| `window_start` | timestamp with time zone | no |  |
| `locked_until` | timestamp with time zone | yes |  |
| `updated_at` | timestamp with time zone | no | default `now()` |

Invariants

- One row per key: `ip:<group>` (any identifier from one address), `idip:<hash>:<block>` (one identifier from one IPv4 /24 or IPv6 /64) and `id:<hash>` (one identifier from anywhere). `<hash>` is the SHA-256 of the lower-cased identifier as typed, so the table holds no email addresses and a key never reveals whether the identifier belongs to an account.
- Persistent, so lockouts survive restarts and work across processes.

#### `auth_tokens`

Single-use emailed tokens: email verification and password reset. Only the SHA-256 is stored.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `account_id` | uuid | no |  |
| `kind` | text | no |  |
| `token_hash` | text | no |  |
| `email` | text | yes |  |
| `created_at` | timestamp with time zone | no | default `now()` |
| `expires_at` | timestamp with time zone | no |  |
| `used_at` | timestamp with time zone | yes |  |

Constraints and indexes

- unique index `auth_tokens_hash_uq` (token_hash)
- index `auth_tokens_account_kind_idx` (account_id, kind)
- foreign key `auth_tokens_account_id_accounts_id_fk` (account_id) references accounts (id) on delete cascade
- check `auth_tokens_kind`: kind in ('email_verify', 'password_reset')

Invariants

- The token hash is unique and `kind` is `email_verify` or `password_reset`.
- Consumption is one conditional `UPDATE ... WHERE used_at IS NULL AND expires_at > now`, so a token works once even under concurrent requests.
- A verification token is void when the account email changed after it was issued.

#### `bans`

Account bans with reason, optional expiry and who issued and lifted them.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `account_id` | uuid | no |  |
| `reason` | text | no |  |
| `created_by` | uuid | yes |  |
| `created_at` | timestamp with time zone | no | default `now()` |
| `expires_at` | timestamp with time zone | yes |  |
| `revoked_at` | timestamp with time zone | yes |  |
| `revoked_by` | uuid | yes |  |
| `revoked_reason` | text | yes |  |

Constraints and indexes

- unique index `bans_one_open_per_account` (account_id) where revoked_at is null
- foreign key `bans_account_id_accounts_id_fk` (account_id) references accounts (id) on delete cascade
- foreign key `bans_created_by_accounts_id_fk` (created_by) references accounts (id) on delete set null
- foreign key `bans_revoked_by_accounts_id_fk` (revoked_by) references accounts (id) on delete set null
- check `bans_reason_len`: length(reason) between 1 and 500

Invariants

- At most one ban per account that has not been revoked (`bans_one_open_per_account`). An expired ban is closed as `expired` when the next one is issued.
- `created_by` / `revoked_by` keep the staff account (set null if that account is deleted); reasons are 1 to 500 characters.

#### `email_outbox`

The development `EmailSender`: every mail the platform would have sent (`EMAIL_DRIVER=outbox`). Live verification and reset tokens are REDACTED in the body, subject and `meta`; in development the full link is logged instead. It delivers nothing.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `seq` | bigint | no | identity |
| `to_address` | text | no |  |
| `subject` | text | no |  |
| `body` | text | no |  |
| `template` | text | no |  |
| `meta` | jsonb | no | default `{}` |
| `created_at` | timestamp with time zone | no | default `now()` |

Constraints and indexes

- index `email_outbox_to_idx` (to_address, created_at)

Invariants

- `seq` is an identity column that gives a strict send order (timestamps tie).
- No row holds a working token: a database read, a backup or a support export cannot take over an account.

#### `game_servers`

The official servers: where they are, how to reach them and the hash of their secret.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | text | no | primary key |
| `name` | text | no |  |
| `region` | text | no |  |
| `host` | text | no |  |
| `port` | integer | no |  |
| `tags` | text[] | no | default `'{}'::text[]` |
| `sort` | integer | no | default `0` |
| `visible` | boolean | no | default `true` |
| `maintenance` | boolean | no | default `false` |
| `maintenance_message` | text | yes |  |
| `secret_hash` | text | no |  |
| `allowed_ips` | text[] | no | default `'{}'::text[]` |
| `created_at` | timestamp with time zone | no | default `now()` |
| `updated_at` | timestamp with time zone | no | default `now()` |

Constraints and indexes

- check `game_servers_id_slug`: id ~ '^[a-z0-9][a-z0-9-]{1,31}$'
- check `game_servers_port`: port between 1 and 65535

Invariants

- `id` is a slug (`[a-z0-9][a-z0-9-]{1,31}`), port 1 to 65535.
- Only `secret_hash` is stored; the secret is shown once at creation or rotation.
- `allowed_ips` holds normalised addresses; an empty list means the server may not call the internal API at all (fail closed).

#### `game_sessions`

Who is currently on which server. Opened by `verify`, closed by `session-end`, a new join, a ban (`banned`) or maintenance.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `account_id` | uuid | no |  |
| `server_id` | text | no |  |
| `join_token_id` | uuid | yes |  |
| `started_at` | timestamp with time zone | no | default `now()` |
| `ended_at` | timestamp with time zone | yes |  |
| `end_reason` | text | yes |  |

Constraints and indexes

- unique index `game_sessions_one_open_per_account` (account_id) where ended_at is null
- index `game_sessions_server_open_idx` (server_id) where ended_at is null
- foreign key `game_sessions_account_id_accounts_id_fk` (account_id) references accounts (id) on delete cascade
- foreign key `game_sessions_server_id_game_servers_id_fk` (server_id) references game_servers (id) on delete cascade
- foreign key `game_sessions_join_token_id_join_tokens_id_fk` (join_token_id) references join_tokens (id) on delete set null

Invariants

- At most one open session per account (`game_sessions_one_open_per_account`, partial unique index).
- Sessions older than GAME_SESSION_MAX_HOURS are closed as `stale`.

#### `grants`

The delivery ledger: what a paid order grants (`kind` = grant) and what a refund or chargeback takes back (`kind` = revoke).

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `seq` | bigint | no | identity |
| `account_id` | uuid | no |  |
| `order_id` | uuid | no |  |
| `line_no` | integer | no |  |
| `entry_no` | integer | no |  |
| `kind` | text | no |  |
| `revokes_grant_id` | uuid | yes |  |
| `type` | text | no |  |
| `sku` | text | no |  |
| `payload` | jsonb | no |  |
| `quantity` | integer | no | default `1` |
| `status` | text | no | default `"pending"` |
| `created_at` | timestamp with time zone | no | default `now()` |
| `acked_at` | timestamp with time zone | yes |  |
| `acked_by` | text | yes |  |
| `delivering_server` | text | yes |  |

Constraints and indexes

- unique `grants_entry_uq` (order_id, line_no, entry_no, kind)
- unique index `grants_seq_uq` (seq)
- unique index `grants_one_revoke_per_grant` (revokes_grant_id) where revokes_grant_id is not null
- index `grants_pending_idx` (account_id, seq) where status = 'pending'
- index `grants_order_idx` (order_id)
- index `grants_delivering_idx` (delivering_server, account_id) where status = 'pending' and delivering_server is not null
- foreign key `grants_account_id_accounts_id_fk` (account_id) references accounts (id) on delete restrict
- foreign key `grants_order_id_orders_id_fk` (order_id) references orders (id) on delete restrict
- foreign key `grants_revokes_fk` (revokes_grant_id) references grants (id) on delete no action
- check `grants_kind`: kind in ('grant', 'revoke')
- check `grants_revoke_link`: (kind = 'revoke') = (revokes_grant_id is not null)
- check `grants_type`: type in ('wallet_credit', 'entitlement', 'item')
- check `grants_status`: status in ('pending', 'acked')
- check `grants_acked_has_time`: (status = 'acked') = (acked_at is not null)
- check `grants_ack_matches_binding`: status = 'pending' or acked_by = 'platform' or coalesce(acked_by = delivering_server, false)
- check `grants_quantity`: quantity between 1 and 100

Invariants

- Unique per `(order_id, line_no, entry_no, kind)`, and a revoke is unique per revoked grant (`grants_one_revoke_per_grant`): a replayed webhook cannot create a second grant or a second revoke.
- `seq` is a global identity: the game server applies pending entries in `seq` order.
- Status is `pending` until acknowledged (`acked_at` is set exactly when status is `acked`). Wallet credits are applied by the platform and are born acknowledged by `platform`.
- A revoke has `revokes_grant_id`; a grant has none.
- `delivering_server` is set when a game server pulls the entry (the server holding the account's open session). Only that server's ack counts: the ack is one conditional UPDATE on it, and `grants_ack_matches_binding` makes the database refuse any other server's ack (the platform's own acks of wallet credits are exempt).

#### `join_tokens`

Single-use 60 second tokens that carry a player from the launcher to a game server. Only the SHA-256 is stored.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `token_hash` | text | no |  |
| `account_id` | uuid | no |  |
| `server_id` | text | no |  |
| `created_at` | timestamp with time zone | no | default `now()` |
| `expires_at` | timestamp with time zone | no |  |
| `used_at` | timestamp with time zone | yes |  |
| `ip` | text | yes |  |

Constraints and indexes

- unique index `join_tokens_hash_uq` (token_hash)
- index `join_tokens_account_idx` (account_id)
- foreign key `join_tokens_account_id_accounts_id_fk` (account_id) references accounts (id) on delete cascade
- foreign key `join_tokens_server_id_game_servers_id_fk` (server_id) references game_servers (id) on delete cascade
- check `join_tokens_expiry_after_create`: expires_at > created_at

Invariants

- Token hash unique; `expires_at` is after `created_at`.
- Bound to one account and one server (deleting either deletes the token). Consumption is a conditional UPDATE inside the verification transaction. An account has at most one unused token: asking again voids the previous one.

#### `oauth_flows`

In-flight Discord OAuth2 logins and links: state hash, PKCE verifier, poll-secret hash, claim hash and the outcome the launcher polls for.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `provider` | text | no |  |
| `intent` | text | no |  |
| `state_hash` | text | no |  |
| `code_verifier` | text | yes |  |
| `poll_secret_hash` | text | no |  |
| `claim_hash` | text | yes |  |
| `account_id` | uuid | yes |  |
| `handoff` | text | no | default `"launcher"` |
| `status` | text | no | default `"pending"` |
| `result_account_id` | uuid | yes |  |
| `profile` | jsonb | yes |  |
| `error_code` | text | yes |  |
| `created_at` | timestamp with time zone | no | default `now()` |
| `expires_at` | timestamp with time zone | no |  |
| `callback_at` | timestamp with time zone | yes |  |
| `consumed_at` | timestamp with time zone | yes |  |

Constraints and indexes

- unique index `oauth_flows_state_uq` (state_hash)
- foreign key `oauth_flows_account_id_accounts_id_fk` (account_id) references accounts (id) on delete cascade
- foreign key `oauth_flows_result_account_id_accounts_id_fk` (result_account_id) references accounts (id) on delete cascade
- check `oauth_flows_intent`: intent in ('login', 'link')
- check `oauth_flows_status`: status in ('pending', 'login_ready', 'needs_username', 'link_ready', 'linked', 'failed', 'consumed')
- check `oauth_flows_link_needs_account`: intent <> 'link' or account_id is not null
- check `oauth_flows_handoff`: handoff in ('launcher', 'web')
- check `oauth_flows_web_is_link`: handoff <> 'web' or intent = 'link'

Invariants

- The state hash is unique and consumed by a conditional UPDATE (status pending, not yet called back, not expired), so a replayed or forged callback matches nothing.
- `claim_hash` is the SHA-256 of the random claim the callback page hands to the approving browser (`gage-mp://auth/discord?flow&claim`). Poll and complete show nothing without it; it is cleared when the flow is consumed.
- The PKCE verifier and the stored Discord profile are cleared as soon as they are no longer needed; rows are purged a day after expiry.
- Status is one of `pending`, `login_ready`, `needs_username`, `link_ready`, `linked` (legacy, no longer written), `failed`, `consumed`. A link flow stops at `link_ready`: the identity is attached by the claimed poll (or the web finish), never by the callback.
- `handoff` is `launcher` (the gage-mp:// URL, finished by the unauthenticated poll) or `web` (a redirect to the website's account page, finished there with a session of `account_id`); only a link flow can be `web` (`oauth_flows_web_is_link`), and the poll refuses web flows.

#### `oauth_identities`

Links between a GAGE account and an external identity (Discord).

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `account_id` | uuid | no |  |
| `provider` | text | no |  |
| `provider_user_id` | text | no |  |
| `provider_username` | text | yes |  |
| `provider_email` | text | yes |  |
| `email_verified` | boolean | no | default `false` |
| `created_at` | timestamp with time zone | no | default `now()` |

Constraints and indexes

- unique `oauth_identities_provider_user_uq` (provider, provider_user_id)
- unique `oauth_identities_account_provider_uq` (account_id, provider)
- foreign key `oauth_identities_account_id_accounts_id_fk` (account_id) references accounts (id) on delete cascade
- check `oauth_identities_provider`: provider in ('discord')

Invariants

- A Discord user id belongs to at most one account, and an account has at most one Discord identity.
- Identities are matched on the immutable provider user id, never on email or name.

#### `order_items`

The lines of an order, copied from the products at checkout (name, price, grant payload).

| column | type | null | notes |
| --- | --- | --- | --- |
| `order_id` | uuid | no |  |
| `line_no` | integer | no |  |
| `product_id` | uuid | no |  |
| `sku` | text | no |  |
| `name` | text | no |  |
| `kind` | text | no |  |
| `unit_price_minor` | bigint | no |  |
| `currency` | text | no |  |
| `quantity` | integer | no |  |
| `grant_payload` | jsonb | no |  |

Constraints and indexes

- primary key (order_id, line_no)
- foreign key `order_items_product_id_products_id_fk` (product_id) references products (id) on delete restrict
- foreign key `order_items_order_currency_fk` (order_id, currency) references orders (id, currency) on delete cascade
- check `order_items_quantity`: quantity between 1 and 100
- check `order_items_price`: unit_price_minor > 0

Invariants

- Immutable: a trigger rejects UPDATE, so changing or deactivating a product never changes what an existing order delivers.
- A composite foreign key `(order_id, currency)` makes it impossible for a line to carry another currency than its order. Products with orders cannot be deleted (`ON DELETE RESTRICT`).

#### `orders`

A purchase attempt and its state: `pending`, `paid`, `fulfilled`, `refunded`, `failed`, `chargeback`.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `account_id` | uuid | no |  |
| `status` | text | no | default `"pending"` |
| `currency` | text | no |  |
| `total_minor` | bigint | no |  |
| `provider` | text | no |  |
| `provider_ref` | text | yes |  |
| `checkout_url` | text | yes |  |
| `idempotency_key` | text | yes |  |
| `failure_reason` | text | yes |  |
| `created_at` | timestamp with time zone | no | default `now()` |
| `updated_at` | timestamp with time zone | no | default `now()` |
| `expires_at` | timestamp with time zone | no |  |
| `paid_at` | timestamp with time zone | yes |  |
| `fulfilled_at` | timestamp with time zone | yes |  |
| `refunded_at` | timestamp with time zone | yes |  |

Constraints and indexes

- unique `orders_id_currency_uq` (id, currency)
- unique index `orders_idempotency_uq` (account_id, idempotency_key) where idempotency_key is not null
- unique index `orders_provider_ref_uq` (provider, provider_ref) where provider_ref is not null
- index `orders_account_idx` (account_id, created_at)
- foreign key `orders_account_id_accounts_id_fk` (account_id) references accounts (id) on delete restrict
- check `orders_status`: status in ('pending', 'paid', 'fulfilled', 'refunded', 'failed', 'chargeback')
- check `orders_total_positive`: total_minor > 0
- check `orders_currency_iso`: currency ~ '^[A-Z]{3}$'
- check `orders_paid_has_paid_at`: status in ('pending', 'failed') or paid_at is not null
- check `orders_fulfilled_has_time`: status <> 'fulfilled' or fulfilled_at is not null

Invariants

- Status changes follow a transition table enforced by a trigger: pending to paid or failed; paid to fulfilled, refunded or chargeback; fulfilled to refunded or chargeback; and failed back to paid only for the reasons `expired` and `payment_failed` (a payment that still arrives is honoured). Everything else is rejected, including any move out of `refunded` and `chargeback`.
- A paid-or-later order has `paid_at`; a fulfilled order has `fulfilled_at`; the total is positive.
- `(account_id, idempotency_key)` is unique, so a retried checkout returns the same order; `(provider, provider_ref)` is unique.

#### `payment_events`

Every verified provider webhook, once.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `provider` | text | no |  |
| `event_id` | text | no |  |
| `type` | text | no |  |
| `order_id` | uuid | yes |  |
| `result` | text | no |  |
| `detail` | text | yes |  |
| `payload` | jsonb | no |  |
| `received_at` | timestamp with time zone | no | default `now()` |

Constraints and indexes

- unique `payment_events_provider_event_uq` (provider, event_id)
- index `payment_events_order_idx` (order_id)
- check `payment_events_result`: result in ('applied', 'ignored', 'unmatched', 'rejected')

Invariants

- `(provider, event_id)` is unique and is inserted first, in the same transaction as the effects: a redelivered event finds its row and does nothing.
- `result` records what happened: `applied`, `ignored` (the order state did not allow it), `unmatched` (no such order) or `rejected` (for example an amount mismatch).

#### `products`

The shop catalogue.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `sku` | text | no |  |
| `name` | text | no |  |
| `description` | text | no | default `""` |
| `price_minor` | bigint | no |  |
| `currency` | text | no |  |
| `kind` | text | no |  |
| `grant_payload` | jsonb | no |  |
| `active` | boolean | no | default `true` |
| `sort` | integer | no | default `0` |
| `image_url` | text | yes |  |
| `max_quantity` | integer | no | default `1` |
| `created_at` | timestamp with time zone | no | default `now()` |
| `updated_at` | timestamp with time zone | no | default `now()` |

Constraints and indexes

- unique index `products_sku_uq` (sku)
- check `products_sku_format`: sku ~ '^[a-z0-9][a-z0-9_.-]{1,62}$'
- check `products_price_positive`: price_minor > 0 and price_minor <= 100000000
- check `products_currency_iso`: currency ~ '^[A-Z]{3}$'
- check `products_kind`: kind in ('currency_pack', 'vip', 'cosmetic', 'bundle')
- check `products_max_quantity`: max_quantity between 1 and 100

Invariants

- SKU unique and URL-safe; price positive and at most 100 000 000 minor units; currency is an ISO 4217 code; `kind` is `currency_pack`, `vip`, `cosmetic` or `bundle`.
- `grant_payload` is validated against the kind before it is stored (a currency pack may only credit currency, a VIP only grants entitlements, and so on).

#### `quest_progress`

Per account and quest: when a visit quest was started and the claim (time, the Discord account it was made under, the ledger entry that paid it).

| column | type | null | notes |
| --- | --- | --- | --- |
| `account_id` | uuid | no |  |
| `quest_id` | uuid | no |  |
| `started_at` | timestamp with time zone | yes |  |
| `claimed_at` | timestamp with time zone | yes |  |
| `discord_user_id` | text | yes |  |
| `ledger_id` | bigint | yes |  |
| `created_at` | timestamp with time zone | no | default `now()` |

Constraints and indexes

- primary key (account_id, quest_id)
- unique index `quest_progress_discord_uq` (discord_user_id, quest_id) where discord_user_id is not null
- unique index `quest_progress_ledger_uq` (ledger_id) where ledger_id is not null
- foreign key `quest_progress_account_id_accounts_id_fk` (account_id) references accounts (id) on delete cascade
- foreign key `quest_progress_quest_id_quests_id_fk` (quest_id) references quests (id) on delete restrict
- foreign key `quest_progress_ledger_id_wallet_ledger_id_fk` (ledger_id) references wallet_ledger (id) on delete restrict
- check `quest_progress_claim_complete`: (claimed_at is null) = (ledger_id is null) and (claimed_at is null) = (discord_user_id is null)

Invariants

- One row per (account, quest): the primary key.
- One claim per (Discord user id, quest) whatever account the Discord account is linked to (`quest_progress_discord_uq`), so unlinking and relinking Discord elsewhere cannot farm a quest again.
- A claim is complete or absent: `claimed_at`, `discord_user_id` and `ledger_id` are set together (`quest_progress_claim_complete`). The ledger entry and the claim are written in one transaction.
- A quest with progress cannot be deleted (deactivate it instead).

#### `quests`

The quest catalogue: tasks that pay coins (`discord_link`, `discord_guild`, `visit`). Changed with `npm run admin:quests`.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `slug` | text | no |  |
| `kind` | text | no |  |
| `title` | text | no |  |
| `description` | text | no | default `""` |
| `url` | text | yes |  |
| `reward_amount` | bigint | no |  |
| `currency` | text | no | default `"coins"` |
| `active` | boolean | no | default `false` |
| `sort` | integer | no | default `0` |
| `created_at` | timestamp with time zone | no | default `now()` |
| `updated_at` | timestamp with time zone | no | default `now()` |

Constraints and indexes

- unique index `quests_slug_uq` (slug)
- check `quests_slug_format`: slug ~ '^[a-z0-9][a-z0-9_]{1,47}$'
- check `quests_kind`: kind in ('discord_link', 'discord_guild', 'visit')
- check `quests_title_len`: length(title) between 1 and 80
- check `quests_description_len`: length(description) <= 500
- check `quests_url_format`: url is null or (url ~ '^https://[^[:space:]]+$' and length(url) <= 2048)
- check `quests_url_only_visit`: kind = 'visit' or url is null
- check `quests_active_visit_has_url`: not active or kind <> 'visit' or url is not null
- check `quests_reward_range`: reward_amount between 1 and 1000000
- check `quests_currency`: currency ~ '^[a-z][a-z0-9_]{1,15}$'

Invariants

- `slug` is unique and matches `[a-z0-9][a-z0-9_]{1,47}`; `kind` is one of the three.
- Only a `visit` quest has a `url`, it is `https://` (at most 2048 characters), and an active visit quest must have one (`quests_active_visit_has_url`).
- The reward is 1 to 1 000 000 of `currency` (`coins`).

#### `rate_limits`

Fixed-window counters behind every per-route rate limit that is not a login failure.

| column | type | null | notes |
| --- | --- | --- | --- |
| `key` | text | no | primary key |
| `window_start` | timestamp with time zone | no |  |
| `count` | integer | no |  |

Invariants

- One row per key; the counter is bumped by a single atomic upsert, so concurrent requests are counted exactly.

#### `sessions`

Login sessions (one per device). The bearer token is opaque; only its SHA-256 is stored.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | uuid | no | primary key; default `gen_random_uuid()` |
| `account_id` | uuid | no |  |
| `token_hash` | text | no |  |
| `created_at` | timestamp with time zone | no | default `now()` |
| `last_seen_at` | timestamp with time zone | no | default `now()` |
| `expires_at` | timestamp with time zone | no |  |
| `absolute_expires_at` | timestamp with time zone | no |  |
| `user_agent` | text | yes |  |
| `device_name` | text | yes |  |
| `ip` | text | yes |  |
| `revoked_at` | timestamp with time zone | yes |  |
| `revoked_reason` | text | yes |  |

Constraints and indexes

- unique index `sessions_token_hash_uq` (token_hash)
- index `sessions_account_idx` (account_id)
- foreign key `sessions_account_id_accounts_id_fk` (account_id) references accounts (id) on delete cascade
- check `sessions_expiry_order`: expires_at <= absolute_expires_at

Invariants

- The token hash is unique.
- `expires_at` slides forward on use (30 days idle by default) but never past `absolute_expires_at` (SESSION_MAX_DAYS), enforced by `sessions_expiry_order`.
- Changing the password revokes every other session (`revoked_at`, `revoked_reason`).

#### `wallet_balances`

The derived balance per account and currency.

| column | type | null | notes |
| --- | --- | --- | --- |
| `account_id` | uuid | no |  |
| `currency` | text | no |  |
| `balance` | bigint | no |  |
| `updated_at` | timestamp with time zone | no | default `now()` |

Constraints and indexes

- primary key (account_id, currency)
- foreign key `wallet_balances_account_id_accounts_id_fk` (account_id) references accounts (id) on delete restrict

Invariants

- Written only by the ledger trigger: a guard trigger rejects every other INSERT, UPDATE and DELETE, so a balance cannot exist without ledger entries that add up to it.

#### `wallet_ledger`

The in-game currency ledger. The balance of an account is the sum of its deltas.

| column | type | null | notes |
| --- | --- | --- | --- |
| `id` | bigint | no | primary key; identity |
| `account_id` | uuid | no |  |
| `currency` | text | no |  |
| `delta` | bigint | no |  |
| `reason` | text | no |  |
| `idempotency_key` | text | no |  |
| `grant_id` | uuid | yes |  |
| `server_id` | text | yes |  |
| `memo` | text | yes |  |
| `created_at` | timestamp with time zone | no | default `now()` |

Constraints and indexes

- unique index `wallet_ledger_idem_uq` (idempotency_key)
- index `wallet_ledger_account_idx` (account_id, currency, id)
- foreign key `wallet_ledger_account_id_accounts_id_fk` (account_id) references accounts (id) on delete restrict
- foreign key `wallet_ledger_grant_id_grants_id_fk` (grant_id) references grants (id) on delete restrict
- check `wallet_ledger_delta_nonzero`: delta <> 0
- check `wallet_ledger_currency`: currency ~ '^[a-z][a-z0-9_]{1,15}$'
- check `wallet_ledger_reason`: reason in ('purchase', 'revoke', 'spend', 'quest')
- check `wallet_ledger_sign`: (reason in ('purchase', 'quest') and delta > 0) or (reason in ('revoke', 'spend') and delta < 0)

Invariants

- Append-only: a trigger rejects UPDATE and DELETE.
- `idempotency_key` is unique (`grant:<id>`, `revoke:<grantId>`, `spend:<serverId>:<key>`, `quest:<questId>:<accountId>`): replaying an operation is a no-op, and a quest can credit an account once.
- Purchases and quest rewards are positive, spends and revokes negative, never zero (`wallet_ledger_sign`).
- A trigger updates `wallet_balances` in the same statement and rejects an insert that would take the balance below zero (`insufficient_funds`), except a `revoke` (a refund may leave a debt that later purchases pay off).

## Environment variables

Unset and empty variables take the default. Integers are plain base-10 digits; flags are exactly `0` or `1`. A bad value stops the process at start with every problem listed (secrets are never echoed).

| variable | default | meaning |
| --- | --- | --- |
| `NODE_ENV` | _unset_ | `development`, `test` or `production`. REQUIRED: an unset value is refused at startup (it used to mean development) unless `ENABLE_DEV_ROUTES=1` says so explicitly. Production refuses an in-memory database, an unset `DATABASE_URL`, `ENABLE_DEV_ROUTES=1` and the fake payment provider (unless explicitly allowed). |
| `ENABLE_DEV_ROUTES` | `0` | `1` mounts `/v1/dev/*` (pay or fail any order without paying) and allows the public built-in fake webhook secret. Development machines only: refused when `NODE_ENV=production`, and never implied by an unset `NODE_ENV`. |
| `HOST` | `127.0.0.1` | Address of the public listener. Keep `127.0.0.1` behind a reverse proxy. |
| `PORT` | `8787` | Port of the public listener (0 to 65535; 0 picks a free port). |
| `INTERNAL_HOST` | `127.0.0.1` | Address of the internal (game server) listener. Bind it to loopback or a private interface and firewall it. |
| `INTERNAL_PORT` | `8788` | Port of the internal listener (0 picks a free port). |
| `INTERNAL_ON_PUBLIC_LISTENER` | `0` | `1` serves `/internal/*` on the public listener too (one port). Then rely on the per-server IP allowlist and a firewall. |
| `DATABASE_URL` | _unset_ | `postgres://...` / `postgresql://...`, or `pglite:memory` / `pglite:<directory>` for the in-process development database. Required in production. |
| `DATABASE_POOL_MAX` | `10` | Maximum Postgres connections (1 to 200). |
| `PUBLIC_API_URL` | `http://127.0.0.1:8787` | Public base URL of the API (http or https, no trailing slash). Used for checkout and Discord redirect URLs and as the OpenAPI server. |
| `CORS_ORIGINS` | `https://app.gage` | Comma separated browser origins allowed to read public responses: the launcher's WebView2 origin. Bare origins only (`scheme://host[:port]`); `*` and paths are refused. |
| `TRUST_PROXY` | `0` | `1` trusts one reverse proxy: the client address is the last `X-Forwarded-For` entry. Never set it without a proxy that appends the header. `/internal/*` ignores it. |
| `LOG_LEVEL` | `info` | `debug`, `info`, `warn` or `error`. Logs are one JSON object per line on stdout. |
| `BODY_LIMIT_BYTES` | `65536` | Default request body limit (1024 to 4194304). A few routes set their own (settings, webhooks). |
| `SHUTDOWN_GRACE_SECONDS` | `10` | How long in-flight requests may take after SIGTERM/SIGINT before connections are cut (1 to 120). |
| `SESSION_TTL_DAYS` | `30` | Idle lifetime of a session; it slides forward on use (1 to 365). |
| `SESSION_MAX_DAYS` | `180` | Absolute lifetime of a session, however active (1 to 3650). Must be at least `SESSION_TTL_DAYS`. |
| `REQUIRE_VERIFIED_EMAIL` | `1` | `1` makes joining a server and buying require a verified email. Logging in never does. |
| `EMAIL_VERIFY_TTL_HOURS` | `24` | Lifetime of an email verification token (1 to 720). |
| `PASSWORD_RESET_TTL_MINUTES` | `60` | Lifetime of a password reset token (5 to 1440). |
| `ARGON2_MEMORY_KIB` | `19456` | argon2id memory cost in KiB. The default (19456) is OWASP's recommended minimum for t=2, p=1. Hashes made with weaker parameters are upgraded at the next successful login. |
| `ARGON2_TIME_COST` | `2` | argon2id iterations (1 to 20). |
| `ARGON2_PARALLELISM` | `1` | argon2id lanes (1 to 16). |
| `ARGON2_MAX_CONCURRENT` | `2` | Password hashes or checks running at once in this process (1 to 64). Keep it below the libuv thread pool (4 unless `UV_THREADPOOL_SIZE` is raised): argon2 runs there next to file and DNS work. |
| `ARGON2_MAX_QUEUE` | `64` | Hashes allowed to wait for a slot. One more is refused at once with 503 `busy` and a `Retry-After`, so a burst of logins cannot pile up memory or starve the pool. |
| `LOGIN_ACCOUNT_IP_MAX_FAILURES` | `5` | Consecutive failed logins for one typed identifier (username or email) from one address block (IPv4 /24, IPv6 /64) before that block is locked out of that identifier. Lower than the account-wide threshold. |
| `LOGIN_ACCOUNT_MAX_FAILURES` | `100` | Consecutive failed logins for one typed identifier from ANYWHERE before it is locked for everyone (a distributed guessing attack; default 100, so it takes failures from at least 20 blocks). Must be at least `LOGIN_ACCOUNT_IP_MAX_FAILURES`. |
| `LOGIN_IP_MAX_FAILURES` | `20` | Failed logins of any identifier per client IP group (IPv6: /64) before the address is locked. |
| `LOGIN_LOCKOUT_BASE_SECONDS` | `30` | First lockout length. Each further failure doubles it. |
| `LOGIN_LOCKOUT_MAX_SECONDS` | `3600` | Upper bound of a lockout. Must be at least the base. |
| `LOGIN_FAILURE_WINDOW_SECONDS` | `900` | Failures are forgotten after this much calm, measured from the later of the last failure and the end of the lock. |
| `EMAIL_DRIVER` | `outbox` | `resend` delivers through Resend's HTTP API (needs `RESEND_API_KEY` and an `EMAIL_FROM` on a domain verified in Resend; a redacted copy of every mail still goes to `email_outbox`). `outbox` writes mail to the `email_outbox` table (development); `console` logs it. Neither of those two delivers mail or keeps a live token: bodies are redacted, and only with `NODE_ENV=development` is the full link logged. |
| `EMAIL_FROM` | `Solar RP <no-reply@gage-mp.invalid>` | Sender shown on outgoing mail. With `EMAIL_DRIVER=resend` it must be an address on the verified domain (e.g. `Solar RP <no-reply@solar-rp.com>`); a `.invalid` address is refused at boot. |
| `EMAIL_LINK_BASE_URL` | `gage-mp://auth` | Base of the links in mail. The default `gage-mp://auth` opens the launcher, which POSTs the token (verification and reset tokens are never consumed by a GET). |
| `RESEND_API_KEY` | _unset_ | Resend API key (sending access is enough). Required with `EMAIL_DRIVER=resend`, ignored otherwise. Never logged. |
| `RESEND_API_BASE` | `https://api.resend.com` | Resend API base URL (only changed in tests). |
| `DISCORD_CLIENT_ID` | _unset_ | Discord application id. Discord login is on only when both id and secret are set. |
| `DISCORD_CLIENT_SECRET` | _unset_ | Discord application secret. Never logged, never sent to the browser. |
| `DISCORD_REDIRECT_URI` | _unset_ | The redirect URI registered with Discord. Defaults to `<PUBLIC_API_URL>/v1/auth/discord/callback`. |
| `DISCORD_API_BASE` | `https://discord.com/api/v10` | Discord API base URL (only changed in tests). |
| `DISCORD_FLOW_TTL_SECONDS` | `600` | How long a Discord login may take from start to callback (60 to 3600). |
| `SITE_ORIGIN` | `https://solar-rp.com` | The website's public origin (`scheme://host[:port]`, no path; https in production). A Discord link started on the website (handoff `web`) returns the browser to `<SITE_ORIGIN>/account/#discord_flow&discord_claim`. Configuration only: the redirect target is never read from a request. |
| `PAYMENT_PROVIDER` | `fake` | The provider used for new checkouts. Only `fake` exists today; real ones implement `PaymentProvider` and are registered in `buildDeps()`. |
| `ALLOW_FAKE_PAYMENTS_IN_PRODUCTION` | `0` | `1` lets a staging box run the fake provider with `NODE_ENV=production`. The fake provider grants items without charging anyone. |
| `FAKE_WEBHOOK_SECRET` | _unset_ | HMAC secret of the fake provider (at least 16 characters). Required whenever the fake provider is used, unless `ENABLE_DEV_ROUTES=1` (only then does it fall back to a public constant). |
| `ORDER_TTL_MINUTES` | `1440` | A pending order older than this is failed as `expired` by the maintenance job (5 to 10080). A payment that still arrives later is honoured. |
| `JOIN_TOKEN_TTL_SECONDS` | `60` | Lifetime of a join token (5 to 600). |
| `GAME_SESSION_MAX_HOURS` | `24` | A game session still open after this long is closed as `stale`. |
| `SERVER_STATUS_TTL_MS` | `5000` | How long a server's probe result is cached (0 to 600000). The server list never probes more often than this per server. |
| `SERVER_STATUS_WAIT_MS` | `400` | How long `GET /v1/servers` waits for a server that has never been probed before reporting it as `unknown`. |
| `PROBE_TIMEOUT_MS` | `1500` | Per-attempt timeout of the UDP query. |
| `PROBE_ATTEMPTS` | `2` | UDP query attempts per probe (UDP can lose a packet). |
| `SETTINGS_MAX_BYTES` | `16384` | Largest settings object a player may store (256 to 1048576). |
| `MAINTENANCE_INTERVAL_SECONDS` | `300` | How often the housekeeping job runs (purges expired rows, fails expired orders, closes stale game sessions). |

<!-- reference:end -->
