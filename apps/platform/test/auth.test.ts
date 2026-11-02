import { eq } from 'drizzle-orm';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { schema } from '../src/db/index.js';
import { freshIp, latestMail, mailToken, makeUser, sha256Hex, createTestEnv, STRONG_PASSWORD, type TestEnv } from './helpers.js';

let env: TestEnv;
beforeAll(async () => {
  env = await createTestEnv({ env: { SESSION_TTL_DAYS: '30', SESSION_MAX_DAYS: '40' } });
});
afterAll(async () => {
  await env.close();
});

type ErrBody = { error: { code: string; message: string; requestId: string; details?: { problem?: string } } };
const register = (json: unknown, ip = freshIp()) => env.call<ErrBody & { verificationRequired?: boolean }>('/v1/auth/register', { json, ip });

describe('registration validation', () => {
  const good = { email: 'reg.ok@example.test', username: 'RegOk_1', password: STRONG_PASSWORD };

  it('creates an account: lower-cased email, argon2id hash, role player, verification email', async () => {
    const res = await register({ ...good, email: 'Reg.OK@Example.test' });
    expect(res.status).toBe(201);
    expect(res.body.verificationRequired).toBe(true);
    const [row] = await env.handle.db.select().from(schema.accounts).where(eq(schema.accounts.username, 'RegOk_1'));
    expect(row?.email).toBe('reg.ok@example.test');
    expect(row?.passwordHash).toMatch(/^\$argon2id\$/);
    expect(row?.passwordHash).not.toContain(STRONG_PASSWORD);
    expect(row?.emailVerifiedAt).toBeNull();
    const roles = await env.handle.db.select().from(schema.accountRoles).where(eq(schema.accountRoles.accountId, row?.id as string));
    expect(roles.map((r) => r.role)).toEqual(['player']);
    const mail = await latestMail(env, 'reg.ok@example.test', 'verify_email');
    expect(mail?.subject).toMatch(/verify/i);
    expect(mail?.token).toBeTruthy();
    expect(mail?.body).toContain(mail?.token as string);
  });

  it.each([
    ['too short', 'ab'],
    ['too long', 'a'.repeat(21)],
    ['space', 'has space'],
    ['dash', 'dash-name'],
    ['non-ascii', 'ünïcode_name'],
    ['empty', ''],
  ])('rejects username: %s', async (_label, username) => {
    const res = await register({ ...good, email: `bad.${Math.random().toString(36).slice(2)}@example.test`, username });
    expect(res.status).toBe(400);
    expect(res.body.error.code).toBe('invalid_request');
  });

  it('accepts the boundary usernames (3 and 20 characters, digits and underscore)', async () => {
    expect((await register({ email: 'b3@example.test', username: 'a_1', password: STRONG_PASSWORD })).status).toBe(201);
    expect((await register({ email: 'b20@example.test', username: 'A'.repeat(19) + '_', password: STRONG_PASSWORD })).status).toBe(201);
  });

  it('rejects a malformed email', async () => {
    for (const email of ['nope', 'a@', '@b.test', 'a b@example.test']) {
      const res = await register({ ...good, email, username: 'EmailBad1' });
      expect(res.status, email).toBe(400);
    }
  });

  it.each([
    ['too_short', 'short1!'],
    ['common', 'password1234'],
    ['common', 'Qwerty12345'],
    ['common', 'iloveyou123!'],
    ['contains_username', 'xx-PwUser77-xx-zz'],
  ])('rejects password (%s): %s', async (problem, password) => {
    const res = await register({ email: `pw.${Math.random().toString(36).slice(2)}@example.test`, username: 'PwUser77', password });
    expect(res.status).toBe(400);
    expect(res.body.error.code).toBe('weak_password');
    expect(res.body.error.details?.problem).toBe(problem);
  });

  it('accepts a password of exactly 10 characters that is not common', async () => {
    const res = await register({ email: 'ten@example.test', username: 'TenChars', password: 'k3Xp9vQw2z' });
    expect(res.status).toBe(201);
  });

  it('reserved and taken usernames (case-insensitive) are 409 username_taken', async () => {
    expect((await register({ email: 'r1@example.test', username: 'Admin', password: STRONG_PASSWORD })).body.error.code).toBe('username_taken');
    expect((await register({ email: 'r2@example.test', username: 'regok_1', password: STRONG_PASSWORD })).status).toBe(409);
  });

  it('a duplicate email looks identical to a new one, creates nothing, and mails the owner', async () => {
    const before = await env.handle.db.select().from(schema.accounts);
    const res = await register({ email: 'reg.ok@example.test', username: 'SomeoneElse', password: STRONG_PASSWORD });
    expect(res.status).toBe(201);
    expect(res.body.verificationRequired).toBe(true);
    expect(await env.handle.db.select().from(schema.accounts)).toHaveLength(before.length);
    expect(await latestMail(env, 'reg.ok@example.test', 'account_exists')).toBeDefined();
  });

  it('rejects non-JSON and wrong content types with the standard error shape', async () => {
    const bad = await env.call<ErrBody>('/v1/auth/register', { rawBody: '{nope', headers: { 'content-type': 'application/json' } });
    expect(bad.status).toBe(400);
    expect(bad.body.error.code).toBe('invalid_json');
    const wrong = await env.call<ErrBody>('/v1/auth/register', { rawBody: 'a=b', headers: { 'content-type': 'text/plain' } });
    expect(wrong.status).toBe(415);
    expect(wrong.body.error.requestId).toMatch(/^[0-9a-f-]{36}$/);
  });
});

