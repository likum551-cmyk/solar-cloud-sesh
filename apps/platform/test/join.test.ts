import { eq } from 'drizzle-orm';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { schema } from '../src/db/index.js';
import { hashToken, randomToken } from '../src/util/tokens.js';
import { asServer, createTestEnv, joinServer, makeServer, makeStaff, makeUser, sha256Hex, type TestEnv, type TestServer, type TestUser } from './helpers.js';

let env: TestEnv;
let admin: TestUser;
let srv: TestServer;
beforeAll(async () => {
  env = await createTestEnv();
  admin = await makeStaff(env, 'admin');
  srv = await makeServer(env, admin);
});
afterAll(async () => {
  await env.close();
});

type ErrBody = { error: { code: string; message: string; details?: Record<string, unknown> } };
type JoinBody = { token: string; expiresAt: string; server: { id: string; host: string; port: number } };
type VerifyBody = { ok: boolean; reason?: string; accountId?: string; username?: string; roles?: string[]; emailVerified?: boolean; sessionId?: string; ban?: { reason: string; expiresAt: string | null } | null };

const join = (user: TestUser, serverId: string) => env.call<JoinBody & ErrBody>('/v1/game/join', { token: user.token, json: { serverId } });
const verify = (server: TestServer, token: string, ip = '127.0.0.1') => asServer(env, server, ip)<VerifyBody>('/internal/game/verify', { json: { token } });

describe('POST /v1/game/join', () => {
  it('issues a 60 s single-use token bound to the account and the server; only its hash is stored', async () => {
    const u = await makeUser(env);
    const res = await join(u, srv.id);
    expect(res.status).toBe(200);
    expect(res.body.token).toMatch(/^gmj_[A-Za-z0-9_-]{43}$/);
    expect(res.body.server).toMatchObject({ id: srv.id, host: '127.0.0.1', port: 24800 });
    const [row] = await env.handle.db.select().from(schema.joinTokens).where(eq(schema.joinTokens.tokenHash, sha256Hex(res.body.token)));
    expect(row?.accountId).toBe(u.accountId);
    expect(row?.serverId).toBe(srv.id);
    expect((row?.expiresAt.getTime() as number) - (row?.createdAt.getTime() as number)).toBe(60_000);
    expect(JSON.stringify(await env.handle.db.select().from(schema.joinTokens))).not.toContain(res.body.token);
  });

  it('needs a session, a verified email, an existing server', async () => {
    expect((await env.call('/v1/game/join', { json: { serverId: srv.id } })).status).toBe(401);
    const unverified = await makeUser(env, { verified: false });
    const r1 = await join(unverified, srv.id);
    expect(r1.status).toBe(403);
    expect(r1.body.error.code).toBe('email_not_verified');
    const u = await makeUser(env);
    expect((await join(u, 'no-such-server')).body.error.code).toBe('server_not_found');
    expect((await env.call<ErrBody>('/v1/game/join', { token: u.token, json: { serverId: 'Bad Id!' } })).status).toBe(400);
  });

  it('refuses a banned account with the ban details', async () => {
    const u = await makeUser(env);
    const ban = await env.call(`/v1/admin/accounts/${u.accountId}/ban`, { token: admin.token, json: { reason: 'cheating', durationSeconds: 3600 } });
    expect(ban.status).toBe(201);
    const res = await join(u, srv.id);
    expect(res.status).toBe(403);
    expect(res.body.error.code).toBe('account_banned');
    expect(res.body.error.details).toMatchObject({ reason: 'cheating' });
    expect(await env.handle.db.select().from(schema.joinTokens).where(eq(schema.joinTokens.accountId, u.accountId))).toHaveLength(0);

    // After the ban expires the player can join again.
    env.clock.advanceSeconds(3601);
    expect((await join(u, srv.id)).status).toBe(200);
  });

  it('a hidden server is 404 for players and joinable by staff; maintenance is 503 for players only', async () => {
    const hidden = await makeServer(env, admin, { visible: false });
    const maint = await makeServer(env, admin, { maintenance: true });
    await env.call(`/v1/admin/servers/${maint.id}`, { method: 'PATCH', token: admin.token, json: { maintenanceMessage: 'Back at 18:00 UTC' } });
    const player = await makeUser(env);
    const mod = await makeStaff(env, 'mod');

    expect((await join(player, hidden.id)).body.error.code).toBe('server_not_found');
    expect((await join(mod, hidden.id)).status).toBe(200);

    const blocked = await join(player, maint.id);
    expect(blocked.status).toBe(503);
    expect(blocked.body.error.code).toBe('server_maintenance');
    expect(blocked.body.error.message).toBe('Back at 18:00 UTC');
    expect((await join(mod, maint.id)).status).toBe(200);
  });

  it('keeps one live token per account: asking again voids the previous one', async () => {
    const u = await makeUser(env);
    const first = await join(u, srv.id);
    const second = await join(u, srv.id);
    expect((await verify(srv, first.body.token)).body).toMatchObject({ ok: false, reason: 'invalid_token' });
    expect((await verify(srv, second.body.token)).body.ok).toBe(true);
  });

  it('is limited to 10 requests per minute per account', async () => {
    const u = await makeUser(env);
    for (let i = 0; i < 10; i++) expect((await join(u, srv.id)).status).toBe(200);
    expect((await join(u, srv.id)).status).toBe(429);
    env.clock.advanceSeconds(61);
    expect((await join(u, srv.id)).status).toBe(200);
  });
});

