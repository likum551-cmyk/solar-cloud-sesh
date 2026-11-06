import { createHash, randomBytes } from 'node:crypto';
import { eq } from 'drizzle-orm';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { schema } from '../src/db/index.js';
import { createApp } from '../src/http/app.js';
import { purgeOauthFlows } from '../src/auth/discord.js';
import { hashToken, randomToken } from '../src/util/tokens.js';
import { createTestEnv, freshIp, makeUser, mailToken, type TestEnv, type TestUser } from './helpers.js';

// A scripted Discord: the platform talks to it through HttpDiscordClient with an injected fetch, so
// the real request building and response parsing run. It enforces what the real one does: the
// authorization code is single use, bound to the PKCE challenge it was issued for, and redeemable
// only with the client credentials of the registered app.

interface DiscordProfile {
  id: string;
  username: string;
  global_name?: string | null;
  email?: string | null;
  verified?: boolean;
}

class FakeDiscord {
  readonly requests: { url: string; method: string }[] = [];
  readonly tokenRequests: URLSearchParams[] = [];
  private readonly codes = new Map<string, { challenge: string; profile: DiscordProfile; used: boolean }>();
  private readonly tokens = new Map<string, DiscordProfile>();
  /** Force the next token or user request to fail. */
  failToken: number | null = null;
  userBody: unknown = null;

  readonly fetch: typeof fetch = async (input, init) => {
    const url = typeof input === 'string' ? input : input instanceof URL ? input.toString() : input.url;
    const method = init?.method ?? 'GET';
    this.requests.push({ url, method });
    if (!url.startsWith('https://discord.test/api/v10')) throw new Error(`the platform called a host the test did not allow: ${url}`);
    const path = url.slice('https://discord.test/api/v10'.length);

    if (path === '/oauth2/token' && method === 'POST') {
      if (this.failToken !== null) return new Response('{}', { status: this.failToken });
      const form = new URLSearchParams(String(init?.body));
      this.tokenRequests.push(form);
      const entry = this.codes.get(form.get('code') ?? '');
      const valid =
        form.get('client_id') === 'test-client-id' &&
        form.get('client_secret') === 'test-client-secret' &&
        form.get('grant_type') === 'authorization_code' &&
        form.get('redirect_uri') === 'http://platform.test/v1/auth/discord/callback' &&
        entry !== undefined &&
        !entry.used &&
        createHash('sha256').update(form.get('code_verifier') ?? '', 'ascii').digest('base64url') === entry.challenge;
      if (!valid || !entry) return new Response(JSON.stringify({ error: 'invalid_grant' }), { status: 400 });
      entry.used = true;
      const accessToken = `at_${randomBytes(8).toString('hex')}`;
      this.tokens.set(accessToken, entry.profile);
      return new Response(JSON.stringify({ access_token: accessToken, token_type: 'Bearer' }), { status: 200 });
    }
    if (path === '/users/@me' && method === 'GET') {
      const auth = new Headers(init?.headers).get('authorization') ?? '';
      const profile = this.tokens.get(auth.replace(/^Bearer /, ''));
      if (!profile) return new Response('{}', { status: 401 });
      return new Response(JSON.stringify(this.userBody ?? profile), { status: 200 });
    }
    return new Response('{}', { status: 404 });
  };

  /** The player approves in the browser: returns what Discord would redirect to. `challenge` overrides the one in the URL (an attacker swapping it). */
  approve(authorizeUrl: string, profile: DiscordProfile, overrideChallenge?: string): { code: string; state: string } {
    const url = new URL(authorizeUrl);
    expect(url.origin + url.pathname).toBe('https://discord.test/oauth2/authorize');
    expect(url.searchParams.get('response_type')).toBe('code');
    expect(url.searchParams.get('client_id')).toBe('test-client-id');
    expect(url.searchParams.get('scope')).toBe('identify email');
    expect(url.searchParams.get('redirect_uri')).toBe('http://platform.test/v1/auth/discord/callback');
    expect(url.searchParams.get('code_challenge_method')).toBe('S256');
    const challenge = url.searchParams.get('code_challenge') ?? '';
    expect(challenge).toMatch(/^[A-Za-z0-9_-]{43}$/);
    const state = url.searchParams.get('state') ?? '';
    expect(state.length).toBeGreaterThanOrEqual(40);
    const code = `code_${randomBytes(8).toString('hex')}`;
    this.codes.set(code, { challenge: overrideChallenge ?? challenge, profile, used: false });
    return { code, state };
  }
}

let env: TestEnv;
let discord: FakeDiscord;

beforeAll(async () => {
  discord = new FakeDiscord();
  env = await createTestEnv({ env: { DISCORD_API_BASE: 'https://discord.test/api/v10' }, fetchImpl: discord.fetch });
});
afterAll(async () => {
  await env.close();
});

interface FlowStart { flowId: string; authorizeUrl: string; pollSecret: string; expiresAt: string; error?: { code: string } }
type Poll = { status: string; code?: string; token?: string; suggestedUsername?: string; discordUsername?: string; account?: { id: string; username: string; email: string | null; emailVerified: boolean; hasPassword: boolean; discord: { id: string; username: string } | null }; error?: { code: string } };

let discordIdCounter = 100000000000000000n;
const newProfile = (over: Partial<DiscordProfile> = {}): DiscordProfile => {
  discordIdCounter += 1n;
  return { id: String(discordIdCounter), username: `dc_user_${discordIdCounter % 10000n}`, global_name: 'Display Name', email: null, verified: false, ...over };
};

/**
 * What the callback page handed to the approving browser (the gage-mp:// URL), by flow. The
 * launcher presents it with every poll and complete, so these helpers do the same; the attacks
 * that must NOT have it (someone else's flow) are in security.test.ts.
 */
