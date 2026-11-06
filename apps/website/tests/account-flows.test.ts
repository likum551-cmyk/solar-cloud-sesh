import { describe, expect, it } from 'vitest';
import { createAccountApi } from '../src/account/api';
import { changePassword, loadAccount, logIn, logOut, revokeOtherSessions, revokeSession, signUp } from '../src/account/flows';
import { createTokenStore } from '../src/account/store';
import { ACCOUNT, apiError, memoryStorage, scriptedFetch, SESSION, TOKEN, type Answer } from './account-helpers';

function setup(answers: Answer[], token: string | null = null, where: 'session' | 'local' = 'session') {
  const f = scriptedFetch(answers);
  const session = memoryStorage();
  const local = memoryStorage();
  if (token) (where === 'session' ? session : local).map.set('solar.session', token);
  const store = createTokenStore(session, local);
  return { api: createAccountApi('/api', f.fetchImpl), store, f, session, local };
}

const loginOk: Answer = { status: 200, json: { token: TOKEN, expiresAt: '2026-12-06T00:00:00.000Z', sessionId: SESSION.id, account: ACCOUNT } };
const signupInput = { username: 'Speedy_Gonzo', email: 'speedy@example.com', password: 'a-long-unusual-passphrase', persist: false, deviceName: 'Website, Chrome on Windows' };

describe('sign up', () => {
  it('registers, then logs in with the username and keeps the token for this tab only', async () => {
    const t = setup([{ status: 201, json: { verificationRequired: true } }, loginOk]);
    const out = await signUp(t.api, t.store, signupInput);
    expect(out.kind).toBe('done');
    expect(t.f.calls.map((c) => c.url)).toEqual(['/api/v1/auth/register', '/api/v1/auth/login']);
    expect(t.f.calls[1]!.body).toEqual({ identifier: 'Speedy_Gonzo', password: signupInput.password, deviceName: signupInput.deviceName });
    expect(t.session.map.get('solar.session')).toBe(TOKEN);
    expect(t.local.map.size).toBe(0);
  });

  it('keeps the token across visits only when asked to', async () => {
    const t = setup([{ status: 201, json: { verificationRequired: true } }, loginOk]);
    await signUp(t.api, t.store, { ...signupInput, persist: true });
    expect(t.local.map.get('solar.session')).toBe(TOKEN);
    expect(t.session.map.size).toBe(0);
  });

  it('stops at the form for problems it can see itself, without a request', async () => {
    const t = setup([]);
    const out = await signUp(t.api, t.store, { ...signupInput, username: 'ab', email: 'nope', password: 'Speedy_Gonzo-123' });
    expect(out).toEqual({
      kind: 'invalid',
      problems: [
        { field: 'username', text: 'Use 3 to 20 characters: letters, digits and underscore.' },
        { field: 'email', text: 'Enter a valid email address.' },
        // "ab" is too short to count, so the password check stops at length (16 is fine) and passes
      ],
    });
    expect(t.f.calls).toHaveLength(0);
    const out2 = await signUp(t.api, t.store, { ...signupInput, password: 'my-speedy_gonzo-pass' });
    expect(out2).toEqual({ kind: 'invalid', problems: [{ field: 'password', text: 'Your password can’t contain your username.' }] });
    expect(t.f.calls).toHaveLength(0);
  });

  it('shows the server’s weak-password reason under the field and does not log in', async () => {
    const t = setup([apiError(400, 'weak_password', { problem: 'common' })]);
    const out = await signUp(t.api, t.store, signupInput);
    expect(out).toEqual({ kind: 'invalid', problems: [{ field: 'password', text: 'That password is too common. Pick something less guessable.' }] });
    expect(t.f.calls).toHaveLength(1);
    expect(t.store.get()).toBeNull();
  });

  it('shows a taken username under the username field', async () => {
    const t = setup([apiError(409, 'username_taken')]);
    const out = await signUp(t.api, t.store, signupInput);
    expect(out).toEqual({ kind: 'invalid', problems: [{ field: 'username', text: 'That username is taken or reserved. Try another.' }] });
  });

  it('never claims success when the email already had an account (the login after it fails)', async () => {
    const t = setup([{ status: 201, json: { verificationRequired: true } }, apiError(401, 'invalid_credentials')]);
    const out = await signUp(t.api, t.store, signupInput);
    expect(out).toEqual({ kind: 'not-signed-in' });
    expect(t.store.get()).toBeNull();
  });

  it('reports a dead API as offline, not as a form problem', async () => {
    const t = setup([{ status: 404, text: '<!doctype html><title>404</title>' }]);
    const out = await signUp(t.api, t.store, signupInput);
    expect(out).toEqual({ kind: 'error', problem: { text: 'Accounts are offline right now. Try again in a few minutes.' } });
  });
});