describe('POST /internal/game/verify', () => {
  it('returns the account, consumes the token, and opens a game session', async () => {
    const u = await makeUser(env);
    const ticket = await join(u, srv.id);
    const res = await verify(srv, ticket.body.token);
    expect(res.status).toBe(200);
    expect(res.body).toMatchObject({ ok: true, accountId: u.accountId, username: u.username, roles: ['player'], emailVerified: true, ban: null });
    const [session] = await env.handle.db.select().from(schema.gameSessions).where(eq(schema.gameSessions.id, res.body.sessionId as string));
    expect(session).toMatchObject({ accountId: u.accountId, serverId: srv.id, endedAt: null });
  });

  it('is single use: the second redeem of the same token is refused', async () => {
    const u = await makeUser(env);
    const ticket = await join(u, srv.id);
    expect((await verify(srv, ticket.body.token)).body.ok).toBe(true);
    const again = await verify(srv, ticket.body.token);
    expect(again.status).toBe(200);
    expect(again.body).toMatchObject({ ok: false, reason: 'used' });
  });

  it('exactly one of several simultaneous redeems wins', async () => {
    const u = await makeUser(env);
    const ticket = await join(u, srv.id);
    const results = await Promise.all(Array.from({ length: 6 }, () => verify(srv, ticket.body.token)));
    expect(results.filter((r) => r.body.ok)).toHaveLength(1);
    expect(results.filter((r) => !r.body.ok).every((r) => r.body.reason === 'used')).toBe(true);
    const sessions = await env.handle.db.select().from(schema.gameSessions).where(eq(schema.gameSessions.accountId, u.accountId));
    expect(sessions).toHaveLength(1);
  });

  it('expires after 60 seconds', async () => {
    const u = await makeUser(env);
    const ticket = await join(u, srv.id);
    env.clock.advanceSeconds(59);
    const early = await verify(srv, (await join(u, srv.id)).body.token);
    expect(early.body.ok).toBe(true); // still inside its own 60 s
    const stale = (await join(u, srv.id)).body.token;
    env.clock.advanceSeconds(61);
    expect((await verify(srv, stale)).body).toMatchObject({ ok: false, reason: 'expired' });
    expect((await verify(srv, ticket.body.token)).body.ok).toBe(false);
    expect((await verify(srv, 'gmj_' + 'q'.repeat(43))).body).toMatchObject({ ok: false, reason: 'invalid_token' });
  });

  it('a token is bound to its server: another server presenting it gets invalid_token and does not consume it', async () => {
    const other = await makeServer(env, admin);
    const u = await makeUser(env);
    const ticket = await join(u, srv.id);
    expect((await verify(other, ticket.body.token)).body).toMatchObject({ ok: false, reason: 'invalid_token' });
    expect(env.logs.some((l) => l.includes('wrong server'))).toBe(true);
    expect((await verify(srv, ticket.body.token)).body.ok).toBe(true);
  });

  it('a ban deletes the player\'s unused join tokens: a token issued before it is plain invalid_token', async () => {
    const u = await makeUser(env);
    const ticket = await join(u, srv.id);
    await env.call(`/v1/admin/accounts/${u.accountId}/ban`, { token: admin.token, json: { reason: 'late ban' } });
    expect(await env.handle.db.select().from(schema.joinTokens).where(eq(schema.joinTokens.accountId, u.accountId))).toHaveLength(0);
    expect((await verify(srv, ticket.body.token)).body).toEqual({ ok: false, reason: 'invalid_token' });
    expect(await env.handle.db.select().from(schema.gameSessions).where(eq(schema.gameSessions.accountId, u.accountId))).toHaveLength(0);
  });

  it('refuses a banned player whose token slipped past the ban (issued in a race), and still consumes the token', async () => {
    const u = await makeUser(env);
    await env.call(`/v1/admin/accounts/${u.accountId}/ban`, { token: admin.token, json: { reason: 'late ban' } });
    // What a join request that passed its ban check just before the ban committed leaves behind.
    const raced = 'gmj_' + randomToken(32);
    await env.handle.db.insert(schema.joinTokens).values({ tokenHash: hashToken(raced), accountId: u.accountId, serverId: srv.id, createdAt: env.clock.now(), expiresAt: new Date(env.clock.now().getTime() + 60_000) });
    const res = await verify(srv, raced);
    expect(res.body).toMatchObject({ ok: false, reason: 'banned', accountId: u.accountId, username: u.username, ban: { reason: 'late ban', expiresAt: null } });
    expect((await verify(srv, raced)).body.reason).toBe('used');
    expect(await env.handle.db.select().from(schema.gameSessions).where(eq(schema.gameSessions.accountId, u.accountId))).toHaveLength(0);
  });

  it('reports the roles of staff', async () => {
    const mod = await makeStaff(env, 'mod');
    const ticket = await join(mod, srv.id);
    expect((await verify(srv, ticket.body.token)).body.roles).toEqual(['player', 'mod']);
  });
});

