import { describe, expect, it } from 'vitest';
import { createAccountApi } from '../src/account/api';
import { describeFailure, OFFLINE_TEXT, NETWORK_TEXT, waitText } from '../src/account/errors';
import { ACCOUNT, apiError, scriptedFetch, SESSION, TOKEN } from './account-helpers';

describe('account API client', () => {
  it('registers with a JSON body on the same-origin /api prefix', async () => {
    const f = scriptedFetch([{ status: 201, json: { verificationRequired: true } }]);
    const r = await createAccountApi('/api', f.fetchImpl).register({ username: 'Speedy_Gonzo', email: 'speedy@example.com', password: 'a-long-unusual-passphrase' });
    expect(r).toEqual({ ok: true, value: { verificationRequired: true } });
    expect(f.calls).toHaveLength(1);
    expect(f.calls[0]).toMatchObject({ method: 'POST', url: '/api/v1/auth/register', body: { username: 'Speedy_Gonzo', email: 'speedy@example.com', password: 'a-long-unusual-passphrase' } });
    expect(f.calls[0]!.headers['content-type']).toBe('application/json');
    expect(f.calls[0]!.headers.authorization).toBeUndefined();
  });

  it('logs in and returns the token and the account', async () => {
    const f = scriptedFetch([{ status: 200, json: { token: TOKEN, expiresAt: '2026-12-06T00:00:00.000Z', sessionId: SESSION.id, account: ACCOUNT } }]);
    const r = await createAccountApi('/api', f.fetchImpl).login({ identifier: 'speedy@example.com', password: 'x', deviceName: 'Website' });
    expect(r.ok && r.value.token).toBe(TOKEN);
    expect(r.ok && r.value.account.username).toBe('Speedy_Gonzo');
    expect(f.calls[0]!.body).toEqual({ identifier: 'speedy@example.com', password: 'x', deviceName: 'Website' });
  });

  it('sends the bearer token on account routes', async () => {
    const f = scriptedFetch([{ status: 200, json: { account: ACCOUNT } }, { status: 200, json: { sessions: [SESSION] } }, { status: 204 }, { status: 200, json: { revoked: 2 } }]);
    const api = createAccountApi('/api', f.fetchImpl);
    expect((await api.me(TOKEN)).ok).toBe(true);
    expect(await api.sessions(TOKEN)).toEqual({ ok: true, value: [SESSION] });
    expect(await api.revokeSession(TOKEN, SESSION.id)).toEqual({ ok: true, value: null });
    expect(await api.revokeOtherSessions(TOKEN)).toEqual({ ok: true, value: { revoked: 2 } });
    expect(f.calls.map((c) => `${c.method} ${c.url}`)).toEqual([
      'GET /api/v1/me',
      'GET /api/v1/me/sessions',
      `DELETE /api/v1/me/sessions/${SESSION.id}`,
      'DELETE /api/v1/me/sessions',
    ]);
    for (const c of f.calls) expect(c.headers.authorization).toBe(`Bearer ${TOKEN}`);
  });

  it('reads the documented error body, request id and Retry-After', async () => {
    const f = scriptedFetch([apiError(429, 'rate_limited', undefined, { 'Retry-After': '42' })]);
    const r = await createAccountApi('/api', f.fetchImpl).login({ identifier: 'a', password: 'b' });
    expect(r).toEqual({ ok: false, failure: { kind: 'api', status: 429, code: 'rate_limited', message: 'dev message for rate_limited', requestId: 'req-0123456789', details: null, retryAfterSeconds: 42 } });
  });

  it('calls a thrown fetch "network" and a non-platform answer "bad-response"', async () => {
    const f = scriptedFetch(['network-down', { status: 502, text: '<html>Bad Gateway</html>' }, { status: 404, text: '' }, { status: 200, json: { token: 'not-a-token', account: ACCOUNT } }, { status: 200, json: { account: { ...ACCOUNT, emailVerified: 'yes' } } }]);
    const api = createAccountApi('/api', f.fetchImpl);
    expect(await api.me(TOKEN)).toEqual({ ok: false, failure: { kind: 'network' } });
    expect(await api.me(TOKEN)).toEqual({ ok: false, failure: { kind: 'bad-response', status: 502 } });
    expect(await api.me(TOKEN)).toEqual({ ok: false, failure: { kind: 'bad-response', status: 404 } });
    expect(await api.login({ identifier: 'a', password: 'b' })).toEqual({ ok: false, failure: { kind: 'bad-response', status: 200 } });
    expect(await api.me(TOKEN)).toEqual({ ok: false, failure: { kind: 'bad-response', status: 200 } });
  });

  it('accepts a ban with no end date and refuses a malformed one', async () => {
    const f = scriptedFetch([
      { status: 200, json: { account: { ...ACCOUNT, ban: { id: 'b', reason: 'cheating', createdAt: ACCOUNT.createdAt, expiresAt: null } } } },
      { status: 200, json: { account: { ...ACCOUNT, ban: { reason: 7 } } } },
    ]);
    const api = createAccountApi('/api', f.fetchImpl);
    const ok = await api.me(TOKEN);
    expect(ok.ok && ok.value.ban).toEqual({ reason: 'cheating', expiresAt: null });
    expect((await api.me(TOKEN)).ok).toBe(false);
  });
});