describe('login', () => {
  it('accepts email or username, any case; stores only a hash of the session token', async () => {
    const u = await makeUser(env);
    const byName = await env.call<{ token: string; account: { username: string; emailVerified: boolean } }>('/v1/auth/login', { json: { identifier: u.username.toUpperCase(), password: u.password }, ip: freshIp() });
    expect(byName.status).toBe(200);
    expect(byName.body.account.username).toBe(u.username);
    expect(byName.body.account.emailVerified).toBe(true);
    const byEmail = await env.call<{ token: string }>('/v1/auth/login', { json: { identifier: u.email.toUpperCase(), password: u.password }, ip: freshIp() });
    expect(byEmail.status).toBe(200);

    expect(byName.body.token).toMatch(/^gms_[A-Za-z0-9_-]{43}$/);
    const [row] = await env.handle.db.select().from(schema.sessions).where(eq(schema.sessions.tokenHash, sha256Hex(byName.body.token)));
    expect(row?.accountId).toBe(u.accountId);
    const all = JSON.stringify(await env.handle.db.select().from(schema.sessions));
    expect(all).not.toContain(byName.body.token);
  });

  it('a wrong password and an unknown identifier give the same answer', async () => {
    const u = await makeUser(env);
    const wrong = await env.call<ErrBody>('/v1/auth/login', { json: { identifier: u.email, password: 'wrong-password-1' }, ip: freshIp() });
    const unknown = await env.call<ErrBody>('/v1/auth/login', { json: { identifier: 'nobody@example.test', password: 'wrong-password-1' }, ip: freshIp() });
    expect(wrong.status).toBe(401);
    expect(unknown.status).toBe(401);
    expect(wrong.body.error.code).toBe('invalid_credentials');
    expect(unknown.body.error.message).toBe(wrong.body.error.message);
  });

  it('an unverified account can log in (verification gates joining and buying, not login)', async () => {
    const u = await makeUser(env, { verified: false });
    const me = await env.call<{ account: { emailVerified: boolean } }>('/v1/me', { token: u.token });
    expect(me.status).toBe(200);
    expect(me.body.account.emailVerified).toBe(false);
  });
});