const claims = new Map<string, string>();
const HANDOFF = /href="gage-mp:\/\/auth\/discord\?flow=([0-9a-f-]{36})&amp;claim=([A-Za-z0-9_-]{43})"/;
const claimOf = (flow: FlowStart): { claim?: string } => {
  const claim = claims.get(flow.flowId);
  return claim === undefined ? {} : { claim };
};

const start = (ip = freshIp()) => env.call<FlowStart>('/v1/auth/discord/start', { method: 'POST', ip });
const callback = async (q: { code?: string; state?: string; error?: string }, ip = freshIp()) => {
  const qs = new URLSearchParams(Object.entries(q).filter(([, v]) => v !== undefined) as [string, string][]).toString();
  const res = await env.call<{ text?: string }>(`/v1/auth/discord/callback${qs ? `?${qs}` : ''}`, { ip });
  const m = HANDOFF.exec(res.body.text ?? '');
  if (m) claims.set(m[1] as string, m[2] as string);
  return res;
};
const poll = (flow: FlowStart, secret = flow.pollSecret) => env.call<Poll>('/v1/auth/discord/poll', { json: { flowId: flow.flowId, pollSecret: secret, ...claimOf(flow) }, ip: freshIp() });
/** A flow nobody approved has no claim: a random one stands in (the schema requires one). */
const complete = (flow: FlowStart, username: string, secret = flow.pollSecret) =>
  env.call<Poll & { sessionId?: string }>('/v1/auth/discord/complete', { json: { flowId: flow.flowId, pollSecret: secret, username, claim: claims.get(flow.flowId) ?? randomToken(32) }, ip: freshIp() });
const flowRow = async (id: string) => (await env.handle.db.select().from(schema.oauthFlows).where(eq(schema.oauthFlows.id, id)))[0];

/** start -> approve -> callback: returns the flow after Discord redirected back. */
async function runLogin(profile: DiscordProfile): Promise<{ flow: FlowStart; callbackStatus: number }> {
  const flow = (await start()).body;
  const { code, state } = discord.approve(flow.authorizeUrl, profile);
  const cb = await callback({ code, state });
  return { flow, callbackStatus: cb.status };
}

/** A brand new Discord player ends up with an account. */
async function signUpWithDiscord(profile: DiscordProfile, username: string) {
  const { flow, callbackStatus } = await runLogin(profile);
  expect(callbackStatus).toBe(200);
  const done = await complete(flow, username);
  expect(done.status, JSON.stringify(done.body)).toBe(200);
  return { flow, done: done.body };
}

describe('starting a flow', () => {
  it('returns an authorize URL with state and a PKCE S256 challenge; only hashes and the verifier are stored', async () => {
    const res = await start();
    expect(res.status).toBe(200);
    const url = new URL(res.body.authorizeUrl);
    expect(url.origin).toBe('https://discord.test');
    const state = url.searchParams.get('state') as string;
    const challenge = url.searchParams.get('code_challenge') as string;
    expect(url.searchParams.get('code_challenge_method')).toBe('S256');
    expect(url.searchParams.get('prompt')).toBe('consent');
    expect(res.body.pollSecret.length).toBeGreaterThanOrEqual(40);
    expect(new Date(res.body.expiresAt).getTime() - env.clock.now().getTime()).toBe(600_000);

    const row = await flowRow(res.body.flowId);
    expect(row).toMatchObject({ intent: 'login', status: 'pending', accountId: null });
    expect(row?.stateHash).toBe(hashToken(state));
    expect(row?.stateHash).not.toBe(state);
    expect(row?.pollSecretHash).toBe(hashToken(res.body.pollSecret));
    // The stored verifier is the one the challenge was derived from (and is never sent to the browser).
    expect(createHash('sha256').update(row?.codeVerifier ?? '', 'ascii').digest('base64url')).toBe(challenge);
    expect(res.body.authorizeUrl).not.toContain(row?.codeVerifier as string);
    expect(res.body.authorizeUrl).not.toContain('test-client-secret');
    expect((await poll(res.body)).body).toEqual({ status: 'pending' });
  });

  it('is a 404 when Discord is not configured', async () => {
    const off = createApp({ ...env.deps, discord: null }, 'all').app;
    const peer = { incoming: { socket: { remoteAddress: freshIp() } } };
    const res = await off.request('/v1/auth/discord/start', { method: 'POST' }, peer);
    expect(res.status).toBe(404);
    expect(((await res.json()) as { error: { code: string } }).error.code).toBe('discord_disabled');
    expect((await off.request('/v1/auth/discord/callback?code=x&state=y', {}, peer)).status).toBe(400);
    // Control: the same request on the configured app works.
    expect((await start()).status).toBe(200);
  });

  it('is rate limited per IP', async () => {
    const ip = freshIp();
    const results = [];
    for (let i = 0; i < 22; i++) results.push((await start(ip)).status);
    expect(results.filter((s) => s === 200)).toHaveLength(20);
    expect(results.filter((s) => s === 429)).toHaveLength(2);
  });
});

