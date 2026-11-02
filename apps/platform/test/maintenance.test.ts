import { eq } from 'drizzle-orm';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { schema } from '../src/db/index.js';
import { runMaintenance, startMaintenance } from '../src/maintenance.js';
import { createTestEnv, freshIp, joinServer, latestMail, makeProduct, makeServer, makeStaff, makeUser, sendWebhook, type TestEnv } from './helpers.js';

let env: TestEnv;
const DAY = 86_400;

beforeAll(async () => {
  env = await createTestEnv();
});
afterAll(async () => {
  await env.close();
});

describe('housekeeping', () => {
  it('purges what can never matter again and leaves what still can, never touching money or the audit log', async () => {
    const admin = await makeStaff(env, 'admin');
    const srv = await makeServer(env, admin);
    const keeper = await makeUser(env);
    const goner = await makeUser(env);
    await env.call('/v1/auth/logout', { method: 'POST', token: goner.token });

    // Money and audit rows that must survive any amount of waiting.
    const product = await makeProduct(env, admin, { sku: 'mt-coins', kind: 'currency_pack', priceMinor: 100, grants: [{ type: 'wallet_credit', currency: 'coins', amount: 10 }] });
    const co = await env.call<{ order: { id: string; totalMinor: number; currency: string } }>('/v1/shop/checkout', { token: keeper.token, json: { items: [{ sku: product.sku, quantity: 1 }] }, headers: { 'idempotency-key': 'maintenance-key-1' }, ip: freshIp() });
    await sendWebhook(env, { type: 'payment.succeeded', orderId: co.body.order.id, amountMinor: co.body.order.totalMinor, currency: co.body.order.currency });
    const money = async () => ({
      orders: (await env.handle.db.select().from(schema.orders)).length,
      ledger: (await env.handle.db.select().from(schema.walletLedger)).length,
      grants: (await env.handle.db.select().from(schema.grants)).length,
      events: (await env.handle.db.select().from(schema.paymentEvents)).length,
      audit: (await env.handle.db.select().from(schema.auditLog)).length,
    });
    const moneyBefore = await money();
    expect(moneyBefore.orders).toBeGreaterThan(0);

    // Rows of every purgeable kind: an unused join token, an old verification token, rate-limit windows, a throttle row, a stale game session.
    const join = await env.call<{ token: string }>('/v1/game/join', { token: keeper.token, json: { serverId: srv.id } });
    expect(join.status).toBe(200);
    await joinServer(env, await makeUser(env), srv);
    for (let i = 0; i < 3; i++) await env.call('/v1/auth/login', { json: { identifier: goner.email, password: 'definitely-wrong-password' }, ip: freshIp() });
    expect(await latestMail(env, keeper.email, 'verify_email')).toBeDefined();
    const before = {
      sessions: (await env.handle.db.select().from(schema.sessions)).length,
      joinTokens: (await env.handle.db.select().from(schema.joinTokens)).length,
      throttles: (await env.handle.db.select().from(schema.authThrottle)).length,
      rateLimits: (await env.handle.db.select().from(schema.rateLimits)).length,
    };
    expect(before.throttles).toBeGreaterThan(0);
    expect(before.rateLimits).toBeGreaterThan(0);

    // Nothing is old enough yet: the first pass purges none of these.
    const first = await runMaintenance(env.deps);
    expect(first).toMatchObject({ sessions: 0, joinTokens: 0, staleGameSessions: 0, oauthFlows: 0 });

    // Two days on: day-old windows, throttles and join tokens go, and the 24 h game session is closed as stale.
    env.clock.advanceSeconds(2 * DAY);
    const second = await runMaintenance(env.deps);
    expect(second.rateLimits).toBeGreaterThan(0);
    expect(second.joinTokens).toBeGreaterThan(0);
    expect(second.staleGameSessions).toBe(1); // GAME_SESSION_MAX_HOURS = 24
    expect(second.throttles).toBeGreaterThan(0);

    // Past the week: used and expired auth tokens, the revoked session and the expired sessions go.
    env.clock.advanceSeconds(8 * DAY);
    const third = await runMaintenance(env.deps);
    expect(third.authTokens).toBeGreaterThan(0);
    expect(third.sessions).toBeGreaterThanOrEqual(1);
    const sessionsLeft = await env.handle.db.select().from(schema.sessions).where(eq(schema.sessions.accountId, goner.accountId));
    expect(sessionsLeft).toHaveLength(0);

    expect(await money()).toEqual(moneyBefore);
  });

  it('is idempotent: a second pass right after the first finds nothing to do', async () => {
    env.clock.advanceSeconds(40 * DAY);
    await runMaintenance(env.deps);
    const again = await runMaintenance(env.deps);
    expect(again).toEqual({ sessions: 0, authTokens: 0, rateLimits: 0, throttles: 0, oauthFlows: 0, joinTokens: 0, staleGameSessions: 0, expiredOrders: 0 });
  });

  it('two passes at once do not fail or double count', async () => {
    const u = await makeUser(env);
    await env.call('/v1/auth/logout', { method: 'POST', token: u.token });
    env.clock.advanceSeconds(10 * DAY);
    const [a, b] = await Promise.all([runMaintenance(env.deps), runMaintenance(env.deps)]);
    expect(a.sessions + b.sessions).toBeGreaterThanOrEqual(1);
  });
});

describe('the timer', () => {
  it('runs a pass at start, logs failures instead of throwing, and stops when told to', async () => {
    const lines: string[] = [];
    const broken = { ...env.deps, db: undefined as unknown as typeof env.deps.db, logger: { ...env.deps.logger, error: (m: string) => void lines.push(m), debug: () => undefined } };
    const stop = startMaintenance(broken, 3600);
    await new Promise((resolve) => setTimeout(resolve, 50));
    stop();
    expect(lines).toContain('maintenance pass failed');
    // The healthy path logs the report at debug level.
    const debug: string[] = [];
    const healthy = { ...env.deps, logger: { ...env.deps.logger, debug: (m: string) => void debug.push(m) } };
    const stop2 = startMaintenance(healthy, 3600);
    await new Promise((resolve) => setTimeout(resolve, 200));
    stop2();
    expect(debug).toContain('maintenance pass');
  });
});
