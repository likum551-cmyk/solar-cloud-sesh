import { asc, eq } from 'drizzle-orm';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { schema } from '../src/db/index.js';
import {
  asServer, createTestEnv, freshIp, makeProduct, makeServer, makeStaff, makeUser, sendWebhook,
  type TestEnv, type TestServer, type TestUser,
} from './helpers.js';

// Character slots (GI 2026-11-07: "1 character min, can buy additional 2 char slots"): the `character_slot` entitlement is applied
// by the platform at payment, counted (minus revokes) into the verify answer's characterSlots, capped at 1 + 2.

let env: TestEnv;
let admin: TestUser;
let srv: TestServer;

beforeAll(async () => {
  env = await createTestEnv();
  admin = await makeStaff(env, 'admin');
  srv = await makeServer(env, admin);
  await makeProduct(env, admin, { sku: 'char-slot', kind: 'vip', priceMinor: 399, maxQuantity: 2, grants: [{ type: 'entitlement', key: 'character_slot' }] });
  await makeProduct(env, admin, {
    sku: 'slot-bundle', kind: 'bundle', priceMinor: 999,
    grants: [{ type: 'entitlement', key: 'character_slot' }, { type: 'item', key: 'hat.blue' }],
  });
});
afterAll(async () => {
  await env.close();
});

type ErrBody = { error: { code: string; details?: Record<string, unknown> } };
interface OrderBody { id: string; totalMinor: number; currency: string }

let keyCounter = 0;
async function checkout(u: TestUser, sku: string, quantity = 1) {
  return env.call<{ order: OrderBody } & ErrBody>('/v1/shop/checkout', {
    token: u.token, json: { items: [{ sku, quantity }] }, headers: { 'idempotency-key': `slots-${++keyCounter}-${Math.random().toString(36).slice(2)}` }, ip: freshIp(),
  });
}
async function buyAndPay(u: TestUser, sku: string, quantity = 1): Promise<OrderBody> {
  const res = await checkout(u, sku, quantity);
  expect(res.status, JSON.stringify(res.body)).toBe(201);
  const pay = await sendWebhook(env, { type: 'payment.succeeded', orderId: res.body.order.id, amountMinor: res.body.order.totalMinor, currency: res.body.order.currency });
  expect(pay.body.result).toBe('applied');
  return res.body.order;
}
/** Join `u` on the server and return the verify answer's characterSlots. */
async function slotsAtJoin(u: TestUser): Promise<unknown> {
  const join = await env.call<{ token: string }>('/v1/game/join', { token: u.token, json: { serverId: srv.id } });
  expect(join.status, JSON.stringify(join.body)).toBe(200);
  const v = await asServer(env, srv)<{ ok: boolean; characterSlots?: unknown }>('/internal/game/verify', { json: { token: join.body.token } });
  expect(v.body.ok, JSON.stringify(v.body)).toBe(true);
  return v.body.characterSlots;
}
const orderStatus = async (id: string) => (await env.handle.db.select().from(schema.orders).where(eq(schema.orders.id, id)))[0]?.status;
const grantRows = (orderId: string) => env.handle.db.select().from(schema.grants).where(eq(schema.grants.orderId, orderId)).orderBy(asc(schema.grants.seq));

describe('character slots', () => {
  it('an account with nothing bought has 1 character slot in the verify answer', async () => {
    const u = await makeUser(env);
    expect(await slotsAtJoin(u)).toBe(1);
  });

  it('a paid slot is applied by the platform at once (born acked, order fulfilled, nothing queued for the game server) and counts at the next verify', async () => {
    const u = await makeUser(env);
    const o = await buyAndPay(u, 'char-slot');
    const rows = await grantRows(o.id);
    expect(rows.map((g) => [g.kind, g.type, g.status, g.ackedBy])).toEqual([['grant', 'entitlement', 'acked', 'platform']]);
    expect(await orderStatus(o.id)).toBe('fulfilled');
    expect(await slotsAtJoin(u)).toBe(2);
    const pulled = await asServer(env, srv)<{ grants: unknown[] }>(`/internal/game/grants?accountId=${u.accountId}`);
    expect(pulled.body.grants).toEqual([]);
  });

  it('two slots (quantity 2) give 3 characters; a third is refused at checkout with character_slots_full', async () => {
    const u = await makeUser(env);
    await buyAndPay(u, 'char-slot', 2);
    expect(await slotsAtJoin(u)).toBe(3);
    const more = await checkout(u, 'char-slot');
    expect(more.status).toBe(409);
    expect(more.body.error.code).toBe('character_slots_full');
    expect(more.body.error.details).toMatchObject({ owned: 2, max: 2 });
    const viaBundle = await checkout(u, 'slot-bundle');
    expect(viaBundle.body.error.code).toBe('character_slots_full');
  });

  it('a bundle delivers its slot through the platform and leaves only the item for the game server', async () => {
    const u = await makeUser(env);
    const o = await buyAndPay(u, 'slot-bundle');
    const rows = await grantRows(o.id);
    expect(rows.map((g) => [g.type, (g.payload as { key: string }).key, g.status]).sort()).toEqual([['entitlement', 'character_slot', 'acked'], ['item', 'hat.blue', 'pending']]);
    expect(await orderStatus(o.id)).toBe('paid'); // the hat still waits for the game server
    expect(await slotsAtJoin(u)).toBe(2);
  });

  it('a refund takes the slot back (the revoke is applied by the platform too)', async () => {
    const u = await makeUser(env);
    const o = await buyAndPay(u, 'char-slot');
    expect(await slotsAtJoin(u)).toBe(2);
    const refund = await sendWebhook(env, { type: 'refund.succeeded', orderId: o.id, amountMinor: o.totalMinor });
    expect(refund.body.result).toBe('applied');
    const rows = await grantRows(o.id);
    expect(rows.map((g) => [g.kind, g.status])).toEqual([['grant', 'acked'], ['revoke', 'acked']]);
    expect(await slotsAtJoin(u)).toBe(1);
    // ...and the slot can be bought again
    expect((await checkout(u, 'char-slot')).status).toBe(201);
  });

  it('never reports more than 3 even when two orders were paid past the cap', async () => {
    const u = await makeUser(env);
    const a = await checkout(u, 'char-slot', 2);
    const b = await checkout(u, 'char-slot', 1); // both created before either is paid: the up-front check passes both
    expect(a.status).toBe(201);
    expect(b.status).toBe(201);
    for (const o of [a.body.order, b.body.order]) await sendWebhook(env, { type: 'payment.succeeded', orderId: o.id, amountMinor: o.totalMinor, currency: o.currency });
    expect(await slotsAtJoin(u)).toBe(3);
  });

  it('a character_slot entitlement must be permanent', async () => {
    const res = await env.call<ErrBody>('/v1/admin/products', {
      token: admin.token,
      json: { sku: 'slot-30d', name: 'Slot', priceMinor: 100, currency: 'USD', kind: 'vip', grantPayload: { grants: [{ type: 'entitlement', key: 'character_slot', durationDays: 30 }] } },
    });
    expect(res.status).toBe(400);
  });
});