describe('a new Discord player', () => {
  it('callback -> needs_username -> complete creates the account, links Discord and logs in; the flow is then spent', async () => {
    const profile = newProfile({ username: 'cool.guy-99', email: 'newbie.dc@example.test', verified: true });
    const { flow, callbackStatus } = await runLogin(profile);
    expect(callbackStatus).toBe(200);

    // PKCE and the client credentials really went to Discord's token endpoint.
    const tokenReq = discord.tokenRequests.at(-1) as URLSearchParams;
    expect(tokenReq.get('client_secret')).toBe('test-client-secret');
    expect(tokenReq.get('code_verifier')).toHaveLength(64);

    // The verifier is gone once the callback finished.
    expect((await flowRow(flow.flowId))?.codeVerifier).toBeNull();

    const waiting = (await poll(flow)).body;
    expect(waiting.status).toBe('needs_username');
    expect(waiting.discordUsername).toBe('cool.guy-99');
    expect(waiting.suggestedUsername).toBe('cool_guy_99'); // invalid characters become "_"
    expect(waiting.token).toBeUndefined();

    const done = await complete(flow, 'CoolGuy99');
    expect(done.status).toBe(200);
    expect(done.body.account).toMatchObject({ username: 'CoolGuy99', email: 'newbie.dc@example.test', emailVerified: true, hasPassword: false, discord: { id: profile.id, username: 'cool.guy-99' } });
    const me = await env.call<{ account: { id: string; username: string } }>('/v1/me', { token: done.body.token as string });
    expect(me.status).toBe(200);
    expect(me.body.account.username).toBe('CoolGuy99');

    const spent = await flowRow(flow.flowId);
    expect(spent).toMatchObject({ status: 'consumed', profile: null });
    expect((await poll(flow)).status).toBe(410);
    expect((await complete(flow, 'Another_Name')).status).toBe(410);
  });

  it('never trusts an unverified Discord email: it is not stored and the account has no verified email', async () => {
    const profile = newProfile({ email: 'unverified.dc@example.test', verified: false });
    const { done } = await signUpWithDiscord(profile, `Unv_${profile.id.slice(-6)}`);
    expect(done.account).toMatchObject({ email: null, emailVerified: false });
    const [identity] = await env.handle.db.select().from(schema.oauthIdentities).where(eq(schema.oauthIdentities.providerUserId, profile.id));
    expect(identity).toMatchObject({ providerEmail: null, emailVerified: false });
  });

  it('a Discord player without an email at all can still sign up', async () => {
    const profile = newProfile({ email: null });
    const { done } = await signUpWithDiscord(profile, `NoMail_${profile.id.slice(-6)}`);
    expect(done.account).toMatchObject({ email: null, hasPassword: false });
  });

  it('a taken, reserved or malformed username does not spend the flow', async () => {
    const taken = await makeUser(env, { username: 'Taken_Name_1' });
    const { flow } = await runLogin(newProfile());
    expect((await complete(flow, 'taken_name_1')).body.error?.code).toBe('username_taken'); // case-insensitive
    expect((await complete(flow, 'admin')).body.error?.code).toBe('username_taken'); // reserved
    expect((await complete(flow, 'x')).status).toBe(400);
    expect((await complete(flow, 'has space')).status).toBe(400);
    expect((await poll(flow)).body.status).toBe('needs_username');
    const ok = await complete(flow, 'Free_Name_1');
    expect(ok.status).toBe(200);
    expect(ok.body.account?.username).toBe('Free_Name_1');
    expect(taken.accountId).not.toBe(ok.body.account?.id);
  });

  it('two simultaneous completions create exactly one account', async () => {
    const profile = newProfile();
    const { flow } = await runLogin(profile);
    const results = await Promise.all([complete(flow, 'Race_One_1'), complete(flow, 'Race_Two_2')]);
    expect(results.map((r) => r.status).sort()).toEqual([200, 410]);
    const identities = await env.handle.db.select().from(schema.oauthIdentities).where(eq(schema.oauthIdentities.providerUserId, profile.id));
    expect(identities).toHaveLength(1);
  });

  it('a verified Discord email taken by a password account in the meantime is refused at completion', async () => {
    const profile = newProfile({ email: 'raced.mail@example.test', verified: true });
    const { flow } = await runLogin(profile);
    const reg = await env.call('/v1/auth/register', { json: { email: 'raced.mail@example.test', username: 'Raced_Mail_Taker', password: 'a-long-unusual-password-77' }, ip: freshIp() });
    expect(reg.status).toBe(201);
    const res = await complete(flow, 'Raced_Mail_Dc');
    expect(res.status).toBe(409);
    expect(res.body.error?.code).toBe('email_in_use');
    const accounts = await env.handle.db.select().from(schema.accounts).where(eq(schema.accounts.username, 'Raced_Mail_Dc'));
    expect(accounts).toHaveLength(0);
  });
});

describe('a returning Discord player', () => {
  it('logs straight in on the Discord id (never the email or name) and the result is delivered once', async () => {
    const profile = newProfile({ username: 'Returning_One' });
    const { done } = await signUpWithDiscord(profile, 'Returning_Acct');
    const accountId = done.account?.id as string;

    // Same Discord id, but Discord now reports another name and a new email: still the same account.
    const again = { ...profile, username: 'renamed_on_discord', email: 'someone.else@example.test', verified: true };
    const { flow, callbackStatus } = await runLogin(again);
    expect(callbackStatus).toBe(200);
    const res = await poll(flow);
    expect(res.body.status).toBe('complete');
    expect(res.body.account?.id).toBe(accountId);
    expect(res.body.token).toBeTruthy();
    const me = await env.call<{ account: { id: string } }>('/v1/me', { token: res.body.token as string });
    expect(me.body.account.id).toBe(accountId);

    const second = await poll(flow);
    expect(second.status).toBe(410);
    expect(second.body.token).toBeUndefined();
    const [acct] = await env.handle.db.select().from(schema.accounts).where(eq(schema.accounts.id, accountId));
    expect(acct?.email).toBeNull(); // the changed Discord email never overwrote anything
  });

  it('concurrent polls of a finished login: exactly one gets the session', async () => {
    const profile = newProfile();
    await signUpWithDiscord(profile, `Poll_Race_${profile.id.slice(-5)}`);
    const { flow } = await runLogin(profile);
    const results = await Promise.all(Array.from({ length: 5 }, () => poll(flow)));
    expect(results.filter((r) => r.body.status === 'complete')).toHaveLength(1);
    expect(results.filter((r) => r.status === 410)).toHaveLength(4);
  });
});