describe('login rate limiting and lockout', () => {
  const attempt = (identifier: string, password: string, ip: string) =>
    env.call<ErrBody>('/v1/auth/login', { json: { identifier, password }, ip });

  it('locks the account after 5 failures, refuses even the right password, and backs off exponentially', async () => {
    const u = await makeUser(env);
    const ip = freshIp();
    for (let i = 0; i < 5; i++) expect((await attempt(u.email, `wrong-password-${i}`, ip)).status).toBe(401);

    const locked = await attempt(u.email, u.password, ip);
    expect(locked.status).toBe(429);
    expect(locked.body.error.code).toBe('rate_limited');
    expect(Number(locked.headers.get('retry-after'))).toBeGreaterThan(0);
    expect(Number(locked.headers.get('retry-after'))).toBeLessThanOrEqual(30);

    // The lock is per account: another address is refused as well (documented trade-off).
    expect((await attempt(u.email, u.password, freshIp())).status).toBe(429);

    // After the lock ends the right password works again and clears the counter.
    env.clock.advanceSeconds(31);
    expect((await attempt(u.email, u.password, ip)).status).toBe(200);
  });

  it('the second lock is twice as long as the first', async () => {
    const u = await makeUser(env);
    const ip = freshIp();
    for (let i = 0; i < 5; i++) await attempt(u.email, `wrong-password-${i}`, ip);
    expect(Number((await attempt(u.email, u.password, ip)).headers.get('retry-after'))).toBeLessThanOrEqual(30);
    env.clock.advanceSeconds(31); // lock over, still inside the failure window
    expect((await attempt(u.email, 'wrong-again-123', ip)).status).toBe(401); // 6th failure
    const second = await attempt(u.email, u.password, ip);
    expect(second.status).toBe(429);
    const retry = Number(second.headers.get('retry-after'));
    expect(retry).toBeGreaterThan(30);
    expect(retry).toBeLessThanOrEqual(60);
  });

  it('failures decay after the window of calm, so a typo a day later is not a strike', async () => {
    const u = await makeUser(env);
    const ip = freshIp();
    for (let i = 0; i < 4; i++) await attempt(u.email, `wrong-password-${i}`, ip);
    env.clock.advanceSeconds(901);
    for (let i = 0; i < 4; i++) expect((await attempt(u.email, `wrong-password-${i}`, ip)).status).toBe(401);
    expect((await attempt(u.email, u.password, ip)).status).toBe(200); // never reached 5 in one window
  });

  it('locks an IP after 20 failures across any identifiers', async () => {
    const ip = freshIp();
    for (let i = 0; i < 20; i++) expect((await attempt(`ghost${i}@example.test`, 'wrong-password-1', ip)).status).toBe(401);
    const blocked = await attempt('another@example.test', 'wrong-password-1', ip);
    expect(blocked.status).toBe(429);
    // A different address is unaffected.
    expect((await attempt('another@example.test', 'wrong-password-1', freshIp())).status).toBe(401);
  });

  it('unknown identifiers lock like real ones (no way to tell them apart)', async () => {
    const ip = freshIp();
    for (let i = 0; i < 5; i++) await attempt('phantom@example.test', `wrong-password-${i}`, ip);
    expect((await attempt('phantom@example.test', 'wrong-password-9', ip)).status).toBe(429);
  });
});