describe('log in', () => {
  it('logs in with an email and trims what was typed', async () => {
    const t = setup([loginOk]);
    const out = await logIn(t.api, t.store, { identifier: '  speedy@example.com ', password: 'pw', persist: false, deviceName: 'Website' });
    expect(out.kind).toBe('done');
    expect(t.f.calls[0]!.body).toEqual({ identifier: 'speedy@example.com', password: 'pw', deviceName: 'Website' });
    expect(t.store.get()).toBe(TOKEN);
  });

  it('asks for both fields before sending anything', async () => {
    const t = setup([]);
    const out = await logIn(t.api, t.store, { identifier: ' ', password: '', persist: false, deviceName: 'Website' });
    expect(out).toEqual({ kind: 'invalid', problems: [{ field: 'identifier', text: 'Enter your username or email.' }, { field: 'password', text: 'Enter your password.' }] });
    expect(t.f.calls).toHaveLength(0);
  });

  it('turns wrong details and a lockout into form-level messages', async () => {
    const t = setup([apiError(401, 'invalid_credentials'), apiError(429, 'rate_limited', undefined, { 'Retry-After': '90' })]);
    const input = { identifier: 'Speedy_Gonzo', password: 'wrong', persist: false, deviceName: 'Website' };
    expect(await logIn(t.api, t.store, input)).toEqual({ kind: 'error', problem: { text: 'That username or email and password don’t match.' } });
    expect(await logIn(t.api, t.store, input)).toEqual({ kind: 'error', problem: { text: 'Too many attempts. Try again in 2 minutes.' } });
    expect(t.store.get()).toBeNull();
  });
});

describe('account page', () => {
  it('loads the account and its sessions with the stored token', async () => {
    const t = setup([{ status: 200, json: { account: ACCOUNT } }, { status: 200, json: { sessions: [SESSION] } }], TOKEN);
    const out = await loadAccount(t.api, t.store);
    expect(out.kind === 'ready' && out.account.emailVerified).toBe(false);
    expect(out.kind === 'ready' && out.sessions).toHaveLength(1);
  });

  it('goes to log in without a request when there is no token', async () => {
    const t = setup([]);
    expect(await loadAccount(t.api, t.store)).toEqual({ kind: 'signed-out' });
    expect(t.f.calls).toHaveLength(0);
  });

  it('drops a token the platform no longer accepts', async () => {
    const t = setup([apiError(401, 'unauthorized'), apiError(401, 'unauthorized')], TOKEN, 'local');
    expect(await loadAccount(t.api, t.store)).toEqual({ kind: 'signed-out' });
    expect(t.local.map.size).toBe(0);
  });

  it('keeps the token and reports when the platform cannot be reached', async () => {
    const t = setup(['network-down', 'network-down'], TOKEN);
    const out = await loadAccount(t.api, t.store);
    expect(out).toEqual({ kind: 'error', problem: { text: 'Couldn’t reach Solar RP. Check your connection and try again.' } });
    expect(t.store.get()).toBe(TOKEN);
  });
});

describe('change password', () => {
  const account = { username: 'Speedy_Gonzo', email: 'speedy@example.com', hasPassword: true };

  it('sends both passwords and reports the sessions it signed out', async () => {
    const t = setup([{ status: 200, json: { revokedSessions: 2 } }], TOKEN);
    const out = await changePassword(t.api, t.store, { currentPassword: 'old-password-1', newPassword: 'brand-new-passphrase', account });
    expect(out).toEqual({ kind: 'done', revokedSessions: 2 });
    expect(t.f.calls[0]).toMatchObject({ method: 'POST', url: '/api/v1/me/password', body: { currentPassword: 'old-password-1', newPassword: 'brand-new-passphrase' } });
  });

  it('puts a wrong current password under that field', async () => {
    const t = setup([apiError(401, 'invalid_credentials')], TOKEN);
    const out = await changePassword(t.api, t.store, { currentPassword: 'nope', newPassword: 'brand-new-passphrase', account });
    expect(out).toEqual({ kind: 'invalid', problems: [{ field: 'currentPassword', text: 'That isn’t your current password.' }] });
    expect(t.store.get()).toBe(TOKEN); // a wrong password is not a dead session
  });

  it('checks the new password against this account before sending', async () => {
    const t = setup([], TOKEN);
    const out = await changePassword(t.api, t.store, { currentPassword: '', newPassword: 'speedy-and-more', account });
    expect(out).toEqual({
      kind: 'invalid',
      problems: [
        { field: 'currentPassword', text: 'Enter your current password.' },
        { field: 'newPassword', text: 'Your password can’t contain the part of your email before the @.' },
      ],
    });
    expect(t.f.calls).toHaveLength(0);
  });

  it('leaves out the current password for an account that has none', async () => {
    const t = setup([{ status: 200, json: { revokedSessions: 0 } }], TOKEN);
    await changePassword(t.api, t.store, { currentPassword: '', newPassword: 'brand-new-passphrase', account: { ...account, hasPassword: false } });
    expect(t.f.calls[0]!.body).toEqual({ newPassword: 'brand-new-passphrase' });
  });
});

describe('sessions and log out', () => {
  it('treats a session that already ended as signed out', async () => {
    const t = setup([apiError(404, 'session_not_found')], TOKEN);
    expect(await revokeSession(t.api, t.store, SESSION.id)).toEqual({ kind: 'done' });
  });

  it('signs out every other session', async () => {
    const t = setup([{ status: 200, json: { revoked: 3 } }], TOKEN);
    expect(await revokeOtherSessions(t.api, t.store)).toEqual({ kind: 'done', revoked: 3 });
  });

  it('logs out: ends the session on the platform, then forgets the token', async () => {
    const t = setup([{ status: 204 }], TOKEN, 'local');
    expect(await logOut(t.api, t.store)).toEqual({ kind: 'signed-out' });
    expect(t.f.calls[0]).toMatchObject({ method: 'POST', url: '/api/v1/auth/logout' });
    expect(t.store.get()).toBeNull();
  });

  it('stays signed in, and says so, when the platform cannot end the session', async () => {
    const t = setup(['network-down'], TOKEN);
    expect(await logOut(t.api, t.store)).toEqual({ kind: 'error', problem: { text: 'Couldn’t reach Solar RP. Check your connection and try again.' } });
    expect(t.store.get()).toBe(TOKEN);
  });
});
