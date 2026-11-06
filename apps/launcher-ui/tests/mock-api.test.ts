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

  it('has no register route: accounts are created on the website', async () => {
    const res = await createMockFetch()('mock://platform/v1/auth/register', { method: 'POST', body: '{}' });
    expect(res.status).toBe(404);
  });

  it('lists the demo quests in the platform shape, Discord linked', async () => {
    const { api, setToken } = client();
    expect(await kindOf(api.quests())).toBe('unauthorized:unauthorized');
    setToken((await api.login({ identifier: 'nightshift', password: 'x' })).token);
    const list = await api.quests();
    expect(list.discordLinked).toBe(true);
    expect(list.visitMinSeconds).toBe(30);
    const by = Object.fromEntries(list.quests.map((q) => [q.slug, q]));
    expect(by.link_discord).toMatchObject({ kind: 'discord_link', status: 'claimable', reward: { currency: 'coins', amount: 500 }, url: null });
    expect(by.join_discord_server).toMatchObject({ status: 'locked', lockReason: 'not_available' });
    expect(by.youtube_subscribe).toMatchObject({ kind: 'visit', status: 'available', startedAt: null, waitSeconds: null });
    expect(by.instagram_follow?.status).toBe('claimed');
    expect(by.tiktok_follow?.status).toBe('started');
    expect(by.tiktok_follow?.waitSeconds).toBeGreaterThan(0);
    expect(list.quests.map((q) => q.sort)).toEqual([...list.quests.map((q) => q.sort)].sort((a, b) => a - b));
  });

  it('start then claim: quest_wait with Retry-After first, then the credit moves the wallet once', async () => {
    const { api, setToken } = client();
    setToken((await api.login({ identifier: 'nightshift', password: 'x' })).token);
    const before = (await api.wallet()).balances[0]!.balance;
    expect(await kindOf(api.claimQuest('youtube_subscribe'))).toBe('refused:quest_not_started');
    const { quest } = await api.startQuest('youtube_subscribe');
    expect(quest).toMatchObject({ status: 'started', waitSeconds: 30, url: 'https://solar-rp.com/' });
    const again = await api.startQuest('youtube_subscribe');
    expect(again.quest.startedAt).toBe(quest.startedAt);    // a repeated start keeps the first time
    const early = await api.claimQuest('youtube_subscribe').then(() => null, (e: unknown) => e);
    expect(early).toBeInstanceOf(ApiFailure);
    expect(early).toMatchObject({ kind: 'refused', code: 'quest_wait', status: 409, details: { waitSeconds: expect.any(Number) } });
    expect((early as ApiFailure).retryAfterSeconds).toBeGreaterThan(25);
    const paid = await api.claimQuest('link_discord');
    expect(paid.credited).toEqual({ currency: 'coins', amount: 500 });
    expect(paid.balance).toBe(before + 500);
    expect(paid.quest.status).toBe('claimed');
    expect(await kindOf(api.claimQuest('link_discord'))).toBe('refused:already_claimed');
    expect((await api.wallet()).balances[0]!.balance).toBe(before + 500);
    expect(await kindOf(api.startQuest('link_discord'))).toBe('refused:not_a_visit_quest');
    expect(await kindOf(api.claimQuest('join_discord_server'))).toBe('refused:quest_unavailable');
    expect(await kindOf(api.claimQuest('no_such_quest'))).toBe('refused:quest_not_found');
  });

  it('without Discord: the link quest and started visit quests are locked, a claim is discord_required', async () => {
    const { api, setToken } = client({ quests: 'nodiscord' });
    const login = await api.login({ identifier: 'nightshift', password: 'x' });
    expect(login.account.discord).toBeNull();
    setToken(login.token);
    const list = await api.quests();
    expect(list.discordLinked).toBe(false);
    const by = Object.fromEntries(list.quests.map((q) => [q.slug, q]));
    expect(by.link_discord).toMatchObject({ status: 'locked', lockReason: 'discord_required' });
    expect(by.tiktok_follow).toMatchObject({ status: 'locked', lockReason: 'discord_required' });
    expect(by.youtube_subscribe?.status).toBe('available');
    await expect(api.claimQuest('link_discord')).rejects.toMatchObject({ kind: 'refused', code: 'discord_required', status: 403 });
  });

  it('serves an empty quest list in the empty scenario', async () => {
    const { api, setToken } = client({ quests: 'empty' });
    setToken((await api.login({ identifier: 'nightshift', password: 'x' })).token);
    expect((await api.quests()).quests).toEqual([]);
  });

  it('fails the quest list as a server fault in the down scenario', async () => {
    const { api, setToken } = client({ quests: 'down' });
    setToken((await api.login({ identifier: 'nightshift', password: 'x' })).token);
    await expect(api.quests()).rejects.toMatchObject({ kind: 'server', status: 500 });
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
    expect(scenarioFromQuery('?servers=down&shop=empty&account=banned&net=offline&quests=nodiscord')).toEqual({ servers: 'down', shop: 'empty', account: 'banned', net: 'offline', quests: 'nodiscord' });
    expect(scenarioFromQuery('?servers=lol')).toEqual(DEFAULT_SCENARIO);
  });
});