describe('never auto-merge by email', () => {
  const owner = async (): Promise<TestUser> => makeUser(env);

  it('a Discord login whose verified email belongs to a GAGE account fails with email_in_use and creates nothing', async () => {
    const u = await owner();
    const profile = newProfile({ email: u.email, verified: true });
    const { flow, callbackStatus } = await runLogin(profile);
    expect(callbackStatus).toBe(400); // the browser page reports a refusal; the reason is in the poll
    const res = await poll(flow);
    expect(res.body).toEqual({ status: 'failed', code: 'email_in_use' });
    expect(await env.handle.db.select().from(schema.oauthIdentities).where(eq(schema.oauthIdentities.providerUserId, profile.id))).toHaveLength(0);
    expect(await env.handle.db.select().from(schema.accounts).where(eq(schema.accounts.email, u.email))).toHaveLength(1);
  });

  it('an UNVERIFIED Discord email that matches an account is refused just the same (and certainly not trusted)', async () => {
    const u = await owner();
    const profile = newProfile({ email: u.email.toUpperCase(), verified: false });
    const { flow } = await runLogin(profile);
    expect((await poll(flow)).body).toEqual({ status: 'failed', code: 'email_in_use' });
    expect(discord.tokenRequests.length).toBeGreaterThan(0);
  });

  it('the account owner links Discord from inside their session; after that Discord login lands on the same account', async () => {
    const u = await owner();
    const profile = newProfile({ username: 'linker', email: u.email, verified: true });

    expect((await env.call('/v1/me/discord/link', { json: { password: u.password }, ip: freshIp() })).status).toBe(401);
    const link = await env.call<FlowStart>('/v1/me/discord/link', { json: { password: u.password }, token: u.token });
    expect(link.status).toBe(200);
    expect((await flowRow(link.body.flowId))).toMatchObject({ intent: 'link', accountId: u.accountId, handoff: 'launcher' });
    const { code, state } = discord.approve(link.body.authorizeUrl, profile);
    expect((await callback({ code, state })).status).toBe(200);
    expect((await poll(link.body)).body).toEqual({ status: 'linked' });
    expect((await poll(link.body)).status).toBe(410);

    const me = await env.call<{ account: { discord: { id: string; username: string } | null; hasPassword: boolean } }>('/v1/me', { token: u.token });
    expect(me.body.account.discord).toEqual({ id: profile.id, username: 'linker' });
    expect(me.body.account.hasPassword).toBe(true);

    const { flow } = await runLogin(profile);
    const res = await poll(flow);
    expect(res.body.status).toBe('complete');
    expect(res.body.account?.id).toBe(u.accountId);
  });

  it('linking is refused when Discord is already linked elsewhere, or the account already has another Discord', async () => {
    const a = await owner();
    const b = await owner();
    const profile = newProfile();
    const startLink = (user: TestUser) => env.call<FlowStart>('/v1/me/discord/link', { json: { password: user.password }, token: user.token });
    const linkWith = async (user: TestUser, p: DiscordProfile, flow?: FlowStart) => {
      const link = flow ?? (await startLink(user)).body;
      const { code, state } = discord.approve(link.authorizeUrl, p);
      await callback({ code, state });
      return poll(link);
    };
    // Two flows of A started before anything is linked: they finish after the first link landed.
    const second = (await startLink(a)).body;
    const third = (await startLink(a)).body;
    expect((await linkWith(a, profile)).body.status).toBe('linked');
    expect((await linkWith(b, profile)).body).toEqual({ status: 'failed', code: 'discord_already_linked' });
    expect((await linkWith(a, newProfile(), second)).body).toEqual({ status: 'failed', code: 'already_has_discord' });
    expect((await linkWith(a, profile, third)).body.status).toBe('linked'); // the same pair again is a harmless no-op
    // Once linked, a new start is refused before anyone is sent to Discord.
    const again = await startLink(a);
    expect(again.status).toBe(409);
    expect((again.body as unknown as { error: { code: string } }).error.code).toBe('already_has_discord');
    const rows = await env.handle.db.select().from(schema.oauthIdentities).where(eq(schema.oauthIdentities.providerUserId, profile.id));
    expect(rows).toHaveLength(1);
    expect(rows[0]?.accountId).toBe(a.accountId);
  });

  it('the database allows one Discord id per account and one account per Discord id', async () => {
    const a = await owner();
    const b = await owner();
    const base = { provider: 'discord', providerUsername: 'x', emailVerified: false, createdAt: env.clock.now() };
    await env.handle.db.insert(schema.oauthIdentities).values({ ...base, accountId: a.accountId, providerUserId: '777000000000000001' });
    await expect(env.handle.db.insert(schema.oauthIdentities).values({ ...base, accountId: b.accountId, providerUserId: '777000000000000001' })).rejects.toBeDefined();
    await expect(env.handle.db.insert(schema.oauthIdentities).values({ ...base, accountId: a.accountId, providerUserId: '777000000000000002' })).rejects.toBeDefined();
  });
});

