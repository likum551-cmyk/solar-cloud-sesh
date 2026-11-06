import { z } from 'zod';
import type { Deps } from '../../deps.js';
import { createJoinToken, joinSchema } from '../../game/join.js';
import { listPublicServers } from '../../servers/service.js';
import { getSettings, putSettings, putSettingsSchema } from '../../settings.js';
import {
  checkout,
  checkoutSchema,
  getBalances,
  getOrder,
  idempotencyKeySchema,
  ledgerQuerySchema,
  listLedger,
  listOrders,
  listOwned,
  listProducts,
} from '../../shop/service.js';
import { claimQuest, listQuests, QUEST_SLUG_PATTERN, QUEST_VISIT_MIN_SECONDS, startQuest } from '../../quests/service.js';
import { ApiError } from '../../util/errors.js';
import { requireSession } from '../helpers.js';
import { uuidParam, type Ctx, type Router } from '../router.js';

/** A quest slug path parameter, or 400. */
function slugParam(c: Ctx): string {
  const v = c.req.param('slug');
  if (v === undefined || !QUEST_SLUG_PATTERN.test(v)) throw new ApiError(400, 'invalid_request', 'Path parameter "slug" must be a quest slug.');
  return v;
}

/** Routes a signed-in player (or the launcher before login) uses: settings, wallet, quests, server list, join, shop. */
export function registerPlayerRoutes(r: Router, deps: Deps): void {
  const { db, config } = deps;

  r.add(
    {
      method: 'get', path: '/v1/me/settings', summary: 'Read the account\'s client settings', auth: 'session', tags: ['settings'],
      response: '200 { settings: object, version: number, updatedAt: string|null }',
    },
    async (c) => c.json(await getSettings(db, requireSession(c).account.id)),
  );

  r.add(
    {
      method: 'put', path: '/v1/me/settings', summary: 'Replace the account\'s client settings', auth: 'session', tags: ['settings'], body: putSettingsSchema,
      description: `A JSON object (at most SETTINGS_MAX_BYTES, default 16384, 6 levels deep, keys [A-Za-z0-9_.:-]{1,64}). With expectedVersion the write fails with 409 if another device wrote first.`,
      response: '200 { settings, version, updatedAt }', errors: ['invalid_settings', 'version_conflict'],
      maxBodyBytes: Math.max(config.settingsMaxBytes + 1024, 4096),
      limits: [{ name: 'settings-write', scope: 'account', limit: 120, windowSeconds: 600 }],
    },
    async (c, { body }) => c.json(await putSettings(db, requireSession(c).account.id, body, config.settingsMaxBytes, deps.clock())),
  );

  r.add(
    { method: 'get', path: '/v1/me/wallet', summary: 'In-game currency balances', auth: 'session', tags: ['wallet'], response: '200 { balances: [{ currency, balance }] }' },
    async (c) => c.json({ balances: await getBalances(db, requireSession(c).account.id) }),
  );

  r.add(
    {
      method: 'get', path: '/v1/me/wallet/ledger', summary: 'Wallet history, newest first', auth: 'session', tags: ['wallet'], query: ledgerQuerySchema,
      response: '200 { entries: [{ id, currency, delta, reason, memo, createdAt }] } (page with ?before=<last id>)',
    },
    async (c, { query }) => c.json({ entries: await listLedger(db, requireSession(c).account.id, query) }),
  );

  r.add(
    {
      method: 'get', path: '/v1/me/quests', summary: 'Quests that pay coins, with this account\'s status', auth: 'session', tags: ['quests'],
      description: `Active quests in display order. status: available (a visit quest not started yet), started (waiting: waitSeconds left), claimable, claimed, locked (lockReason discord_required = link Discord first; discord_already_claimed = this Discord account already claimed it on another Solar RP account; not_available = cannot be completed yet). A visit quest is claimable ${QUEST_VISIT_MIN_SECONDS} s after its start and only with a linked Discord account; each quest pays once per account and once per Discord account.`,
      response: '200 { discordLinked, visitMinSeconds, quests: [{ slug, kind, title, description, url, reward: { currency, amount }, sort, status, lockReason, startedAt, claimableAt, waitSeconds, claimedAt }] }',
    },
    async (c) => c.json(await listQuests(deps, requireSession(c).account.id)),
  );

  r.add(
    {
      method: 'post', path: '/v1/me/quests/:slug/start', summary: 'Start a visit quest (then open its url)', auth: 'session', tags: ['quests'],
      description: `Records when the quest was started; the claim opens ${QUEST_VISIT_MIN_SECONDS} s later. Idempotent: a repeated start keeps the first time. Returns the quest with its url for the client to open.`,
      response: '200 { quest }', errors: ['quest_not_found', 'quest_inactive', 'not_a_visit_quest'],
      limits: [{ name: 'quest-start', scope: 'account', limit: 60, windowSeconds: 600 }],
    },
    async (c) => c.json({ quest: await startQuest(deps, requireSession(c).account.id, slugParam(c)) }),
  );

  r.add(
    {
      method: 'post', path: '/v1/me/quests/:slug/claim', summary: 'Claim a quest\'s reward into the wallet', auth: 'session', tags: ['quests'],
      description: 'Pays the reward once (ledger reason quest). The credit and the claim are one transaction, and the database refuses a second credit for the same account or Discord account, so concurrent claims pay once.',
      response: '200 { quest, credited: { currency, amount }, balance }',
      errors: ['quest_not_found', 'quest_inactive', 'already_claimed', 'quest_unavailable', 'quest_not_started', 'discord_required', 'discord_already_claimed', 'quest_wait', 'account_banned'],
      limits: [{ name: 'quest-claim', scope: 'account', limit: 30, windowSeconds: 600 }],
    },
    async (c) => c.json(await claimQuest(deps, requireSession(c).account.id, slugParam(c))),
  );

  r.add(
    { method: 'get', path: '/v1/me/items', summary: 'Items and entitlements the account owns', auth: 'session', tags: ['shop'], response: '200 { items: [{ id, sku, type, payload, quantity, orderId, delivered, createdAt }] }' },
    async (c) => c.json({ items: await listOwned(db, requireSession(c).account.id) }),
  );

  r.add(
    {
      method: 'get', path: '/v1/servers', summary: 'The official servers with live player counts', auth: 'none', tags: ['servers'],
      description: 'Live state comes from the GAGE UDP query (apps/master/PROTOCOL.md), cached about 5 s per server. The request never waits on a slow server: a server not probed yet may report state "unknown".',
      response: '200 { servers: [{ id, name, region, host, port, tags, sort, maintenance: { enabled, message }, status: { state, players, maxPlayers, pingMs, protocol, checkedAt } }] }',
    },
    async (c) => c.json({ servers: await listPublicServers(deps) }),
  );

  r.add(
    {
      method: 'post', path: '/v1/game/join', summary: 'Get a single-use join token for a server', auth: 'session', tags: ['game'], body: joinSchema,
      description: 'The token is valid JOIN_TOKEN_TTL_SECONDS (default 60), bound to this account and server, and consumed by the game server\'s POST /internal/game/verify.',
      response: '200 { token, expiresAt, server: { id, name, host, port } }',
      errors: ['email_not_verified', 'account_banned', 'server_not_found', 'server_maintenance'],
    },
    async (c, { body }) => {
      const s = requireSession(c);
      return c.json(await createJoinToken(deps, s.account, s.roles, body.serverId, c.get('clientIp')));
    },
  );

  r.add(
    { method: 'get', path: '/v1/shop/products', summary: 'Products on sale', auth: 'none', tags: ['shop'], response: '200 { products: [{ id, sku, name, description, priceMinor, currency, kind, grants, sort, imageUrl, maxQuantity }] }' },
    async (c) => c.json({ products: await listProducts(db, { activeOnly: true }) }),
  );

  r.add(
    {
      method: 'post', path: '/v1/shop/checkout', summary: 'Create an order and a checkout at the payment provider', auth: 'session', tags: ['shop'], body: checkoutSchema,
      description: 'Send an Idempotency-Key header (8-64 chars of [A-Za-z0-9_-]) so a retry returns the same order. Open checkoutUrl in the browser; the order becomes paid when the provider\'s webhook arrives.',
      response: '201 { order }', successStatus: 201,
      errors: ['email_not_verified', 'account_banned', 'product_not_found', 'product_inactive', 'quantity_exceeds_limit', 'mixed_currency', 'idempotency_mismatch', 'payment_unavailable'],
      limits: [{ name: 'checkout-ip', scope: 'ip', limit: 60, windowSeconds: 3600 }],
    },
    async (c, { body }) => {
      const s = requireSession(c);
      const rawKey = c.req.header('idempotency-key');
      let key: string | null = null;
      if (rawKey !== undefined) {
        const parsed = idempotencyKeySchema.safeParse(rawKey);
        if (!parsed.success) throw new ApiError(400, 'invalid_request', 'Idempotency-Key must be 8-64 characters of A-Z a-z 0-9 _ -.');
        key = parsed.data;
      }
      return c.json({ order: await checkout(deps, s.account, body, key) }, 201);
    },
  );

  r.add(
    {
      method: 'get', path: '/v1/shop/orders', summary: 'The account\'s orders, newest first', auth: 'session', tags: ['shop'],
      query: z.object({ limit: z.coerce.number().int().min(1).max(100).default(50) }),
      response: '200 { orders: [order] }',
    },
    async (c, { query }) => c.json({ orders: await listOrders(deps, requireSession(c).account.id, query.limit) }),
  );

  r.add(
    { method: 'get', path: '/v1/shop/orders/:id', summary: 'One of the account\'s orders (poll it after checkout)', auth: 'session', tags: ['shop'], response: '200 { order }', errors: ['order_not_found'] },
    async (c) => c.json({ order: await getOrder(deps, uuidParam(c, 'id'), requireSession(c).account.id) }),
  );
}
