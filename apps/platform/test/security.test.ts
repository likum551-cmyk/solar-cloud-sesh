// Security regression tests. Each case below began as a working exploit (the review's repro suite
// passed = the attack worked); here every assertion is inverted so the attack must FAIL. Every test
// was run against the unfixed code first and failed there (see the task report), and each of
// R1, R2, R5 and R6 was mutation-checked: reverting its fix makes its tests fail again.
//
//   R1  pre-hijack of an unverified account through a Discord link, surviving the owner's reset
//   R2  Discord login phishing: whoever started the flow polled the session of whoever approved
//   R3  lockout as an oracle that links a public username to a hidden email
//   R4  password-reset budget burned from a rate-limited IP; lockout DoS from one address
//   R5  cross-server grant ack; a banned account keeping its game session (and wallet)
//   R6  banned staff keeping their powers and lifting their own ban
//   R7  a Discord link started on the website: the starter must not get the approver's Discord
//       (and R1 now holds with a password re-authentication instead of a verified email)
import { createHash, randomBytes } from 'node:crypto';
import { and, eq, isNull } from 'drizzle-orm';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { schema } from '../src/db/index.js';
import { listRoutes } from '../src/http/app.js';
import { hashToken, randomToken } from '../src/util/tokens.js';
import {
  asServer, createTestEnv, freshIp, joinServer, latestMail, mailToken, makeProduct, makeServer, makeStaff, makeUser, sendWebhook,
  STRONG_PASSWORD, type TestEnv, type TestServer, type TestUser,
} from './helpers.js';

interface DiscordProfile { id: string; username: string; global_name?: string | null; email?: string | null; verified?: boolean }

