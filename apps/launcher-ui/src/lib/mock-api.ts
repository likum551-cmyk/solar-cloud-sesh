import type { FetchLike } from './api';
import { ROUTES } from './api';
import type { Account, Order, Product, Server, SessionRow } from './schemas';
import { GAGE_SERVER_SEEDS, type GageServerSeed } from './servers';

// A fake platform API behind a fetch-compatible function, so mock mode runs the real API client,
// the real zod validation and the real error mapping. Everything here is DEMONSTRATION content:
// the UI marks it DEMO wherever it shows numbers or prices, and it never runs inside SolarLauncher.exe.

export interface MockScenario {
  /** Server list: ok, empty, one server in maintenance, every server offline. */
  servers: 'ok' | 'empty' | 'maintenance' | 'down';
  /** Shop catalogue: ok or empty. */
  shop: 'ok' | 'empty';
  /** Account flavour returned on login. */
  account: 'ok' | 'unverified' | 'banned';
  /** offline = every request fails at the network layer. */
  net: 'ok' | 'offline' | 'slow';
}

export const DEFAULT_SCENARIO: MockScenario = { servers: 'ok', shop: 'ok', account: 'ok', net: 'ok' };

const now = () => new Date();
const iso = (d: Date) => d.toISOString();
// Demo records are dated from one instant, taken on first use: re-dating them on every request
// moved the VIP end between two reads, so Orders and Profile showed different time left.
let anchor: number | null = null;
const t0 = () => (anchor ??= Date.now());
const daysAgo = (n: number) => iso(new Date(t0() - n * 86_400_000));
const daysAhead = (n: number) => iso(new Date(t0() + n * 86_400_000));

function demoAccount(s: MockScenario): Account {
  return {
    id: '7d0c8a2e-1b9f-4c55-9a5e-2f1d6c3b9e10',
    username: 'nightshift',
    email: 'nightshift@example.com',
    emailVerified: s.account !== 'unverified',
    roles: ['player', 'mod'],
    hasPassword: true,
    discord: { id: '412233445566778899', username: 'nightshift' },
    ban: s.account === 'banned' ? { id: 'b1', reason: 'Demo ban: combat logging', createdAt: daysAgo(1), expiresAt: daysAhead(6) } : null,
    createdAt: daysAgo(214),
  };
}

// The real GAGE lineup (lib/servers.ts) rendered as a server list. The one live server shows a
// demo count/ping (labelled "Demo counts" in the UI — real numbers need the platform/master, or a
// native status bridge); the coming-soon servers carry no status and are never joinable.
function demoServers(s: MockScenario): Server[] {
  if (s.servers === 'empty') return [];
  const checkedAt = iso(now());
  const live = () => ({ state: 'online' as const, players: 128, maxPlayers: 300, pingMs: 24, protocol: 3, checkedAt });
  const soon = () => ({ state: 'unknown' as const, players: null, maxPlayers: null, pingMs: null, protocol: null, checkedAt });
  const off = { state: 'offline' as const, players: null, maxPlayers: null, pingMs: null, protocol: null, checkedAt, detail: 'timeout' };
  const toServer = (seed: GageServerSeed, i: number): Server => ({
    id: seed.id, name: seed.name, region: seed.region, host: seed.host, port: seed.port,
    tags: seed.tags, sort: i + 1, comingSoon: seed.comingSoon,
    maintenance: { enabled: false, message: null },
    status: seed.comingSoon ? soon() : live(),
  });
  const list = GAGE_SERVER_SEEDS.map(toServer);
  if (s.servers === 'maintenance') {
    const m = list.find((x) => !x.comingSoon);
    if (m) m.maintenance = { enabled: true, message: 'Economy update rolling out. Back around 21:00 CET.' };
  }
  if (s.servers === 'down') for (const sv of list) if (!sv.comingSoon) sv.status = off;
  return list;
}