describe('unlinking', () => {
  const unlink = (u: TestUser, password = u.password) => env.call<{ error?: { code: string } }>('/v1/me/discord', { method: 'DELETE', json: { password }, token: u.token, ip: freshIp() });

  it('a password account can unlink with its current password; unlinking twice is a 404', async () => {
    const u = await makeUser(env);
    const profile = newProfile();
    const link = await env.call<FlowStart>('/v1/me/discord/link', { json: { password: u.password }, token: u.token });
    const { code, state } = discord.approve(link.body.authorizeUrl, profile);
    await callback({ code, state });
    expect((await poll(link.body)).body.status).toBe('linked');
    // Re-authentication: no password is a 400, a wrong one 401 (and nothing is removed).
    expect((await env.call('/v1/me/discord', { method: 'DELETE', token: u.token })).status).toBe(400);
    const wrong = await unlink(u, 'not-the-password-123');
    expect(wrong.status).toBe(401);
    expect(wrong.body.error?.code).toBe('invalid_credentials');
    expect((await env.call<{ account: { discord: unknown } }>('/v1/me', { token: u.token })).body.account.discord).not.toBeNull();
    expect((await unlink(u)).status).toBe(204);
    const me = await env.call<{ account: { discord: unknown } }>('/v1/me', { token: u.token });
    expect(me.body.account.discord).toBeNull();
    expect((await unlink(u)).body.error?.code).toBe('not_linked');
  });

  it('a Discord-only account cannot unlink until it has set a password', async () => {
    const profile = newProfile({ email: 'discord.only@example.test', verified: true });
    const { done } = await signUpWithDiscord(profile, 'Discord_Only_1');
    const token = done.token as string;
    const res = await env.call<{ error: { code: string } }>('/v1/me/discord', { method: 'DELETE', json: { password: 'any-password-at-all' }, token });
    expect(res.status).toBe(409);
    expect(res.body.error.code).toBe('last_login_method');

    const set = await env.call('/v1/me/password', { json: { newPassword: 'first-ever-password-42' }, token });
    expect(set.status, JSON.stringify(set.body)).toBe(200);
    expect((await env.call('/v1/me/discord', { method: 'DELETE', json: { password: 'first-ever-password-42' }, token })).status).toBe(204);
    const login = await env.call('/v1/auth/login', { json: { identifier: 'discord.only@example.test', password: 'first-ever-password-42' }, ip: freshIp() });
    expect(login.status).toBe(200);
  });

  it('a Discord-only account without an email cannot set a password at all (and so stays linked)', async () => {
    const profile = newProfile({ email: null });
    const { done } = await signUpWithDiscord(profile, `Mailless_${profile.id.slice(-6)}`);
    const token = done.token as string;
    const set = await env.call<{ error: { code: string } }>('/v1/me/password', { json: { newPassword: 'first-ever-password-43' }, token });
    expect(set.status).toBe(409);
    expect(set.body.error.code).toBe('email_required');
    expect((await env.call('/v1/me/discord', { method: 'DELETE', json: { password: 'any-password-at-all' }, token })).status).toBe(409);
  });

  it('a Discord-only account can also reach a password through the emailed reset', async () => {
    const profile = newProfile({ email: 'dc.reset@example.test', verified: true });
    const { done } = await signUpWithDiscord(profile, 'Discord_Reset_1');
    await env.call('/v1/auth/forgot-password', { json: { email: 'dc.reset@example.test' }, ip: freshIp() });
    const token = await mailToken(env, 'dc.reset@example.test', 'password_reset');
    const reset = await env.call('/v1/auth/reset-password', { json: { token, password: 'reset-through-mail-99' }, ip: freshIp() });
    expect(reset.status).toBe(200);
    const login = await env.call<{ token: string; account: { id: string } }>('/v1/auth/login', { json: { identifier: 'Discord_Reset_1', password: 'reset-through-mail-99' }, ip: freshIp() });
    expect(login.status).toBe(200);
    expect(login.body.account.id).toBe(done.account?.id);
    expect((await env.call('/v1/me/discord', { method: 'DELETE', json: { password: 'reset-through-mail-99' }, token: login.body.token })).status).toBe(204);
  });
});