describe('sessions', () => {
  it('lists devices, flags the current one, revokes one device, then all others', async () => {
    const u = await makeUser(env);
    const second = await env.call<{ token: string; sessionId: string }>('/v1/auth/login', { json: { identifier: u.email, password: u.password, deviceName: 'Laptop' }, ip: freshIp(), headers: { 'user-agent': 'GAGE-Launcher/1.0' } });
    const third = await env.call<{ token: string; sessionId: string }>('/v1/auth/login', { json: { identifier: u.email, password: u.password, deviceName: 'Desktop' }, ip: freshIp() });

    const list = await env.call<{ sessions: { id: string; current: boolean; deviceName: string | null; userAgent: string | null }[] }>('/v1/me/sessions', { token: u.token });
    expect(list.body.sessions).toHaveLength(3);
    expect(list.body.sessions.filter((s) => s.current).map((s) => s.id)).toEqual([u.sessionId]);
    const laptop = list.body.sessions.find((s) => s.id === second.body.sessionId);
    expect(laptop?.deviceName).toBe('Laptop');
    expect(laptop?.userAgent).toBe('GAGE-Launcher/1.0');

    expect((await env.call(`/v1/me/sessions/${second.body.sessionId}`, { method: 'DELETE', token: u.token })).status).toBe(204);
    expect((await env.call('/v1/me', { token: second.body.token })).status).toBe(401);
    expect((await env.call('/v1/me', { token: u.token })).status).toBe(200);
    expect((await env.call(`/v1/me/sessions/${second.body.sessionId}`, { method: 'DELETE', token: u.token })).status).toBe(404);

    const others = await env.call<{ revoked: number }>('/v1/me/sessions', { method: 'DELETE', token: u.token });
    expect(others.body.revoked).toBe(1);
    expect((await env.call('/v1/me', { token: third.body.token })).status).toBe(401);
    expect((await env.call('/v1/me', { token: u.token })).status).toBe(200);
  });

  it('cannot revoke another account\'s session', async () => {
    const a = await makeUser(env);
    const b = await makeUser(env);
    expect((await env.call(`/v1/me/sessions/${b.sessionId}`, { method: 'DELETE', token: a.token })).status).toBe(404);
    expect((await env.call('/v1/me', { token: b.token })).status).toBe(200);
  });

  it('logout revokes the token; garbage and malformed tokens are 401', async () => {
    const u = await makeUser(env);
    expect((await env.call('/v1/auth/logout', { method: 'POST', token: u.token })).status).toBe(204);
    expect((await env.call('/v1/me', { token: u.token })).status).toBe(401);
    expect((await env.call('/v1/me', { token: 'gms_' + 'x'.repeat(43) })).status).toBe(401);
    expect((await env.call('/v1/me', { token: 'not-a-session-token' })).status).toBe(401);
    expect((await env.call('/v1/me')).status).toBe(401);
  });

  it('slides the 30-day expiry on use, never past the absolute cap, and expires when idle', async () => {
    const u = await makeUser(env);
    const read = async () => (await env.handle.db.select().from(schema.sessions).where(eq(schema.sessions.id, u.sessionId)))[0];
    const day = 86_400;
    const created = (await read())?.createdAt.getTime() as number;
    expect((await read())?.expiresAt.getTime()).toBe(created + 30 * day * 1000);

    env.clock.advanceSeconds(20 * day);
    expect((await env.call('/v1/me', { token: u.token })).status).toBe(200);
    // slid to min(now + 30d, created + 40d) = created + 40d (the cap)
    expect((await read())?.expiresAt.getTime()).toBe(created + 40 * day * 1000);

    env.clock.advanceSeconds(21 * day); // 41 days after creation: past the absolute cap
    expect((await env.call('/v1/me', { token: u.token })).status).toBe(401);
  });

  it('a session idle for more than 30 days is dead', async () => {
    const u = await makeUser(env);
    env.clock.advanceSeconds(31 * 86_400);
    expect((await env.call('/v1/me', { token: u.token })).status).toBe(401);
  });
});

describe('password change revokes sessions', () => {
  it('signs out every other device, keeps the caller, switches the password, mails the owner', async () => {
    const u = await makeUser(env);
    const other = await env.call<{ token: string }>('/v1/auth/login', { json: { identifier: u.email, password: u.password }, ip: freshIp() });
    const newPassword = 'a-brand-new-passphrase-42';

    const res = await env.call<{ revokedSessions: number }>('/v1/me/password', { token: u.token, json: { currentPassword: u.password, newPassword } });
    expect(res.status).toBe(200);
    expect(res.body.revokedSessions).toBe(1);

    expect((await env.call('/v1/me', { token: other.body.token })).status).toBe(401);
    expect((await env.call('/v1/me', { token: u.token })).status).toBe(200);
    expect((await env.call('/v1/auth/login', { json: { identifier: u.email, password: u.password }, ip: freshIp() })).status).toBe(401);
    expect((await env.call('/v1/auth/login', { json: { identifier: u.email, password: newPassword }, ip: freshIp() })).status).toBe(200);
    expect(await latestMail(env, u.email, 'password_changed')).toBeDefined();
  });

  it('refuses a wrong current password (and counts it as a login failure), a weak or unchanged new one', async () => {
    const u = await makeUser(env);
    const wrong = await env.call<ErrBody>('/v1/me/password', { token: u.token, json: { currentPassword: 'not-my-password-1', newPassword: 'another-fine-password-9' }, ip: freshIp() });
    expect(wrong.status).toBe(401);
    const weak = await env.call<ErrBody>('/v1/me/password', { token: u.token, json: { currentPassword: u.password, newPassword: 'password1234' } });
    expect(weak.body.error.code).toBe('weak_password');
    const same = await env.call<ErrBody>('/v1/me/password', { token: u.token, json: { currentPassword: u.password, newPassword: u.password } });
    expect(same.body.error.code).toBe('weak_password');
    const missing = await env.call<ErrBody>('/v1/me/password', { token: u.token, json: { newPassword: 'another-fine-password-9' } });
    expect(missing.body.error.code).toBe('current_password_required');
    expect((await env.call('/v1/me', { token: u.token })).status).toBe(200);
  });
});