function demoProducts(s: MockScenario): Product[] {
  if (s.shop === 'empty') return [];
  return [
    { id: 'p1', sku: 'vehicle.vanta-gt', name: 'Vanta GT', description: 'Two-door concept coupe, delivered to your garage on every roleplay server.', priceMinor: 1999, currency: 'EUR', kind: 'cosmetic', grants: [{ type: 'item', key: 'vehicle.vanta_gt', quantity: 1 }], sort: 1, imageUrl: null, maxQuantity: 1 },
    { id: 'p2', sku: 'vip.30d', name: 'VIP · 30 days', description: 'Priority queue and a second character slot for 30 days. Stacks with time you already have.', priceMinor: 799, currency: 'EUR', kind: 'vip', grants: [{ type: 'entitlement', key: 'vip', durationDays: 30 }], sort: 2, imageUrl: null, maxQuantity: 3 },
    { id: 'p3', sku: 'outfit.field-jacket', name: 'Field Jacket Outfit', description: 'A canvas work jacket over a plain white T-shirt, with dark jeans, for your character.', priceMinor: 499, currency: 'EUR', kind: 'cosmetic', grants: [{ type: 'item', key: 'outfit.field_jacket', quantity: 1 }], sort: 3, imageUrl: null, maxQuantity: 1 },
    { id: 'p4', sku: 'cosmetic.glow-trim', name: 'Glow Trim Kit', description: 'Light-strip trim for your bike and riding suit, in cyan or orange.', priceMinor: 299, currency: 'EUR', kind: 'cosmetic', grants: [{ type: 'item', key: 'cosmetic.glow_trim', quantity: 1 }], sort: 4, imageUrl: null, maxQuantity: 1 },
    { id: 'p5', sku: 'coins.5000', name: '5 000 Solar RP Coins', description: 'In-game currency, credited to your wallet as soon as the payment clears.', priceMinor: 999, currency: 'EUR', kind: 'currency_pack', grants: [{ type: 'wallet_credit', currency: 'coins', amount: 5000 }], sort: 5, imageUrl: null, maxQuantity: 10 },
  ];
}

function demoOrders(): Order[] {
  const base = { provider: 'demo', failureReason: null, refundedAt: null } as const;
  const line = (sku: string, name: string, kind: string, unitPriceMinor: number) => [{ lineNo: 1, sku, name, kind, quantity: 1, unitPriceMinor }];
  return [
    { ...base, id: 'o-1041', status: 'pending', currency: 'EUR', totalMinor: 299, checkoutUrl: 'https://pay.example.com/demo/o-1041', createdAt: daysAgo(0.01), expiresAt: daysAhead(0.02), paidAt: null, fulfilledAt: null, items: line('cosmetic.glow-trim', 'Glow Trim Kit', 'cosmetic', 299) },
    { ...base, id: 'o-1038', status: 'fulfilled', currency: 'EUR', totalMinor: 799, checkoutUrl: null, createdAt: daysAgo(13), expiresAt: daysAgo(12.9), paidAt: daysAgo(13), fulfilledAt: daysAgo(13), items: line('vip.30d', 'VIP · 30 days', 'vip', 799) },
    { ...base, id: 'o-1040', status: 'paid', currency: 'EUR', totalMinor: 999, checkoutUrl: null, createdAt: daysAgo(0.2), expiresAt: daysAgo(0.1), paidAt: daysAgo(0.19), fulfilledAt: null, items: line('coins.5000', '5 000 Solar RP Coins', 'currency_pack', 999) },
    { ...base, id: 'o-1027', status: 'refunded', currency: 'EUR', totalMinor: 999, checkoutUrl: null, createdAt: daysAgo(40), expiresAt: daysAgo(39.9), paidAt: daysAgo(40), fulfilledAt: null, refundedAt: daysAgo(38), items: line('coins.5000', '5 000 Solar RP Coins', 'currency_pack', 999) },
    { ...base, id: 'o-1019', status: 'failed', currency: 'EUR', totalMinor: 1999, checkoutUrl: null, failureReason: 'Card declined by the issuer.', createdAt: daysAgo(52), expiresAt: daysAgo(51.9), paidAt: null, fulfilledAt: null, items: line('vehicle.vanta-gt', 'Vanta GT', 'cosmetic', 1999) },
    // The order behind the owned Field Jacket (owned item g2), so the record and the sheet agree.
    { ...base, id: 'o-0990', status: 'fulfilled', currency: 'EUR', totalMinor: 499, checkoutUrl: null, createdAt: daysAgo(70), expiresAt: daysAgo(69.9), paidAt: daysAgo(70), fulfilledAt: daysAgo(70), items: line('outfit.field-jacket', 'Field Jacket Outfit', 'cosmetic', 499) },
  ];
}

