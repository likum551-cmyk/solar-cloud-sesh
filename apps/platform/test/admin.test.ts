import { and, eq } from 'drizzle-orm';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { schema } from '../src/db/index.js';
import { asServer, createTestEnv, freshIp, joinServer, makeProduct, makeServer, makeStaff, makeUser, sendWebhook, type TestEnv, type TestUser } from './helpers.js';

let env: TestEnv;
let admin: TestUser;
let mod: TestUser;

beforeAll(async () => {
  env = await createTestEnv();
  admin = await makeStaff(env, 'admin', { username: 'Boss_Admin' });
  mod = await makeStaff(env, 'mod', { username: 'Helper_Mod' });
});
afterAll(async () => {
  await env.close();
});

type ErrBody = { error: { code: string; message: string; details?: Record<string, unknown> } };
interface BanBody { ban: { id: string; reason: string; createdAt: string; expiresAt: string | null } }
interface AuditEntry { id: number; at: string; actorAccountId: string | null; actorLabel: string; action: string; targetType: string; targetId: string; details: Record<string, unknown>; ip: string | null; requestId: string | null }
interface AdminAccount { id: string; username: string; email: string | null; emailVerified: boolean; roles: string[]; ban: { reason: string; expiresAt: string | null } | null; lastLoginAt: string | null }

const ban = (by: TestUser, target: string, json: Record<string, unknown> = { reason: 'cheating' }) => env.call<BanBody & ErrBody>(`/v1/admin/accounts/${target}/ban`, { token: by.token, json });
const unban = (by: TestUser, target: string, json: Record<string, unknown> = {}) => env.call<{ ok?: boolean } & ErrBody>(`/v1/admin/accounts/${target}/unban`, { token: by.token, json });
const setRoles = (by: TestUser, target: string, roles: unknown) => env.call<{ roles: string[] } & ErrBody>(`/v1/admin/accounts/${target}/roles`, { method: 'PUT', token: by.token, json: { roles } });
const accountView = async (id: string) => (await env.call<{ account: AdminAccount }>(`/v1/admin/accounts/${id}`, { token: admin.token })).body.account;
const auditCount = async () => (await env.handle.db.select().from(schema.auditLog)).length;

describe('who may call what', () => {
  it('needs a session; players get 403; mods reach accounts and bans only; admins reach everything', async () => {
    const player = await makeUser(env);
    const routes: { method: string; path: string; json?: unknown; need: 'mod' | 'admin' }[] = [
      { method: 'GET', path: '/v1/admin/accounts?q=a', need: 'mod' },
      { method: 'GET', path: `/v1/admin/accounts/${player.accountId}`, need: 'mod' },
      { method: 'POST', path: `/v1/admin/accounts/${player.accountId}/unban`, json: {}, need: 'mod' },
      { method: 'PUT', path: `/v1/admin/accounts/${player.accountId}/roles`, json: { roles: [] }, need: 'admin' },
      { method: 'GET', path: '/v1/admin/audit', need: 'admin' },
      { method: 'GET', path: '/v1/admin/servers', need: 'admin' },
      { method: 'GET', path: '/v1/admin/products', need: 'admin' },
      { method: 'GET', path: '/v1/admin/orders', need: 'admin' },
    ];
    for (const r of routes) {
      const base = { method: r.method, ...(r.json === undefined ? {} : { json: r.json }) };
      expect((await env.call(r.path, base)).status, `${r.method} ${r.path} anonymous`).toBe(401);
      expect((await env.call(r.path, { ...base, token: player.token })).status, `${r.method} ${r.path} player`).toBe(403);
      const asMod = (await env.call(r.path, { ...base, token: mod.token })).status;
      if (r.need === 'admin') expect(asMod, `${r.method} ${r.path} mod`).toBe(403);
      else expect(asMod, `${r.method} ${r.path} mod`).not.toBe(403);
      expect((await env.call(r.path, { ...base, token: admin.token })).status, `${r.method} ${r.path} admin`).not.toBe(403);
    }
  });

  it('roles are read on every request: a promotion or demotion takes effect on the existing session', async () => {
    const u = await makeUser(env);
    expect((await env.call('/v1/admin/accounts?q=a', { token: u.token })).status).toBe(403);
    expect((await setRoles(admin, u.accountId, ['mod'])).body.roles).toEqual(['player', 'mod']);
    expect((await env.call('/v1/admin/accounts?q=a', { token: u.token })).status).toBe(200);
    expect((await setRoles(admin, u.accountId, [])).body.roles).toEqual(['player']);
    expect((await env.call('/v1/admin/accounts?q=a', { token: u.token })).status).toBe(403);
  });
});

