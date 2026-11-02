// Database schema (PostgreSQL). Every table and its invariants are documented in docs/api.md
// ("Tables"). Rules that matter for correctness live here as constraints and unique indexes, and in
// the hand-written trigger migration (drizzle/0001_invariants.sql), not only in application code:
//
//   - one account per lower(username) and per lower(email); one active session token hash; one
//     join-token hash; one non-revoked ban per account; one open game session per account
//   - join tokens and email/reset tokens are single use because consumption is a conditional UPDATE
//   - grants are unique per (order, line, entry, kind) and a revoke is unique per revoked grant, so a
//     replayed webhook cannot create a second grant or a second revoke
//   - the wallet balance is maintained only by a trigger on the append-only wallet ledger; a spend
//     that would overdraw is rejected by the database
//   - order status changes follow a transition table enforced by a trigger
import { sql } from 'drizzle-orm';
import {
  bigint,
  boolean,
  check,
  foreignKey,
  index,
  integer,
  jsonb,
  pgTable,
  primaryKey,
  text,
  timestamp,
  unique,
  uniqueIndex,
  uuid,
} from 'drizzle-orm/pg-core';

const tstz = (name: string) => timestamp(name, { withTimezone: true, mode: 'date' });
const createdAt = () => tstz('created_at').notNull().defaultNow();

export const ROLES = ['player', 'mod', 'admin'] as const;
export type Role = (typeof ROLES)[number];

export const PRODUCT_KINDS = ['currency_pack', 'vip', 'cosmetic', 'bundle'] as const;
export type ProductKind = (typeof PRODUCT_KINDS)[number];

export const ORDER_STATUSES = ['pending', 'paid', 'fulfilled', 'refunded', 'failed', 'chargeback'] as const;
export type OrderStatus = (typeof ORDER_STATUSES)[number];

// ---------------------------------------------------------------------------------------------
// Accounts

export const accounts = pgTable(
  'accounts',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    /** Lower-case. NULL only for a Discord-only account whose Discord email was missing. */
    email: text('email'),
    emailVerifiedAt: tstz('email_verified_at'),
    /** The in-game name. Display case is kept; uniqueness is on lower(username). */
    username: text('username').notNull(),
    /** argon2id PHC string. NULL for a Discord-only account until it sets a password. */
    passwordHash: text('password_hash'),
    passwordChangedAt: tstz('password_changed_at'),
    createdAt: createdAt(),
    updatedAt: tstz('updated_at').notNull().defaultNow(),
    lastLoginAt: tstz('last_login_at'),
  },
  (t) => [
    uniqueIndex('accounts_username_lower_uq').on(sql`lower(${t.username})`),
    uniqueIndex('accounts_email_lower_uq').on(sql`lower(${t.email})`).where(sql`${t.email} is not null`),
    check('accounts_username_format', sql`${t.username} ~ '^[A-Za-z0-9_]{3,20}$'`),
    check('accounts_email_lowercase', sql`${t.email} is null or (${t.email} = lower(${t.email}) and length(${t.email}) <= 254)`),
    check('accounts_password_needs_email', sql`${t.passwordHash} is null or ${t.email} is not null`),
    check('accounts_verified_needs_email', sql`${t.emailVerifiedAt} is null or ${t.email} is not null`),
  ],
);

export const accountRoles = pgTable(
  'account_roles',
  {
    accountId: uuid('account_id')
      .notNull()
      .references(() => accounts.id, { onDelete: 'cascade' }),
    role: text('role').notNull(),
    grantedAt: tstz('granted_at').notNull().defaultNow(),
  },
  (t) => [
    primaryKey({ columns: [t.accountId, t.role] }),
    check('account_roles_role', sql`${t.role} in ('player', 'mod', 'admin')`),
  ],
);

export const sessions = pgTable(
  'sessions',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    accountId: uuid('account_id')
      .notNull()
      .references(() => accounts.id, { onDelete: 'cascade' }),
    /** hex SHA-256 of the opaque bearer token; the token itself is never stored. */
    tokenHash: text('token_hash').notNull(),
    createdAt: createdAt(),
    lastSeenAt: tstz('last_seen_at').notNull().defaultNow(),
    /** Sliding: pushed forward on use, never past absoluteExpiresAt. */
    expiresAt: tstz('expires_at').notNull(),
    absoluteExpiresAt: tstz('absolute_expires_at').notNull(),
    userAgent: text('user_agent'),
    deviceName: text('device_name'),
    ip: text('ip'),
    revokedAt: tstz('revoked_at'),
    revokedReason: text('revoked_reason'),
  },
  (t) => [
    uniqueIndex('sessions_token_hash_uq').on(t.tokenHash),
    index('sessions_account_idx').on(t.accountId),
    check('sessions_expiry_order', sql`${t.expiresAt} <= ${t.absoluteExpiresAt}`),
  ],
);

