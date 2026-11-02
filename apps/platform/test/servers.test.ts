import { eq } from 'drizzle-orm';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { schema } from '../src/db/index.js';
import { createTestEnv, makeServer, makeStaff, makeUser, startFakeUdpServer, type FakeGameServerUdp, type TestEnv, type TestUser } from './helpers.js';

// Server list against a real UDP responder (the query of apps/master/PROTOCOL.md), real timers for the
// probe and a fake clock for the cache.

let env: TestEnv;
let admin: TestUser;
const udps: FakeGameServerUdp[] = [];
const resolved: string[] = [];

beforeAll(async () => {
  env = await createTestEnv({
    env: { PROBE_TIMEOUT_MS: '1000', PROBE_ATTEMPTS: '1', SERVER_STATUS_WAIT_MS: '200', SERVER_STATUS_TTL_MS: '5000' },
    resolveHost: (host) => {
      resolved.push(host);
      if (host === 'game.test') return Promise.resolve('127.0.0.1');
      return Promise.reject(new Error('NXDOMAIN'));
    },
  });
  admin = await makeStaff(env, 'admin');
});
afterAll(async () => {
  await Promise.all(udps.map((u) => u.close()));
  await env.close();
});

interface ListedServer {
  id: string;
  name: string;
  host: string;
  port: number;
  sort: number;
  maintenance: { enabled: boolean; message: string | null };
  status: { state: string; players: number | null; maxPlayers: number | null; pingMs: number | null; protocol: number | null; checkedAt: string | null; detail?: string };
}
const list = async () => (await env.call<{ servers: ListedServer[] }>('/v1/servers')).body.servers;
const find = async (id: string) => (await list()).find((s) => s.id === id) as ListedServer;
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
async function waitFor<T>(what: string, probe: () => Promise<T | undefined>, timeoutMs = 4000): Promise<T> {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = await probe();
    if (v !== undefined) return v;
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await sleep(25);
  }
}

async function udpServer(init?: { players?: number; maxPlayers?: number }) {
  const udp = await startFakeUdpServer(init);
  udps.push(udp);
  return udp;
}

