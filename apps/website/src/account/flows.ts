// The account flows, free of the DOM so they can be tested against a mocked API: each takes the API,
// the token store and the form's values, and answers with one outcome the page renders.

import type { Account, AccountApi, ApiFailure, SessionInfo } from './api';
import { describeFailure, ISSUE_TEXT_FOR, PASSWORD_TEXT, type Problem } from './errors';
import { firstPasswordProblem, looksLikeEmail, USERNAME_PATTERN } from './password';
import type { TokenStore } from './store';

export type FormOutcome<T> =
  | ({ kind: 'done' } & T)
  /** field-level problems found before or by the server; nothing was changed */
  | { kind: 'invalid'; problems: Problem[] }
  /** a problem for the whole form (rate limit, offline, server error) */
  | { kind: 'error'; problem: Problem }
  /** the token is gone or no longer valid: the page goes to log in */
  | { kind: 'signed-out' };

export interface SignUpInput {
  username: string;
  email: string;
  password: string;
  persist: boolean;
  deviceName: string;
}

/** Problems the form can see without asking the server, in field order. */
export function checkSignUp(input: Pick<SignUpInput, 'username' | 'email' | 'password'>): Problem[] {
  const out: Problem[] = [];
  if (!USERNAME_PATTERN.test(input.username)) out.push({ field: 'username', text: ISSUE_TEXT_FOR.username });
  if (!looksLikeEmail(input.email)) out.push({ field: 'email', text: ISSUE_TEXT_FOR.email });
  const p = firstPasswordProblem(input.password, { username: input.username, email: input.email });
  if (p) out.push({ field: 'password', text: PASSWORD_TEXT[p] });
  return out;
}

/**
 * Register, then log in with the same details. The platform answers a registration the same way whether
 * or not the email already had an account (so nobody can probe who is registered); the login after it
 * is what tells the page the truth. When it fails, the page says so plainly instead of claiming success.
 */
export async function signUp(api: AccountApi, store: TokenStore, input: SignUpInput): Promise<FormOutcome<{ account: Account }> | { kind: 'not-signed-in' }> {
  const local = checkSignUp(input);
  if (local.length > 0) return { kind: 'invalid', problems: local };
  const email = input.email.trim();
  const reg = await api.register({ username: input.username, email, password: input.password });
  if (!reg.ok) {
    const p = describeFailure(reg.failure, 'register');
    return p.field ? { kind: 'invalid', problems: [p] } : { kind: 'error', problem: p };
  }
  const login = await api.login({ identifier: input.username, password: input.password, deviceName: input.deviceName });
  if (!login.ok) {
    if (login.failure.kind === 'api' && login.failure.code === 'invalid_credentials') return { kind: 'not-signed-in' };
    return { kind: 'error', problem: describeFailure(login.failure, 'login') };
  }
  store.set(login.value.token, input.persist);
  return { kind: 'done', account: login.value.account };
}

export interface LogInInput {
  identifier: string;
  password: string;
  persist: boolean;
  deviceName: string;
}

export async function logIn(api: AccountApi, store: TokenStore, input: LogInInput): Promise<FormOutcome<{ account: Account }>> {
  const identifier = input.identifier.trim();
  const problems: Problem[] = [];
  if (identifier.length === 0) problems.push({ field: 'identifier', text: ISSUE_TEXT_FOR.identifier });
  if (input.password.length === 0) problems.push({ field: 'password', text: 'Enter your password.' });
  if (problems.length > 0) return { kind: 'invalid', problems };
  const r = await api.login({ identifier, password: input.password, deviceName: input.deviceName });
  if (!r.ok) {
    const p = describeFailure(r.failure, 'login');
    return p.field ? { kind: 'invalid', problems: [p] } : { kind: 'error', problem: p };
  }
  store.set(r.value.token, input.persist);
  return { kind: 'done', account: r.value.account };
}

export type LoadOutcome =
  | { kind: 'ready'; account: Account; sessions: SessionInfo[] }
  | { kind: 'signed-out' }
  | { kind: 'error'; problem: Problem };

/** The session token is gone or invalid. By CODE, not status: a wrong current password is also a 401
 *  (`invalid_credentials` from POST /v1/me/password) and must not log the player out. */
