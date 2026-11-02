import { and, asc, eq, sql } from 'drizzle-orm';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { schema } from '../src/db/index.js';
import { isCheckViolation } from '../src/util/errors.js';
import { runMaintenance } from '../src/maintenance.js';
import {
  asServer, createTestEnv, freshIp, joinServer, makeProduct, makeServer, makeStaff, makeUser, sendWebhook,
  type TestEnv, type TestProduct, type TestServer, type TestUser,
} from './helpers.js';

let env: TestEnv;
let admin: TestUser;
let srv: TestServer;
let hat: TestProduct;
let euro: TestProduct;

beforeAll(async () => {
  env = await createTestEnv();
  admin = await makeStaff(env, 'admin');
  srv = await makeServer(env, admin);
  await makeProduct(env, admin, { sku: 'coins-1000', kind: 'currency_pack', priceMinor: 499, maxQuantity: 5, grants: [{ type: 'wallet_credit', currency: 'coins', amount: 1000 }] });
  await makeProduct(env, admin, { sku: 'vip-30', kind: 'vip', priceMinor: 999, grants: [{ type: 'entitlement', key: 'vip', durationDays: 30 }] });
  hat = await makeProduct(env, admin, { sku: 'hat-red', kind: 'cosmetic', priceMinor: 299, grants: [{ type: 'item', key: 'hat.red' }] });
  await makeProduct(env, admin, {
    sku: 'starter-bundle', kind: 'bundle', priceMinor: 1299,
    grants: [{ type: 'wallet_credit', currency: 'coins', amount: 500 }, { type: 'entitlement', key: 'vip', durationDays: 7 }, { type: 'item', key: 'car.sultan' }],
  });
  euro = await makeProduct(env, admin, { sku: 'eur-thing', kind: 'cosmetic', priceMinor: 100, currency: 'EUR', grants: [{ type: 'item', key: 'thing.eur' }] });
});
afterAll(async () => {
  await env.close();
});

type ErrBody = { error: { code: string; message: string; details?: Record<string, unknown> } };
interface OrderBody { id: string; status: string; totalMinor: number; currency: string; checkoutUrl: string | null; failureReason: string | null; items: { sku: string; quantity: number; unitPriceMinor: number }[] }
interface GrantBody { id: string; seq: number; kind: 'grant' | 'revoke'; revokesGrantId: string | null; type: string; sku: string; payload: Record<string, unknown>; quantity: number; orderId: string }

const db = () => env.handle.db;
let keyCounter = 0;

async function checkout(user: TestUser, items: { sku: string; quantity: number }[], key?: string) {
  const headers: Record<string, string> = {};
  if (key) headers['idempotency-key'] = key;
  return env.call<{ order: OrderBody } & ErrBody>('/v1/shop/checkout', { token: user.token, json: { items }, headers, ip: freshIp() });
}
async function buy(user: TestUser, items: { sku: string; quantity: number }[]): Promise<OrderBody> {
  const res = await checkout(user, items, `key-${++keyCounter}-${Math.random().toString(36).slice(2)}`);
  expect(res.status, JSON.stringify(res.body)).toBe(201);
  return res.body.order;
}
const pay = (o: OrderBody, extra: { id?: string } = {}) => sendWebhook(env, { type: 'payment.succeeded', orderId: o.id, amountMinor: o.totalMinor, currency: o.currency, ...extra });
const orderRow = async (id: string) => (await db().select().from(schema.orders).where(eq(schema.orders.id, id)))[0];
const grantRows = (orderId: string) => db().select().from(schema.grants).where(eq(schema.grants.orderId, orderId)).orderBy(asc(schema.grants.seq));
const ledgerRows = (accountId: string) => db().select().from(schema.walletLedger).where(eq(schema.walletLedger.accountId, accountId)).orderBy(asc(schema.walletLedger.id));
const balanceOf = async (u: TestUser, currency = 'coins') => {
  const r = await env.call<{ balances: { currency: string; balance: number }[] }>('/v1/me/wallet', { token: u.token });
  return r.body.balances.find((b) => b.currency === currency)?.balance ?? 0;
};
const pull = (u: TestUser, server = srv) => asServer(env, server)<{ grants: GrantBody[] } & ErrBody>(`/internal/game/grants?accountId=${u.accountId}`);
const ack = (id: string, server = srv) => asServer(env, server)<{ id: string; alreadyAcked: boolean } & ErrBody>(`/internal/game/grants/${id}/ack`, { method: 'POST' });