export const authTokens = pgTable(
  'auth_tokens',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    accountId: uuid('account_id')
      .notNull()
      .references(() => accounts.id, { onDelete: 'cascade' }),
    kind: text('kind').notNull(),
    tokenHash: text('token_hash').notNull(),
    /** For email_verify: the address the token proves. A token is void if the account email changed. */
    email: text('email'),
    createdAt: createdAt(),
    expiresAt: tstz('expires_at').notNull(),
    usedAt: tstz('used_at'),
  },
  (t) => [
    uniqueIndex('auth_tokens_hash_uq').on(t.tokenHash),
    index('auth_tokens_account_kind_idx').on(t.accountId, t.kind),
    check('auth_tokens_kind', sql`${t.kind} in ('email_verify', 'password_reset')`),
  ],
);

/** Failure counters with lockout backoff for login (keys: ip:<group>, acct:<id>, ident:<text>). */
export const authThrottle = pgTable('auth_throttle', {
  key: text('key').primaryKey(),
  failures: integer('failures').notNull().default(0),
  windowStart: tstz('window_start').notNull(),
  lockedUntil: tstz('locked_until'),
  updatedAt: tstz('updated_at').notNull().defaultNow(),
});

/** Fixed-window request counters for everything that is not a login attempt. */
export const rateLimits = pgTable('rate_limits', {
  key: text('key').primaryKey(),
  windowStart: tstz('window_start').notNull(),
  count: integer('count').notNull(),
});

export const oauthFlows = pgTable(
  'oauth_flows',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    provider: text('provider').notNull(),
    intent: text('intent').notNull(),
    /** hex SHA-256 of the `state` parameter; the state is single use. */
    stateHash: text('state_hash').notNull(),
    /** PKCE verifier. Short lived (flow TTL) and useless without the authorization code. */
    codeVerifier: text('code_verifier'),
    /** hex SHA-256 of the secret only the launcher that started the flow holds. */
    pollSecretHash: text('poll_secret_hash').notNull(),
    /**
     * hex SHA-256 of the claim the callback page hands to the launcher through the gage-mp:// URL
     * scheme. Set when the callback finishes (any outcome), erased when the flow is consumed. Poll
     * and complete show nothing without it, so whoever merely started the flow cannot read the
     * result of somebody else's approval.
     */
    claimHash: text('claim_hash'),
    /** The account to link to (intent = link). */
    accountId: uuid('account_id').references(() => accounts.id, { onDelete: 'cascade' }),
    status: text('status').notNull().default('pending'),
    resultAccountId: uuid('result_account_id').references(() => accounts.id, { onDelete: 'cascade' }),
    /** Discord profile held while a new user picks a username (status needs_username). */
    profile: jsonb('profile'),
    errorCode: text('error_code'),
    createdAt: createdAt(),
    expiresAt: tstz('expires_at').notNull(),
    callbackAt: tstz('callback_at'),
    consumedAt: tstz('consumed_at'),
  },
  (t) => [
    uniqueIndex('oauth_flows_state_uq').on(t.stateHash),
    check('oauth_flows_intent', sql`${t.intent} in ('login', 'link')`),
    check(
      'oauth_flows_status',
      sql`${t.status} in ('pending', 'login_ready', 'needs_username', 'link_ready', 'linked', 'failed', 'consumed')`,
    ),
    check('oauth_flows_link_needs_account', sql`${t.intent} <> 'link' or ${t.accountId} is not null`),
  ],
);

export const oauthIdentities = pgTable(
  'oauth_identities',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    accountId: uuid('account_id')
      .notNull()
      .references(() => accounts.id, { onDelete: 'cascade' }),
    provider: text('provider').notNull(),
    providerUserId: text('provider_user_id').notNull(),
    providerUsername: text('provider_username'),
    providerEmail: text('provider_email'),
    emailVerified: boolean('email_verified').notNull().default(false),
    createdAt: createdAt(),
  },
  (t) => [
    unique('oauth_identities_provider_user_uq').on(t.provider, t.providerUserId),
    unique('oauth_identities_account_provider_uq').on(t.accountId, t.provider),
    check('oauth_identities_provider', sql`${t.provider} in ('discord')`),
  ],
);