const isUnauthorized = (f: ApiFailure) => f.kind === 'api' && f.code === 'unauthorized';

/** The signed-in account and its sessions. A missing or rejected token clears the store. */
export async function loadAccount(api: AccountApi, store: TokenStore): Promise<LoadOutcome> {
  const token = store.get();
  if (token === null) return { kind: 'signed-out' };
  const [me, sessions] = await Promise.all([api.me(token), api.sessions(token)]);
  for (const r of [me, sessions]) {
    if (!r.ok && isUnauthorized(r.failure)) {
      store.clear();
      return { kind: 'signed-out' };
    }
  }
  if (!me.ok) return { kind: 'error', problem: describeFailure(me.failure, 'load') };
  if (!sessions.ok) return { kind: 'error', problem: describeFailure(sessions.failure, 'load') };
  return { kind: 'ready', account: me.value, sessions: sessions.value };
}

export interface ChangePasswordInput {
  currentPassword: string;
  newPassword: string;
  account: Pick<Account, 'username' | 'email' | 'hasPassword'>;
}

export async function changePassword(api: AccountApi, store: TokenStore, input: ChangePasswordInput): Promise<FormOutcome<{ revokedSessions: number }>> {
  const token = store.get();
  if (token === null) return { kind: 'signed-out' };
  const problems: Problem[] = [];
  if (input.account.hasPassword && input.currentPassword.length === 0) problems.push({ field: 'currentPassword', text: ISSUE_TEXT_FOR.currentPassword });
  const p = firstPasswordProblem(input.newPassword, { username: input.account.username, email: input.account.email ?? undefined });
  if (p) problems.push({ field: 'newPassword', text: PASSWORD_TEXT[p] });
  if (problems.length > 0) return { kind: 'invalid', problems };
  const body = input.account.hasPassword ? { currentPassword: input.currentPassword, newPassword: input.newPassword } : { newPassword: input.newPassword };
  const r = await api.changePassword(token, body);
  if (!r.ok) {
    if (isUnauthorized(r.failure)) {
      store.clear();
      return { kind: 'signed-out' };
    }
    const d = describeFailure(r.failure, 'change-password');
    return d.field ? { kind: 'invalid', problems: [d] } : { kind: 'error', problem: d };
  }
  return { kind: 'done', revokedSessions: r.value.revokedSessions };
}

export type ActionOutcome<T = object> = ({ kind: 'done' } & T) | { kind: 'error'; problem: Problem } | { kind: 'signed-out' };

/** Sign out one other device. A session that already ended counts as done: the list is refreshed either way. */
export async function revokeSession(api: AccountApi, store: TokenStore, id: string): Promise<ActionOutcome> {
  const token = store.get();
  if (token === null) return { kind: 'signed-out' };
  const r = await api.revokeSession(token, id);
  if (r.ok) return { kind: 'done' };
  if (isUnauthorized(r.failure)) {
    store.clear();
    return { kind: 'signed-out' };
  }
  if (r.failure.kind === 'api' && r.failure.code === 'session_not_found') return { kind: 'done' };
  return { kind: 'error', problem: describeFailure(r.failure, 'sessions') };
}

export async function revokeOtherSessions(api: AccountApi, store: TokenStore): Promise<ActionOutcome<{ revoked: number }>> {
  const token = store.get();
  if (token === null) return { kind: 'signed-out' };
  const r = await api.revokeOtherSessions(token);
  if (r.ok) return { kind: 'done', revoked: r.value.revoked };
  if (isUnauthorized(r.failure)) {
    store.clear();
    return { kind: 'signed-out' };
  }
  return { kind: 'error', problem: describeFailure(r.failure, 'sessions') };
}

/**
 * Log out. The token is dropped only once the platform has ended the session (or says it already has):
 * when the platform cannot be reached the player stays signed in and is told so, instead of a "logged
 * out" page over a session that is still live.
 */
export async function logOut(api: AccountApi, store: TokenStore): Promise<ActionOutcome> {
  const token = store.get();
  if (token === null) return { kind: 'signed-out' };
  const r = await api.logout(token);
  if (r.ok || isUnauthorized(r.failure)) {
    store.clear();
    return { kind: 'signed-out' };
  }
  return { kind: 'error', problem: describeFailure(r.failure, 'logout') };
}