describe('products (admin)', () => {
  const base = { sku: 'p-test', name: 'P', priceMinor: 100, currency: 'USD', kind: 'cosmetic', grantPayload: { grants: [{ type: 'item', key: 'x.y' }] } };
  const create = (json: Record<string, unknown>, token = admin.token) => env.call<{ product?: Record<string, unknown> } & ErrBody>('/v1/admin/products', { token, json });

  it('validates the payload against the kind, the price, currency, image and sku', async () => {
    const bad: Record<string, unknown>[] = [
      { ...base, sku: 'p-bad-1', kind: 'currency_pack' }, // item grant in a currency pack
      { ...base, sku: 'p-bad-2', kind: 'vip' },
      { ...base, sku: 'p-bad-3', grantPayload: { grants: [] } },
      { ...base, sku: 'p-bad-4', grantPayload: { grants: [{ type: 'wallet_credit', currency: 'coins', amount: 0 }] }, kind: 'currency_pack' },
      { ...base, sku: 'p-bad-5', grantPayload: { grants: [{ type: 'mystery', key: 'x' }] } },
      { ...base, sku: 'p-bad-6', grantPayload: { grants: [{ type: 'item', key: 'x.y', extra: 1 }] } },
      { ...base, sku: 'p-bad-7', priceMinor: 0 },
      { ...base, sku: 'p-bad-8', priceMinor: 1.5 },
      { ...base, sku: 'p-bad-9', currency: 'usd' },
      { ...base, sku: 'p-bad-10', imageUrl: 'http://insecure.example/x.png' },
      { ...base, sku: 'BAD SKU' },
    ];
    for (const b of bad) expect((await create(b)).status, JSON.stringify(b)).toBe(400);
    expect((await create({ ...base, sku: 'p-ok-1', imageUrl: 'https://cdn.example/x.png' })).status).toBe(201);
  });

  it('rejects duplicate SKUs and non-admins', async () => {
    expect((await create({ ...base, sku: 'p-dup' })).status).toBe(201);
    expect((await create({ ...base, sku: 'p-dup' })).body.error.code).toBe('sku_exists');
    const mod = await makeStaff(env, 'mod');
    expect((await create({ ...base, sku: 'p-dup-2' }, mod.token)).status).toBe(403);
  });

  it('PATCH leaves absent fields alone and keeps kind and payload consistent', async () => {
    const created = await create({ ...base, sku: 'p-patch', sort: 7, description: 'keep me', maxQuantity: 3 });
    const id = (created.body.product as { id: string }).id;
    const patched = await env.call<{ product: Record<string, unknown> }>(`/v1/admin/products/${id}`, { method: 'PATCH', token: admin.token, json: { name: 'Renamed' } });
    expect(patched.body.product).toMatchObject({ name: 'Renamed', sort: 7, description: 'keep me', maxQuantity: 3, active: true, priceMinor: 100 });
    const inconsistent = await env.call<ErrBody>(`/v1/admin/products/${id}`, { method: 'PATCH', token: admin.token, json: { kind: 'vip' } });
    expect(inconsistent.status).toBe(400);
    const both = await env.call<{ product: { kind: string } }>(`/v1/admin/products/${id}`, { method: 'PATCH', token: admin.token, json: { kind: 'vip', grantPayload: { grants: [{ type: 'entitlement', key: 'vip' }] } } });
    expect(both.body.product.kind).toBe('vip');
  });

  it('the public list shows only active products; inactive ones cannot be bought', async () => {
    const hidden = await makeProduct(env, admin, { sku: 'p-inactive', kind: 'cosmetic', grants: [{ type: 'item', key: 'x.z' }], active: false });
    const pub = await env.call<{ products: { sku: string; grants: unknown[] }[] }>('/v1/shop/products');
    expect(pub.status).toBe(200);
    const skus = pub.body.products.map((p) => p.sku);
    expect(skus).toContain('coins-1000');
    expect(skus).not.toContain(hidden.sku);
    expect(pub.body.products.find((p) => p.sku === 'vip-30')?.grants).toEqual([{ type: 'entitlement', key: 'vip', durationDays: 30 }]);
    const u = await makeUser(env);
    expect((await checkout(u, [{ sku: hidden.sku, quantity: 1 }])).body.error.code).toBe('product_inactive');
  });

  it('a product with orders cannot be deleted, an unused one can', async () => {
    const u = await makeUser(env);
    await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    expect((await env.call<ErrBody>(`/v1/admin/products/${hat.id}`, { method: 'DELETE', token: admin.token })).body.error.code).toBe('product_in_use');
    const unused = await makeProduct(env, admin, { sku: 'p-unused', kind: 'cosmetic', grants: [{ type: 'item', key: 'u.u' }] });
    expect((await env.call(`/v1/admin/products/${unused.id}`, { method: 'DELETE', token: admin.token })).status).toBe(204);
  });
});