/** Dev EmailSender target: what a real provider would have sent. Holds live tokens: dev/test only. */
export const emailOutbox = pgTable(
  'email_outbox',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    /** Send order (created_at ties are common). */
    seq: bigint('seq', { mode: 'number' }).generatedAlwaysAsIdentity().notNull(),
    toAddress: text('to_address').notNull(),
    subject: text('subject').notNull(),
    body: text('body').notNull(),
    template: text('template').notNull(),
    /** Machine-readable parts of the message (for tests and tooling), e.g. { token }. */
    meta: jsonb('meta').notNull().default({}),
    createdAt: createdAt(),
  },
  (t) => [index('email_outbox_to_idx').on(t.toAddress, t.createdAt)],
);

// ---------------------------------------------------------------------------------------------
// Bans and audit

export const bans = pgTable(
  'bans',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    accountId: uuid('account_id')
      .notNull()
      .references(() => accounts.id, { onDelete: 'cascade' }),
    reason: text('reason').notNull(),
    createdBy: uuid('created_by').references(() => accounts.id, { onDelete: 'set null' }),
    createdAt: createdAt(),
    /** NULL = permanent. */
    expiresAt: tstz('expires_at'),
    revokedAt: tstz('revoked_at'),
    revokedBy: uuid('revoked_by').references(() => accounts.id, { onDelete: 'set null' }),
    revokedReason: text('revoked_reason'),
  },
  (t) => [
    // At most one ban per account that has not been revoked (an expired one is revoked as "expired"
    // when the next ban is issued), so "the ban" of an account is unambiguous.
    uniqueIndex('bans_one_open_per_account').on(t.accountId).where(sql`${t.revokedAt} is null`),
    check('bans_reason_len', sql`length(${t.reason}) between 1 and 500`),
  ],
);

export const auditLog = pgTable(
  'audit_log',
  {
    id: bigint('id', { mode: 'number' }).primaryKey().generatedAlwaysAsIdentity(),
    at: tstz('at').notNull().defaultNow(),
    /** NULL for the CLI and system actions; actorLabel then says who ("cli:<os user>", "system"). */
    actorAccountId: uuid('actor_account_id'),
    actorLabel: text('actor_label').notNull(),
    action: text('action').notNull(),
    targetType: text('target_type').notNull(),
    targetId: text('target_id').notNull(),
    details: jsonb('details').notNull().default({}),
    ip: text('ip'),
    requestId: text('request_id'),
  },
  (t) => [
    index('audit_log_at_idx').on(t.at),
    index('audit_log_target_idx').on(t.targetType, t.targetId),
  ],
);

// ---------------------------------------------------------------------------------------------
// Official servers, join tokens, game sessions

export const gameServers = pgTable(
  'game_servers',
  {
    /** Slug used in configs and in the X-Gage-Server-Id header, e.g. "eu-1". */
    id: text('id').primaryKey(),
    name: text('name').notNull(),
    region: text('region').notNull(),
    /** IP or DNS name the launcher connects to and the UDP query is sent to. */
    host: text('host').notNull(),
    port: integer('port').notNull(),
    tags: text('tags').array().notNull().default(sql`'{}'::text[]`),
    sort: integer('sort').notNull().default(0),
    visible: boolean('visible').notNull().default(true),
    maintenance: boolean('maintenance').notNull().default(false),
    maintenanceMessage: text('maintenance_message'),
    /** hex SHA-256 of the per-server secret (shown once at creation / rotation). */
    secretHash: text('secret_hash').notNull(),
    /** Normalised IPs the internal API accepts this server's calls from. Empty = none (fail closed). */
    allowedIps: text('allowed_ips').array().notNull().default(sql`'{}'::text[]`),
    createdAt: createdAt(),
    updatedAt: tstz('updated_at').notNull().defaultNow(),
  },
  (t) => [
    check('game_servers_id_slug', sql`${t.id} ~ '^[a-z0-9][a-z0-9-]{1,31}$'`),
    check('game_servers_port', sql`${t.port} between 1 and 65535`),
  ],
);