describe('error codes as words', () => {
  const fail = (code: string, extra: { status?: number; details?: Record<string, unknown>; retryAfterSeconds?: number } = {}) =>
    ({ kind: 'api', status: extra.status ?? 400, code, message: 'dev', requestId: 'abcdef0123456789', details: extra.details ?? null, retryAfterSeconds: extra.retryAfterSeconds ?? null }) as const;

  it.each([
    ['too_short', 'Use at least 10 characters.'],
    ['too_long', 'Use at most 128 characters.'],
    ['common', 'That password is too common. Pick something less guessable.'],
    ['contains_username', 'Your password can’t contain your username.'],
    ['contains_email', 'Your password can’t contain the part of your email before the @.'],
  ])('weak_password %s goes under the password field', (problem, text) => {
    expect(describeFailure(fail('weak_password', { details: { problem } }), 'register')).toEqual({ field: 'password', text });
    expect(describeFailure(fail('weak_password', { details: { problem } }), 'change-password')).toEqual({ field: 'newPassword', text });
  });

  it('never trusts a details.problem that is not a known reason', () => {
    expect(describeFailure(fail('weak_password', { details: { problem: 'toString' } }), 'register')).toEqual({ field: 'password', text: 'Pick a stronger password.' });
  });

  it('maps the account codes', () => {
    expect(describeFailure(fail('username_taken', { status: 409 }), 'register')).toEqual({ field: 'username', text: 'That username is taken or reserved. Try another.' });
    expect(describeFailure(fail('invalid_credentials', { status: 401 }), 'login')).toEqual({ text: 'That username or email and password don’t match.' });
    expect(describeFailure(fail('invalid_credentials', { status: 401 }), 'change-password')).toEqual({ field: 'currentPassword', text: 'That isn’t your current password.' });
    expect(describeFailure(fail('current_password_required'), 'change-password').field).toBe('currentPassword');
    expect(describeFailure(fail('busy', { status: 503 }), 'login').text).toBe('The server is busy checking passwords. Try again in a few seconds.');
    expect(describeFailure(fail('email_not_verified', { status: 403 }), 'change-password').text).toMatch(/verification isn’t available yet/);
  });

  it('says how long to wait after a lockout, rounded up', () => {
    expect(describeFailure(fail('rate_limited', { status: 429, retryAfterSeconds: 30 }), 'login').text).toBe('Too many attempts. Try again in 30 seconds.');
    expect(describeFailure(fail('rate_limited', { status: 429, retryAfterSeconds: 61 }), 'login').text).toBe('Too many attempts. Try again in 2 minutes.');
    expect(describeFailure(fail('rate_limited', { status: 429 }), 'login').text).toBe('Too many attempts. Wait a while, then try again.');
    expect(waitText(1)).toBe('1 second');
    expect(waitText(3600)).toBe('1 hour');
    expect(waitText(3601)).toBe('2 hours');
  });

  it('puts invalid_request issues under their field', () => {
    const issues = [{ path: 'email', message: 'Invalid email address' }];
    expect(describeFailure(fail('invalid_request', { details: { issues } }), 'register')).toEqual({ field: 'email', text: 'Enter a valid email address.' });
    expect(describeFailure(fail('invalid_request', { details: { issues: [{ path: 'constructor', message: 'x' }] } }), 'register')).toEqual({ text: 'Check the form and try again.' });
  });

  it('tells an offline API from a dead network, and adds a reference to server errors', () => {
    expect(describeFailure({ kind: 'network' }, 'login').text).toBe(NETWORK_TEXT);
    expect(describeFailure({ kind: 'bad-response', status: 404 }, 'login').text).toBe(OFFLINE_TEXT);
    expect(describeFailure({ kind: 'bad-response', status: 502 }, 'login').text).toBe(OFFLINE_TEXT);
    expect(describeFailure(fail('internal_error', { status: 500 }), 'login').text).toBe('Something went wrong on our side. Try again in a moment (reference abcdef01).');
  });
});
