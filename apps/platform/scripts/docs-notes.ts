// Hand-written prose for the generated parts of docs/api.md: what each table is for and which
// invariants the database itself enforces, and what each environment variable does. The column,
// index and constraint lists next to them are generated from the schema (scripts/gen-reference.ts),
// and `npm run docs:check` fails when a table or variable has no note here.

export interface TableNote {
  purpose: string;
  /** Rules the database enforces (constraints, indexes, triggers), in plain words. */
  invariants: string[];
}

export const TABLE_NOTES: Record<string, TableNote> = {
  accounts: {
    purpose: 'One row per player. `username` is the in-game name.',
    invariants: [
      'Usernames are unique case-insensitively (`accounts_username_lower_uq`) and match `[A-Za-z0-9_]{3,20}`; display case is kept.',
      'Emails are stored lower-case and are unique case-insensitively when present.',
      'A password hash or a verified email needs an email address (a Discord-only account may have neither).',
      'The password column holds an argon2id PHC string, never a password.',
    ],
  },
  account_roles: {
    purpose: 'Roles of an account: `player` (everyone), `mod`, `admin`.',
    invariants: ['One row per (account, role); only those three values exist.', 'Deleting an account removes its roles.'],
  },
  sessions: {
    purpose: 'Login sessions (one per device). The bearer token is opaque; only its SHA-256 is stored.',
    invariants: [
      'The token hash is unique.',
      '`expires_at` slides forward on use (30 days idle by default) but never past `absolute_expires_at` (SESSION_MAX_DAYS), enforced by `sessions_expiry_order`.',
      'Changing the password revokes every other session (`revoked_at`, `revoked_reason`).',
    ],
  },
  auth_tokens: {
    purpose: 'Single-use emailed tokens: email verification and password reset. Only the SHA-256 is stored.',
    invariants: [
      'The token hash is unique and `kind` is `email_verify` or `password_reset`.',
      'Consumption is one conditional `UPDATE ... WHERE used_at IS NULL AND expires_at > now`, so a token works once even under concurrent requests.',
      'A verification token is void when the account email changed after it was issued.',
    ],
  },
  auth_throttle: {
    purpose: 'Login failure counters with exponential lockout, per client IP group and per TYPED identifier (never per account).',
    invariants: [
      'One row per key: `ip:<group>` (any identifier from one address), `idip:<hash>:<block>` (one identifier from one IPv4 /24 or IPv6 /64) and `id:<hash>` (one identifier from anywhere). `<hash>` is the SHA-256 of the lower-cased identifier as typed, so the table holds no email addresses and a key never reveals whether the identifier belongs to an account.',
      'Persistent, so lockouts survive restarts and work across processes.',
    ],
  },
  rate_limits: {
    purpose: 'Fixed-window counters behind every per-route rate limit that is not a login failure.',
    invariants: ['One row per key; the counter is bumped by a single atomic upsert, so concurrent requests are counted exactly.'],
  },
  oauth_flows: {
    purpose: 'In-flight Discord OAuth2 logins and links: state hash, PKCE verifier, poll-secret hash, claim hash and the outcome the launcher polls for.',
    invariants: [
      'The state hash is unique and consumed by a conditional UPDATE (status pending, not yet called back, not expired), so a replayed or forged callback matches nothing.',
      '`claim_hash` is the SHA-256 of the random claim the callback page hands to the approving browser (`gage-mp://auth/discord?flow&claim`). Poll and complete show nothing without it; it is cleared when the flow is consumed.',
      'The PKCE verifier and the stored Discord profile are cleared as soon as they are no longer needed; rows are purged a day after expiry.',
      'Status is one of `pending`, `login_ready`, `needs_username`, `link_ready`, `linked` (legacy, no longer written), `failed`, `consumed`. A link flow stops at `link_ready`: the identity is attached by the claimed poll, never by the callback.',
    ],
  },
  oauth_identities: {
    purpose: 'Links between a GAGE account and an external identity (Discord).',
    invariants: [
      'A Discord user id belongs to at most one account, and an account has at most one Discord identity.',
      'Identities are matched on the immutable provider user id, never on email or name.',
    ],
  },
  email_outbox: {
    purpose: 'The development `EmailSender`: every mail the platform would have sent (`EMAIL_DRIVER=outbox`). Live verification and reset tokens are REDACTED in the body, subject and `meta`; in development the full link is logged instead. It delivers nothing.',
    invariants: ['`seq` is an identity column that gives a strict send order (timestamps tie).', 'No row holds a working token: a database read, a backup or a support export cannot take over an account.'],
  },
  bans: {
    purpose: 'Account bans with reason, optional expiry and who issued and lifted them.',
    invariants: [
      'At most one ban per account that has not been revoked (`bans_one_open_per_account`). An expired ban is closed as `expired` when the next one is issued.',
      '`created_by` / `revoked_by` keep the staff account (set null if that account is deleted); reasons are 1 to 500 characters.',
    ],
  },
  audit_log: {
    purpose: 'Every admin and moderator action.',
    invariants: [
      'Append-only: a trigger rejects UPDATE and DELETE.',
      'Written in the same transaction as the change it records, so there is no admin action without a row.',
      '`actor_label` says who acted when there is no account (`cli:<os user>`, `system`).',
    ],
  },
  game_servers: {
    purpose: 'The official servers: where they are, how to reach them and the hash of their secret.',
    invariants: [
      '`id` is a slug (`[a-z0-9][a-z0-9-]{1,31}`), port 1 to 65535.',
      'Only `secret_hash` is stored; the secret is shown once at creation or rotation.',
      '`allowed_ips` holds normalised addresses; an empty list means the server may not call the internal API at all (fail closed).',
    ],
  },
  join_tokens: {
    purpose: 'Single-use 60 second tokens that carry a player from the launcher to a game server. Only the SHA-256 is stored.',
    invariants: [
      'Token hash unique; `expires_at` is after `created_at`.',
      'Bound to one account and one server (deleting either deletes the token). Consumption is a conditional UPDATE inside the verification transaction. An account has at most one unused token: asking again voids the previous one.',
    ],
  },
  game_sessions: {
    purpose: 'Who is currently on which server. Opened by `verify`, closed by `session-end`, a new join, a ban (`banned`) or maintenance.',
    invariants: ['At most one open session per account (`game_sessions_one_open_per_account`, partial unique index).', 'Sessions older than GAME_SESSION_MAX_HOURS are closed as `stale`.'],
  },
  products: {
    purpose: 'The shop catalogue.',
    invariants: [
      'SKU unique and URL-safe; price positive and at most 100 000 000 minor units; currency is an ISO 4217 code; `kind` is `currency_pack`, `vip`, `cosmetic` or `bundle`.',
      '`grant_payload` is validated against the kind before it is stored (a currency pack may only credit currency, a VIP only grants entitlements, and so on).',
    ],
  },
  orders: {
    purpose: 'A purchase attempt and its state: `pending`, `paid`, `fulfilled`, `refunded`, `failed`, `chargeback`.',
    invariants: [
      'Status changes follow a transition table enforced by a trigger: pending to paid or failed; paid to fulfilled, refunded or chargeback; fulfilled to refunded or chargeback; and failed back to paid only for the reasons `expired` and `payment_failed` (a payment that still arrives is honoured). Everything else is rejected, including any move out of `refunded` and `chargeback`.',
      'A paid-or-later order has `paid_at`; a fulfilled order has `fulfilled_at`; the total is positive.',
      '`(account_id, idempotency_key)` is unique, so a retried checkout returns the same order; `(provider, provider_ref)` is unique.',
    ],
  },
  order_items: {
    purpose: 'The lines of an order, copied from the products at checkout (name, price, grant payload).',
    invariants: [
      'Immutable: a trigger rejects UPDATE, so changing or deactivating a product never changes what an existing order delivers.',
      'A composite foreign key `(order_id, currency)` makes it impossible for a line to carry another currency than its order. Products with orders cannot be deleted (`ON DELETE RESTRICT`).',
    ],
  },
  payment_events: {
    purpose: 'Every verified provider webhook, once.',
    invariants: [
      '`(provider, event_id)` is unique and is inserted first, in the same transaction as the effects: a redelivered event finds its row and does nothing.',
      '`result` records what happened: `applied`, `ignored` (the order state did not allow it), `unmatched` (no such order) or `rejected` (for example an amount mismatch).',
    ],
  },
  grants: {
    purpose: 'The delivery ledger: what a paid order grants (`kind` = grant) and what a refund or chargeback takes back (`kind` = revoke).',
    invariants: [
      'Unique per `(order_id, line_no, entry_no, kind)`, and a revoke is unique per revoked grant (`grants_one_revoke_per_grant`): a replayed webhook cannot create a second grant or a second revoke.',
      '`seq` is a global identity: the game server applies pending entries in `seq` order.',
      'Status is `pending` until acknowledged (`acked_at` is set exactly when status is `acked`). Wallet credits are applied by the platform and are born acknowledged by `platform`.',
      'A revoke has `revokes_grant_id`; a grant has none.',
      '`delivering_server` is set when a game server pulls the entry (the server holding the account\'s open session). Only that server\'s ack counts: the ack is one conditional UPDATE on it, and `grants_ack_matches_binding` makes the database refuse any other server\'s ack (the platform\'s own acks of wallet credits are exempt).',
    ],
  },
  wallet_ledger: {
    purpose: 'The in-game currency ledger. The balance of an account is the sum of its deltas.',
    invariants: [
      'Append-only: a trigger rejects UPDATE and DELETE.',
      '`idempotency_key` is unique (`grant:<id>`, `revoke:<grantId>`, `spend:<serverId>:<key>`): replaying an operation is a no-op.',
      'Purchases are positive, spends and revokes negative, never zero (`wallet_ledger_sign`).',
      'A trigger updates `wallet_balances` in the same statement and rejects an insert that would take the balance below zero (`insufficient_funds`), except a `revoke` (a refund may leave a debt that later purchases pay off).',
    ],
  },
  wallet_balances: {
    purpose: 'The derived balance per account and currency.',
    invariants: ['Written only by the ledger trigger: a guard trigger rejects every other INSERT, UPDATE and DELETE, so a balance cannot exist without ledger entries that add up to it.'],
  },
  account_settings: {
    purpose: 'The launcher\'s per-account settings object.',
    invariants: ['`data` is a JSON object; `version` counts writes and backs the optimistic `expectedVersion` check.'],
  },
};