export const joinTokens = pgTable(
  'join_tokens',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    tokenHash: text('token_hash').notNull(),
    accountId: uuid('account_id')
      .notNull()
      .references(() => accounts.id, { onDelete: 'cascade' }),
    serverId: text('server_id')
      .notNull()
      .references(() => gameServers.id, { onDelete: 'cascade' }),
    createdAt: createdAt(),
    expiresAt: tstz('expires_at').notNull(),
    usedAt: tstz('used_at'),
    ip: text('ip'),
  },
  (t) => [
    uniqueIndex('join_tokens_hash_uq').on(t.tokenHash),
    index('join_tokens_account_idx').on(t.accountId),
    check('join_tokens_expiry_after_create', sql`${t.expiresAt} > ${t.createdAt}`),
  ],
);

export const gameSessions = pgTable(
  'game_sessions',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    accountId: uuid('account_id')
      .notNull()
      .references(() => accounts.id, { onDelete: 'cascade' }),
    serverId: text('server_id')
      .notNull()
      .references(() => gameServers.id, { onDelete: 'cascade' }),
    joinTokenId: uuid('join_token_id').references(() => joinTokens.id, { onDelete: 'set null' }),
    startedAt: tstz('started_at').notNull().defaultNow(),
    endedAt: tstz('ended_at'),
    endReason: text('end_reason'),
  },
  (t) => [
    uniqueIndex('game_sessions_one_open_per_account').on(t.accountId).where(sql`${t.endedAt} is null`),
    index('game_sessions_server_open_idx').on(t.serverId).where(sql`${t.endedAt} is null`),
  ],
);

// ---------------------------------------------------------------------------------------------
// Shop

export const products = pgTable(
  'products',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    sku: text('sku').notNull(),
    name: text('name').notNull(),
    description: text('description').notNull().default(''),
    /** Minor units of `currency` (cents). */
    priceMinor: bigint('price_minor', { mode: 'number' }).notNull(),
    currency: text('currency').notNull(),
    kind: text('kind').notNull(),
    /** Validated by grantPayloadSchema (src/shop/payload.ts). */
    grantPayload: jsonb('grant_payload').notNull(),
    active: boolean('active').notNull().default(true),
    sort: integer('sort').notNull().default(0),
    imageUrl: text('image_url'),
    maxQuantity: integer('max_quantity').notNull().default(1),
    createdAt: createdAt(),
    updatedAt: tstz('updated_at').notNull().defaultNow(),
  },
  (t) => [
    uniqueIndex('products_sku_uq').on(t.sku),
    check('products_sku_format', sql`${t.sku} ~ '^[a-z0-9][a-z0-9_.-]{1,62}$'`),
    check('products_price_positive', sql`${t.priceMinor} > 0 and ${t.priceMinor} <= 100000000`),
    check('products_currency_iso', sql`${t.currency} ~ '^[A-Z]{3}$'`),
    check('products_kind', sql`${t.kind} in ('currency_pack', 'vip', 'cosmetic', 'bundle')`),
    check('products_max_quantity', sql`${t.maxQuantity} between 1 and 100`),
  ],
);

export const orders = pgTable(
  'orders',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    accountId: uuid('account_id')
      .notNull()
      .references(() => accounts.id, { onDelete: 'restrict' }),
    status: text('status').notNull().default('pending'),
    currency: text('currency').notNull(),
    totalMinor: bigint('total_minor', { mode: 'number' }).notNull(),
    provider: text('provider').notNull(),
    providerRef: text('provider_ref'),
    checkoutUrl: text('checkout_url'),
    /** Client-supplied Idempotency-Key: a retried checkout returns this order instead of a new one. */
    idempotencyKey: text('idempotency_key'),
    failureReason: text('failure_reason'),
    createdAt: createdAt(),
    updatedAt: tstz('updated_at').notNull().defaultNow(),
    expiresAt: tstz('expires_at').notNull(),
    paidAt: tstz('paid_at'),
    fulfilledAt: tstz('fulfilled_at'),
    refundedAt: tstz('refunded_at'),
  },
  (t) => [
    // Target of order_items' composite FK so a line can never carry another currency than its order.
    unique('orders_id_currency_uq').on(t.id, t.currency),
    uniqueIndex('orders_idempotency_uq')
      .on(t.accountId, t.idempotencyKey)
      .where(sql`${t.idempotencyKey} is not null`),
    uniqueIndex('orders_provider_ref_uq').on(t.provider, t.providerRef).where(sql`${t.providerRef} is not null`),
    index('orders_account_idx').on(t.accountId, t.createdAt),
    check('orders_status', sql`${t.status} in ('pending', 'paid', 'fulfilled', 'refunded', 'failed', 'chargeback')`),
    check('orders_total_positive', sql`${t.totalMinor} > 0`),
    check('orders_currency_iso', sql`${t.currency} ~ '^[A-Z]{3}$'`),
    check(
      'orders_paid_has_paid_at',
      sql`${t.status} in ('pending', 'failed') or ${t.paidAt} is not null`,
    ),
    check('orders_fulfilled_has_time', sql`${t.status} <> 'fulfilled' or ${t.fulfilledAt} is not null`),
  ],
);

