// One worked example per route, and the meaning of every error code. scripts/gen-reference.ts
// renders them into docs/api.md; `npm run docs:check` fails for a route without an example or a
// route error code without an explanation here. Values are samples, not real secrets.

export interface Example {
  /** A concrete path (ids and query string filled in). Defaults to the route's own path. */
  path?: string;
  /** Extra request headers besides the auth headers the generator adds itself. */
  headers?: Record<string, string>;
  request?: unknown;
  /** Defaults to the route's success status. */
  status?: number;
  response?: unknown;
  note?: string;
}

const ACCOUNT_ID = '7c9e6679-7425-40de-944b-e07fc1f90ae7';
const OTHER_ACCOUNT_ID = '0b6d2e94-5d3c-4c1e-8f0a-6a1c9f2b7d11';
const SESSION_ID = '3f2a8c1e-91b4-4d7a-b6c5-2e8f1a0d9c33';
const ORDER_ID = 'b5e0b6c4-0f3a-4e7a-9a53-3f0f0c4a9d21';
const PRODUCT_ID = '9d1f6a52-3c47-4b88-a1e0-5c2d7b9e4f60';
const GRANT_ID = 'e2c4a7f1-6b38-4d95-8a10-7f3b5c9d2e84';
const FLOW_ID = 'a8d3f5b2-7e61-4c09-9b24-1d6e8f0a3c57';
const SESSION_TOKEN = 'gms_Qm9uZUpvdXJub3NoZ2FtZXBsYXRmb3JtMDEyMzQ1Ng';
const JOIN_TOKEN = 'gmj_dGhpc19pc19hX3NhbXBsZV9qb2luX3Rva2VuXzEyMzQ1Ng';
const SERVER_SECRET = 'gss_c2FtcGxlX3NlcnZlcl9zZWNyZXRfbm90X3JlYWxfX18';
const POLL_SECRET = 'cG9sbF9zZWNyZXRfc2FtcGxlX25vdF9yZWFsXzAxMjM0NTY';
const CLAIM = 'Y2xhaW1fc2FtcGxlX25vdF9yZWFsX2Zvcl9kb2NzXzA';
const NOW = '2026-10-29T12:00:00.000Z';
const LATER = '2026-11-28T12:00:00.000Z';

const account = {
  id: ACCOUNT_ID,
  username: 'Speedy_Gonzo',
  email: 'speedy@example.com',
  emailVerified: true,
  roles: ['player'],
  hasPassword: true,
  discord: null,
  ban: null,
  createdAt: '2026-10-01T09:30:00.000Z',
};

const order = {
  id: ORDER_ID,
  status: 'pending',
  currency: 'USD',
  totalMinor: 1498,
  provider: 'fake',
  checkoutUrl: `http://127.0.0.1:8787/v1/dev/fake-checkout/${ORDER_ID}`,
  failureReason: null,
  createdAt: NOW,
  expiresAt: '2026-10-30T12:00:00.000Z',
  paidAt: null,
  fulfilledAt: null,
  refundedAt: null,
  items: [
    { lineNo: 0, sku: 'coins-1000', name: '1000 coins', kind: 'currency_pack', quantity: 2, unitPriceMinor: 499 },
    { lineNo: 1, sku: 'hat-red', name: 'Red hat', kind: 'cosmetic', quantity: 1, unitPriceMinor: 500 },
  ],
};

const product = {
  id: PRODUCT_ID,
  sku: 'vip-30',
  name: 'VIP, 30 days',
  description: 'Priority queue and a gold name tag.',
  priceMinor: 999,
  currency: 'USD',
  kind: 'vip',
  grants: [{ type: 'entitlement', key: 'vip', durationDays: 30 }],
  active: true,
  sort: 10,
  imageUrl: 'https://cdn.example.com/shop/vip.png',
  maxQuantity: 1,
};

const adminServer = {
  id: 'eu-1',
  name: 'GAGE EU 1',
  region: 'eu',
  host: 'eu1.gage-mp.example',
  port: 24800,
  tags: ['freeroam'],
  sort: 0,
  visible: true,
  maintenance: false,
  maintenanceMessage: null,
  allowedIps: ['203.0.113.10'],
  createdAt: NOW,
  updatedAt: NOW,
};

const grantEntry = {
  id: GRANT_ID,
  seq: 412,
  kind: 'grant',
  revokesGrantId: null,
  type: 'entitlement',
  sku: 'vip-30',
  payload: { type: 'entitlement', key: 'vip', durationDays: 30 },
  quantity: 1,
  orderId: ORDER_ID,
  createdAt: NOW,
};