/** What each environment variable does. The default shown in the table comes from the schema. */
export const ENV_NOTES: Record<string, string> = {
  NODE_ENV: '`development`, `test` or `production`. REQUIRED: an unset value is refused at startup (it used to mean development) unless `ENABLE_DEV_ROUTES=1` says so explicitly. Production refuses an in-memory database, an unset `DATABASE_URL`, `ENABLE_DEV_ROUTES=1` and the fake payment provider (unless explicitly allowed).',
  ENABLE_DEV_ROUTES: '`1` mounts `/v1/dev/*` (pay or fail any order without paying) and allows the public built-in fake webhook secret. Development machines only: refused when `NODE_ENV=production`, and never implied by an unset `NODE_ENV`.',
  HOST: 'Address of the public listener. Keep `127.0.0.1` behind a reverse proxy.',
  PORT: 'Port of the public listener (0 to 65535; 0 picks a free port).',
  INTERNAL_HOST: 'Address of the internal (game server) listener. Bind it to loopback or a private interface and firewall it.',
  INTERNAL_PORT: 'Port of the internal listener (0 picks a free port).',
  INTERNAL_ON_PUBLIC_LISTENER: '`1` serves `/internal/*` on the public listener too (one port). Then rely on the per-server IP allowlist and a firewall.',
  DATABASE_URL: '`postgres://...` / `postgresql://...`, or `pglite:memory` / `pglite:<directory>` for the in-process development database. Required in production.',
  DATABASE_POOL_MAX: 'Maximum Postgres connections (1 to 200).',
  PUBLIC_API_URL: 'Public base URL of the API (http or https, no trailing slash). Used for checkout and Discord redirect URLs and as the OpenAPI server.',
  CORS_ORIGINS: 'Comma separated browser origins allowed to read public responses: the launcher\'s WebView2 origin. Bare origins only (`scheme://host[:port]`); `*` and paths are refused.',
  TRUST_PROXY: '`1` trusts one reverse proxy: the client address is the last `X-Forwarded-For` entry. Never set it without a proxy that appends the header. `/internal/*` ignores it.',
  LOG_LEVEL: '`debug`, `info`, `warn` or `error`. Logs are one JSON object per line on stdout.',
  BODY_LIMIT_BYTES: 'Default request body limit (1024 to 4194304). A few routes set their own (settings, webhooks).',
  SHUTDOWN_GRACE_SECONDS: 'How long in-flight requests may take after SIGTERM/SIGINT before connections are cut (1 to 120).',
  SESSION_TTL_DAYS: 'Idle lifetime of a session; it slides forward on use (1 to 365).',
  SESSION_MAX_DAYS: 'Absolute lifetime of a session, however active (1 to 3650). Must be at least `SESSION_TTL_DAYS`.',
  REQUIRE_VERIFIED_EMAIL: '`1` makes joining a server and buying require a verified email. Logging in never does.',
  EMAIL_VERIFY_TTL_HOURS: 'Lifetime of an email verification token (1 to 720).',
  PASSWORD_RESET_TTL_MINUTES: 'Lifetime of a password reset token (5 to 1440).',
  ARGON2_MEMORY_KIB: 'argon2id memory cost in KiB. The default (19456) is OWASP\'s recommended minimum for t=2, p=1. Hashes made with weaker parameters are upgraded at the next successful login.',
  ARGON2_TIME_COST: 'argon2id iterations (1 to 20).',
  ARGON2_PARALLELISM: 'argon2id lanes (1 to 16).',
  ARGON2_MAX_CONCURRENT: 'Password hashes or checks running at once in this process (1 to 64). Keep it below the libuv thread pool (4 unless `UV_THREADPOOL_SIZE` is raised): argon2 runs there next to file and DNS work.',
  ARGON2_MAX_QUEUE: 'Hashes allowed to wait for a slot. One more is refused at once with 503 `busy` and a `Retry-After`, so a burst of logins cannot pile up memory or starve the pool.',
  LOGIN_ACCOUNT_IP_MAX_FAILURES: 'Consecutive failed logins for one typed identifier (username or email) from one address block (IPv4 /24, IPv6 /64) before that block is locked out of that identifier. Lower than the account-wide threshold.',
  LOGIN_ACCOUNT_MAX_FAILURES: 'Consecutive failed logins for one typed identifier from ANYWHERE before it is locked for everyone (a distributed guessing attack; default 100, so it takes failures from at least 20 blocks). Must be at least `LOGIN_ACCOUNT_IP_MAX_FAILURES`.',
  LOGIN_IP_MAX_FAILURES: 'Failed logins of any identifier per client IP group (IPv6: /64) before the address is locked.',
  LOGIN_LOCKOUT_BASE_SECONDS: 'First lockout length. Each further failure doubles it.',
  LOGIN_LOCKOUT_MAX_SECONDS: 'Upper bound of a lockout. Must be at least the base.',
  LOGIN_FAILURE_WINDOW_SECONDS: 'Failures are forgotten after this much calm, measured from the later of the last failure and the end of the lock.',
  EMAIL_DRIVER: '`outbox` writes mail to the `email_outbox` table (development); `console` logs it. Neither delivers mail and neither keeps a live token: bodies are redacted, and only with `NODE_ENV=development` is the full link logged. Plug a real `EmailSender` into `buildDeps()` before launch.',
  EMAIL_FROM: 'Sender shown on outgoing mail.',
  EMAIL_LINK_BASE_URL: 'Base of the links in mail. The default `gage-mp://auth` opens the launcher, which POSTs the token (verification and reset tokens are never consumed by a GET).',
  DISCORD_CLIENT_ID: 'Discord application id. Discord login is on only when both id and secret are set.',
  DISCORD_CLIENT_SECRET: 'Discord application secret. Never logged, never sent to the browser.',
  DISCORD_REDIRECT_URI: 'The redirect URI registered with Discord. Defaults to `<PUBLIC_API_URL>/v1/auth/discord/callback`.',
  DISCORD_API_BASE: 'Discord API base URL (only changed in tests).',
  DISCORD_FLOW_TTL_SECONDS: 'How long a Discord login may take from start to callback (60 to 3600).',
  PAYMENT_PROVIDER: 'The provider used for new checkouts. Only `fake` exists today; real ones implement `PaymentProvider` and are registered in `buildDeps()`.',
  ALLOW_FAKE_PAYMENTS_IN_PRODUCTION: '`1` lets a staging box run the fake provider with `NODE_ENV=production`. The fake provider grants items without charging anyone.',
  FAKE_WEBHOOK_SECRET: 'HMAC secret of the fake provider (at least 16 characters). Required whenever the fake provider is used, unless `ENABLE_DEV_ROUTES=1` (only then does it fall back to a public constant).',
  ORDER_TTL_MINUTES: 'A pending order older than this is failed as `expired` by the maintenance job (5 to 10080). A payment that still arrives later is honoured.',
  JOIN_TOKEN_TTL_SECONDS: 'Lifetime of a join token (5 to 600).',
  GAME_SESSION_MAX_HOURS: 'A game session still open after this long is closed as `stale`.',
  SERVER_STATUS_TTL_MS: 'How long a server\'s probe result is cached (0 to 600000). The server list never probes more often than this per server.',
  SERVER_STATUS_WAIT_MS: 'How long `GET /v1/servers` waits for a server that has never been probed before reporting it as `unknown`.',
  PROBE_TIMEOUT_MS: 'Per-attempt timeout of the UDP query.',
  PROBE_ATTEMPTS: 'UDP query attempts per probe (UDP can lose a packet).',
  SETTINGS_MAX_BYTES: 'Largest settings object a player may store (256 to 1048576).',
  MAINTENANCE_INTERVAL_SECONDS: 'How often the housekeeping job runs (purges expired rows, fails expired orders, closes stale game sessions).',
};