describe('checkout', () => {
  it('creates a pending order with a snapshot of the lines and a checkout URL', async () => {
    const u = await makeUser(env);
    const res = await checkout(u, [{ sku: 'coins-1000', quantity: 2 }, { sku: 'hat-red', quantity: 1 }]);
    expect(res.status).toBe(201);
    const o = res.body.order;
    expect(o).toMatchObject({ status: 'pending', totalMinor: 2 * 499 + 299, currency: 'USD', failureReason: null });
    expect(o.checkoutUrl).toBe(`http://platform.test/v1/dev/fake-checkout/${o.id}`);
    expect(o.items.map((i) => [i.sku, i.quantity, i.unitPriceMinor]).sort()).toEqual([['coins-1000', 2, 499], ['hat-red', 1, 299]]);
    const row = await orderRow(o.id);
    expect(row?.providerRef).toMatch(/^fake_cs_/);
    expect(row?.provider).toBe('fake');
  });

  it('merges duplicate SKUs; refuses unknown/over-limit/mixed-currency/empty requests', async () => {
    const u = await makeUser(env);
    const merged = await checkout(u, [{ sku: 'coins-1000', quantity: 2 }, { sku: 'coins-1000', quantity: 1 }]);
    expect(merged.body.order.items).toEqual([expect.objectContaining({ sku: 'coins-1000', quantity: 3 })]);
    expect((await checkout(u, [{ sku: 'nope', quantity: 1 }])).body.error.code).toBe('product_not_found');
    expect((await checkout(u, [{ sku: 'coins-1000', quantity: 6 }])).body.error.code).toBe('quantity_exceeds_limit');
    expect((await checkout(u, [{ sku: 'vip-30', quantity: 2 }])).body.error.code).toBe('quantity_exceeds_limit'); // max 1
    expect((await checkout(u, [{ sku: 'hat-red', quantity: 1 }, { sku: 'eur-thing', quantity: 1 }])).body.error.code).toBe('mixed_currency');
    expect((await checkout(u, [])).status).toBe(400);
    expect((await checkout(u, [{ sku: 'hat-red', quantity: 0 }])).status).toBe(400);
  });

  it('needs a signed-in, verified, unbanned account', async () => {
    expect((await env.call('/v1/shop/checkout', { json: { items: [{ sku: 'hat-red', quantity: 1 }] } })).status).toBe(401);
    const unverified = await makeUser(env, { verified: false });
    expect((await checkout(unverified, [{ sku: 'hat-red', quantity: 1 }])).body.error.code).toBe('email_not_verified');
    const banned = await makeUser(env);
    await env.call(`/v1/admin/accounts/${banned.accountId}/ban`, { token: admin.token, json: { reason: 'chargeback abuse' } });
    expect((await checkout(banned, [{ sku: 'hat-red', quantity: 1 }])).body.error.code).toBe('account_banned');
  });

  it('is idempotent per Idempotency-Key and rejects a key reused for a different basket', async () => {
    const u = await makeUser(env);
    const first = await checkout(u, [{ sku: 'vip-30', quantity: 1 }], 'idem-key-0001');
    const retry = await checkout(u, [{ sku: 'vip-30', quantity: 1 }], 'idem-key-0001');
    expect(first.status).toBe(201);
    expect(retry.status).toBe(201);
    expect(retry.body.order.id).toBe(first.body.order.id);
    expect(retry.body.order.checkoutUrl).toBe(first.body.order.checkoutUrl);
    expect(await db().select().from(schema.orders).where(eq(schema.orders.accountId, u.accountId))).toHaveLength(1);
    expect((await checkout(u, [{ sku: 'hat-red', quantity: 1 }], 'idem-key-0001')).body.error.code).toBe('idempotency_mismatch');
    expect((await checkout(u, [{ sku: 'hat-red', quantity: 1 }], 'short')).status).toBe(400);
    // The same key under another account is a different order.
    const other = await makeUser(env);
    const o2 = await checkout(other, [{ sku: 'vip-30', quantity: 1 }], 'idem-key-0001');
    expect(o2.body.order.id).not.toBe(first.body.order.id);
  });

  it('simultaneous requests with one key create one order', async () => {
    const u = await makeUser(env);
    const results = await Promise.all(Array.from({ length: 5 }, () => checkout(u, [{ sku: 'hat-red', quantity: 1 }], 'idem-race-0001')));
    expect(new Set(results.map((r) => r.body.order?.id)).size).toBe(1);
    expect(await db().select().from(schema.orders).where(eq(schema.orders.accountId, u.accountId))).toHaveLength(1);
  });

  it('a later price change does not touch an existing order', async () => {
    const p = await makeProduct(env, admin, { sku: 'price-move', kind: 'cosmetic', priceMinor: 500, grants: [{ type: 'item', key: 'p.m' }] });
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: p.sku, quantity: 1 }]);
    await env.call(`/v1/admin/products/${p.id}`, { method: 'PATCH', token: admin.token, json: { priceMinor: 900, grantPayload: { grants: [{ type: 'item', key: 'p.changed' }] } } });
    const read = await env.call<{ order: OrderBody }>(`/v1/shop/orders/${o.id}`, { token: u.token });
    expect(read.body.order.totalMinor).toBe(500);
    expect((await pay(o)).body.result).toBe('applied');
    expect((await grantRows(o.id))[0]?.payload).toMatchObject({ type: 'item', key: 'p.m' });
  });

  it('orders are private to their owner and listed newest first', async () => {
    const a = await makeUser(env);
    const b = await makeUser(env);
    const o1 = await buy(a, [{ sku: 'hat-red', quantity: 1 }]);
    env.clock.advance(1000);
    const o2 = await buy(a, [{ sku: 'vip-30', quantity: 1 }]);
    const list = await env.call<{ orders: OrderBody[] }>('/v1/shop/orders', { token: a.token });
    expect(list.body.orders.map((o) => o.id)).toEqual([o2.id, o1.id]);
    expect((await env.call(`/v1/shop/orders/${o1.id}`, { token: b.token })).status).toBe(404);
    expect((await env.call('/v1/shop/orders/not-a-uuid', { token: a.token })).status).toBe(400);
  });
});