const banView = { id: '5a1c7e90-2b4d-4f36-8c18-9e0d3a6b1f72', reason: 'aimbot in ranked', createdAt: NOW, expiresAt: LATER };

export const EXAMPLES: Record<string, Example> = {
  'GET /health': { response: { status: 'ok', uptimeSeconds: 86400, version: '0.1.0' } },
  'GET /v1/openapi.json': { response: { openapi: '3.1.0', info: { title: 'GAGE-MP platform API', version: '0.1.0' }, paths: { '/health': { get: { summary: 'Liveness and database check' } } } }, note: 'Abbreviated: the real document lists every mounted route with its request schema.' },

  // --- accounts
  'POST /v1/auth/register': { request: { email: 'speedy@example.com', username: 'Speedy_Gonzo', password: 'a-long-unusual-passphrase' }, response: { verificationRequired: true } },
  'POST /v1/auth/login': { request: { identifier: 'Speedy_Gonzo', password: 'a-long-unusual-passphrase', deviceName: 'Gaming PC' }, response: { token: SESSION_TOKEN, expiresAt: LATER, sessionId: SESSION_ID, account } },
  'POST /v1/auth/logout': {},
  'POST /v1/auth/verify-email': { request: { token: 'dmVyaWZ5X3Rva2VuX3NhbXBsZV9ub3RfcmVhbF8wMTIzNDU2' }, response: { account } },
  'POST /v1/auth/resend-verification': { response: { sent: true } },
  'POST /v1/auth/forgot-password': { request: { email: 'speedy@example.com' }, response: { sent: true } },
  'POST /v1/auth/reset-password': { request: { token: 'cmVzZXRfdG9rZW5fc2FtcGxlX25vdF9yZWFsXzAxMjM0NTY', password: 'another-long-unusual-passphrase' }, response: { ok: true } },

  // --- discord
  'POST /v1/auth/discord/start': { response: { flowId: FLOW_ID, authorizeUrl: 'https://discord.com/oauth2/authorize?response_type=code&client_id=123456789012345678&scope=identify+email&redirect_uri=https%3A%2F%2Fapi.gage-mp.example%2Fv1%2Fauth%2Fdiscord%2Fcallback&state=...&code_challenge=...&code_challenge_method=S256&prompt=consent', pollSecret: POLL_SECRET, expiresAt: '2026-10-29T12:10:00.000Z' } },
  'GET /v1/auth/discord/callback': {
    path: '/v1/auth/discord/callback?code=<code from Discord>&state=<state from the authorize URL>',
    response: '<!doctype html><html lang="en"><head>...<meta http-equiv="refresh" content="0;url=gage-mp://auth/discord?flow=<flowId>&amp;claim=<claim>">...</head><body><main><h1>Approved</h1><p>Return to GAGE to finish signing in. You can close this window afterwards.</p><p><a class="btn" href="gage-mp://auth/discord?flow=<flowId>&amp;claim=<claim>">Return to GAGE</a></p></main></body></html>',
    note: 'The page redirects to the launcher\'s `gage-mp://auth/discord?flow=<flowId>&claim=<claim>` URL and offers the same URL as a "Return to GAGE" button; the claim (32 random bytes, base64url, 43 characters) is only in that URL, never in visible text. HTTP 400 with the heading "Discord sign-in did not finish" when Discord refused (the URL is still handed over, so the launcher can read why); HTTP 400 without any URL when the state is unknown, used or expired. The page never repeats anything from the request, and is sent with `Content-Security-Policy: default-src \'none\'`, `Referrer-Policy: no-referrer` and `Cache-Control: no-store`.',
  },
  'POST /v1/auth/discord/poll': { request: { flowId: FLOW_ID, pollSecret: POLL_SECRET, claim: CLAIM }, response: { status: 'needs_username', suggestedUsername: 'cool_guy_99', discordUsername: 'cool.guy-99' }, note: '`claim` comes from the `gage-mp://` URL; without it, or with a wrong one, the answer is always `{ "status": "pending" }`. Other results: `{ "status": "failed", "code": "email_in_use" }` (also `access_denied`, `discord_error`, `invalid_request`; for a link: `discord_already_linked`, `already_has_discord`, `email_not_verified`), `{ "status": "linked" }` (link flows) and `{ "status": "complete", "token", "expiresAt", "sessionId", "account" }`. `complete` and `linked` are returned once and spend the claim.' },
  'POST /v1/auth/discord/complete': { request: { flowId: FLOW_ID, pollSecret: POLL_SECRET, claim: CLAIM, username: 'CoolGuy99' }, response: { token: SESSION_TOKEN, expiresAt: LATER, sessionId: SESSION_ID, account: { ...account, username: 'CoolGuy99', hasPassword: false, discord: { id: '123456789012345678', username: 'cool.guy-99' } } } },
  'POST /v1/me/discord/link': { response: { flowId: FLOW_ID, authorizeUrl: 'https://discord.com/oauth2/authorize?...', pollSecret: POLL_SECRET, expiresAt: '2026-10-29T12:10:00.000Z' }, note: 'Finish exactly like a login flow (callback, `gage-mp://` handoff, then poll with the claim); the poll answers `{ "status": "linked" }` and only then is the identity attached. Needs a verified email (403 `email_not_verified`). The owner is emailed when the link is made.' },
  'DELETE /v1/me/discord': {},

  // --- the account
  'GET /v1/me': { response: { account } },
  'POST /v1/me/password': { request: { currentPassword: 'a-long-unusual-passphrase', newPassword: 'another-long-unusual-passphrase' }, response: { revokedSessions: 2 } },
  'GET /v1/me/sessions': { response: { sessions: [{ id: SESSION_ID, deviceName: 'Gaming PC', userAgent: 'GAGE-MP-Launcher/0.2', ip: '203.0.113.50', createdAt: NOW, lastSeenAt: NOW, expiresAt: LATER, current: true }] } },
  'DELETE /v1/me/sessions/:id': { path: `/v1/me/sessions/${SESSION_ID}` },
  'DELETE /v1/me/sessions': { response: { revoked: 2 } },

  // --- settings
  'GET /v1/me/settings': { response: { settings: { language: 'ka', gameFolder: 'D:\\Games\\GTA V' }, version: 3, updatedAt: NOW } },
  'PUT /v1/me/settings': { request: { settings: { language: 'en', gameFolder: 'D:\\Games\\GTA V', startMinimised: true }, expectedVersion: 3 }, response: { settings: { language: 'en', gameFolder: 'D:\\Games\\GTA V', startMinimised: true }, version: 4, updatedAt: NOW } },

  // --- wallet and items
  'GET /v1/me/wallet': { response: { balances: [{ currency: 'coins', balance: 1500 }] } },
  'GET /v1/me/wallet/ledger': { path: '/v1/me/wallet/ledger?limit=2', response: { entries: [{ id: 17, currency: 'coins', delta: -500, reason: 'spend', memo: 'Sultan RS', createdAt: NOW }, { id: 12, currency: 'coins', delta: 2000, reason: 'purchase', memo: null, createdAt: '2026-10-28T18:00:00.000Z' }] }, note: 'Next page: `?limit=2&before=12` (the last id received).' },
  'GET /v1/me/items': { response: { items: [{ id: GRANT_ID, sku: 'vip-30', type: 'entitlement', payload: { type: 'entitlement', key: 'vip', durationDays: 30 }, quantity: 1, orderId: ORDER_ID, delivered: true, createdAt: NOW }] } },

  // --- servers and joining
  'GET /v1/servers': { response: { servers: [{ id: 'eu-1', name: 'GAGE EU 1', region: 'eu', host: 'eu1.gage-mp.example', port: 24800, tags: ['freeroam'], sort: 0, maintenance: { enabled: false, message: null }, status: { state: 'online', players: 12, maxPlayers: 64, pingMs: 31, protocol: 1, checkedAt: NOW } }] } },
  'POST /v1/game/join': { request: { serverId: 'eu-1' }, response: { token: JOIN_TOKEN, expiresAt: '2026-10-29T12:01:00.000Z', server: { id: 'eu-1', name: 'GAGE EU 1', host: 'eu1.gage-mp.example', port: 24800 } } },

  // --- shop
  'GET /v1/shop/products': { response: { products: [product] } },
  'POST /v1/shop/checkout': { headers: { 'Idempotency-Key': 'checkout-7f3a9c21-0001' }, request: { items: [{ sku: 'coins-1000', quantity: 2 }, { sku: 'hat-red', quantity: 1 }] }, response: { order } },
  'GET /v1/shop/orders': { path: '/v1/shop/orders?limit=20', response: { orders: [order] } },
  'GET /v1/shop/orders/:id': { path: `/v1/shop/orders/${ORDER_ID}`, response: { order: { ...order, status: 'paid', checkoutUrl: null, paidAt: '2026-10-29T12:03:10.000Z' } } },

  // --- payments
  'POST /v1/webhooks/:provider': {
    path: '/v1/webhooks/fake',
    headers: { 'X-Fake-Signature': 't=1793275390,v1=<hex HMAC-SHA256 of "<t>.<raw body>">' },
    request: { id: 'evt_5d8e2f', type: 'payment.succeeded', orderId: ORDER_ID, providerRef: 'fake_cs_1a2b3c', amountMinor: 1498, currency: 'USD' },
    response: { received: true, duplicate: false, result: 'applied' },
    note: 'The signature header is provider specific; this one is the fake provider\'s. A redelivery of the same event id answers `{ "received": true, "duplicate": true, "result": "duplicate" }`.',
  },
  'GET /v1/dev/fake-checkout/:orderId': { path: `/v1/dev/fake-checkout/${ORDER_ID}`, response: { orderId: ORDER_ID, status: 'pending', totalMinor: 1498, currency: 'USD', complete: { method: 'POST', url: `http://127.0.0.1:8787/v1/dev/fake-checkout/${ORDER_ID}/complete`, body: { outcome: 'succeeded | failed' } } } },
  'POST /v1/dev/fake-checkout/:orderId/complete': { path: `/v1/dev/fake-checkout/${ORDER_ID}/complete`, request: { outcome: 'succeeded' }, response: { received: true, duplicate: false, result: 'applied' } },

  // --- admin
  'GET /v1/admin/accounts': { path: '/v1/admin/accounts?q=speedy&limit=10', response: { accounts: [{ id: ACCOUNT_ID, username: 'Speedy_Gonzo', email: 'speedy@example.com', emailVerified: true, roles: ['player'], ban: null, createdAt: '2026-10-01T09:30:00.000Z', lastLoginAt: NOW }] } },
  'GET /v1/admin/accounts/:id': { path: `/v1/admin/accounts/${ACCOUNT_ID}`, response: { account: { id: ACCOUNT_ID, username: 'Speedy_Gonzo', email: 'speedy@example.com', emailVerified: true, roles: ['player'], ban: banView, createdAt: '2026-10-01T09:30:00.000Z', lastLoginAt: NOW } } },
  'POST /v1/admin/accounts/:id/ban': { path: `/v1/admin/accounts/${ACCOUNT_ID}/ban`, request: { reason: 'aimbot in ranked', durationSeconds: 2592000 }, response: { ban: banView } },
  'POST /v1/admin/accounts/:id/unban': { path: `/v1/admin/accounts/${ACCOUNT_ID}/unban`, request: { reason: 'appeal accepted' }, response: { ok: true } },
  'PUT /v1/admin/accounts/:id/roles': { path: `/v1/admin/accounts/${OTHER_ACCOUNT_ID}/roles`, request: { roles: ['mod'] }, response: { roles: ['player', 'mod'] } },
  'GET /v1/admin/audit': { path: '/v1/admin/audit?action=account.ban&limit=1', response: { entries: [{ id: 301, at: NOW, actorAccountId: OTHER_ACCOUNT_ID, actorLabel: `account:${OTHER_ACCOUNT_ID}`, action: 'account.ban', targetType: 'account', targetId: ACCOUNT_ID, details: { reason: 'aimbot in ranked', expiresAt: LATER, username: 'Speedy_Gonzo' }, ip: '203.0.113.20', requestId: '4b1f0c6e-9d2a-4e57-8a31-6c0f2e9b7d15' }] }, note: 'Page back with `before=<id of the last entry>`.' },
  'GET /v1/admin/servers': { response: { servers: [adminServer] } },
  'POST /v1/admin/servers': { request: { id: 'eu-1', name: 'GAGE EU 1', region: 'eu', host: 'eu1.gage-mp.example', port: 24800, tags: ['freeroam'], allowedIps: ['203.0.113.10'] }, response: { server: adminServer, secret: SERVER_SECRET }, note: 'The secret is shown once. Only its hash is stored.' },
  'PATCH /v1/admin/servers/:id': { path: '/v1/admin/servers/eu-1', request: { maintenance: true, maintenanceMessage: 'Back at 14:00 UTC' }, response: { server: { ...adminServer, maintenance: true, maintenanceMessage: 'Back at 14:00 UTC' } } },
  'POST /v1/admin/servers/:id/rotate-secret': { path: '/v1/admin/servers/eu-1/rotate-secret', response: { secret: SERVER_SECRET } },
  'DELETE /v1/admin/servers/:id': { path: '/v1/admin/servers/eu-1' },
  'GET /v1/admin/products': { response: { products: [product] } },
  'POST /v1/admin/products': { request: { sku: 'vip-30', name: 'VIP, 30 days', description: 'Priority queue and a gold name tag.', priceMinor: 999, currency: 'USD', kind: 'vip', grantPayload: { grants: [{ type: 'entitlement', key: 'vip', durationDays: 30 }] }, sort: 10, imageUrl: 'https://cdn.example.com/shop/vip.png' }, response: { product } },
  'PATCH /v1/admin/products/:id': { path: `/v1/admin/products/${PRODUCT_ID}`, request: { priceMinor: 799 }, response: { product: { ...product, priceMinor: 799 } } },
  'DELETE /v1/admin/products/:id': { path: `/v1/admin/products/${PRODUCT_ID}` },
  'GET /v1/admin/orders': { path: `/v1/admin/orders?status=paid&accountId=${ACCOUNT_ID}`, response: { orders: [{ ...order, status: 'paid', checkoutUrl: null, paidAt: NOW, accountId: ACCOUNT_ID }] } },
  'GET /v1/admin/orders/:id': { path: `/v1/admin/orders/${ORDER_ID}`, response: { order: { ...order, status: 'fulfilled', checkoutUrl: null, paidAt: NOW, fulfilledAt: NOW } } },
  'POST /v1/admin/orders/:id/refund': { path: `/v1/admin/orders/${ORDER_ID}/refund`, request: { reason: 'customer request' }, response: { order: { ...order, status: 'refunded', checkoutUrl: null, paidAt: NOW, refundedAt: NOW } } },

  // --- game server (internal)
  'POST /internal/game/verify': { request: { token: JOIN_TOKEN }, response: { ok: true, accountId: ACCOUNT_ID, username: 'Speedy_Gonzo', roles: ['player'], emailVerified: true, sessionId: SESSION_ID, ban: null }, note: 'Refusals are also HTTP 200: `{ "ok": false, "reason": "invalid_token" | "expired" | "used" }`, or `{ "ok": false, "reason": "banned", "accountId", "username", "ban": { "reason", "expiresAt" } }`.' },
  'POST /internal/game/session-end': { request: { accountId: ACCOUNT_ID, reason: 'quit' }, response: { ended: 1 } },
  'GET /internal/game/grants': { path: `/internal/game/grants?accountId=${ACCOUNT_ID}`, response: { grants: [grantEntry] } },
  'POST /internal/game/grants/pull': { request: { accountId: ACCOUNT_ID }, response: { grants: [grantEntry] } },
  'POST /internal/game/grants/:id/ack': { path: `/internal/game/grants/${GRANT_ID}/ack`, response: { id: GRANT_ID, alreadyAcked: false }, note: 'Only the server that pulled the entry may ack it (409 `grant_not_pulled` otherwise).' },
  'GET /internal/game/wallet': { path: `/internal/game/wallet?accountId=${ACCOUNT_ID}`, response: { balances: [{ currency: 'coins', balance: 1500 }] } },
  'POST /internal/game/wallet/balance': { request: { accountId: ACCOUNT_ID }, response: { balances: [{ currency: 'coins', balance: 1500 }] } },
  'POST /internal/game/wallet/spend': { request: { accountId: ACCOUNT_ID, currency: 'coins', amount: 500, idempotencyKey: 'eu-1:purchase:8841', memo: 'Sultan RS' }, response: { replayed: false, balance: 1000 } },
};