/** A scripted Discord (same shape as test/discord.test.ts, reduced to what these attacks need). */
class FakeDiscord {
  private readonly codes = new Map<string, { challenge: string; profile: DiscordProfile; used: boolean }>();
  private readonly tokens = new Map<string, DiscordProfile>();
  readonly fetch: typeof fetch = async (input, init) => {
    const url = typeof input === 'string' ? input : input instanceof URL ? input.toString() : input.url;
    const method = init?.method ?? 'GET';
    const path = url.slice('https://discord.test/api/v10'.length);
    if (path === '/oauth2/token' && method === 'POST') {
      const form = new URLSearchParams(String(init?.body));
      const entry = this.codes.get(form.get('code') ?? '');
      const ok = entry && !entry.used && createHash('sha256').update(form.get('code_verifier') ?? '', 'ascii').digest('base64url') === entry.challenge;
      if (!ok || !entry) return new Response(JSON.stringify({ error: 'invalid_grant' }), { status: 400 });
      entry.used = true;
      const at = 'at_' + randomBytes(8).toString('hex');
      this.tokens.set(at, entry.profile);
      return new Response(JSON.stringify({ access_token: at, token_type: 'Bearer' }), { status: 200 });
    }
    if (path === '/users/@me') {
      const auth = new Headers(init?.headers).get('authorization') ?? '';
      const p = this.tokens.get(auth.slice('Bearer '.length));
      return p ? new Response(JSON.stringify(p), { status: 200 }) : new Response('{}', { status: 401 });
    }
    return new Response('{}', { status: 404 });
  };
  approve(authorizeUrl: string, profile: DiscordProfile): { code: string; state: string } {
    const u = new URL(authorizeUrl);
    const code = 'code_' + randomBytes(8).toString('hex');
    this.codes.set(code, { challenge: u.searchParams.get('code_challenge') ?? '', profile, used: false });
    return { code, state: u.searchParams.get('state') ?? '' };
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

type ErrBody = { error: { code: string; message: string; details?: Record<string, unknown> } };
interface Flow { flowId: string; authorizeUrl: string; pollSecret: string }
type PollBody = { status: string; token?: string; code?: string; account?: { id: string; email?: string | null; emailVerified?: boolean } } & Partial<ErrBody>;
interface Handoff { flow: string; claim: string }

let discordIdCounter = 900000000000000000n;
const newProfile = (over: Partial<DiscordProfile> = {}): DiscordProfile => {
  discordIdCounter += 1n;
  return { id: String(discordIdCounter), username: `dc_${discordIdCounter % 100000n}`, global_name: null, email: null, verified: false, ...over };
};

const startLogin = (ip = freshIp()) => env.call<Flow>('/v1/auth/discord/start', { method: 'POST', ip });
/** The browser of whoever approves Discord's consent screen lands on the callback page; the handoff link (if the page has one) is only visible there. */
async function approveAndCallback(flow: Flow, profile: DiscordProfile): Promise<{ status: number; html: string; handoff: Handoff | null }> {
  const { code, state } = discord.approve(flow.authorizeUrl, profile);
  const res = await env.call<{ text?: string }>('/v1/auth/discord/callback?' + new URLSearchParams({ code, state }).toString(), { ip: freshIp() });
  const html = res.body.text ?? '';
  const m = /href="gage-mp:\/\/auth\/discord\?flow=([0-9a-f-]{36})&amp;claim=([A-Za-z0-9_-]{43})"/.exec(html);
  return { status: res.status, html, handoff: m ? { flow: m[1] as string, claim: m[2] as string } : null };
}
const poll = (flow: Flow, claim?: string | undefined, ip = freshIp()) =>
  env.call<PollBody>('/v1/auth/discord/poll', { json: { flowId: flow.flowId, pollSecret: flow.pollSecret, ...(claim === undefined ? {} : { claim }) }, ip });
const complete = (flow: Flow, username: string, claim?: string | undefined) =>
  env.call<PollBody>('/v1/auth/discord/complete', { json: { flowId: flow.flowId, pollSecret: flow.pollSecret, username, ...(claim === undefined ? {} : { claim }) }, ip: freshIp() });
const identitiesOf = async (accountId: string) => env.handle.db.select().from(schema.oauthIdentities).where(eq(schema.oauthIdentities.accountId, accountId));
const flowRow = async (id: string) => (await env.handle.db.select().from(schema.oauthFlows).where(eq(schema.oauthFlows.id, id)))[0];

/** A Discord player that exists already (signed up through the full flow with its own launcher). */
async function signUpWithDiscord(profile: DiscordProfile, username: string): Promise<{ accountId: string }> {
  const flow = (await startLogin()).body;
  const cb = await approveAndCallback(flow, profile);
  expect(cb.status).toBe(200);
  const done = await complete(flow, username, cb.handoff?.claim);
  expect(done.status, JSON.stringify(done.body)).toBe(200);
  return { accountId: (done.body.account as { id: string }).id };
}

// ---------------------------------------------------------------------------------------------
describe('R1 pre-hijack of an unverified account through a Discord link', () => {
  // Solar RP sends no email, so a verified email can no longer be what a link change needs (nobody
  // could link). The proof is the current password; what must still hold is that a squatter's link
  // never outlives the real owner's reset, including a link that is in flight when the reset lands.
  it('starting a Discord link needs the current password, whatever the email state (a stolen session is not enough)', async () => {
    const squatter = await makeUser(env, { verified: false });
    const ip = freshIp();
    const noPassword = await env.call<ErrBody>('/v1/me/discord/link', { method: 'POST', token: squatter.token, ip });
    expect(noPassword.status).toBe(400);
    const wrong = await env.call<ErrBody>('/v1/me/discord/link', { json: { password: 'stolen-session-guess-1' }, token: squatter.token, ip });
    expect(wrong.status).toBe(401);
    expect(wrong.body.error.code).toBe('invalid_credentials');
    expect(await env.handle.db.select().from(schema.oauthFlows).where(eq(schema.oauthFlows.accountId, squatter.accountId))).toHaveLength(0);

    // Control: with the password the same account may start one (the email gate is gone for linking).
    expect((await env.call('/v1/me/discord/link', { json: { password: squatter.password }, token: squatter.token, ip })).status).toBe(200);
  });

  it('a link the squatter has in flight is refused once the real owner has reset the password', async () => {
    const victimEmail = 'victim-inflight@example.test';
    const attackerIp = freshIp();
    const reg = await env.call('/v1/auth/register', { json: { email: victimEmail, username: 'squatter_inflight', password: STRONG_PASSWORD + 'sq' }, ip: attackerIp });
    expect(reg.status).toBe(201);
    const login = await env.call<{ token: string; account: { id: string } }>('/v1/auth/login', { json: { identifier: victimEmail, password: STRONG_PASSWORD + 'sq' }, ip: attackerIp });
    const accountId = login.body.account.id;
    const link = (await env.call<Flow>('/v1/me/discord/link', { json: { password: STRONG_PASSWORD + 'sq' }, token: login.body.token, ip: attackerIp })).body;
    const cb = await approveAndCallback(link, newProfile({ username: 'squatter_dc' }));
    expect(cb.handoff).not.toBeNull();

    // The real owner resets the password before the squatter's launcher presents the claim.
    env.clock.advanceSeconds(5);
    expect((await env.call('/v1/auth/forgot-password', { json: { email: victimEmail }, ip: freshIp() })).status).toBe(202);
    const resetToken = await mailToken(env, victimEmail, 'password_reset');
    expect((await env.call('/v1/auth/reset-password', { json: { token: resetToken, password: STRONG_PASSWORD + 'owner' }, ip: freshIp() })).status).toBe(200);

    expect((await poll(link, cb.handoff?.claim, attackerIp)).body).toEqual({ status: 'failed', code: 'password_changed' });
    expect(await identitiesOf(accountId)).toHaveLength(0);
  });

  it('unlinking re-authenticates too: no password or a wrong one removes nothing', async () => {
    const squatter = await makeUser(env, { verified: false });
    await env.handle.db.insert(schema.oauthIdentities).values({ accountId: squatter.accountId, provider: 'discord', providerUserId: String(newProfile().id), providerUsername: 'planted', providerEmail: null, emailVerified: false });
    const ip = freshIp();
    expect((await env.call<ErrBody>('/v1/me/discord', { method: 'DELETE', token: squatter.token, ip })).status).toBe(400);
    const wrong = await env.call<ErrBody>('/v1/me/discord', { method: 'DELETE', json: { password: 'stolen-session-guess-2' }, token: squatter.token, ip });
    expect(wrong.status).toBe(401);
    expect(wrong.body.error.code).toBe('invalid_credentials');
    expect(await identitiesOf(squatter.accountId)).toHaveLength(1);
    // Control: the password removes it.
    expect((await env.call('/v1/me/discord', { method: 'DELETE', json: { password: squatter.password }, token: squatter.token, ip })).status).toBe(204);
    expect(await identitiesOf(squatter.accountId)).toHaveLength(0);
  });

  it('a Discord identity on an unverified account does not survive the real owner resetting the password', async () => {
    const victimEmail = 'victim1@example.test';
    const attackerIp = freshIp();
    const attackerDiscord = newProfile({ username: 'attacker' });

    // The attacker registers the victim's address first and never verifies it.
    const reg = await env.call('/v1/auth/register', { json: { email: victimEmail, username: 'squatter1', password: STRONG_PASSWORD + 'x1' }, ip: attackerIp });
    expect(reg.status).toBe(201);
    const login = await env.call<{ token: string; account: { id: string; emailVerified: boolean } }>('/v1/auth/login', { json: { identifier: victimEmail, password: STRONG_PASSWORD + 'x1' }, ip: attackerIp });
    expect(login.body.account.emailVerified).toBe(false);
    const accountId = login.body.account.id;
    // An identity that got onto the account anyway (planted directly: it models one linked before the fix).
    await env.handle.db.insert(schema.oauthIdentities).values({ accountId, provider: 'discord', providerUserId: attackerDiscord.id, providerUsername: 'attacker', providerEmail: null, emailVerified: false });

    // The real owner registers (answered as if new), asks for a reset and sets a password.
    expect((await env.call('/v1/auth/register', { json: { email: victimEmail, username: 'victim_real1', password: STRONG_PASSWORD + 'v1' }, ip: freshIp() })).status).toBe(201);
    expect((await env.call('/v1/auth/forgot-password', { json: { email: victimEmail }, ip: freshIp() })).status).toBe(202);
    const resetToken = await mailToken(env, victimEmail, 'password_reset');
    expect((await env.call('/v1/auth/reset-password', { json: { token: resetToken, password: STRONG_PASSWORD + 'v1new' }, ip: freshIp() })).status).toBe(200);

    // The squatter's identity is gone with the rest of what the squatter put on the account.
    expect(await identitiesOf(accountId)).toHaveLength(0);
    expect((await env.call('/v1/me', { token: login.body.token })).status).toBe(401);

    // The attacker's Discord now leads to a brand new account, never to the victim's.
    const flow = (await startLogin(attackerIp)).body;
    const cb = await approveAndCallback(flow, attackerDiscord);
    const done = await poll(flow, cb.handoff?.claim, attackerIp);
    expect(done.body.status).not.toBe('complete');
    expect(done.body.token).toBeUndefined();
    expect(done.body.account?.id).not.toBe(accountId);
  });

  it('linking and unlinking Discord mail the account owner', async () => {
    const owner = await makeUser(env);
    const profile = newProfile({ username: 'mailed_link' });
    const link = (await env.call<Flow>('/v1/me/discord/link', { json: { password: owner.password }, token: owner.token, ip: freshIp() })).body;
    const cb = await approveAndCallback(link, profile);
    expect((await poll(link, cb.handoff?.claim)).body.status).toBe('linked');
    const linked = await latestMail(env, owner.email, 'discord_linked');
    expect(linked, 'a discord_linked email to the owner').toBeDefined();
    expect(await latestMail(env, owner.email, 'discord_unlinked')).toBeUndefined();

    expect((await env.call('/v1/me/discord', { method: 'DELETE', json: { password: owner.password }, token: owner.token, ip: freshIp() })).status).toBe(204);
    expect(await latestMail(env, owner.email, 'discord_unlinked'), 'a discord_unlinked email to the owner').toBeDefined();
  });
});

// ---------------------------------------------------------------------------------------------
describe('R2 Discord login phishing', () => {
  it('whoever started the flow cannot take the session of whoever approves it', async () => {
    const victimDiscord = newProfile({ username: 'victim2', email: 'victim2@example.test', verified: true });
    const victim = await signUpWithDiscord(victimDiscord, 'victim_two');

    // The attacker starts a flow and sends the authorize URL to the victim, who approves it. The
    // handoff link in the callback page opens in the VICTIM's browser; the attacker never sees it.
    const atk = (await startLogin()).body;
    const victimsBrowser = await approveAndCallback(atk, victimDiscord);
    expect(victimsBrowser.status).toBe(200);

    const stolen = await poll(atk);
    expect(stolen.body.status).toBe('pending');
    expect(stolen.body.token).toBeUndefined();
    expect(stolen.body.account).toBeUndefined();
    // Guessing the claim gets nothing either (and looks the same as asking without one).
    const guessed = await poll(atk, randomToken(32));
    expect(guessed.body).toEqual({ status: 'pending' });
    // complete needs the claim too.
    expect((await complete(atk, 'stolen_name')).status).toBe(400);
    expect((await complete(atk, 'stolen_name', randomToken(32))).status).toBe(404);
    expect((await flowRow(atk.flowId))?.status).toBe('login_ready'); // nothing was consumed by the attempts

    // The victim's own launcher holds the claim but not the poll secret: it cannot finish a flow it did not start.
    const noSecret = await env.call('/v1/auth/discord/poll', { json: { flowId: atk.flowId, pollSecret: randomToken(32), claim: victimsBrowser.handoff?.claim }, ip: freshIp() });
    expect(noSecret.status).toBe(404);

    // Control: the party that holds BOTH (the launcher that started the flow, on the machine whose browser approved) gets the session.
    expect(victimsBrowser.handoff, 'the callback page hands the claim to the launcher').not.toBeNull();
    const legit = await poll(atk, victimsBrowser.handoff?.claim);
    expect(legit.body.status).toBe('complete');
    expect(legit.body.account?.id).toBe(victim.accountId);
  });

  it('the callback page redirects to the launcher scheme, shows "Return to GAGE" and never shows the claim', async () => {
    const flow = (await startLogin()).body;
    const profile = newProfile();
    const { code, state } = discord.approve(flow.authorizeUrl, profile);
    const url = '/v1/auth/discord/callback?' + new URLSearchParams({ code, state }).toString();
    const res = await env.app.request(url, {}, { incoming: { socket: { remoteAddress: freshIp() } } });
    const html = await res.text();
    expect(res.status).toBe(200);
    const m = /href="gage-mp:\/\/auth\/discord\?flow=([0-9a-f-]{36})&amp;claim=([A-Za-z0-9_-]{43})"/.exec(html);
    expect(m, 'a gage-mp://auth/discord link with flow and a 32-byte base64url claim').not.toBeNull();
    const [, flowId, claim] = m as RegExpExecArray;
    expect(flowId).toBe(flow.flowId);
    expect(html).toContain('http-equiv="refresh"');
    const visible = html.replace(/<style[\s\S]*?<\/style>/g, ' ').replace(/<[^>]*>/g, ' ');
    expect(visible).toMatch(/Return to GAGE/);
    expect(visible).not.toContain(claim as string);
    expect(visible).not.toContain(flowId as string);
    expect(res.headers.get('referrer-policy')).toBe('no-referrer');
    expect(res.headers.get('cache-control')).toBe('no-store');
    expect(res.headers.get('content-security-policy')).toContain("default-src 'none'");

    // Stored hashed, never in clear.
    const row = await flowRow(flow.flowId);
    expect(row?.claimHash).toBe(hashToken(claim as string));
    expect(JSON.stringify(row)).not.toContain(claim as string);

    // A replayed callback is refused and carries no handoff.
    const replay = await env.app.request(url, {}, { incoming: { socket: { remoteAddress: freshIp() } } });
    expect(replay.status).toBe(400);
    expect(await replay.text()).not.toContain('gage-mp://');

    // A refused sign-in also hands over a claim, so the launcher (and nobody else) can read why.
    const denied = (await startLogin()).body;
    const deniedState = new URL(denied.authorizeUrl).searchParams.get('state') as string;
    const deniedRes = await env.call<{ text?: string }>('/v1/auth/discord/callback?' + new URLSearchParams({ error: 'access_denied', state: deniedState }).toString(), { ip: freshIp() });
    expect(deniedRes.status).toBe(400);
    const dm = /claim=([A-Za-z0-9_-]{43})"/.exec(deniedRes.body.text ?? '');
    expect(dm).not.toBeNull();
    expect((await poll(denied)).body).toEqual({ status: 'pending' });
    expect((await poll(denied, dm?.[1])).body).toEqual({ status: 'failed', code: 'access_denied' });
  });

  it('the claim is single use and dies with the flow', async () => {
    // login_ready: the first poll with the claim consumes the flow and erases the claim.
    const profile = newProfile();
    await signUpWithDiscord(profile, `Single_${profile.id.slice(-6)}`);
    const flow = (await startLogin()).body;
    const cb = await approveAndCallback(flow, profile);
    expect(cb.handoff).not.toBeNull();
    const claim = cb.handoff?.claim as string;
    expect((await poll(flow, claim)).body.status).toBe('complete');
    expect((await flowRow(flow.flowId))?.claimHash).toBeNull();
    expect((await poll(flow, claim)).status).toBe(410);

    // needs_username: readable with the claim until the username is chosen; the choice spends the claim.
    const fresh = newProfile();
    const f2 = (await startLogin()).body;
    const cb2 = await approveAndCallback(f2, fresh);
    const claim2 = cb2.handoff?.claim as string;
    expect((await poll(f2, claim2)).body.status).toBe('needs_username');
    expect((await poll(f2, claim2)).body.status).toBe('needs_username');
    expect((await complete(f2, 'Claim_Wrong_1', randomToken(32))).status).toBe(404);
    expect((await complete(f2, 'Claim_Right_1', claim2)).status).toBe(200);
    expect((await flowRow(f2.flowId))?.claimHash).toBeNull();
    expect((await complete(f2, 'Claim_Again_1', claim2)).status).toBe(410);

    // The claim expires with the flow.
    const f3 = (await startLogin()).body;
    const cb3 = await approveAndCallback(f3, newProfile());
    env.clock.advanceSeconds(601);
    expect((await poll(f3, cb3.handoff?.claim)).body.error?.code).toBe('flow_expired');
  });

  it('a Discord identity is never linked to an account by someone else approving the flow', async () => {
    // The attacker (a verified account) starts a LINK flow and sends the URL to a victim, hoping the
    // victim's Discord ends up on the attacker's account.
    const attacker = await makeUser(env);
    const victimDiscord = newProfile({ username: 'victim_link' });
    const link = (await env.call<Flow>('/v1/me/discord/link', { json: { password: attacker.password }, token: attacker.token, ip: freshIp() })).body;
    const victimsBrowser = await approveAndCallback(link, victimDiscord);

    expect((await poll(link)).body.status).toBe('pending');
    expect((await poll(link, randomToken(32))).body.status).toBe('pending');
    expect(await identitiesOf(attacker.accountId)).toHaveLength(0);
    expect(await env.handle.db.select().from(schema.oauthIdentities).where(eq(schema.oauthIdentities.providerUserId, victimDiscord.id))).toHaveLength(0);

    // Control: with the claim (the launcher of the signed-in account, on the approving machine) the link happens.
    const done = await poll(link, victimsBrowser.handoff?.claim);
    expect(done.body.status).toBe('linked');
    expect((await identitiesOf(attacker.accountId))[0]?.providerUserId).toBe(victimDiscord.id);
  });
});

// ---------------------------------------------------------------------------------------------
describe('R7 Discord link from the website: whoever starts the flow cannot get somebody else\'s Discord', () => {
  const finish = (token: string, body: { flowId: string; pollSecret: string; claim: string }) =>
    env.call<PollBody>('/v1/me/discord/link/finish', { json: body, token, ip: freshIp() });

  it('an attacker who sends their web link\'s authorize URL to a victim links nothing; the victim\'s page cannot finish it either', async () => {
    const attacker = await makeUser(env);
    const victim = await makeUser(env);
    const victimDiscord = newProfile({ username: 'victim_web' });

    // The attacker starts a WEB link on their own account (their tab keeps the poll secret) and sends
    // the authorize URL to the victim, who approves it with their Discord.
    const atk = (await env.call<Flow>('/v1/me/discord/link', { json: { password: attacker.password, handoff: 'web' }, token: attacker.token, ip: freshIp() })).body;
    const { code, state } = discord.approve(atk.authorizeUrl, victimDiscord);
    const cb = await env.call('/v1/auth/discord/callback?' + new URLSearchParams({ code, state }).toString(), { ip: freshIp() });
    // The claim lands in the VICTIM's browser, on the account page's fragment.
    expect(cb.status).toBe(303);
    const landing = new URL(cb.headers.get('location') ?? 'about:blank');
    const fragment = new URLSearchParams(landing.hash.slice(1));
    expect(landing.origin + landing.pathname).toBe('https://solar-rp.com/account/');
    expect(fragment.get('discord_flow')).toBe(atk.flowId);
    const victimsClaim = fragment.get('discord_claim') as string;
    expect(victimsClaim).toMatch(/^[A-Za-z0-9_-]{43}$/);

    // The attacker holds the poll secret and their own session, but not the claim.
    expect((await finish(attacker.token, { flowId: atk.flowId, pollSecret: atk.pollSecret, claim: randomToken(32) })).status).toBe(404);
    expect((await env.call('/v1/me/discord/link/finish', { json: { flowId: atk.flowId, pollSecret: atk.pollSecret }, token: attacker.token, ip: freshIp() })).status).toBe(400);
    // The unauthenticated poll does not read web flows at all (not even with both secrets).
    expect((await poll(atk)).status).toBe(404);
    expect((await poll(atk, victimsClaim)).status).toBe(404);

    // The victim's account page holds the claim and the victim's session, but not the poll secret ...
    expect((await finish(victim.token, { flowId: atk.flowId, pollSecret: randomToken(32), claim: victimsClaim })).status).toBe(404);
    // ... and even with the attacker's poll secret (had it leaked) the session is the victim's, not the flow's account.
    expect((await finish(victim.token, { flowId: atk.flowId, pollSecret: atk.pollSecret, claim: victimsClaim })).status).toBe(404);

    // Nothing was linked to anyone, and nothing was spent.
    expect(await identitiesOf(attacker.accountId)).toHaveLength(0);
    expect(await identitiesOf(victim.accountId)).toHaveLength(0);
    expect(await env.handle.db.select().from(schema.oauthIdentities).where(eq(schema.oauthIdentities.providerUserId, victimDiscord.id))).toHaveLength(0);
    expect((await flowRow(atk.flowId))?.status).toBe('link_ready');

    // Control: all three pieces together (the starting tab's poll secret, the starting account's
    // session and the claim of the browser that approved) do link, so the refusals above are the guard.
    expect((await finish(attacker.token, { flowId: atk.flowId, pollSecret: atk.pollSecret, claim: victimsClaim })).body).toEqual({ status: 'linked' });
    expect((await identitiesOf(attacker.accountId))[0]?.providerUserId).toBe(victimDiscord.id);
  });

  it('a launcher link flow cannot be finished through the web finish (handoffs do not mix)', async () => {
    const user = await makeUser(env);
    const link = (await env.call<Flow>('/v1/me/discord/link', { json: { password: user.password }, token: user.token, ip: freshIp() })).body;
    const cb = await approveAndCallback(link, newProfile());
    expect(cb.handoff).not.toBeNull();
    expect((await finish(user.token, { flowId: link.flowId, pollSecret: link.pollSecret, claim: cb.handoff?.claim as string })).status).toBe(404);
    expect((await poll(link, cb.handoff?.claim)).body.status).toBe('linked');
  });
});

// ---------------------------------------------------------------------------------------------
describe('R3 the lockout is not an oracle for which email belongs to which username', () => {
  const login = (identifier: string, ip: string, password = 'whatever-guess') => env.call<ErrBody>('/v1/auth/login', { json: { identifier, password }, ip });

  it('typing the email of a locked username answers exactly like typing any other email', async () => {
    const u = await makeUser(env, { username: 'Linkable_3' });
    const attackerIp = freshIp();
    for (let i = 0; i < 5; i++) expect((await login('Linkable_3', attackerIp, 'wrong-password-' + i)).status).toBe(401);

    for (const ip of [attackerIp, freshIp(), '203.0.113.201']) {
      const right = await login(u.email, ip);
      const other = await login('someone.else@example.test', ip);
      expect(right.status, `from ${ip}`).toBe(other.status);
      expect(right.body.error.code).toBe(other.body.error.code);
      expect(right.body.error.message).toBe(other.body.error.message);
    }
  });

  it('and the other way round: locking the email says nothing about the username', async () => {
    const u = await makeUser(env, { username: 'Linkable_3b' });
    const attackerIp = freshIp();
    for (let i = 0; i < 5; i++) await login(u.email, attackerIp, 'wrong-password-' + i);
    const right = await login('Linkable_3b', attackerIp);
    const other = await login('Not_A_Real_User_3', attackerIp);
    expect(right.status).toBe(other.status);
    expect(right.body.error.code).toBe(other.body.error.code);
  });
});

// ---------------------------------------------------------------------------------------------
describe('R4 reset blocking and lockout denial of service', () => {
  it('a rate-limited address cannot burn the victim\'s password-reset budget', async () => {
    const u = await makeUser(env, { username: 'Resetless_4' });
    const ip = freshIp();
    for (let i = 0; i < 10; i++) await env.call('/v1/auth/forgot-password', { json: { email: 'filler' + i + '@example.test' }, ip });
    for (let i = 0; i < 3; i++) expect((await env.call('/v1/auth/forgot-password', { json: { email: u.email }, ip })).status).toBe(429);
    const rows = (await env.handle.db.select().from(schema.emailOutbox)).filter((r) => r.toAddress === u.email && r.template === 'password_reset');
    expect(rows.length).toBe(0);

    // The owner, from their own address, still gets their reset mail.
    const own = await env.call('/v1/auth/forgot-password', { json: { email: u.email }, ip: freshIp() });
    expect(own.status).toBe(202);
    expect(await mailToken(env, u.email, 'password_reset')).toBeTruthy();
  });

  it('forgot-password is limited per client IP by the route (documented in the reference)', async () => {
    const route = listRoutes().find((r) => r.method === 'post' && r.path === '/v1/auth/forgot-password');
    expect(route?.limits).toContainEqual({ name: 'forgot-password', scope: 'ip', limit: 10, windowSeconds: 3600 });
    const ip = freshIp();
    const statuses: number[] = [];
    for (let i = 0; i < 12; i++) statuses.push((await env.call('/v1/auth/forgot-password', { json: { email: `burst${i}@example.test` }, ip })).status);
    expect(statuses.filter((s) => s === 202)).toHaveLength(10);
    expect(statuses.filter((s) => s === 429)).toHaveLength(2);
  });

  it('one attacker address cannot lock the real owner out of password login', async () => {
    const owner = await makeUser(env, { username: 'Locked_Out_4' });
    const attackerIp = '198.51.100.77';
    const ownerIp = '203.0.113.77';
    // The attacker hammers both identifiers of the account from one address.
    for (let i = 0; i < 12; i++) await env.call('/v1/auth/login', { json: { identifier: i % 2 === 0 ? owner.email : owner.username, password: 'wrong-guess-' + i }, ip: attackerIp });
    // The attacker's own address is shut out ...
    expect((await env.call('/v1/auth/login', { json: { identifier: owner.email, password: 'another-guess-1' }, ip: attackerIp })).status).toBe(429);
    // ... the owner, from elsewhere, still logs in, by email and by username.
    expect((await env.call('/v1/auth/login', { json: { identifier: owner.email, password: owner.password }, ip: ownerIp })).status).toBe(200);
    expect((await env.call('/v1/auth/login', { json: { identifier: owner.username, password: owner.password }, ip: ownerIp })).status).toBe(200);
  });

  it('addresses in the same /24 share one budget for an account, so a block of addresses is not a multiplier', async () => {
    const owner = await makeUser(env, { username: 'Same_Block_4' });
    for (let i = 0; i < 5; i++) await env.call('/v1/auth/login', { json: { identifier: owner.email, password: 'wrong-guess-' + i }, ip: `192.0.2.${10 + i}` });
    expect((await env.call('/v1/auth/login', { json: { identifier: owner.email, password: 'wrong-guess-x' }, ip: '192.0.2.200' })).status).toBe(429);
    expect((await env.call('/v1/auth/login', { json: { identifier: owner.email, password: 'wrong-guess-x' }, ip: '192.0.3.200' })).status).toBe(401);
  });

  it('the account-wide threshold is 100 failures: 99 from 20 address blocks lock nobody out', async () => {
    const owner = await makeUser(env, { username: 'Distributed_4a' });
    for (let block = 0; block < 20; block++) {
      for (let i = 0; i < 5; i++) {
        if (block === 19 && i === 4) break; // 99 failures in all
        await env.call('/v1/auth/login', { json: { identifier: owner.email, password: 'wrong-guess-' + i }, ip: `100.65.${block}.5` });
      }
    }
    expect((await env.call('/v1/auth/login', { json: { identifier: owner.email, password: owner.password }, ip: '203.0.113.170' })).status).toBe(200);
  });

  it('a distributed attack (100 failures from 20 address blocks) still locks the identifier for everyone', async () => {
    const owner = await makeUser(env, { username: 'Distributed_4' });
    for (let block = 0; block < 20; block++) {
      for (let i = 0; i < 5; i++) await env.call('/v1/auth/login', { json: { identifier: owner.email, password: 'wrong-guess-' + i }, ip: `100.64.${block}.5` });
    }
    const blocked = await env.call<ErrBody>('/v1/auth/login', { json: { identifier: owner.email, password: owner.password }, ip: '203.0.113.180' });
    expect(blocked.status).toBe(429);
    expect(blocked.body.error.code).toBe('rate_limited');
    // Password reset still works and clears it.
    expect((await env.call('/v1/auth/forgot-password', { json: { email: owner.email }, ip: freshIp() })).status).toBe(202);
    const token = await mailToken(env, owner.email, 'password_reset');
    expect((await env.call('/v1/auth/reset-password', { json: { token, password: 'after-the-storm-passphrase-1' }, ip: freshIp() })).status).toBe(200);
    expect((await env.call('/v1/auth/login', { json: { identifier: owner.email, password: 'after-the-storm-passphrase-1' }, ip: '203.0.113.181' })).status).toBe(200);
  });
});

// ---------------------------------------------------------------------------------------------
describe('R5 grant delivery is bound to one server; a ban ends the player\'s game session', () => {
  async function paidBundle(sku: string): Promise<{ player: TestUser; orderId: string }> {
    const admin = await makeStaff(env, 'admin');
    await makeProduct(env, admin, { sku, kind: 'bundle', priceMinor: 500, grants: [{ type: 'item', key: 'hat' }, { type: 'wallet_credit', currency: 'coins', amount: 100 }] });
    const player = await makeUser(env);
    const co = await env.call<{ order: { id: string } }>('/v1/shop/checkout', { token: player.token, json: { items: [{ sku, quantity: 1 }] }, ip: freshIp() });
    expect(co.status).toBe(201);
    expect((await sendWebhook(env, { type: 'payment.succeeded', orderId: co.body.order.id, amountMinor: 500, currency: 'USD' })).body.result).toBe('applied');
    return { player, orderId: co.body.order.id };
  }
  const twoServers = async (): Promise<{ a: TestServer; b: TestServer }> => {
    const admin = await makeStaff(env, 'admin');
    return { a: await makeServer(env, admin), b: await makeServer(env, admin) };
  };
  const pull = (server: TestServer, accountId: string) => asServer(env, server)<{ grants: { id: string }[] } & Partial<ErrBody>>('/internal/game/grants?accountId=' + accountId);
  const ack = (server: TestServer, id: string) => asServer(env, server)<{ alreadyAcked: boolean } & Partial<ErrBody>>(`/internal/game/grants/${id}/ack`, { method: 'POST' });

  it('server B cannot acknowledge a grant that server A delivers for a player on A', async () => {
    const { player } = await paidBundle('hat-r5a');
    const { a, b } = await twoServers();
    await joinServer(env, player, a);
    const grantId = (await pull(a, player.accountId)).body.grants[0]?.id as string;
    expect(grantId).toBeTruthy();

    const stolen = await ack(b, grantId);
    expect(stolen.status).toBe(409);
    expect(stolen.body.error?.code).toBe('grant_not_pulled');
    // Nothing changed: the grant is still pending for A, and only A's ack counts.
    expect((await pull(a, player.accountId)).body.grants.map((g) => g.id)).toEqual([grantId]);
    const real = await ack(a, grantId);
    expect(real.status).toBe(200);
    expect(real.body.alreadyAcked).toBe(false);
    expect((await ack(a, grantId)).body.alreadyAcked).toBe(true); // a retried ack of the same server is still harmless
    expect((await ack(b, grantId)).status).toBe(409); // another server never learns "already acked" about a grant that is not its own
  });

  it('a grant nobody pulled cannot be acknowledged at all, and the database refuses an ack that breaks the binding', async () => {
    const { player, orderId } = await paidBundle('hat-r5b');
    const { a } = await twoServers();
    await joinServer(env, player, a);
    const [pending] = await env.handle.db.select().from(schema.grants).where(and(eq(schema.grants.orderId, orderId), eq(schema.grants.type, 'item')));
    const grantId = pending?.id as string;
    expect((await ack(a, grantId)).status).toBe(409); // joined, but never pulled it

    // The same rule enforced by the database, so no code path can skip it.
    await expect(env.handle.db.update(schema.grants).set({ status: 'acked', ackedAt: new Date(), ackedBy: a.id }).where(eq(schema.grants.id, grantId))).rejects.toBeDefined();
    expect((await env.handle.db.select().from(schema.grants).where(eq(schema.grants.id, grantId)))[0]?.status).toBe('pending');
  });

  it('when the player moves to another server the delivery moves with them', async () => {
    const { player } = await paidBundle('hat-r5c');
    const { a, b } = await twoServers();
    await joinServer(env, player, a);
    const grantId = (await pull(a, player.accountId)).body.grants[0]?.id as string;
    await asServer(env, a)('/internal/game/session-end', { json: { accountId: player.accountId } });
    await joinServer(env, player, b);
    expect((await pull(b, player.accountId)).body.grants.map((g) => g.id)).toEqual([grantId]);
    expect((await ack(a, grantId)).status).toBe(409); // A no longer holds the delivery
    expect((await ack(b, grantId)).body.alreadyAcked).toBe(false);
  });

  it('a ban closes the open game session, voids unused join tokens and ends spending, pulls and verifies', async () => {
    const { player } = await paidBundle('hat-r5d');
    const { a, b } = await twoServers();
    const mod = await makeStaff(env, 'mod');
    await joinServer(env, player, a);
    const spendBody = (key: string) => ({ json: { accountId: player.accountId, currency: 'coins', amount: 10, idempotencyKey: key } });
    expect((await asServer(env, a)('/internal/game/wallet/spend', spendBody('spend-before-ban-1'))).status).toBe(200);
    const spare = await env.call<{ token: string }>('/v1/game/join', { token: player.token, json: { serverId: b.id } });
    expect(spare.status).toBe(200);

    expect((await env.call('/v1/admin/accounts/' + player.accountId + '/ban', { token: mod.token, json: { reason: 'cheating' } })).status).toBe(201);

    const sessions = await env.handle.db.select().from(schema.gameSessions).where(eq(schema.gameSessions.accountId, player.accountId));
    expect(sessions.filter((s) => s.endedAt === null)).toHaveLength(0);
    expect(sessions.map((s) => s.endReason)).toEqual(['banned']);
    expect(await env.handle.db.select().from(schema.joinTokens).where(and(eq(schema.joinTokens.accountId, player.accountId), isNull(schema.joinTokens.usedAt)))).toHaveLength(0);

    const spend = await asServer(env, a)<ErrBody>('/internal/game/wallet/spend', spendBody('spend-after-ban-1'));
    expect(spend.status).toBe(403);
    expect(spend.body.error.code).toBe('account_banned');
    expect((await asServer(env, a)<ErrBody>('/internal/game/wallet/spend', spendBody('spend-before-ban-1'))).status).toBe(403); // not even an idempotent replay
    expect((await pull(a, player.accountId)).status).toBe(403);
    expect((await asServer(env, b)<{ ok: boolean; reason: string }>('/internal/game/verify', { json: { token: spare.body.token } })).body.ok).toBe(false);

    // A token that slipped past the ban check is still refused at verify.
    const raced = 'gmj_' + randomToken(32);
    await env.handle.db.insert(schema.joinTokens).values({ tokenHash: hashToken(raced), accountId: player.accountId, serverId: b.id, createdAt: env.clock.now(), expiresAt: new Date(env.clock.now().getTime() + 60_000) });
    expect((await asServer(env, b)<{ ok: boolean; reason: string }>('/internal/game/verify', { json: { token: raced } })).body).toMatchObject({ ok: false, reason: 'banned' });

    // The banned player can still sign in to read the reason.
    expect((await env.call('/v1/me/sessions', { token: player.token })).status).toBe(200);
  });
});

// ---------------------------------------------------------------------------------------------
describe('R6 banned staff and self-unban', () => {
  const unban = (by: TestUser, target: string) => env.call<ErrBody>('/v1/admin/accounts/' + target + '/unban', { token: by.token, json: {} });
  const ban = (by: TestUser, target: string, reason = 'abuse of powers') => env.call<ErrBody>('/v1/admin/accounts/' + target + '/ban', { token: by.token, json: { reason } });
  const banOf = async (u: TestUser) => (await env.call<{ account: { ban: unknown } }>('/v1/me', { token: u.token })).body.account.ban;

  it('a banned mod cannot use moderator powers, and cannot lift their own ban', async () => {
    const admin = await makeStaff(env, 'admin');
    const mod = await makeStaff(env, 'mod');
    const player = await makeUser(env);
    expect((await ban(admin, mod.accountId)).status).toBe(201);
    expect(await banOf(mod)).not.toBeNull();

    const revenge = await ban(mod, player.accountId, 'revenge');
    expect(revenge.status).toBe(403);
    expect(revenge.body.error.code).toBe('account_banned');
    expect((await env.call('/v1/admin/accounts/' + player.accountId, { token: mod.token })).status).toBe(403);
    expect((await env.handle.db.select().from(schema.bans).where(eq(schema.bans.accountId, player.accountId)))).toHaveLength(0);

    const self = await unban(mod, mod.accountId);
    expect(self.status).toBe(403);
    expect(await banOf(mod)).not.toBeNull();

    // The ban holder is still an ordinary signed-in account (they can read why they are banned).
    expect((await env.call('/v1/me', { token: mod.token })).status).toBe(200);
  });

  it('a banned admin loses admin powers too, and cannot unban themselves', async () => {
    const admin = await makeStaff(env, 'admin');
    const other = await makeStaff(env, 'admin');
    // Nobody may ban an admin (demote first), so the ban is placed the way an incident would: demote, ban, promote back.
    expect((await env.call('/v1/admin/accounts/' + admin.accountId + '/roles', { method: 'PUT', token: other.token, json: { roles: ['mod'] } })).status).toBe(200);
    expect((await ban(other, admin.accountId, 'compromised')).status).toBe(201);
    await env.handle.db.insert(schema.accountRoles).values({ accountId: admin.accountId, role: 'admin' });
    expect((await env.call('/v1/admin/audit', { token: admin.token })).status).toBe(403);
    expect((await env.call('/v1/admin/servers', { token: admin.token })).status).toBe(403);
    expect((await unban(admin, admin.accountId)).status).toBe(403);
    expect(await banOf(admin)).not.toBeNull();
  });

  it('only an admin may lift a ban on a mod or admin (mirrors who may place it), and refusals are audited', async () => {
    const admin = await makeStaff(env, 'admin');
    const mod = await makeStaff(env, 'mod');
    const bannedMod = await makeStaff(env, 'mod');
    const player = await makeUser(env);
    expect((await ban(admin, bannedMod.accountId)).status).toBe(201);
    expect((await ban(mod, player.accountId, 'cheating')).status).toBe(201);

    const refused = await unban(mod, bannedMod.accountId);
    expect(refused.status).toBe(403);
    expect(refused.body.error.code).toBe('forbidden_target');
    expect((await env.handle.db.select().from(schema.bans).where(and(eq(schema.bans.accountId, bannedMod.accountId), isNull(schema.bans.revokedAt))))).toHaveLength(1);

    const audit = await env.handle.db.select().from(schema.auditLog).where(eq(schema.auditLog.targetId, bannedMod.accountId));
    const refusal = audit.find((e) => e.action === 'account.unban_refused');
    expect(refusal, 'an audit row for the refused unban').toBeDefined();
    expect(refusal?.actorAccountId).toBe(mod.accountId);

    // Controls: a mod may still lift a ban on a player; an admin may lift one on a mod.
    expect((await unban(mod, player.accountId)).status).toBe(200);
    expect((await unban(admin, bannedMod.accountId)).status).toBe(200);
  });
});