describe('paying: checkout -> webhook -> paid -> grant -> pull -> ack', () => {
  it('a currency pack credits the wallet at once and the order is fulfilled', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 2 }]);
    const res = await pay(o);
    expect(res.status).toBe(200);
    expect(res.body).toEqual({ received: true, duplicate: false, result: 'applied' });

    const row = await orderRow(o.id);
    expect(row?.status).toBe('fulfilled');
    expect(row?.paidAt).not.toBeNull();
    expect(row?.fulfilledAt).not.toBeNull();
    expect(await balanceOf(u)).toBe(2000);
    const [g] = await grantRows(o.id);
    expect(g).toMatchObject({ kind: 'grant', type: 'wallet_credit', status: 'acked', ackedBy: 'platform', quantity: 2 });
    const ledger = await ledgerRows(u.accountId);
    expect(ledger).toHaveLength(1);
    expect(ledger[0]).toMatchObject({ delta: 2000, reason: 'purchase', idempotencyKey: `grant:${g?.id}`, grantId: g?.id });
    // Nothing for the game server to do.
    const { sessionId } = await joinServer(env, u, srv);
    expect(sessionId).toBeTruthy();
    expect((await pull(u)).body.grants).toEqual([]);
  });

  it('item and entitlement grants wait for the game server: pull is repeatable, ack is once, the order is fulfilled by the last ack', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'vip-30', quantity: 1 }, { sku: 'hat-red', quantity: 1 }]);
    expect((await pay(o)).body.result).toBe('applied');
    expect((await orderRow(o.id))?.status).toBe('paid');

    await joinServer(env, u, srv);
    const first = await pull(u);
    expect(first.status).toBe(200);
    expect(first.body.grants.map((g) => [g.kind, g.type, g.sku])).toEqual(expect.arrayContaining([['grant', 'entitlement', 'vip-30'], ['grant', 'item', 'hat-red']]));
    expect(first.body.grants).toHaveLength(2);
    const seqs = first.body.grants.map((g) => g.seq);
    expect([...seqs].sort((a, b) => a - b)).toEqual(seqs);
    const vipGrant = first.body.grants.find((g) => g.type === 'entitlement') as GrantBody;
    expect(vipGrant.payload).toEqual({ type: 'entitlement', key: 'vip', durationDays: 30 });
    expect(vipGrant.orderId).toBe(o.id);

    // Reading does not consume.
    expect((await pull(u)).body.grants).toHaveLength(2);

    const a1 = await ack(first.body.grants[0]?.id as string);
    expect(a1.body).toMatchObject({ alreadyAcked: false });
    expect((await orderRow(o.id))?.status).toBe('paid'); // one grant still pending
    expect((await pull(u)).body.grants).toHaveLength(1);

    const a2 = await ack(first.body.grants[1]?.id as string);
    expect(a2.body.alreadyAcked).toBe(false);
    expect((await orderRow(o.id))?.status).toBe('fulfilled');
    expect((await pull(u)).body.grants).toEqual([]);

    // Not re-delivered, and a repeated ack is a harmless no-op.
    const again = await ack(first.body.grants[0]?.id as string);
    expect(again.status).toBe(200);
    expect(again.body.alreadyAcked).toBe(true);
    expect((await ack('00000000-0000-4000-8000-000000000000')).status).toBe(404);

    const rows = await grantRows(o.id);
    expect(rows.every((g) => g.status === 'acked' && g.ackedBy === srv.id)).toBe(true);
    const items = await env.call<{ items: { sku: string; delivered: boolean }[] }>('/v1/me/items', { token: u.token });
    expect(items.body.items.map((i) => [i.sku, i.delivered]).sort()).toEqual([['hat-red', true], ['vip-30', true]]);
  });

  it('delivery survives a server restart: an unacked grant comes back, an acked one never does', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    await pay(o);
    await joinServer(env, u, srv);
    const delivered = (await pull(u)).body.grants;
    expect(delivered).toHaveLength(1);
    // The server crashes after pulling and before acking: its process memory is gone, the session stays open.
    const afterRestart = (await pull(u)).body.grants;
    expect(afterRestart.map((g) => g.id)).toEqual(delivered.map((g) => g.id));

    // The player leaves and joins a different server: the grant is still pending there.
    await asServer(env, srv)('/internal/game/session-end', { json: { accountId: u.accountId } });
    const other = await makeServer(env, admin);
    await joinServer(env, u, other);
    const there = await pull(u, other);
    expect(there.body.grants.map((g) => g.id)).toEqual(delivered.map((g) => g.id));
    expect((await ack(delivered[0]?.id as string, other)).body.alreadyAcked).toBe(false);
    expect((await pull(u, other)).body.grants).toEqual([]);
  });

  it('simultaneous acks: exactly one is the first', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    await pay(o);
    await joinServer(env, u, srv);
    const id = (await pull(u)).body.grants[0]?.id as string;
    const results = await Promise.all(Array.from({ length: 6 }, () => ack(id)));
    expect(results.filter((r) => r.body.alreadyAcked === false)).toHaveLength(1);
    expect(results.every((r) => r.status === 200)).toBe(true);
  });

  it('pulls need an open session of that account on that server', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    await pay(o);
    expect((await pull(u)).body.error.code).toBe('no_active_session');
    const other = await makeServer(env, admin);
    await joinServer(env, u, other);
    expect((await pull(u, srv)).body.error.code).toBe('no_active_session'); // in a session, but on the other server
    expect((await pull(u, other)).body.grants).toHaveLength(1);
    expect((await asServer(env, srv)<ErrBody>('/internal/game/grants/pull', { json: { accountId: u.accountId } })).status).toBe(409);
    expect((await asServer(env, other)<{ grants: unknown[] }>('/internal/game/grants/pull', { json: { accountId: u.accountId } })).body.grants).toHaveLength(1);
    expect((await asServer(env, other)<ErrBody>('/internal/game/grants?accountId=not-a-uuid')).status).toBe(400);
  });

  it('a bundle credits the wallet immediately and queues the rest', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'starter-bundle', quantity: 1 }]);
    await pay(o);
    expect(await balanceOf(u)).toBe(500);
    expect((await orderRow(o.id))?.status).toBe('paid');
    const rows = await grantRows(o.id);
    expect(rows.map((g) => [g.type, g.status]).sort()).toEqual([['entitlement', 'pending'], ['item', 'pending'], ['wallet_credit', 'acked']]);
  });
});

describe('webhook security', () => {
  it('rejects a missing, wrong, tampered or stale signature and changes nothing', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    const event = { type: 'payment.succeeded' as const, orderId: o.id, amountMinor: o.totalMinor, currency: o.currency };
    const raw = JSON.stringify({ id: 'evt_sec_1', ...event, providerRef: null });

    expect((await sendWebhook(env, event, { rawBody: raw, signature: null })).body.error?.code).toBe('invalid_webhook');
    expect((await sendWebhook(env, event, { rawBody: raw, signature: 'garbage' })).status).toBe(400);
    expect((await sendWebhook(env, event, { rawBody: raw, signature: `t=${Math.floor(Date.now() / 1000)},v1=${'0'.repeat(64)}` })).status).toBe(400);
    const sigOfOther = env.fake.sign(raw.replace('evt_sec_1', 'evt_sec_2'));
    expect((await sendWebhook(env, event, { rawBody: raw, signature: sigOfOther })).status).toBe(400); // signature of a different body
    const stale = env.fake.sign(raw, new Date(env.clock.now().getTime() - 10 * 60_000));
    expect((await sendWebhook(env, event, { rawBody: raw, signature: stale })).status).toBe(400);
    const notJson = '{"id":';
    expect((await sendWebhook(env, event, { rawBody: notJson, signature: env.fake.sign(notJson) })).status).toBe(400);
    const wrongShape = JSON.stringify({ id: 'evt_sec_3', type: 'payment.succeeded' });
    expect((await sendWebhook(env, event, { rawBody: wrongShape, signature: env.fake.sign(wrongShape) })).status).toBe(400);

    expect((await orderRow(o.id))?.status).toBe('pending');
    expect(await grantRows(o.id)).toHaveLength(0);
    expect(await db().select().from(schema.paymentEvents).where(eq(schema.paymentEvents.orderId, o.id))).toHaveLength(0);

    const ok = await sendWebhook(env, event, { rawBody: raw });
    expect(ok.body.result).toBe('applied');
  });

  it('an unknown provider is 404', async () => {
    const res = await env.call<ErrBody>('/v1/webhooks/stripe', { rawBody: '{}', headers: { 'content-type': 'application/json' } });
    expect(res.status).toBe(404);
    expect(res.body.error.code).toBe('unknown_provider');
  });
});