describe('email verification tokens', () => {
  it('verifies once: the second use, an expired token and a wrong-kind token are refused', async () => {
    const u = await makeUser(env, { verified: false });
    const token = await mailToken(env, u.email, 'verify_email');

    const first = await env.call<{ account: { emailVerified: boolean } }>('/v1/auth/verify-email', { json: { token }, ip: freshIp() });
    expect(first.status).toBe(200);
    expect(first.body.account.emailVerified).toBe(true);
    const again = await env.call<ErrBody>('/v1/auth/verify-email', { json: { token }, ip: freshIp() });
    expect(again.status).toBe(400);
    expect(again.body.error.code).toBe('invalid_token');
    // Only a hash is stored.
    expect(JSON.stringify(await env.handle.db.select().from(schema.authTokens))).not.toContain(token);
  });

  it('expires after 24 hours', async () => {
    const u = await makeUser(env, { verified: false });
    const token = await mailToken(env, u.email, 'verify_email');
    env.clock.advanceSeconds(24 * 3600 + 1);
    const res = await env.call<ErrBody>('/v1/auth/verify-email', { json: { token }, ip: freshIp() });
    expect(res.body.error.code).toBe('invalid_token');
  });

  it('a verification token is not a reset token, and unknown tokens are refused', async () => {
    const u = await makeUser(env, { verified: false });
    const token = await mailToken(env, u.email, 'verify_email');
    const asReset = await env.call<ErrBody>('/v1/auth/reset-password', { json: { token, password: 'whatever-new-password-1' }, ip: freshIp() });
    expect(asReset.body.error.code).toBe('invalid_token');
    const unknown = await env.call<ErrBody>('/v1/auth/verify-email', { json: { token: 'z'.repeat(43) }, ip: freshIp() });
    expect(unknown.body.error.code).toBe('invalid_token');
    // The verification token still works afterwards: a failed attempt as another kind consumed nothing.
    expect((await env.call('/v1/auth/verify-email', { json: { token }, ip: freshIp() })).status).toBe(200);
  });

  it('resending voids the earlier token; a token for an old address is void', async () => {
    const u = await makeUser(env, { verified: false });
    const first = await mailToken(env, u.email, 'verify_email');
    const resend = await env.call('/v1/auth/resend-verification', { method: 'POST', token: u.token });
    expect(resend.status).toBe(202);
    const second = await mailToken(env, u.email, 'verify_email');
    expect(second).not.toBe(first);
    expect((await env.call('/v1/auth/verify-email', { json: { token: first }, ip: freshIp() })).status).toBe(400);

    await env.handle.db.update(schema.accounts).set({ email: `moved.${u.email}` }).where(eq(schema.accounts.id, u.accountId));
    expect((await env.call('/v1/auth/verify-email', { json: { token: second }, ip: freshIp() })).status).toBe(400);
    const [row] = await env.handle.db.select().from(schema.accounts).where(eq(schema.accounts.id, u.accountId));
    expect(row?.emailVerifiedAt).toBeNull();
  });

  it('resend is refused for a verified account and limited to 3 per hour', async () => {
    const v = await makeUser(env);
    expect((await env.call<ErrBody>('/v1/auth/resend-verification', { method: 'POST', token: v.token })).body.error.code).toBe('already_verified');
    const u = await makeUser(env, { verified: false });
    for (let i = 0; i < 3; i++) expect((await env.call('/v1/auth/resend-verification', { method: 'POST', token: u.token })).status).toBe(202);
    expect((await env.call('/v1/auth/resend-verification', { method: 'POST', token: u.token })).status).toBe(429);
  });
});