export const orderItems = pgTable(
  'order_items',
  {
    orderId: uuid('order_id').notNull(),
    lineNo: integer('line_no').notNull(),
    productId: uuid('product_id')
      .notNull()
      .references(() => products.id, { onDelete: 'restrict' }),
    // Snapshots: editing or deactivating a product never changes what an existing order delivers.
    sku: text('sku').notNull(),
    name: text('name').notNull(),
    kind: text('kind').notNull(),
    unitPriceMinor: bigint('unit_price_minor', { mode: 'number' }).notNull(),
    currency: text('currency').notNull(),
    quantity: integer('quantity').notNull(),
    grantPayload: jsonb('grant_payload').notNull(),
  },
  (t) => [
    primaryKey({ columns: [t.orderId, t.lineNo] }),
    foreignKey({
      columns: [t.orderId, t.currency],
      foreignColumns: [orders.id, orders.currency],
      name: 'order_items_order_currency_fk',
    }).onDelete('cascade'),
    check('order_items_quantity', sql`${t.quantity} between 1 and 100`),
    check('order_items_price', sql`${t.unitPriceMinor} > 0`),
  ],
);

/** Every verified provider webhook, once. (provider, event_id) is the idempotency key. */
export const paymentEvents = pgTable(
  'payment_events',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    provider: text('provider').notNull(),
    eventId: text('event_id').notNull(),
    type: text('type').notNull(),
    orderId: uuid('order_id'),
    /** applied | ignored (state did not allow it) | unmatched (no such order) | rejected (e.g. amount mismatch) */
    result: text('result').notNull(),
    detail: text('detail'),
    payload: jsonb('payload').notNull(),
    receivedAt: tstz('received_at').notNull().defaultNow(),
  },
  (t) => [
    unique('payment_events_provider_event_uq').on(t.provider, t.eventId),
    index('payment_events_order_idx').on(t.orderId),
    check('payment_events_result', sql`${t.result} in ('applied', 'ignored', 'unmatched', 'rejected')`),
  ],
);