describe('bans', () => {
  it('records reason, expiry and who banned; the account sees it; joining and checkout are refused', async () => {
    const target = await makeUser(env);
    const srv = await makeServer(env, admin);
    const res = await ban(mod, target.accountId, { reason: '  aimbot in ranked  ', durationSeconds: 3600 });
    expect(res.status).toBe(201);
    expect(res.body.ban.reason).toBe('aimbot in ranked');
    expect(new Date(res.body.ban.expiresAt as string).getTime() - env.clock.now().getTime()).toBe(3_600_000);

    const [row] = await env.handle.db.select().from(schema.bans).where(eq(schema.bans.id, res.body.ban.id));
    expect(row).toMatchObject({ accountId: target.accountId, createdBy: mod.accountId, revokedAt: null });

    const me = await env.call<{ account: { ban: { reason: string } | null } }>('/v1/me', { token: target.token });
    expect(me.body.account.ban?.reason).toBe('aimbot in ranked'); // logging in stays possible so the player can see why
    const join = await env.call<ErrBody>('/v1/game/join', { token: target.token, json: { serverId: srv.id } });
    expect(join.status).toBe(403);
    expect(join.body.error.code).toBe('account_banned');
    expect(join.body.error.details).toMatchObject({ reason: 'aimbot in ranked' });
    expect((await accountView(target.accountId)).ban?.reason).toBe('aimbot in ranked');
  });

  it('a temporary ban ends by itself; a new one can then be issued', async () => {
    const target = await makeUser(env);
    const srv = await makeServer(env, admin);
    await ban(mod, target.accountId, { reason: 'first', durationSeconds: 120 });
    expect((await ban(mod, target.accountId, { reason: 'second' })).body.error.code).toBe('already_banned');
    env.clock.advanceSeconds(121);
    expect((await accountView(target.accountId)).ban).toBeNull();
    expect((await env.call('/v1/game/join', { token: target.token, json: { serverId: srv.id } })).status).toBe(200);
    const again = await ban(mod, target.accountId, { reason: 'second' });
    expect(again.status).toBe(201);
    expect(again.body.ban.expiresAt).toBeNull(); // permanent
    const rows = await env.handle.db.select().from(schema.bans).where(eq(schema.bans.accountId, target.accountId));
    expect(rows.map((r) => [r.reason, r.revokedReason]).sort()).toEqual([['first', 'expired'], ['second', null]]);
  });

  it('validates the request and the target', async () => {
    const target = await makeUser(env);
    expect((await ban(mod, target.accountId, {})).status).toBe(400);
    expect((await ban(mod, target.accountId, { reason: '   ' })).status).toBe(400);
    expect((await ban(mod, target.accountId, { reason: 'x'.repeat(501) })).status).toBe(400);
    expect((await ban(mod, target.accountId, { reason: 'short', durationSeconds: 10 })).status).toBe(400);
    expect((await ban(mod, target.accountId, { reason: 'long', durationSeconds: 1.5 })).status).toBe(400);
    expect((await ban(mod, '11111111-1111-4111-8111-111111111111')).body.error.code).toBe('account_not_found');
    expect((await ban(mod, 'not-a-uuid')).status).toBe(400);
    expect((await accountView(target.accountId)).ban).toBeNull();
  });

  it('who may ban whom: mods ban players; admins also ban mods; nobody bans an admin or themselves', async () => {
    const player = await makeUser(env);
    const otherMod = await makeStaff(env, 'mod');
    const otherAdmin = await makeStaff(env, 'admin');
    const refused = async (by: TestUser, target: TestUser) => {
      const res = await ban(by, target.accountId);
      expect(res.status).toBe(403);
      expect(res.body.error.code).toBe('forbidden_target');
    };
    await refused(mod, mod);
    await refused(mod, otherMod);
    await refused(mod, admin);
    await refused(admin, otherAdmin);
    await refused(admin, admin);
    expect((await ban(admin, otherMod.accountId)).status).toBe(201);
    expect((await ban(mod, player.accountId)).status).toBe(201);
  });

  it('unban lifts the ban, records who and why, and cannot be repeated', async () => {
    const target = await makeUser(env);
    const b = await ban(mod, target.accountId);
    expect((await unban(admin, target.accountId, { reason: 'appeal accepted' })).body).toEqual({ ok: true });
    const [row] = await env.handle.db.select().from(schema.bans).where(eq(schema.bans.id, b.body.ban.id));
    expect(row).toMatchObject({ revokedBy: admin.accountId, revokedReason: 'appeal accepted' });
    expect(row?.revokedAt).not.toBeNull();
    expect((await accountView(target.accountId)).ban).toBeNull();
    expect((await unban(admin, target.accountId)).body.error.code).toBe('not_banned');
    // The reason is optional, and the body may be absent altogether.
    await ban(mod, target.accountId);
    expect((await env.call(`/v1/admin/accounts/${target.accountId}/unban`, { method: 'POST', token: mod.token })).status).toBe(200);
  });

  it('two simultaneous bans produce one ban in force', async () => {
    const target = await makeUser(env);
    const results = await Promise.all([ban(mod, target.accountId, { reason: 'a' }), ban(admin, target.accountId, { reason: 'b' })]);
    expect(results.map((r) => r.status).sort()).toEqual([201, 409]);
    const open = (await env.handle.db.select().from(schema.bans).where(eq(schema.bans.accountId, target.accountId))).filter((r) => r.revokedAt === null);
    expect(open).toHaveLength(1);
  });

  it('a ban closes the player\'s game session at once (end reason banned) and they cannot come back', async () => {
    const target = await makeUser(env);
    const srv = await makeServer(env, admin);
    await joinServer(env, target, srv);
    await ban(mod, target.accountId);
    // The platform ends the session itself; the game server learns of it at its next call for the account (spend/pull answer 403 account_banned).
    const sessions = await env.handle.db.select().from(schema.gameSessions).where(eq(schema.gameSessions.accountId, target.accountId));
    expect(sessions.filter((s) => s.endedAt === null)).toHaveLength(0);
    expect(sessions.map((s) => s.endReason)).toEqual(['banned']);
    expect((await asServer(env, srv)('/internal/game/session-end', { json: { accountId: target.accountId } })).body).toEqual({ ended: 0 });
    expect((await env.call<ErrBody>('/v1/game/join', { token: target.token, json: { serverId: srv.id } })).body.error.code).toBe('account_banned');
  });
});