describe('webhook idempotency', () => {
  it('a redelivered event is acknowledged and applied once', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 1 }, { sku: 'hat-red', quantity: 1 }]);
    const first = await pay(o, { id: 'evt_dup_1' });
    const second = await pay(o, { id: 'evt_dup_1' });
    expect(first.body).toEqual({ received: true, duplicate: false, result: 'applied' });
    expect(second.status).toBe(200);
    expect(second.body).toEqual({ received: true, duplicate: true, result: 'duplicate' });

    expect(await grantRows(o.id)).toHaveLength(2);
    expect(await ledgerRows(u.accountId)).toHaveLength(1);
    expect(await balanceOf(u)).toBe(1000);
    expect(await db().select().from(schema.paymentEvents).where(and(eq(schema.paymentEvents.provider, 'fake'), eq(schema.paymentEvents.eventId, 'evt_dup_1')))).toHaveLength(1);
  });

  it('simultaneous deliveries of one event: exactly one applies', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    const results = await Promise.all(Array.from({ length: 6 }, () => pay(o, { id: 'evt_race_1' })));
    expect(results.every((r) => r.status === 200)).toBe(true);
    expect(results.filter((r) => r.body.result === 'applied')).toHaveLength(1);
    expect(results.filter((r) => r.body.duplicate)).toHaveLength(5);
    expect(await balanceOf(u)).toBe(1000);
  });

  it('a second payment event with a new id for an already paid order is ignored, not granted twice', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    expect((await pay(o, { id: 'evt_twice_a' })).body.result).toBe('applied');
    const again = await pay(o, { id: 'evt_twice_b' });
    expect(again.body.result).toBe('ignored');
    expect(await balanceOf(u)).toBe(1000);
    expect(await grantRows(o.id)).toHaveLength(1);
  });

  it('the database itself refuses a second grant for the same order line and entry', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    await pay(o);
    const [g] = await grantRows(o.id);
    await expect(
      db().insert(schema.grants).values({ accountId: u.accountId, orderId: o.id, lineNo: g?.lineNo as number, entryNo: g?.entryNo as number, kind: 'grant', type: 'item', sku: 'hat-red', payload: {} }),
    ).rejects.toSatisfy((e) => (e as { cause?: { code?: string } }).cause?.code === '23505' || (e as { code?: string }).code === '23505');
    // And a second ledger credit for the same grant.
    await expect(
      db().insert(schema.walletLedger).values({ accountId: u.accountId, currency: 'coins', delta: 5, reason: 'purchase', idempotencyKey: `grant:${g?.id}-x` }).then(() =>
        db().insert(schema.walletLedger).values({ accountId: u.accountId, currency: 'coins', delta: 5, reason: 'purchase', idempotencyKey: `grant:${g?.id}-x` })),
    ).rejects.toBeDefined();
  });
});

describe('webhook edge cases', () => {
  it('a payment of the wrong amount or currency delivers nothing and fails the order', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    const res = await sendWebhook(env, { type: 'payment.succeeded', orderId: o.id, amountMinor: 1, currency: o.currency });
    expect(res.body.result).toBe('rejected');
    const row = await orderRow(o.id);
    expect(row).toMatchObject({ status: 'failed', failureReason: 'amount_mismatch' });
    expect(await grantRows(o.id)).toHaveLength(0);
    expect(await balanceOf(u)).toBe(0);
    expect(env.logs.some((l) => l.includes('amount mismatch'))).toBe(true);

    const o2 = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    expect((await sendWebhook(env, { type: 'payment.succeeded', orderId: o2.id, amountMinor: o2.totalMinor, currency: 'EUR' })).body.result).toBe('rejected');
    const o3 = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    expect((await sendWebhook(env, { type: 'payment.succeeded', orderId: o3.id })).body.result).toBe('rejected'); // no amount at all
  });

  it('an event for an unknown order is acknowledged as unmatched', async () => {
    const res = await sendWebhook(env, { type: 'payment.succeeded', orderId: '11111111-1111-4111-8111-111111111111', amountMinor: 100, currency: 'USD' });
    expect(res.status).toBe(200);
    expect(res.body.result).toBe('unmatched');
  });

  it('payment.failed fails a pending order; a later success (the customer retried the same checkout) revives it and delivers', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    expect((await sendWebhook(env, { type: 'payment.failed', orderId: o.id })).body.result).toBe('applied');
    expect(await orderRow(o.id)).toMatchObject({ status: 'failed', failureReason: 'payment_failed' });
    expect(await grantRows(o.id)).toHaveLength(0);
    expect((await pay(o)).body.result).toBe('applied');
    expect(await orderRow(o.id)).toMatchObject({ status: 'paid', failureReason: null });
    expect(await grantRows(o.id)).toHaveLength(1);
    const detail = await db().select().from(schema.paymentEvents).where(eq(schema.paymentEvents.orderId, o.id));
    expect(detail.map((e) => e.detail)).toContain('late_payment:payment_failed');
    expect(env.logs.some((l) => l.includes('payment arrived after the order had failed'))).toBe(true);
  });

  it('a payment for an order that failed on an amount mismatch is never revived: nothing is delivered and the operator is told', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    await sendWebhook(env, { type: 'payment.succeeded', orderId: o.id, amountMinor: 1, currency: o.currency });
    expect((await orderRow(o.id))?.failureReason).toBe('amount_mismatch');
    env.logs.length = 0;
    expect((await pay(o)).body.result).toBe('ignored');
    expect(await orderRow(o.id)).toMatchObject({ status: 'failed', failureReason: 'amount_mismatch' });
    expect(await grantRows(o.id)).toHaveLength(0);
    expect(env.logs.some((l) => l.includes('cannot be revived'))).toBe(true);
  });

  it('a late payment of the wrong amount fails the order for good instead of reviving it', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    await sendWebhook(env, { type: 'payment.failed', orderId: o.id });
    expect((await sendWebhook(env, { type: 'payment.succeeded', orderId: o.id, amountMinor: 5, currency: o.currency })).body.result).toBe('rejected');
    expect(await orderRow(o.id)).toMatchObject({ status: 'failed', failureReason: 'amount_mismatch' });
    expect((await pay(o)).body.result).toBe('ignored');
    expect(await balanceOf(u)).toBe(0);
  });

  it('refund and chargeback events on a pending order are ignored; a partial refund is rejected', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    expect((await sendWebhook(env, { type: 'refund.succeeded', orderId: o.id, amountMinor: o.totalMinor })).body.result).toBe('ignored');
    expect((await sendWebhook(env, { type: 'chargeback.opened', orderId: o.id })).body.result).toBe('ignored');
    await pay(o);
    const partial = await sendWebhook(env, { type: 'refund.succeeded', orderId: o.id, amountMinor: 100 });
    expect(partial.body.result).toBe('rejected');
    expect((await orderRow(o.id))?.status).toBe('fulfilled');
    expect(await balanceOf(u)).toBe(1000);
  });
});