describe('game server authentication', () => {
  it('rejects a wrong secret, an unknown id and missing headers with the same 401', async () => {
    const u = await makeUser(env);
    const ticket = await join(u, srv.id);
    const call = (headers: Record<string, string>) => env.call<ErrBody>('/internal/game/verify', { json: { token: ticket.body.token }, ip: '127.0.0.1', headers });

    const wrongSecret = await call({ 'x-gage-server-id': srv.id, 'x-gage-server-secret': 'gss_' + 'a'.repeat(43) });
    const unknownId = await call({ 'x-gage-server-id': 'nope-1', 'x-gage-server-secret': srv.secret });
    const none = await call({});
    for (const r of [wrongSecret, unknownId, none]) {
      expect(r.status).toBe(401);
      expect(r.body.error.code).toBe('invalid_server_credentials');
    }
    expect(wrongSecret.body.error.message).toBe(unknownId.body.error.message);
    // None of the failures consumed the token.
    expect((await verify(srv, ticket.body.token)).body.ok).toBe(true);
  });

  it('another server\'s secret does not work for this server id', async () => {
    const other = await makeServer(env, admin);
    const res = await asServer(env, { id: srv.id, secret: other.secret })<ErrBody>('/internal/game/verify', { json: { token: 'gmj_whatever-token' } });
    expect(res.status).toBe(401);
  });

  it('only accepts calls from the configured server IPs (an empty list accepts nobody)', async () => {
    const bound = await makeServer(env, admin, { allowedIps: ['198.51.100.20', '2001:db8::5'] });
    const empty = await makeServer(env, admin, { allowedIps: [] });
    const body = { json: { token: 'gmj_whatever-token' } };
    expect((await asServer(env, bound, '198.51.100.20')<VerifyBody>('/internal/game/verify', body)).status).toBe(200);
    expect((await asServer(env, bound, '2001:db8::5')<VerifyBody>('/internal/game/verify', body)).status).toBe(200);
    const wrongIp = await asServer(env, bound, '198.51.100.21')<ErrBody>('/internal/game/verify', body);
    expect(wrongIp.status).toBe(403);
    expect(wrongIp.body.error.code).toBe('ip_not_allowed');
    expect((await asServer(env, empty, '127.0.0.1')<ErrBody>('/internal/game/verify', body)).status).toBe(403);
  });

  it('never trusts X-Forwarded-For for the allowlist', async () => {
    const bound = await makeServer(env, admin, { allowedIps: ['198.51.100.30'] });
    const res = await env.call<ErrBody>('/internal/game/verify', { json: { token: 'gmj_whatever-token' }, ip: '203.0.113.50', headers: { 'x-gage-server-id': bound.id, 'x-gage-server-secret': bound.secret, 'x-forwarded-for': '198.51.100.30' } });
    expect(res.status).toBe(403);
  });

  it('a rotated secret replaces the old one at once; only hashes are stored', async () => {
    const s = await makeServer(env, admin);
    const rotate = await env.call<{ secret: string }>(`/v1/admin/servers/${s.id}/rotate-secret`, { method: 'POST', token: admin.token });
    expect(rotate.status).toBe(200);
    expect((await asServer(env, s)<ErrBody>('/internal/game/verify', { json: { token: 'gmj_whatever-token' } })).status).toBe(401);
    expect((await asServer(env, { id: s.id, secret: rotate.body.secret })<VerifyBody>('/internal/game/verify', { json: { token: 'gmj_whatever-token' } })).status).toBe(200);
    const dump = JSON.stringify(await env.handle.db.select().from(schema.gameServers));
    expect(dump).not.toContain(s.secret);
    expect(dump).not.toContain(rotate.body.secret);
  });
});