describe('linking from the website (handoff "web")', () => {
  type Err = { error?: { code: string } };
  const startWeb = (u: TestUser, password = u.password, ip = freshIp()) =>
    env.call<FlowStart & Err>('/v1/me/discord/link', { json: { password, handoff: 'web' }, token: u.token, ip });
  /** The approving browser hits the callback; a web flow answers with a redirect to the account page. */
  async function webCallback(q: { code?: string; state?: string; error?: string }, app = env.app) {
    const qs = new URLSearchParams(Object.entries(q).filter(([, v]) => v !== undefined) as [string, string][]).toString();
    const res = await app.request(`/v1/auth/discord/callback?${qs}`, {}, { incoming: { socket: { remoteAddress: freshIp() } } });
    const location = res.headers.get('location');
    const url = location === null ? null : new URL(location);
    const fragment = new URLSearchParams(url?.hash.slice(1) ?? '');
    return { status: res.status, location, url, flowId: fragment.get('discord_flow'), claim: fragment.get('discord_claim'), body: await res.text() };
  }
  const finish = (u: TestUser, body: Record<string, unknown>) => env.call<Poll>('/v1/me/discord/link/finish', { json: body, token: u.token, ip: freshIp() });
  const coins = async (u: TestUser) =>
    (await env.call<{ balances: { currency: string; balance: number }[] }>('/v1/me/wallet', { token: u.token })).body.balances.find((b) => b.currency === 'coins')?.balance ?? 0;
  const linkQuest = async (u: TestUser) =>
    (await env.call<{ discordLinked: boolean; quests: { slug: string; status: string; lockReason: string | null }[] }>('/v1/me/quests', { token: u.token })).body;

  it('start (password) -> Discord -> 303 to the account page with flow and claim in the fragment -> finish links; the link quest then pays 500', async () => {
    const u = await makeUser(env);
    const before = await linkQuest(u);
    expect(before.discordLinked).toBe(false);
    expect(before.quests.find((q) => q.slug === 'link_discord')).toMatchObject({ status: 'locked', lockReason: 'discord_required' });
    expect((await env.call<Err>('/v1/me/quests/link_discord/claim', { method: 'POST', token: u.token })).body.error?.code).toBe('discord_required');

    const start = await startWeb(u);
    expect(start.status, JSON.stringify(start.body)).toBe(200);
    expect(await flowRow(start.body.flowId)).toMatchObject({ intent: 'link', handoff: 'web', accountId: u.accountId, status: 'pending' });

    const profile = newProfile({ username: 'web.linker' });
    const { code, state } = discord.approve(start.body.authorizeUrl, profile);
    const cb = await webCallback({ code, state });
    expect(cb.status).toBe(303);
    // The configured site origin, the account page, nothing in the query (so nothing in any access log), flow and claim in the fragment.
    expect(cb.url?.origin).toBe('https://solar-rp.com');
    expect(cb.url?.pathname).toBe('/account/');
    expect(cb.url?.search).toBe('');
    expect(cb.flowId).toBe(start.body.flowId);
    expect(cb.claim).toMatch(/^[A-Za-z0-9_-]{43}$/);
    expect(cb.location).not.toContain(code);
    expect(cb.location).not.toContain(state);
    expect(cb.body).not.toContain('gage-mp://');
    expect((await flowRow(start.body.flowId))?.claimHash).toBe(hashToken(cb.claim as string));

    const done = await finish(u, { flowId: start.body.flowId, pollSecret: start.body.pollSecret, claim: cb.claim });
    expect(done.status, JSON.stringify(done.body)).toBe(200);
    expect(done.body).toEqual({ status: 'linked' });
    const me = await env.call<{ account: { discord: { id: string; username: string } | null } }>('/v1/me', { token: u.token });
    expect(me.body.account.discord).toEqual({ id: profile.id, username: 'web.linker' });
    // Spent: a second finish is 410.
    expect((await finish(u, { flowId: start.body.flowId, pollSecret: start.body.pollSecret, claim: cb.claim })).status).toBe(410);

    // The quest the link unlocks is now claimable and pays.
    const after = await linkQuest(u);
    expect(after.discordLinked).toBe(true);
    expect(after.quests.find((q) => q.slug === 'link_discord')?.status).toBe('claimable');
    const paid = await env.call<{ credited: { currency: string; amount: number }; balance: number }>('/v1/me/quests/link_discord/claim', { method: 'POST', token: u.token });
    expect(paid.status, JSON.stringify(paid.body)).toBe(200);
    expect(paid.body.credited).toEqual({ currency: 'coins', amount: 500 });
    expect(await coins(u)).toBe(500);
  });

  it('a refusal also comes back to the account page, and the page reads its code: Discord already linked to another account, access denied', async () => {
    const owner = await makeUser(env);
    const other = await makeUser(env);
    const profile = newProfile();
    const first = (await startWeb(owner)).body;
    const a = discord.approve(first.authorizeUrl, profile);
    const cbA = await webCallback(a);
    expect((await finish(owner, { flowId: first.flowId, pollSecret: first.pollSecret, claim: cbA.claim })).body).toEqual({ status: 'linked' });

    const taken = (await startWeb(other)).body;
    const cbB = await webCallback(discord.approve(taken.authorizeUrl, profile));
    expect(cbB.status).toBe(303);
    expect((await finish(other, { flowId: taken.flowId, pollSecret: taken.pollSecret, claim: cbB.claim })).body).toEqual({ status: 'failed', code: 'discord_already_linked' });
    expect((await env.call<{ account: { discord: unknown } }>('/v1/me', { token: other.token })).body.account.discord).toBeNull();
    const rows = await env.handle.db.select().from(schema.oauthIdentities).where(eq(schema.oauthIdentities.providerUserId, profile.id));
    expect(rows.map((r) => r.accountId)).toEqual([owner.accountId]);

    const denied = (await startWeb(other)).body;
    const cbD = await webCallback({ error: 'access_denied', state: new URL(denied.authorizeUrl).searchParams.get('state') as string });
    expect(cbD.status).toBe(303);
    expect((await finish(other, { flowId: denied.flowId, pollSecret: denied.pollSecret, claim: cbD.claim })).body).toEqual({ status: 'failed', code: 'access_denied' });
  });

  it('a wrong password is refused, counted and then locked like a login (shared with the password change); another address block is not locked out', async () => {
    const u = await makeUser(env);
    const ip = '192.0.2.10';
    for (let i = 0; i < 3; i++) {
      const wrong = await startWeb(u, `wrong-password-${i}-xx`, ip);
      expect(wrong.status).toBe(401);
      expect(wrong.body.error?.code).toBe('invalid_credentials');
    }
    // Two more wrong guesses through the password change: one budget for every re-authentication.
    for (let i = 0; i < 2; i++) {
      expect((await env.call('/v1/me/password', { json: { currentPassword: `wrong-password-cp-${i}`, newPassword: 'a-brand-new-password-1' }, token: u.token, ip })).status).toBe(401);
    }
    expect(await env.handle.db.select().from(schema.oauthFlows).where(eq(schema.oauthFlows.accountId, u.accountId))).toHaveLength(0);
    // Locked from this block: even the right password is refused before it is checked.
    const locked = await startWeb(u, u.password, ip);
    expect(locked.status).toBe(429);
    expect(locked.body.error?.code).toBe('rate_limited');
    expect(Number(locked.headers.get('retry-after'))).toBeGreaterThan(0);
    // The account's owner elsewhere is not locked out by somebody else's guesses.
    expect((await startWeb(u, u.password, '203.0.113.200')).status).toBe(200);
  });

  it('a link finished after the password changed is refused (password_changed), and nothing is linked', async () => {
    const u = await makeUser(env);
    const start = (await startWeb(u)).body;
    const cb = await webCallback(discord.approve(start.authorizeUrl, newProfile()));
    env.clock.advanceSeconds(2);
    const changed = await env.call('/v1/me/password', { json: { currentPassword: u.password, newPassword: 'changed-after-start-77' }, token: u.token, ip: freshIp() });
    expect(changed.status, JSON.stringify(changed.body)).toBe(200);
    expect((await finish(u, { flowId: start.flowId, pollSecret: start.pollSecret, claim: cb.claim })).body).toEqual({ status: 'failed', code: 'password_changed' });
    expect((await env.call<{ account: { discord: unknown } }>('/v1/me', { token: u.token })).body.account.discord).toBeNull();
  });

  it('the redirect target is configuration only: a query parameter cannot move it, SITE_ORIGIN does', async () => {
    const u = await makeUser(env);
    const start = (await startWeb(u)).body;
    const { code, state } = discord.approve(start.authorizeUrl, newProfile());
    const params = new URLSearchParams({ code, state, redirect_uri: 'https://evil.test/', return_to: 'https://evil.test/', next: '//evil.test' });
    const res = await env.app.request(`/v1/auth/discord/callback?${params.toString()}`, {}, { incoming: { socket: { remoteAddress: freshIp() } } });
    expect(res.status).toBe(303);
    expect(new URL(res.headers.get('location') ?? '').origin).toBe('https://solar-rp.com');

    // Another deployment's origin comes from its configuration.
    const elsewhere = createApp({ ...env.deps, config: { ...env.config, siteOrigin: 'https://staging.solar-rp.test' } }, 'all').app;
    const s2 = (await startWeb(u)).body;
    const cb2 = await webCallback(discord.approve(s2.authorizeUrl, newProfile()), elsewhere);
    expect(cb2.url?.origin).toBe('https://staging.solar-rp.test');
    expect(cb2.url?.pathname).toBe('/account/');
  });

  it('needs a session, a password and a known handoff; a Discord-only account is told it already has Discord', async () => {
    const u = await makeUser(env);
    expect((await env.call('/v1/me/discord/link', { json: { password: u.password, handoff: 'web' }, ip: freshIp() })).status).toBe(401);
    expect((await env.call('/v1/me/discord/link', { json: { handoff: 'web' }, token: u.token, ip: freshIp() })).status).toBe(400);
    expect((await env.call('/v1/me/discord/link', { json: { password: u.password, handoff: 'https://evil.test' }, token: u.token, ip: freshIp() })).status).toBe(400);
    expect((await env.call('/v1/me/discord/link/finish', { json: { flowId: '11111111-1111-4111-8111-111111111111', pollSecret: randomToken(32), claim: randomToken(32) }, ip: freshIp() })).status).toBe(401);

    const { done } = await signUpWithDiscord(newProfile({ email: null }), `WebDcOnly_${String(discordIdCounter).slice(-5)}`);
    const res = await env.call<Err>('/v1/me/discord/link', { json: { password: 'whatever-password-1', handoff: 'web' }, token: done.token as string, ip: freshIp() });
    expect(res.status).toBe(409);
    expect(res.body.error?.code).toBe('already_has_discord');
  });

  it('the database refuses a web handoff on a login flow, and any handoff it does not know', async () => {
    const row = (over: Partial<typeof schema.oauthFlows.$inferInsert>) =>
      env.handle.db.insert(schema.oauthFlows).values({ provider: 'discord', intent: 'login', stateHash: hashToken(randomToken(32)), pollSecretHash: hashToken(randomToken(32)), expiresAt: env.clock.now(), ...over });
    await expect(row({ handoff: 'web' })).rejects.toThrow();
    await expect(row({ handoff: 'browser' })).rejects.toThrow();
    // Controls: the same row as a launcher login, and as a web link, are accepted.
    await expect(row({ handoff: 'launcher' })).resolves.toBeDefined();
    const u = await makeUser(env);
    await expect(row({ intent: 'link', accountId: u.accountId, handoff: 'web' })).resolves.toBeDefined();
  });
});

