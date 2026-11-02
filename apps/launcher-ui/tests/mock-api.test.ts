import { describe, expect, it } from 'vitest';
import { createApi } from '../src/lib/api';
import { ApiFailure } from '../src/lib/errors';
import { createMockFetch, DEFAULT_SCENARIO, scenarioFromQuery, type MockScenario } from '../src/lib/mock-api';

// The mock network must speak exactly the shapes the real client validates, or demo mode and the
// captures would show states the real API can never produce.

function client(scenario: Partial<MockScenario> = {}) {
  let token: string | null = null;
  const s: MockScenario = { ...DEFAULT_SCENARIO, ...scenario };
  const api = createApi({ baseUrl: 'mock://platform', getToken: () => token, fetchImpl: createMockFetch(() => s) });
  return { api, setToken: (t: string | null) => { token = t; } };
}

async function kindOf(p: Promise<unknown>): Promise<string> {
  try {
    await p;
    return 'resolved';
  } catch (e) {
    return e instanceof ApiFailure ? `${e.kind}:${e.code ?? ''}` : `threw:${String(e)}`;
  }
}

describe('mock platform through the real client', () => {
  it('logs in and then reads the account, wallet, items, orders and sessions', async () => {
    const { api, setToken } = client();
    const login = await api.login({ identifier: 'nightshift', password: 'correct horse' });
    setToken(login.token);
    expect((await api.me()).account.username).toBe(login.account.username);
    expect((await api.wallet()).balances.length).toBeGreaterThan(0);
    expect(Array.isArray((await api.items()).items)).toBe(true);
    const { orders } = await api.orders();
    expect(orders.length).toBe(6);
    // Every owned item points at an order on the record (the sheet and the ledger agree).
    const ids = new Set(orders.map((o) => o.id));
    const owned = (await api.items()).items;
    expect(owned.length).toBeGreaterThan(0);
    for (const it of owned) expect(ids.has(it.orderId ?? '')).toBe(true);
    expect((await api.sessions()).sessions.filter((x) => x.current)).toHaveLength(1);
  });

  it('refuses a wrong password, locks "locked*" with Retry-After 540, and rejects a stale token', async () => {
    const { api, setToken } = client();
    expect(await kindOf(api.login({ identifier: 'nightshift', password: 'wrong' }))).toBe('refused:invalid_credentials');
    await expect(api.login({ identifier: 'locked-user', password: 'x' })).rejects.toMatchObject({ kind: 'rate_limited', retryAfterSeconds: 540 });
    setToken('not-the-demo-token');
    expect(await kindOf(api.me())).toBe('unauthorized:unauthorized');
  });

  it('refuses a taken username and a short password on register', async () => {
    const { api } = client();
    expect(await kindOf(api.register({ email: 'a@b.c', username: 'taken', password: 'longenough1' }))).toBe('refused:username_taken');
    expect(await kindOf(api.register({ email: 'a@b.c', username: 'fresh', password: 'short' }))).toBe('refused:weak_password');
  });

  it('serves every server scenario in a valid shape', async () => {
    const ok = await client().api.servers();
    expect(ok.servers.length).toBe(3);   // SOL (live), LUNA and TERRA (coming soon)
    expect((await client({ servers: 'empty' }).api.servers()).servers).toHaveLength(0);
    const maint = await client({ servers: 'maintenance' }).api.servers();
    expect(maint.servers.some((s) => s.maintenance.enabled)).toBe(true);
    const down = await client({ servers: 'down' }).api.servers();
    expect(down.servers.every((s) => s.status.state !== 'online')).toBe(true);
  });

  it('serves the catalogue, an empty shop and a checkout that returns a pending order', async () => {
    const { api, setToken } = client();
    const products = (await api.products()).products;
    expect(products.length).toBe(5);
    expect((await client({ shop: 'empty' }).api.products()).products).toHaveLength(0);
    const login = await api.login({ identifier: 'nightshift', password: 'correct horse' });
    setToken(login.token);
    const first = products[0];
    if (!first) throw new Error('no demo product');
    const { order } = await api.checkout(first.sku, 1, 'key-aaaaaaaa');
    expect(order.status).toBe('pending');
    expect(order.checkoutUrl).toMatch(/^https:\/\//);
  });

  it('fails every request as offline in the offline scenario', async () => {
    expect(await kindOf(client({ net: 'offline' }).api.servers())).toBe('offline:');
  });

  it('reads scenarios from the query and ignores unknown values', () => {
    expect(scenarioFromQuery('?servers=down&shop=empty&account=banned&net=offline')).toEqual({ servers: 'down', shop: 'empty', account: 'banned', net: 'offline' });
    expect(scenarioFromQuery('?servers=lol')).toEqual(DEFAULT_SCENARIO);
  });
});