function demoSessions(): SessionRow[] {
  return [
    { id: 's-cur', deviceName: 'Solar Launcher on DESKTOP-NIGHT', userAgent: 'SolarLauncher/0.2.0', ip: '203.0.113.24', createdAt: daysAgo(3), lastSeenAt: iso(now()), expiresAt: daysAhead(27), current: true },
    { id: 's-lap', deviceName: 'Solar Launcher on LAPTOP-7', userAgent: 'SolarLauncher/0.2.0', ip: '198.51.100.7', createdAt: daysAgo(19), lastSeenAt: daysAgo(2), expiresAt: daysAhead(11), current: false },
    { id: 's-old', deviceName: null, userAgent: 'SolarLauncher/0.1.29', ip: '192.0.2.200', createdAt: daysAgo(28), lastSeenAt: daysAgo(26), expiresAt: daysAhead(2), current: false },
  ];
}

const json = (status: number, body: unknown, headers: Record<string, string> = {}) =>
  new Response(status === 204 ? null : JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json', ...headers } });
const err = (status: number, code: string, message: string, headers: Record<string, string> = {}) => json(status, { error: { code, message, requestId: 'demo' } }, headers);

/** The mock network. `scenario` is read on every request so tests and captures can flip it. */
export function createMockFetch(getScenario: () => MockScenario = () => DEFAULT_SCENARIO): FetchLike {
  let sessions = demoSessions();
  let discordPolls = 0;
  const token = 'demo-session-token-0123456789abcdef';

  return async (input, init) => {
    const s = getScenario();
    if (s.net === 'offline') throw new TypeError('Failed to fetch');
    await new Promise((r) => setTimeout(r, s.net === 'slow' ? 1800 : 180));
    const url = new URL(input.replace(/^mock:\/\/[^/]+/, 'https://mock.invalid'));
    const path = url.pathname;
    const method = (init.method ?? 'GET').toUpperCase();
    const body = typeof init.body === 'string' ? (JSON.parse(init.body) as Record<string, unknown>) : {};
    const authed = new Headers(init.headers).get('Authorization') === `Bearer ${token}`;
    const needAuth = () => (authed ? null : err(401, 'unauthorized', 'Session expired or revoked.'));

    switch (`${method} ${path}`) {
      case `POST ${ROUTES.login}`: {
        const id = String(body.identifier ?? '');
        if (id.startsWith('locked')) return err(429, 'rate_limited', 'Too many failed logins for this account.', { 'Retry-After': '540' });
        if (String(body.password ?? '') === 'wrong') {
          return err(401, 'invalid_credentials', 'Email/username or password is wrong.');
        }
        return json(200, { token, expiresAt: daysAhead(30), sessionId: 's-cur', account: demoAccount(s) });
      }
      case `POST ${ROUTES.register}`: {
        if (String(body.username ?? '').toLowerCase() === 'taken') return err(409, 'username_taken', 'That username is taken.');
        if (String(body.password ?? '').length < 10) return err(400, 'weak_password', 'Use at least 10 characters.', {});
        return json(201, { verificationRequired: true });
      }
      case `POST ${ROUTES.forgotPassword}`: return json(202, { sent: true });
      case `POST ${ROUTES.resetPassword}`: return String(body.token ?? '').startsWith('bad') ? err(400, 'invalid_token', 'Invalid or expired token.') : json(200, { ok: true });
      case `POST ${ROUTES.verifyEmail}`: return json(200, { account: { ...demoAccount(s), emailVerified: true } });
      case `POST ${ROUTES.resendVerification}`: return needAuth() ?? json(202, { sent: true });
      case `POST ${ROUTES.logout}`: return needAuth() ?? json(204, null);
      case `POST ${ROUTES.discordStart}`:
      case `POST ${ROUTES.discordLink}`:
        discordPolls = 0;
        return json(200, { flowId: '3f1e2d4c-5b6a-4789-8abc-def012345678', authorizeUrl: 'https://discord.com/oauth2/authorize?client_id=demo', pollSecret: 'demo-poll-secret-0123456789', expiresAt: daysAhead(0.01) });
      case `POST ${ROUTES.discordPoll}`:
        discordPolls++;
        return discordPolls < 3 ? json(200, { status: 'pending' }) : json(200, { status: 'complete', token, expiresAt: daysAhead(30), sessionId: 's-cur', account: demoAccount(s) });
      case `POST ${ROUTES.discordComplete}`: return json(200, { token, expiresAt: daysAhead(30), sessionId: 's-cur', account: demoAccount(s) });
      case `DELETE ${ROUTES.discordUnlink}`: return needAuth() ?? json(204, null);
      case `GET ${ROUTES.me}`: return needAuth() ?? json(200, { account: demoAccount(s) });
      case `POST ${ROUTES.password}`:
        if (!authed) return err(401, 'unauthorized', 'Session expired.');
        if (String(body.currentPassword ?? '') === 'wrong') return err(401, 'invalid_credentials', 'Current password is wrong.');
        return json(200, { revokedSessions: sessions.filter((x) => !x.current).length });
      case `GET ${ROUTES.sessions}`: return needAuth() ?? json(200, { sessions });
      case `DELETE ${ROUTES.sessions}`: {
        const n = sessions.filter((x) => !x.current).length;
        sessions = sessions.filter((x) => x.current);
        return needAuth() ?? json(200, { revoked: n });
      }
      case `GET ${ROUTES.settings}`: return needAuth() ?? json(200, { settings: {}, version: 0, updatedAt: null });
      case `GET ${ROUTES.wallet}`: return needAuth() ?? json(200, { balances: [{ currency: 'coins', balance: 12_450 }] });
      case `GET ${ROUTES.items}`: return needAuth() ?? json(200, {
        items: [
          { id: 'g1', sku: 'vip.30d', type: 'entitlement', payload: { type: 'entitlement', key: 'vip', durationDays: 30 }, quantity: 1, orderId: 'o-1038', delivered: true, createdAt: daysAgo(13) },
          { id: 'g2', sku: 'outfit.field-jacket', type: 'item', payload: { type: 'item', key: 'outfit.field_jacket', quantity: 1 }, quantity: 1, orderId: 'o-0990', delivered: true, createdAt: daysAgo(70) },
        ],
      });
      case `GET ${ROUTES.servers}`: return json(200, { servers: demoServers(s) });
      case `POST ${ROUTES.join}`: {
        if (!authed) return err(401, 'unauthorized', 'Session expired.');
        if (s.account === 'unverified') return err(403, 'email_not_verified', 'Verify your email before joining.');
        const sv = demoServers(s).find((x) => x.id === body.serverId);
        if (!sv) return err(404, 'server_not_found', 'No such server.');
        if (sv.comingSoon) return err(503, 'server_unavailable', 'This server is not live yet.');
        if (sv.maintenance.enabled) return err(503, 'server_maintenance', sv.maintenance.message ?? 'Maintenance.');
        return json(200, { token: 'demo-join-token-abcdef', expiresAt: daysAhead(60 / 86_400), server: { id: sv.id, name: sv.name, host: sv.host, port: sv.port } });
      }
      case `GET ${ROUTES.products}`: return json(200, { products: demoProducts(s) });
      case `POST ${ROUTES.checkout}`: {
        if (!authed) return err(401, 'unauthorized', 'Session expired.');
        const items = (body.items as Array<{ sku: string; quantity: number }> | undefined) ?? [];
        const p = demoProducts(s).find((x) => x.sku === items[0]?.sku);
        if (!p) return err(404, 'product_not_found', 'No such product.');
        return json(201, { order: { id: 'o-demo', status: 'pending', currency: p.currency, totalMinor: p.priceMinor, provider: 'demo', checkoutUrl: 'https://pay.example.com/demo/o-demo', failureReason: null, createdAt: iso(now()), expiresAt: daysAhead(0.02), paidAt: null, fulfilledAt: null, refundedAt: null, items: [{ lineNo: 1, sku: p.sku, name: p.name, kind: p.kind, quantity: 1, unitPriceMinor: p.priceMinor }] } });
      }
      case `GET ${ROUTES.orders}`: return needAuth() ?? json(200, { orders: demoOrders() });
    }
    if (method === 'DELETE' && path.startsWith(`${ROUTES.sessions}/`)) {
      const id = decodeURIComponent(path.slice(ROUTES.sessions.length + 1));
      if (!sessions.some((x) => x.id === id)) return err(404, 'session_not_found', 'No such session.');
      sessions = sessions.filter((x) => x.id !== id);
      return needAuth() ?? json(204, null);
    }
    return err(404, 'not_found', 'No such route.');
  };
}

/** Read a scenario from URL query params (?servers=maintenance&shop=empty&account=unverified&net=offline). */
export function scenarioFromQuery(search: string): MockScenario {
  const q = new URLSearchParams(search);
  const pick = <T extends string>(key: string, allowed: readonly T[], dflt: T): T => {
    const v = q.get(key);
    return (allowed as readonly string[]).includes(v ?? '') ? (v as T) : dflt;
  };
  return {
    servers: pick('servers', ['ok', 'empty', 'maintenance', 'down'] as const, 'ok'),
    shop: pick('shop', ['ok', 'empty'] as const, 'ok'),
    account: pick('account', ['ok', 'unverified', 'banned'] as const, 'ok'),
    net: pick('net', ['ok', 'offline', 'slow'] as const, 'ok'),
  };
}