describe('state, PKCE and flow validation', () => {
  it('a forged, missing, oversized or replayed state exchanges nothing with Discord', async () => {
    const before = discord.tokenRequests.length;
    expect((await callback({ code: 'c', state: 'forged-state-value' })).status).toBe(400);
    expect((await callback({ code: 'c' })).status).toBe(400);
    expect((await callback({ state: 's'.repeat(300), code: 'c' })).status).toBe(400);
    expect((await callback({})).status).toBe(400);
    expect(discord.tokenRequests.length).toBe(before);

    const flow = (await start()).body;
    const { code, state } = discord.approve(flow.authorizeUrl, newProfile());
    expect((await callback({ code, state })).status).toBe(200);
    const exchanged = discord.tokenRequests.length;
    expect(exchanged).toBe(before + 1);
    // Replaying the very same redirect is refused before any exchange.
    const replay = await callback({ code, state });
    expect(replay.status).toBe(400);
    expect(discord.tokenRequests.length).toBe(exchanged);
    // Another flow's state with this flow's code cannot be used either: the code is single use at Discord.
    const other = (await start()).body;
    const otherState = new URL(other.authorizeUrl).searchParams.get('state') as string;
    expect((await callback({ code, state: otherState })).status).toBe(400);
    expect((await poll(other)).body).toEqual({ status: 'failed', code: 'discord_error' });
  });

  it('the callback page echoes nothing from the request and locks down content', async () => {
    const res = await env.app.request('/v1/auth/discord/callback?state=%3Cscript%3Ealert(1)%3C%2Fscript%3E&code=x', {}, { incoming: { socket: { remoteAddress: freshIp() } } });
    const html = await res.text();
    expect(res.status).toBe(400);
    expect(html).not.toContain('<script');
    expect(html).not.toContain('alert(1)');
    expect(res.headers.get('content-security-policy')).toContain("default-src 'none'");
    expect(res.headers.get('content-type')).toContain('text/html');
  });

  it('an expired flow cannot be called back, polled or completed', async () => {
    const pending = (await start()).body;
    const waiting = await runLogin(newProfile());
    env.clock.advanceSeconds(601);
    const { code, state } = discord.approve(pending.authorizeUrl, newProfile());
    const before = discord.tokenRequests.length;
    expect((await callback({ code, state })).status).toBe(400);
    expect(discord.tokenRequests.length).toBe(before);
    expect((await poll(pending)).body.error?.code).toBe('flow_expired');
    expect((await poll(waiting.flow)).body.error?.code).toBe('flow_expired');
    expect((await complete(waiting.flow, 'Too_Late_1')).body.error?.code).toBe('flow_expired');
  });

  it('a code bound to another PKCE challenge is rejected by Discord, so no session results', async () => {
    const flow = (await start()).body;
    const attackerChallenge = createHash('sha256').update('attacker-chosen-verifier', 'ascii').digest('base64url');
    const { code, state } = discord.approve(flow.authorizeUrl, newProfile(), attackerChallenge);
    expect((await callback({ code, state })).status).toBe(400);
    expect((await poll(flow)).body).toEqual({ status: 'failed', code: 'discord_error' });
    expect((await flowRow(flow.flowId))?.codeVerifier).toBeNull();
  });

  it('access_denied and a missing code are recorded as failures the launcher can read', async () => {
    const denied = (await start()).body;
    const state = new URL(denied.authorizeUrl).searchParams.get('state') as string;
    expect((await callback({ error: 'access_denied', state })).status).toBe(400);
    expect((await poll(denied)).body).toEqual({ status: 'failed', code: 'access_denied' });

    const odd = (await start()).body;
    const oddState = new URL(odd.authorizeUrl).searchParams.get('state') as string;
    await callback({ error: 'something_unexpected', state: oddState });
    expect((await poll(odd)).body).toEqual({ status: 'failed', code: 'discord_error' });

    const nocode = (await start()).body;
    await callback({ state: new URL(nocode.authorizeUrl).searchParams.get('state') as string });
    expect((await poll(nocode)).body).toEqual({ status: 'failed', code: 'invalid_request' });
  });

  it('Discord failing (5xx, garbage profile) fails the flow cleanly', async () => {
    const down = (await start()).body;
    const a = discord.approve(down.authorizeUrl, newProfile());
    discord.failToken = 503;
    try {
      expect((await callback(a)).status).toBe(400);
    } finally {
      discord.failToken = null;
    }
    expect((await poll(down)).body).toEqual({ status: 'failed', code: 'discord_error' });

    const garbage = (await start()).body;
    const b = discord.approve(garbage.authorizeUrl, newProfile());
    discord.userBody = { id: 'not-a-snowflake', username: '' };
    try {
      expect((await callback(b)).status).toBe(400);
    } finally {
      discord.userBody = null;
    }
    expect((await poll(garbage)).body).toEqual({ status: 'failed', code: 'discord_error' });
    expect(env.logs.some((l) => l.includes('discord exchange failed'))).toBe(true);
  });

  it('the poll secret is the only way to read a flow', async () => {
    const { flow } = await runLogin(newProfile());
    expect((await poll(flow, 'x'.repeat(43))).body.error?.code).toBe('flow_not_found');
    expect((await poll(flow, 'short')).status).toBe(400);
    expect((await complete(flow, 'Secret_Thief_1', 'y'.repeat(43))).body.error?.code).toBe('flow_not_found');
    const unknown = { ...flow, flowId: '11111111-1111-4111-8111-111111111111' };
    expect((await poll(unknown)).body.error?.code).toBe('flow_not_found');
    expect((await env.call('/v1/auth/discord/poll', { json: { flowId: 'nope', pollSecret: flow.pollSecret }, ip: freshIp() })).status).toBe(400);
    // complete needs the claim: for a flow nobody approved it is indistinguishable from an unknown flow ...
    const login = await start();
    expect((await complete(login.body, 'Early_Bird_1')).body.error?.code).toBe('flow_not_found');
    // ... and with the claim it is only valid while a username is awaited (here the Discord id is a returning player's).
    const returning = newProfile();
    await signUpWithDiscord(returning, `Early_${returning.id.slice(-6)}`);
    const { flow: loginReady } = await runLogin(returning);
    expect((await complete(loginReady, 'Early_Bird_2')).body.error?.code).toBe('flow_state');
  });

  it('abandoned flows are purged a day after they expire', async () => {
    const fresh = (await start()).body;
    const before = (await env.handle.db.select().from(schema.oauthFlows)).length;
    expect(await purgeOauthFlows(env.deps, env.clock.now())).toBe(0);
    env.clock.advanceSeconds(600 + 86_400 + 5);
    const removed = await purgeOauthFlows(env.deps, env.clock.now());
    expect(removed).toBeGreaterThanOrEqual(1);
    expect(await flowRow(fresh.flowId)).toBeUndefined();
    expect((await env.handle.db.select().from(schema.oauthFlows)).length).toBe(before - removed);
  });
});