describe('refund and chargeback create revoke entries', () => {
  it('a refunded currency pack claws the coins back; spending first leaves a debt, not an error', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 2 }]);
    await pay(o);
    await joinServer(env, u, srv);
    const spent = await asServer(env, srv)<{ balance: number; replayed: boolean }>('/internal/game/wallet/spend', { json: { accountId: u.accountId, currency: 'coins', amount: 500, idempotencyKey: 'spend-aaaa-0001' } });
    expect(spent.body).toEqual({ replayed: false, balance: 1500 });

    const refund = await sendWebhook(env, { type: 'refund.succeeded', orderId: o.id, amountMinor: o.totalMinor });
    expect(refund.body.result).toBe('applied');
    expect(await balanceOf(u)).toBe(-500);
    const row = await orderRow(o.id);
    expect(row?.status).toBe('refunded');
    expect(row?.refundedAt).not.toBeNull();
    const rows = await grantRows(o.id);
    expect(rows.map((g) => [g.kind, g.status])).toEqual([['grant', 'acked'], ['revoke', 'acked']]);
    expect(rows[1]?.revokesGrantId).toBe(rows[0]?.id);
    const ledger = await ledgerRows(u.accountId);
    expect(ledger.map((l) => [l.reason, l.delta])).toEqual([['purchase', 2000], ['spend', -500], ['revoke', -2000]]);

    // Further spending is blocked until the debt is paid off.
    const blocked = await asServer(env, srv)<ErrBody>('/internal/game/wallet/spend', { json: { accountId: u.accountId, currency: 'coins', amount: 1, idempotencyKey: 'spend-aaaa-0002' } });
    expect(blocked.body.error.code).toBe('insufficient_funds');
    // A duplicate refund event does nothing.
    expect((await sendWebhook(env, { type: 'refund.succeeded', orderId: o.id, amountMinor: o.totalMinor })).body.result).toBe('ignored');
    expect(await balanceOf(u)).toBe(-500);
  });

  it('a refunded item order queues a revoke for the game server after its grant', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'vip-30', quantity: 1 }, { sku: 'hat-red', quantity: 1 }]);
    await pay(o);
    await joinServer(env, u, srv);
    for (const g of (await pull(u)).body.grants) await ack(g.id);
    expect((await orderRow(o.id))?.status).toBe('fulfilled');
    const owned = await env.call<{ items: unknown[] }>('/v1/me/items', { token: u.token });
    expect(owned.body.items).toHaveLength(2);

    expect((await sendWebhook(env, { type: 'refund.succeeded', orderId: o.id, amountMinor: o.totalMinor })).body.result).toBe('applied');
    expect((await orderRow(o.id))?.status).toBe('refunded');
    const revokes = (await pull(u)).body.grants;
    expect(revokes).toHaveLength(2);
    expect(revokes.every((g) => g.kind === 'revoke' && g.revokesGrantId !== null)).toBe(true);
    const originals = (await grantRows(o.id)).filter((g) => g.kind === 'grant').map((g) => g.id).sort();
    expect(revokes.map((g) => g.revokesGrantId as string).sort()).toEqual(originals);
    expect(revokes[0]?.payload).toMatchObject({ type: expect.stringMatching(/entitlement|item/) });

    for (const g of revokes) expect((await ack(g.id)).body.alreadyAcked).toBe(false);
    expect((await pull(u)).body.grants).toEqual([]);
    expect((await env.call<{ items: unknown[] }>('/v1/me/items', { token: u.token })).body.items).toHaveLength(0);
    expect((await orderRow(o.id))?.status).toBe('refunded'); // acking revokes does not resurrect it
  });

  it('refunding a paid order before the server pulled queues the grant and its revoke in order', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    await pay(o);
    await sendWebhook(env, { type: 'refund.succeeded', orderId: o.id, amountMinor: o.totalMinor });
    await joinServer(env, u, srv);
    const pending = (await pull(u)).body.grants;
    expect(pending.map((g) => g.kind)).toEqual(['grant', 'revoke']);
    expect(pending[0]?.seq).toBeLessThan(pending[1]?.seq as number);
  });

  it('a chargeback revokes too and is terminal', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'starter-bundle', quantity: 1 }]);
    await pay(o);
    expect(await balanceOf(u)).toBe(500);
    expect((await sendWebhook(env, { type: 'chargeback.opened', orderId: o.id })).body.result).toBe('applied');
    expect((await orderRow(o.id))?.status).toBe('chargeback');
    expect(await balanceOf(u)).toBe(0);
    const kinds = (await grantRows(o.id)).map((g) => `${g.kind}:${g.type}:${g.status}`).sort();
    expect(kinds).toEqual([
      'grant:entitlement:pending', 'grant:item:pending', 'grant:wallet_credit:acked',
      'revoke:entitlement:pending', 'revoke:item:pending', 'revoke:wallet_credit:acked',
    ]);
    expect((await sendWebhook(env, { type: 'refund.succeeded', orderId: o.id, amountMinor: o.totalMinor })).body.result).toBe('ignored');
  });

  it('the database refuses a second revoke for the same grant', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    await pay(o);
    await sendWebhook(env, { type: 'refund.succeeded', orderId: o.id, amountMinor: o.totalMinor });
    const [g] = await grantRows(o.id);
    await expect(
      db().insert(schema.grants).values({ accountId: u.accountId, orderId: o.id, lineNo: 5, entryNo: 5, kind: 'revoke', revokesGrantId: g?.id as string, type: 'item', sku: 'hat-red', payload: {} }),
    ).rejects.toBeDefined();
  });
});