describe('roles', () => {
  it('replaces the set, always keeps player, ignores nothing silently and rejects unknown roles', async () => {
    const u = await makeUser(env);
    expect((await setRoles(admin, u.accountId, ['admin', 'mod', 'mod'])).body.roles).toEqual(['player', 'mod', 'admin']);
    expect((await accountView(u.accountId)).roles).toEqual(['player', 'mod', 'admin']);
    expect((await setRoles(admin, u.accountId, ['mod'])).body.roles).toEqual(['player', 'mod']);
    expect((await setRoles(admin, u.accountId, ['player'])).body.roles).toEqual(['player']);
    expect((await setRoles(admin, u.accountId, ['superuser'])).status).toBe(400);
    expect((await setRoles(admin, u.accountId, 'admin')).status).toBe(400);
    expect((await setRoles(admin, u.accountId, ['player', 'mod', 'admin', 'mod'])).status).toBe(400); // more than 3
    expect((await setRoles(admin, '11111111-1111-4111-8111-111111111111', [])).body.error.code).toBe('account_not_found');
    const rows = await env.handle.db.select().from(schema.accountRoles).where(eq(schema.accountRoles.accountId, u.accountId));
    expect(rows.map((r) => r.role)).toEqual(['player']);
  });

  it('mods cannot change roles, not even their own', async () => {
    const res = await setRoles(mod, mod.accountId, ['admin']);
    expect(res.status).toBe(403);
    expect((await accountView(mod.accountId)).roles).toEqual(['player', 'mod']);
  });

  it('the last admin cannot be demoted, not even by themselves; two admins demoting each other cannot both succeed', async () => {
    const solo = await createTestEnv();
    try {
      const only = await makeStaff(solo, 'admin');
      const call = (by: TestUser, target: string, roles: string[]) => solo.call<{ roles: string[] } & ErrBody>(`/v1/admin/accounts/${target}/roles`, { method: 'PUT', token: by.token, json: { roles } });
      const refused = await call(only, only.accountId, []);
      expect(refused.status).toBe(409);
      expect(refused.body.error.code).toBe('last_admin');
      expect(await solo.handle.db.select().from(schema.accountRoles).where(eq(schema.accountRoles.role, 'admin'))).toHaveLength(1);

      const second = await makeStaff(solo, 'admin');
      const race = await Promise.all([call(only, second.accountId, ['mod']), call(second, only.accountId, ['mod'])]);
      expect(race.map((r) => r.status).sort()).toEqual([200, 409]);
      const admins = await solo.handle.db.select().from(schema.accountRoles).where(eq(schema.accountRoles.role, 'admin'));
      expect(admins).toHaveLength(1);
    } finally {
      await solo.close();
    }
  });
});