describe('game sessions', () => {
  it('a new join on another server supersedes the open session', async () => {
    const second = await makeServer(env, admin);
    const u = await makeUser(env);
    const first = await joinServer(env, u, srv);
    const next = await joinServer(env, u, second);
    const rows = await env.handle.db.select().from(schema.gameSessions).where(eq(schema.gameSessions.accountId, u.accountId));
    expect(rows).toHaveLength(2);
    expect(rows.find((r) => r.id === first.sessionId)?.endReason).toBe('superseded');
    expect(rows.find((r) => r.id === next.sessionId)?.endedAt).toBeNull();
  });

  it('session-end closes the session, is idempotent, and cannot close another server\'s session', async () => {
    const other = await makeServer(env, admin);
    const u = await makeUser(env);
    const { sessionId } = await joinServer(env, u, srv);

    const foreign = await asServer(env, other)<{ ended: number }>('/internal/game/session-end', { json: { sessionId } });
    expect(foreign.body.ended).toBe(0);

    const end = await asServer(env, srv)<{ ended: number }>('/internal/game/session-end', { json: { sessionId, reason: 'quit' } });
    expect(end.body.ended).toBe(1);
    const [row] = await env.handle.db.select().from(schema.gameSessions).where(eq(schema.gameSessions.id, sessionId));
    expect(row?.endReason).toBe('quit');
    expect(row?.endedAt).not.toBeNull();
    expect((await asServer(env, srv)<{ ended: number }>('/internal/game/session-end', { json: { sessionId } })).body.ended).toBe(0);
  });

  it('session-end by accountId, and it needs one of the two ids', async () => {
    const u = await makeUser(env);
    await joinServer(env, u, srv);
    expect((await asServer(env, srv)<{ ended: number }>('/internal/game/session-end', { json: { accountId: u.accountId, reason: 'timeout' } })).body.ended).toBe(1);
    expect((await asServer(env, srv)<ErrBody>('/internal/game/session-end', { json: { reason: 'quit' } })).status).toBe(400);
  });
});

describe('listeners', () => {
  it('the public listener does not serve /internal and the internal listener does not serve the public API', async () => {
    const publicApp = env.appFor('public');
    const internalApp = env.appFor('internal');
    const bindings = { incoming: { socket: { remoteAddress: '127.0.0.1', remotePort: 1, remoteFamily: 'IPv4' } } };
    const headers = { 'x-gage-server-id': srv.id, 'x-gage-server-secret': srv.secret, 'content-type': 'application/json' };
    const onPublic = await publicApp.request('/internal/game/verify', { method: 'POST', headers, body: '{"token":"gmj_whatever-token"}' }, bindings);
    expect(onPublic.status).toBe(404);
    const onInternal = await internalApp.request('/internal/game/verify', { method: 'POST', headers, body: '{"token":"gmj_whatever-token"}' }, bindings);
    expect(onInternal.status).toBe(200);
    expect((await internalApp.request('/v1/servers', {}, bindings)).status).toBe(404);
    expect((await internalApp.request('/health', {}, bindings)).status).toBe(200);
    expect((await publicApp.request('/health', {}, bindings)).status).toBe(200);
  });
});