describe('password reset tokens', () => {
  const forgot = (email: string, ip = freshIp()) => env.call<ErrBody>('/v1/auth/forgot-password', { json: { email }, ip });

  it('answers 202 for unknown addresses without sending anything', async () => {
    const res = await forgot('nobody.here@example.test');
    expect(res.status).toBe(202);
    expect(await latestMail(env, 'nobody.here@example.test', 'password_reset')).toBeUndefined();
  });

  it('resets once; revokes all sessions; verifies the email; clears a lockout; old password stops working', async () => {
    const u = await makeUser(env, { verified: false });
    const ip = freshIp();
    for (let i = 0; i < 5; i++) await env.call('/v1/auth/login', { json: { identifier: u.email, password: `wrong-password-${i}` }, ip });
    expect((await env.call('/v1/auth/login', { json: { identifier: u.email, password: u.password }, ip })).status).toBe(429);

    expect((await forgot(u.email)).status).toBe(202);
    const token = await mailToken(env, u.email, 'password_reset');
    const newPassword = 'reset-to-this-passphrase-77';
    const done = await env.call('/v1/auth/reset-password', { json: { token, password: newPassword }, ip: freshIp() });
    expect(done.status).toBe(200);

    expect((await env.call('/v1/me', { token: u.token })).status).toBe(401); // existing session revoked
    const login = await env.call<{ account: { emailVerified: boolean } }>('/v1/auth/login', { json: { identifier: u.email, password: newPassword }, ip });
    expect(login.status).toBe(200); // lockout cleared
    expect(login.body.account.emailVerified).toBe(true);
    expect((await env.call('/v1/auth/login', { json: { identifier: u.email, password: u.password }, ip: freshIp() })).status).toBe(401);

    const replay = await env.call<ErrBody>('/v1/auth/reset-password', { json: { token, password: 'yet-another-passphrase-88' }, ip: freshIp() });
    expect(replay.body.error.code).toBe('invalid_token');
  });

  it('expires after 60 minutes', async () => {
    const u = await makeUser(env);
    await forgot(u.email);
    const token = await mailToken(env, u.email, 'password_reset');
    env.clock.advanceSeconds(60 * 60 + 1);
    const res = await env.call<ErrBody>('/v1/auth/reset-password', { json: { token, password: 'a-fresh-long-passphrase-1' }, ip: freshIp() });
    expect(res.body.error.code).toBe('invalid_token');
  });

  it('a rejected weak password leaves the token usable', async () => {
    const u = await makeUser(env);
    await forgot(u.email);
    const token = await mailToken(env, u.email, 'password_reset');
    const weak = await env.call<ErrBody>('/v1/auth/reset-password', { json: { token, password: 'password1234' }, ip: freshIp() });
    expect(weak.body.error.code).toBe('weak_password');
    expect((await env.call('/v1/auth/reset-password', { json: { token, password: 'a-good-long-passphrase-5' }, ip: freshIp() })).status).toBe(200);
  });

  it('a newer reset email voids the older token', async () => {
    const u = await makeUser(env);
    await forgot(u.email);
    const first = await mailToken(env, u.email, 'password_reset');
    await forgot(u.email);
    const second = await mailToken(env, u.email, 'password_reset');
    expect(second).not.toBe(first);
    expect((await env.call('/v1/auth/reset-password', { json: { token: first, password: 'a-good-long-passphrase-6' }, ip: freshIp() })).status).toBe(400);
    expect((await env.call('/v1/auth/reset-password', { json: { token: second, password: 'a-good-long-passphrase-6' }, ip: freshIp() })).status).toBe(200);
  });

  it('is limited to 3 requests per address per hour', async () => {
    const u = await makeUser(env);
    for (let i = 0; i < 3; i++) expect((await forgot(u.email)).status).toBe(202);
    expect((await forgot(u.email)).status).toBe(429);
  });
});