/** Meaning of every error code a route lists. Codes every route can return are in docs/api.md ("Errors"). */
export const ERROR_CODES: Record<string, string> = {
  invalid_request: 'The body, query string or a path parameter failed validation. `details.issues` lists `{ path, message }` (at most 10).',
  invalid_json: 'The body is not valid JSON.',
  unsupported_media_type: 'A body was sent without `Content-Type: application/json`.',
  payload_too_large: 'The body is larger than the route\'s limit.',
  unauthorized: 'No valid session token. The response carries `WWW-Authenticate: Bearer`.',
  forbidden: 'The account lacks the role this route needs.',
  not_found: 'No such route.',
  rate_limited: 'Too many requests or failed attempts. `Retry-After` says how many seconds to wait.',
  internal_error: 'An unexpected failure on the server (logged with the request id).',
  weak_password: 'The password is too short, too long, too common or contains the username or email name. `details.problem` is one of `too_short`, `too_long`, `common`, `contains_username`, `contains_email`.',
  username_taken: 'The username is taken (case-insensitively) or reserved.',
  invalid_credentials: 'Wrong identifier or password (the same answer for an unknown account).',
  invalid_token: 'The emailed token is unknown, expired, already used or for another purpose.',
  already_verified: 'The email address is already verified.',
  no_email: 'The account has no email address.',
  email_in_use: 'The Discord email belongs to an existing GAGE account. Log in with the password and link Discord from the account settings.',
  email_required: 'Add an email address before setting a password.',
  current_password_required: 'The account has a password, so the current one must be sent.',
  last_login_method: 'The account has no password: Discord is its only way in. Set a password first.',
  not_linked: 'No Discord account is linked.',
  discord_disabled: 'Discord login is not configured on this server.',
  discord_already_linked: 'That Discord account is linked to another GAGE account.',
  flow_not_found: 'No such Discord flow, or the poll secret is wrong, or (for `complete`) the claim is wrong: all look the same.',
  flow_expired: 'The Discord flow is older than DISCORD_FLOW_TTL_SECONDS.',
  flow_used: 'The result of this Discord flow was already delivered.',
  flow_state: '`complete` was called on a flow that is not waiting for a username.',
  session_not_found: 'No such live session on this account.',
  invalid_settings: 'The settings object breaks a rule. `details.problems` lists up to 5 reasons.',
  version_conflict: 'Another device wrote first. `details.currentVersion` is the stored version.',
  email_not_verified: 'Verify the email address first: to join or buy (while REQUIRE_VERIFIED_EMAIL=1), and always before linking or unlinking Discord or setting a first password.',
  account_banned: 'The account is banned. `details` has `reason` and `expiresAt` (null = permanent). Also returned to a banned mod or admin by every admin route, and to a game server for a banned account on spend and pull.',
  account_not_found: 'No such account.',
  server_not_found: 'No such server (hidden servers do not exist for players).',
  server_maintenance: 'The server is in maintenance; staff may still join.',
  server_exists: 'A server with that id already exists.',
  product_not_found: 'No such product.',
  product_inactive: 'The product is not on sale.',
  product_in_use: 'Orders reference the product: deactivate it instead of deleting it.',
  sku_exists: 'A product with that SKU already exists.',
  quantity_exceeds_limit: 'More than the product\'s `maxQuantity` in one order.',
  mixed_currency: 'The basket mixes currencies.',
  idempotency_mismatch: 'The Idempotency-Key (checkout, 422) or idempotencyKey (spend, 409) was already used for a different request.',
  payment_unavailable: 'The payment provider could not create the checkout. The order is marked failed; try again.',
  order_not_found: 'No such order (an order of another account looks the same).',
  order_not_refundable: 'Only paid or fulfilled orders with a provider payment can be refunded.',
  refund_failed: 'The provider refused or could not be reached; nothing was changed.',
  invalid_webhook: 'The webhook signature or body did not verify.',
  unknown_provider: 'No payment provider with that id.',
  forbidden_target: 'You may not ban this account (yourself, an admin, or a mod unless you are an admin), or lift this ban (your own, or that of a mod or admin unless you are an admin). Refused unbans are audited.',
  busy: 'The server is busy checking passwords (ARGON2_MAX_CONCURRENT running, ARGON2_MAX_QUEUE waiting). Nothing was counted against you; retry after `Retry-After` seconds.',
  grant_not_pulled: 'The grant was not delivered to this server: it was never pulled, or another server holds the delivery (the player moved). Pull again while the account has a session here.',
  already_banned: 'The account already has a ban in force; unban first to change it.',
  not_banned: 'The account has no ban to lift.',
  last_admin: 'This is the last admin: promote another account first.',
  no_active_session: 'The account has no open game session on this server (call verify first).',
  grant_not_found: 'No such grant for this server.',
  insufficient_funds: 'The balance is lower than the amount. `details.balance` has the balance.',
  database_unavailable: 'The database did not answer (health check).',
  invalid_server_credentials: 'Unknown server id or wrong secret.',
  ip_not_allowed: 'The call came from an address outside the server\'s allowlist.',
};