describe('admin refund', () => {
  it('asks the provider (idempotently), revokes, audits; the later provider webhook is ignored', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    await pay(o);
    const res = await env.call<{ order: OrderBody }>(`/v1/admin/orders/${o.id}/refund`, { token: admin.token, json: { reason: 'customer request' } });
    expect(res.status).toBe(200);
    expect(res.body.order.status).toBe('refunded');
    expect(await balanceOf(u)).toBe(0);
    expect(env.fake.refunds.filter((r) => r.orderId === o.id)).toEqual([expect.objectContaining({ idempotencyKey: `refund:${o.id}`, amountMinor: o.totalMinor, currency: 'USD', reason: 'customer request' })]);
    const audit = await db().select().from(schema.auditLog).where(and(eq(schema.auditLog.targetId, o.id), eq(schema.auditLog.action, 'order.refund')));
    expect(audit).toHaveLength(1);
    expect(audit[0]?.actorAccountId).toBe(admin.accountId);
    expect((await sendWebhook(env, { type: 'refund.succeeded', orderId: o.id, amountMinor: o.totalMinor })).body.result).toBe('ignored');
    expect((await env.call<ErrBody>(`/v1/admin/orders/${o.id}/refund`, { token: admin.token, json: {} })).body.error.code).toBe('order_not_refundable');
  });

  it('a provider failure changes nothing', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    await pay(o);
    env.fake.failNextRefund = true;
    const res = await env.call<ErrBody>(`/v1/admin/orders/${o.id}/refund`, { token: admin.token, json: {} });
    expect(res.status).toBe(503);
    expect(res.body.error.code).toBe('refund_failed');
    expect((await orderRow(o.id))?.status).toBe('fulfilled');
    expect(await balanceOf(u)).toBe(1000);
  });

  it('pending orders cannot be refunded and only admins may', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    expect((await env.call<ErrBody>(`/v1/admin/orders/${o.id}/refund`, { token: admin.token, json: {} })).body.error.code).toBe('order_not_refundable');
    const mod = await makeStaff(env, 'mod');
    expect((await env.call(`/v1/admin/orders/${o.id}/refund`, { token: mod.token, json: {} })).status).toBe(403);
    const list = await env.call<{ orders: { id: string; accountId: string }[] }>(`/v1/admin/orders?accountId=${u.accountId}`, { token: admin.token });
    expect(list.body.orders).toEqual([expect.objectContaining({ id: o.id, accountId: u.accountId })]);
  });
});

describe('order state machine (enforced by the database)', () => {
  const setStatus = (id: string, status: string) => db().execute(sql`update orders set status = ${status} where id = ${id}`);

  it('rejects every transition outside the table', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    await expect(setStatus(o.id, 'fulfilled')).rejects.toSatisfy((e) => isCheckViolation(e, 'illegal order status transition'));
    await expect(setStatus(o.id, 'refunded')).rejects.toSatisfy((e) => isCheckViolation(e));
    await pay(o);
    await expect(setStatus(o.id, 'pending')).rejects.toSatisfy((e) => isCheckViolation(e));
    await expect(setStatus(o.id, 'failed')).rejects.toSatisfy((e) => isCheckViolation(e));

    const failed = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    await sendWebhook(env, { type: 'payment.succeeded', orderId: failed.id, amountMinor: 1, currency: failed.currency }); // -> failed: amount_mismatch
    await expect(db().execute(sql`update orders set status = 'paid', paid_at = now() where id = ${failed.id}`)).rejects.toSatisfy((e) => isCheckViolation(e, 'illegal order status transition'));
    await expect(setStatus(failed.id, 'refunded')).rejects.toSatisfy((e) => isCheckViolation(e));
    await expect(setStatus(failed.id, 'pending')).rejects.toSatisfy((e) => isCheckViolation(e));

    // The one allowed way out of "failed": a payment after our expiry or an earlier failed attempt.
    const retried = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    await sendWebhook(env, { type: 'payment.failed', orderId: retried.id });
    await db().execute(sql`update orders set status = 'paid', paid_at = now(), failure_reason = null where id = ${retried.id}`);
    expect((await orderRow(retried.id))?.status).toBe('paid');

    const refunded = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    await pay(refunded);
    await sendWebhook(env, { type: 'refund.succeeded', orderId: refunded.id, amountMinor: refunded.totalMinor });
    await expect(setStatus(refunded.id, 'paid')).rejects.toSatisfy((e) => isCheckViolation(e));
    await expect(setStatus(refunded.id, 'chargeback')).rejects.toSatisfy((e) => isCheckViolation(e));
  });

  it('order lines cannot be edited and a line cannot carry another currency than its order', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    await expect(db().execute(sql`update order_items set unit_price_minor = 1 where order_id = ${o.id}`)).rejects.toBeDefined();
    await expect(
      db().insert(schema.orderItems).values({ orderId: o.id, lineNo: 9, productId: euro.id, sku: 'eur-thing', name: 'x', kind: 'cosmetic', unitPriceMinor: 100, currency: 'EUR', quantity: 1, grantPayload: {} }),
    ).rejects.toBeDefined();
  });
});