describe('searching accounts', () => {
  it('matches a username prefix case-insensitively or an exact email, sorted, without secrets', async () => {
    const a = await makeUser(env, { username: 'Zed_Search_B' });
    const b = await makeUser(env, { username: 'zed_search_a' });
    await makeUser(env, { username: 'Other_Person_9' });
    const byPrefix = await env.call<{ accounts: AdminAccount[] }>('/v1/admin/accounts?q=ZED_SEARCH', { token: mod.token });
    expect(byPrefix.body.accounts.map((x) => x.username)).toEqual(['Zed_Search_B', 'zed_search_a'].sort());
    const byEmail = await env.call<{ accounts: AdminAccount[] }>(`/v1/admin/accounts?q=${encodeURIComponent(a.email.toUpperCase())}`, { token: mod.token });
    expect(byEmail.body.accounts.map((x) => x.id)).toEqual([a.accountId]);
    expect(byEmail.body.accounts[0]).toMatchObject({ emailVerified: true, roles: ['player'], ban: null });
    expect(byEmail.body.accounts[0]?.lastLoginAt).not.toBeNull();
    expect(JSON.stringify(byPrefix.body)).not.toMatch(/password|hash/i);
    expect(b.accountId).toBeTruthy();
    // An email is matched exactly, not by prefix.
    expect((await env.call<{ accounts: unknown[] }>(`/v1/admin/accounts?q=${encodeURIComponent(a.email.slice(0, -5))}`, { token: mod.token })).body.accounts).toEqual([]);
  });

  it('treats LIKE wildcards as plain characters and caps the page', async () => {
    const wild = await env.call<{ accounts: unknown[] }>('/v1/admin/accounts?q=%25', { token: mod.token });
    expect(wild.body.accounts).toEqual([]);
    const underscore = await env.call<{ accounts: unknown[] }>('/v1/admin/accounts?q=_', { token: mod.token });
    expect(underscore.body.accounts.length).toBeLessThan(5); // usernames that literally start with "_": none
    expect((await env.call('/v1/admin/accounts', { token: mod.token })).status).toBe(400);
    expect((await env.call('/v1/admin/accounts?q=a&limit=51', { token: mod.token })).status).toBe(400);
    const limited = await env.call<{ accounts: unknown[] }>('/v1/admin/accounts?q=Player&limit=2', { token: mod.token });
    expect(limited.body.accounts).toHaveLength(2);
  });

  it('shows one account or 404, and shows an active ban', async () => {
    const u = await makeUser(env);
    await ban(mod, u.accountId, { reason: 'visible', durationSeconds: 600 });
    const one = await env.call<{ account: AdminAccount }>(`/v1/admin/accounts/${u.accountId}`, { token: mod.token });
    expect(one.body.account).toMatchObject({ username: u.username, ban: { reason: 'visible' } });
    expect((await env.call('/v1/admin/accounts/11111111-1111-4111-8111-111111111111', { token: mod.token })).status).toBe(404);
  });
});