describe('GET /v1/servers', () => {
  it('lists servers anonymously with live players, max players, ping and protocol from the UDP query', async () => {
    const udp = await udpServer({ players: 7, maxPlayers: 48 });
    const s = await makeServer(env, admin, { port: udp.port });
    const row = await find(s.id);
    expect(row.status.state).toBe('online');
    expect(row.status).toMatchObject({ players: 7, maxPlayers: 48, protocol: 1 });
    expect(typeof row.status.pingMs).toBe('number');
    expect(row.status.checkedAt).not.toBeNull();
    expect(row.maintenance).toEqual({ enabled: false, message: null });
    expect(JSON.stringify(row)).not.toContain('secret');
    expect(JSON.stringify(row)).not.toContain('allowedIps');
  });

  it('sorts by sort then name and hides hidden servers (admins still see them)', async () => {
    const udp = await udpServer();
    const a = await makeServer(env, admin, { port: udp.port, id: 'sort-b' });
    const b = await makeServer(env, admin, { port: udp.port, id: 'sort-a' });
    const hidden = await makeServer(env, admin, { port: udp.port, id: 'sort-hidden', visible: false });
    await env.call(`/v1/admin/servers/${a.id}`, { method: 'PATCH', token: admin.token, json: { sort: -5, name: 'AAA first' } });
    await env.call(`/v1/admin/servers/${b.id}`, { method: 'PATCH', token: admin.token, json: { sort: -5, name: 'BBB second' } });
    const ids = (await list()).map((s) => s.id);
    expect(ids.indexOf(a.id)).toBeLessThan(ids.indexOf(b.id));
    expect(ids).not.toContain(hidden.id);
    const admins = await env.call<{ servers: { id: string }[] }>('/v1/admin/servers', { token: admin.token });
    expect(admins.body.servers.map((s) => s.id)).toContain(hidden.id);
  });

  it('caches for 5 s: repeated requests send no extra UDP; after the TTL it serves the old value and refreshes in the background', async () => {
    const udp = await udpServer({ players: 3 });
    const s = await makeServer(env, admin, { port: udp.port });
    expect((await find(s.id)).status.players).toBe(3);
    expect(udp.queries).toBe(1);

    for (let i = 0; i < 5; i++) await find(s.id);
    expect(udp.queries).toBe(1);

    udp.players = 11;
    env.clock.advance(4900);
    expect((await find(s.id)).status.players).toBe(3); // still inside the TTL
    expect(udp.queries).toBe(1);

    env.clock.advance(200); // 5.1 s: stale
    const stale = await find(s.id);
    expect(stale.status.players).toBe(3); // answered from the cache, not by waiting
    await waitFor('the background refresh', async () => (udp.queries === 2 ? true : undefined));
    const fresh = await waitFor('the refreshed value', async () => ((await find(s.id)).status.players === 11 ? true : undefined));
    expect(fresh).toBe(true);
    expect(udp.queries).toBe(2);
  });

  it('probes once however many requests arrive at the same time (single flight)', async () => {
    const udp = await udpServer();
    const s = await makeServer(env, admin, { port: udp.port });
    const answers = await Promise.all(Array.from({ length: 12 }, () => env.call<{ servers: ListedServer[] }>('/v1/servers')));
    expect(answers.every((a) => a.status === 200)).toBe(true);
    expect(udp.queries).toBe(1);
    expect((await find(s.id)).status.state).toBe('online');
  });

  it('never blocks on a slow server: it answers within the wait budget with "unknown", then reports "offline" once the probe times out', async () => {
    const quiet = await udpServer();
    quiet.respond = false;
    const lively = await udpServer({ players: 5 });
    const slow = await makeServer(env, admin, { port: quiet.port });
    const fast = await makeServer(env, admin, { port: lively.port });

    const t0 = Date.now();
    const first = await env.call<{ servers: ListedServer[] }>('/v1/servers');
    const took = Date.now() - t0;
    expect(took).toBeLessThan(700); // probe timeout is 1000 ms; the budget is 200 ms
    const byId = new Map(first.body.servers.map((s) => [s.id, s]));
    expect(byId.get(slow.id)?.status.state).toBe('unknown');
    expect(byId.get(fast.id)?.status).toMatchObject({ state: 'online', players: 5 });

    const offline = await waitFor('the timeout to be recorded', async () => {
      const row = await find(slow.id);
      return row.status.state === 'offline' ? row : undefined;
    });
    expect(offline.status).toMatchObject({ players: null, detail: 'timeout' });
    expect(quiet.queries).toBe(1);

    // It comes back: the next refresh after the TTL flips it to online without blocking a request.
    quiet.respond = true;
    env.clock.advance(5100);
    const t1 = Date.now();
    expect((await find(slow.id)).status.state).toBe('offline');
    expect(Date.now() - t1).toBeLessThan(300);
    await waitFor('the server to come back', async () => ((await find(slow.id)).status.state === 'online' ? true : undefined));
  });

  it('resolves host names (cached) and reports an unresolvable name as offline/dns', async () => {
    const udp = await udpServer({ players: 2 });
    const named = await makeServer(env, admin, { host: 'game.test', port: udp.port });
    const ghost = await makeServer(env, admin, { host: 'nxdomain.test', port: 24800 });
    expect((await find(named.id)).status).toMatchObject({ state: 'online', players: 2 });
    await waitFor('the dns failure', async () => ((await find(ghost.id)).status.detail === 'dns' ? true : undefined));
    expect((await find(ghost.id)).status.state).toBe('offline');
    const lookups = resolved.filter((h) => h === 'game.test').length;
    env.clock.advance(5100);
    await find(named.id);
    await waitFor('the refresh', async () => (udp.queries >= 2 ? true : undefined));
    expect(resolved.filter((h) => h === 'game.test').length).toBe(lookups); // answer reused inside the DNS TTL
  });

  it('shows the maintenance flag and message', async () => {
    const udp = await udpServer();
    const s = await makeServer(env, admin, { port: udp.port });
    await env.call(`/v1/admin/servers/${s.id}`, { method: 'PATCH', token: admin.token, json: { maintenance: true, maintenanceMessage: 'Updating to v2' } });
    expect((await find(s.id)).maintenance).toEqual({ enabled: true, message: 'Updating to v2' });
    await env.call(`/v1/admin/servers/${s.id}`, { method: 'PATCH', token: admin.token, json: { maintenance: false } });
    expect((await find(s.id)).maintenance).toEqual({ enabled: false, message: null });
  });

  it('something that answers with garbage is not "online"', async () => {
    const udp = await udpServer();
    udp.garbage = true;
    const s = await makeServer(env, admin, { port: udp.port });
    await find(s.id);
    const row = await waitFor('offline', async () => {
      const r = await find(s.id);
      return r.status.state === 'offline' ? r : undefined;
    });
    expect(row.status).toMatchObject({ players: null, detail: 'bad-reply' });
  });
});