describe('wallet', () => {
  const spend = (u: TestUser, amount: number, key: string, currency = 'coins') =>
    asServer(env, srv)<{ replayed: boolean; balance: number } & ErrBody>('/internal/game/wallet/spend', { json: { accountId: u.accountId, currency, amount, idempotencyKey: key } });

  async function fundedPlayer(coinsQty: number): Promise<TestUser> {
    const u = await makeUser(env);
    await pay(await buy(u, [{ sku: 'coins-1000', quantity: coinsQty }]));
    await joinServer(env, u, srv);
    return u;
  }

  it('balances are the sum of the ledger; spends are idempotent per key', async () => {
    const u = await fundedPlayer(1);
    expect((await spend(u, 300, 'wallet-key-0001')).body).toEqual({ replayed: false, balance: 700 });
    expect((await spend(u, 300, 'wallet-key-0001')).body).toEqual({ replayed: true, balance: 700 });
    expect((await spend(u, 250, 'wallet-key-0002')).body).toEqual({ replayed: false, balance: 450 });
    expect((await spend(u, 999, 'wallet-key-0001')).body.error.code).toBe('idempotency_mismatch');

    const ledger = await ledgerRows(u.accountId);
    expect(ledger.reduce((s, l) => s + l.delta, 0)).toBe(450);
    expect(await balanceOf(u)).toBe(450);
    const viaServer = await asServer(env, srv)<{ balances: { currency: string; balance: number }[] }>(`/internal/game/wallet?accountId=${u.accountId}`);
    expect(viaServer.body.balances).toEqual([{ currency: 'coins', balance: 450 }]);
    const viaPost = await asServer(env, srv)<{ balances: unknown[] }>('/internal/game/wallet/balance', { json: { accountId: u.accountId } });
    expect(viaPost.body.balances).toHaveLength(1);

    const history = await env.call<{ entries: { delta: number; reason: string }[] }>('/v1/me/wallet/ledger', { token: u.token });
    expect(history.body.entries.map((e) => [e.reason, e.delta])).toEqual([['spend', -250], ['spend', -300], ['purchase', 1000]]);
    const page = await env.call<{ entries: { id: number }[] }>(`/v1/me/wallet/ledger?limit=1`, { token: u.token });
    const next = await env.call<{ entries: { delta: number }[] }>(`/v1/me/wallet/ledger?limit=5&before=${page.body.entries[0]?.id}`, { token: u.token });
    expect(next.body.entries.map((e) => e.delta)).toEqual([-300, 1000]);
  });

  it('refuses an overdraw with the balance and spends nothing', async () => {
    const u = await fundedPlayer(1);
    const res = await spend(u, 1001, 'wallet-key-0003');
    expect(res.status).toBe(409);
    expect(res.body.error).toMatchObject({ code: 'insufficient_funds', details: { balance: 1000 } });
    expect(await balanceOf(u)).toBe(1000);
    expect((await spend(u, 1000, 'wallet-key-0004')).body.balance).toBe(0);
  });

  it('concurrent spends can never overdraw', async () => {
    const u = await fundedPlayer(1);
    const results = await Promise.all(Array.from({ length: 10 }, (_, i) => spend(u, 300, `wallet-race-${i}-0001`)));
    expect(results.filter((r) => r.status === 200)).toHaveLength(3);
    expect(results.filter((r) => r.status === 409)).toHaveLength(7);
    expect(await balanceOf(u)).toBe(100);
  });

  it('needs a game session on that server, a valid key and positive integer amount', async () => {
    const u = await makeUser(env);
    expect((await spend(u, 1, 'wallet-key-0005')).body.error.code).toBe('no_active_session');
    const p = await fundedPlayer(1);
    expect((await spend(p, 0, 'wallet-key-0006')).status).toBe(400);
    expect((await spend(p, 1.5, 'wallet-key-0006')).status).toBe(400);
    expect((await spend(p, 1, 'short')).status).toBe(400);
    expect((await spend(p, 1, 'wallet-key-0007', 'Coins')).status).toBe(400);
  });

  it('every account balance equals the sum of its ledger (nothing writes a balance except the ledger)', async () => {
    const mismatches = await db().execute(sql`
      select b.account_id, b.currency, b.balance, coalesce(l.total, 0) as total
      from wallet_balances b
      left join (select account_id, currency, sum(delta)::bigint as total from wallet_ledger group by 1, 2) l
        on l.account_id = b.account_id and l.currency = b.currency
      where b.balance <> coalesce(l.total, 0)`);
    expect((mismatches as unknown as { rows: unknown[] }).rows).toEqual([]);
    const count = await db().execute(sql`select count(*)::int as n from wallet_balances`);
    expect(((count as unknown as { rows: { n: number }[] }).rows[0]?.n ?? 0)).toBeGreaterThan(3);
  });
});

describe('housekeeping', () => {
  it('pending orders expire after ORDER_TTL_MINUTES; a payment that still arrives is honoured (money in, goods out)', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'hat-red', quantity: 1 }]);
    const unpaid = await buy(u, [{ sku: 'vip-30', quantity: 1 }]);
    env.clock.advance(1441 * 60_000);
    const report = await runMaintenance(env.deps);
    expect(report.expiredOrders).toBeGreaterThanOrEqual(2);
    expect(await orderRow(o.id)).toMatchObject({ status: 'failed', failureReason: 'expired' });
    expect(await grantRows(o.id)).toHaveLength(0);
    expect((await pay(o)).body.result).toBe('applied');
    expect(await orderRow(o.id)).toMatchObject({ status: 'paid', failureReason: null });
    expect(await grantRows(o.id)).toHaveLength(1);
    // The one nobody paid stays expired.
    expect(await orderRow(unpaid.id)).toMatchObject({ status: 'failed', failureReason: 'expired' });
    expect(await grantRows(unpaid.id)).toHaveLength(0);
  });
});

describe('the dev fake checkout', () => {
  it('pays an order through the real webhook path', async () => {
    const u = await makeUser(env);
    const o = await buy(u, [{ sku: 'coins-1000', quantity: 1 }]);
    const info = await env.call<{ orderId: string; complete: { url: string } }>(`/v1/dev/fake-checkout/${o.id}`);
    expect(info.body.orderId).toBe(o.id);
    const done = await env.call<{ result: string }>(`/v1/dev/fake-checkout/${o.id}/complete`, { method: 'POST', json: { outcome: 'succeeded' } });
    expect(done.body.result).toBe('applied');
    expect(await balanceOf(u)).toBe(1000);
  });

  it('is not mounted in production', async () => {
    const prod = await createTestEnv({ env: { NODE_ENV: 'production', ENABLE_DEV_ROUTES: '0', DATABASE_URL: 'postgres://unused/unused', ALLOW_FAKE_PAYMENTS_IN_PRODUCTION: '1' } });
    try {
      expect((await prod.call('/v1/dev/fake-checkout/11111111-1111-4111-8111-111111111111')).status).toBe(404);
    } finally {
      await prod.close();
    }
  });
});