describe('the audit log', () => {
  it('has exactly one row per admin action, with actor, target, details, address and request id', async () => {
    const player = await makeUser(env);
    const buyer = await makeUser(env);
    const seen: { action: string; target: string }[] = [];
    const step = async (label: string, fn: () => Promise<{ status: number }>, expectAction: string, target: () => string) => {
      const before = await auditCount();
      const res = await fn();
      expect(res.status, label).toBeLessThan(300);
      expect(await auditCount(), `${label} writes exactly one audit row`).toBe(before + 1);
      seen.push({ action: expectAction, target: target() });
    };
    let banId = '';
    await step('ban', async () => { const r = await ban(mod, player.accountId, { reason: 'audit me' }); banId = r.body.ban?.id ?? ''; return r; }, 'account.ban', () => player.accountId);
    await step('unban', () => unban(mod, player.accountId, { reason: 'ok' }), 'account.unban', () => player.accountId);
    await step('roles', () => setRoles(admin, player.accountId, ['mod']), 'account.roles', () => player.accountId);
    await setRoles(admin, player.accountId, []);
    const server = await makeServer(env, admin, { id: 'audit-srv' });
    await step('server.update', () => env.call('/v1/admin/servers/audit-srv', { method: 'PATCH', token: admin.token, json: { name: 'Renamed' } }), 'server.update', () => 'audit-srv');
    await step('server.rotate', () => env.call('/v1/admin/servers/audit-srv/rotate-secret', { method: 'POST', token: admin.token }), 'server.rotate_secret', () => 'audit-srv');
    await step('server.delete', () => env.call('/v1/admin/servers/audit-srv', { method: 'DELETE', token: admin.token }), 'server.delete', () => 'audit-srv');
    let productId = '';
    await step('product.create', async () => {
      const r = await env.call<{ product: { id: string } }>('/v1/admin/products', { token: admin.token, json: { sku: 'audit-sku', name: 'A', priceMinor: 100, currency: 'USD', kind: 'cosmetic', grantPayload: { grants: [{ type: 'item', key: 'a.b' }] } } });
      productId = r.body.product?.id ?? '';
      return r;
    }, 'product.create', () => productId);
    await step('product.update', () => env.call(`/v1/admin/products/${productId}`, { method: 'PATCH', token: admin.token, json: { name: 'B' } }), 'product.update', () => productId);
    const checkout = await env.call<{ order: { id: string; totalMinor: number; currency: string } }>('/v1/shop/checkout', { token: buyer.token, json: { items: [{ sku: 'audit-sku', quantity: 1 }] }, headers: { 'idempotency-key': 'audit-key-0001' }, ip: freshIp() });
    await sendWebhook(env, { type: 'payment.succeeded', orderId: checkout.body.order.id, amountMinor: checkout.body.order.totalMinor, currency: checkout.body.order.currency });
    await step('order.refund', () => env.call(`/v1/admin/orders/${checkout.body.order.id}/refund`, { token: admin.token, json: { reason: 'audit' } }), 'order.refund', () => checkout.body.order.id);
    const unused = await makeProduct(env, admin, { sku: 'audit-unused', kind: 'cosmetic', grants: [{ type: 'item', key: 'a.c' }] });
    await step('product.delete', () => env.call(`/v1/admin/products/${unused.id}`, { method: 'DELETE', token: admin.token }), 'product.delete', () => unused.id);

    const log = await env.call<{ entries: AuditEntry[] }>('/v1/admin/audit?limit=200', { token: admin.token });
    for (const s of seen) {
      const entry = log.body.entries.find((e) => e.action === s.action && e.targetId === s.target);
      expect(entry, `${s.action} ${s.target}`).toBeDefined();
      expect(entry).toMatchObject({ actorAccountId: expect.any(String), ip: '203.0.113.7' });
      expect(entry?.actorLabel).toBe(`account:${entry?.actorAccountId}`);
      expect(entry?.requestId).toMatch(/^[0-9a-f-]{36}$/);
    }
    const banEntry = log.body.entries.find((e) => e.action === 'account.ban' && e.targetId === player.accountId);
    expect(banEntry?.actorAccountId).toBe(mod.accountId);
    expect(banEntry?.details).toMatchObject({ reason: 'audit me', username: player.username, expiresAt: null });
    expect(banId).toBeTruthy();
    // Newest first: the earliest roles change for this player is the last match.
    const rolesEntry = log.body.entries.filter((e) => e.action === 'account.roles' && e.targetId === player.accountId).at(-1);
    expect(rolesEntry?.details).toMatchObject({ before: ['player'], after: ['player', 'mod'] });
    expect(server.id).toBe('audit-srv');
    expect(JSON.stringify(log.body)).not.toContain(server.secret); // secrets never reach the log
  });

  it('refused actions leave no record (nothing changed)', async () => {
    const player = await makeUser(env);
    const before = await auditCount();
    expect((await ban(mod, admin.accountId)).status).toBe(403);
    expect((await unban(mod, player.accountId)).status).toBe(404);
    expect((await setRoles(mod, player.accountId, ['mod'])).status).toBe(403);
    expect((await ban(mod, player.accountId, {})).status).toBe(400);
    expect(await auditCount()).toBe(before);
  });

  it('pages newest first, filters by action and target, and is append-only', async () => {
    const t = await makeUser(env);
    await ban(mod, t.accountId, { reason: 'p1' });
    await unban(mod, t.accountId);
    await ban(mod, t.accountId, { reason: 'p2' });

    const forTarget = await env.call<{ entries: AuditEntry[] }>(`/v1/admin/audit?targetId=${t.accountId}`, { token: admin.token });
    expect(forTarget.body.entries.map((e) => e.action)).toEqual(['account.ban', 'account.unban', 'account.ban']);
    const ids = forTarget.body.entries.map((e) => e.id);
    expect([...ids].sort((x, y) => y - x)).toEqual(ids);

    const onlyBans = await env.call<{ entries: AuditEntry[] }>(`/v1/admin/audit?targetId=${t.accountId}&action=account.ban`, { token: admin.token });
    expect(onlyBans.body.entries).toHaveLength(2);
    const page1 = await env.call<{ entries: AuditEntry[] }>(`/v1/admin/audit?targetId=${t.accountId}&limit=2`, { token: admin.token });
    const page2 = await env.call<{ entries: AuditEntry[] }>(`/v1/admin/audit?targetId=${t.accountId}&limit=2&before=${page1.body.entries.at(-1)?.id}`, { token: admin.token });
    expect(page1.body.entries).toHaveLength(2);
    expect(page2.body.entries.map((e) => e.id)).toEqual([ids[2]]);
    expect((await env.call('/v1/admin/audit?limit=0', { token: admin.token })).status).toBe(400);
    expect((await env.call('/v1/admin/audit?limit=201', { token: admin.token })).status).toBe(400);

    const first = forTarget.body.entries[0] as AuditEntry;
    await expect(env.handle.db.update(schema.auditLog).set({ action: 'tampered' }).where(eq(schema.auditLog.id, first.id))).rejects.toBeDefined();
    await expect(env.handle.db.delete(schema.auditLog).where(and(eq(schema.auditLog.id, first.id)))).rejects.toBeDefined();
  });
});