describe('admin server CRUD', () => {
  const create = (json: Record<string, unknown>, token = admin.token) => env.call<{ server?: Record<string, unknown>; secret?: string; error?: { code: string } }>('/v1/admin/servers', { token, json });
  const valid = { name: 'S', region: 'eu', host: '203.0.113.9', port: 24800 };

  it('validates id, host, port, ips and tags', async () => {
    for (const bad of [
      { ...valid, id: 'Bad_Id' },
      { ...valid, id: 'x' },
      { ...valid, id: 'ok-id-1', port: 0 },
      { ...valid, id: 'ok-id-2', port: 70000 },
      { ...valid, id: 'ok-id-3', host: 'not a host' },
      { ...valid, id: 'ok-id-4', allowedIps: ['999.1.1.1'] },
      { ...valid, id: 'ok-id-5', tags: ['UPPER'] },
    ]) {
      const res = await create(bad);
      expect(res.status, JSON.stringify(bad)).toBe(400);
    }
  });

  it('creates with a one-time secret, normalises allowed IPs, rejects duplicates', async () => {
    const res = await create({ ...valid, id: 'crud-1', allowedIps: ['::ffff:203.0.113.9', '2001:DB8::1'] });
    expect(res.status).toBe(201);
    expect(res.body.secret).toMatch(/^gss_[A-Za-z0-9_-]{43}$/);
    expect(res.body.server).toMatchObject({ id: 'crud-1', allowedIps: ['203.0.113.9', '2001:db8::1'] });
    expect(JSON.stringify(res.body.server)).not.toContain('secretHash');
    expect((await create({ ...valid, id: 'crud-1' })).body.error?.code).toBe('server_exists');
    const [row] = await env.handle.db.select().from(schema.gameServers).where(eq(schema.gameServers.id, 'crud-1'));
    expect(row?.secretHash).toMatch(/^[0-9a-f]{64}$/);
  });

  it('PATCH changes only the fields sent (absent fields keep their values)', async () => {
    await create({ ...valid, id: 'crud-2', tags: ['roleplay'], sort: 4, visible: false, allowedIps: ['203.0.113.9'] });
    const res = await env.call<{ server: Record<string, unknown> }>('/v1/admin/servers/crud-2', { method: 'PATCH', token: admin.token, json: { name: 'Renamed' } });
    expect(res.status).toBe(200);
    expect(res.body.server).toMatchObject({ name: 'Renamed', tags: ['roleplay'], sort: 4, visible: false, allowedIps: ['203.0.113.9'], maintenance: false });
    expect((await env.call('/v1/admin/servers/crud-2', { method: 'PATCH', token: admin.token, json: {} })).status).toBe(400);
    expect((await env.call('/v1/admin/servers/nope-9', { method: 'PATCH', token: admin.token, json: { name: 'x' } })).status).toBe(404);
  });

  it('delete removes the server and its join tokens; every change is audited', async () => {
    await create({ ...valid, id: 'crud-3' });
    await env.call('/v1/admin/servers/crud-3/rotate-secret', { method: 'POST', token: admin.token });
    expect((await env.call('/v1/admin/servers/crud-3', { method: 'DELETE', token: admin.token })).status).toBe(204);
    expect((await env.call('/v1/admin/servers/crud-3', { method: 'DELETE', token: admin.token })).status).toBe(404);
    const audit = await env.handle.db.select().from(schema.auditLog).where(eq(schema.auditLog.targetId, 'crud-3'));
    expect(audit.map((a) => a.action).sort()).toEqual(['server.create', 'server.delete', 'server.rotate_secret']);
    expect(audit.every((a) => a.actorAccountId === admin.accountId && a.requestId)).toBe(true);
  });

  it('is admin only', async () => {
    const mod = await makeStaff(env, 'mod');
    const player = await makeUser(env);
    expect((await create({ ...valid, id: 'crud-4' }, mod.token)).status).toBe(403);
    expect((await create({ ...valid, id: 'crud-4' }, player.token)).status).toBe(403);
    expect((await env.call('/v1/admin/servers', { json: { ...valid, id: 'crud-4' } })).status).toBe(401);
    expect((await env.call('/v1/admin/servers', { token: mod.token })).status).toBe(403);
  });
});