export const grants = pgTable(
  'grants',
  {
    id: uuid('id').primaryKey().defaultRandom(),
    /** Global delivery order. The game server applies pending entries in seq order. */
    seq: bigint('seq', { mode: 'number' }).generatedAlwaysAsIdentity().notNull(),
    accountId: uuid('account_id')
      .notNull()
      .references(() => accounts.id, { onDelete: 'restrict' }),
    orderId: uuid('order_id')
      .notNull()
      .references(() => orders.id, { onDelete: 'restrict' }),
    lineNo: integer('line_no').notNull(),
    entryNo: integer('entry_no').notNull(),
    kind: text('kind').notNull(),
    /** For a revoke: the grant it takes back. */
    revokesGrantId: uuid('revokes_grant_id'),
    /** wallet_credit (applied by the platform) | entitlement | item (delivered to the game server) */
    type: text('type').notNull(),
    sku: text('sku').notNull(),
    payload: jsonb('payload').notNull(),
    quantity: integer('quantity').notNull().default(1),
    status: text('status').notNull().default('pending'),
    createdAt: createdAt(),
    ackedAt: tstz('acked_at'),
    /** The game server id, or "platform" for entries the platform applied itself (wallet). */
    ackedBy: text('acked_by'),
    /**
     * The game server this entry is being delivered by: set (or moved) when a server with an open
     * session of the account pulls it. Only that server's ack counts (grants_ack_matches_binding).
     * No foreign key: it must survive as the record of who acked after a server row is deleted.
     */
    deliveringServer: text('delivering_server'),
  },
  (t) => [
    uniqueIndex('grants_seq_uq').on(t.seq),
    unique('grants_entry_uq').on(t.orderId, t.lineNo, t.entryNo, t.kind),
    uniqueIndex('grants_one_revoke_per_grant').on(t.revokesGrantId).where(sql`${t.revokesGrantId} is not null`),
    index('grants_pending_idx').on(t.accountId, t.seq).where(sql`${t.status} = 'pending'`),
    index('grants_order_idx').on(t.orderId),
    index('grants_delivering_idx').on(t.deliveringServer, t.accountId).where(sql`${t.status} = 'pending' and ${t.deliveringServer} is not null`),
    foreignKey({ columns: [t.revokesGrantId], foreignColumns: [t.id], name: 'grants_revokes_fk' }),
    check('grants_kind', sql`${t.kind} in ('grant', 'revoke')`),
    check('grants_revoke_link', sql`(${t.kind} = 'revoke') = (${t.revokesGrantId} is not null)`),
    check('grants_type', sql`${t.type} in ('wallet_credit', 'entitlement', 'item')`),
    check('grants_status', sql`${t.status} in ('pending', 'acked')`),
    check('grants_acked_has_time', sql`(${t.status} = 'acked') = (${t.ackedAt} is not null)`),
    // A game server's ack is valid only for the entry it is delivering; the platform's own acks (wallet credits) are exempt.
    check('grants_ack_matches_binding', sql`${t.status} = 'pending' or ${t.ackedBy} = 'platform' or coalesce(${t.ackedBy} = ${t.deliveringServer}, false)`),
    check('grants_quantity', sql`${t.quantity} between 1 and 100`),
  ],
);

/** Append-only (trigger). The balance of an account is the sum of its deltas. */
export const walletLedger = pgTable(
  'wallet_ledger',
  {
    id: bigint('id', { mode: 'number' }).primaryKey().generatedAlwaysAsIdentity(),
    accountId: uuid('account_id')
      .notNull()
      .references(() => accounts.id, { onDelete: 'restrict' }),
    currency: text('currency').notNull(),
    /** Signed. Credits are positive, spends and revokes negative. */
    delta: bigint('delta', { mode: 'number' }).notNull(),
    reason: text('reason').notNull(),
    /** Unique: replaying the same operation (webhook, retry, game server resend) is a no-op. */
    idempotencyKey: text('idempotency_key').notNull(),
    grantId: uuid('grant_id').references(() => grants.id, { onDelete: 'restrict' }),
    serverId: text('server_id'),
    memo: text('memo'),
    createdAt: createdAt(),
  },
  (t) => [
    uniqueIndex('wallet_ledger_idem_uq').on(t.idempotencyKey),
    index('wallet_ledger_account_idx').on(t.accountId, t.currency, t.id),
    check('wallet_ledger_delta_nonzero', sql`${t.delta} <> 0`),
    check('wallet_ledger_currency', sql`${t.currency} ~ '^[a-z][a-z0-9_]{1,15}$'`),
    check('wallet_ledger_reason', sql`${t.reason} in ('purchase', 'revoke', 'spend')`),
    check(
      'wallet_ledger_sign',
      sql`(${t.reason} = 'purchase' and ${t.delta} > 0) or (${t.reason} in ('revoke', 'spend') and ${t.delta} < 0)`,
    ),
  ],
);

/** Derived. Only the wallet_ledger trigger may write it (a guard trigger rejects everything else). */
export const walletBalances = pgTable(
  'wallet_balances',
  {
    accountId: uuid('account_id')
      .notNull()
      .references(() => accounts.id, { onDelete: 'restrict' }),
    currency: text('currency').notNull(),
    balance: bigint('balance', { mode: 'number' }).notNull(),
    updatedAt: tstz('updated_at').notNull().defaultNow(),
  },
  (t) => [primaryKey({ columns: [t.accountId, t.currency] })],
);

// ---------------------------------------------------------------------------------------------
// Settings

export const accountSettings = pgTable(
  'account_settings',
  {
    accountId: uuid('account_id')
      .primaryKey()
      .references(() => accounts.id, { onDelete: 'cascade' }),
    data: jsonb('data').notNull(),
    version: integer('version').notNull().default(1),
    updatedAt: tstz('updated_at').notNull().defaultNow(),
  },
  (t) => [check('account_settings_object', sql`jsonb_typeof(${t.data}) = 'object'`)],
);
