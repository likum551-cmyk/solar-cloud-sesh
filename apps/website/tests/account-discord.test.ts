import { describe, expect, it } from 'vitest';
import { createAccountApi, parseAccount } from '../src/account/api';
import { finishLink, isDiscordAuthorizeUrl, NO_STORAGE_TEXT, PENDING_KEY, readReturn, startLink, withoutFragment } from '../src/account/discord';
import { LINK_FAILED_DEFAULT } from '../src/account/errors';
import { createTokenStore } from '../src/account/store';
import { ACCOUNT, apiError, memoryStorage, scriptedFetch, TOKEN, type Answer } from './account-helpers';

// The web Discord link (apps/platform/docs/api.md, "Linking from the website"): the password starts it,
// the poll secret waits in this tab's sessionStorage while the player is on Discord, and the flow and
// claim come back in the account page's fragment.

const FLOW = '0b7f2c1e-8a4d-4e6f-9c3b-1d2e3f4a5b6c';
const SECRET = 'cG9sbC1zZWNyZXQtZm9yLXRoZS10ZXN0cy0wMTIzNDU';
const CLAIM = 'Y2xhaW0tZm9yLXRoZS10ZXN0cy0wMTIzNDU2Nzg5YWJ'; // 43 characters, the shape of 32 random bytes in base64url
const AUTHORIZE = 'https://discord.com/oauth2/authorize?response_type=code&client_id=1&scope=identify+email&state=s&code_challenge=c&code_challenge_method=S256';
const started: Answer = { status: 200, json: { flowId: FLOW, authorizeUrl: AUTHORIZE, pollSecret: SECRET, expiresAt: '2026-11-06T12:10:00.000Z' } };

function setup(answers: Answer[], opts: { token?: string | null; tab?: ReturnType<typeof memoryStorage> | null } = {}) {
  const f = scriptedFetch(answers);
  const session = memoryStorage();
  const local = memoryStorage();
  const token = opts.token === undefined ? TOKEN : opts.token;
  if (token) session.map.set('solar.session', token);
  const store = createTokenStore(session, local);
  const tab = opts.tab === undefined ? session : opts.tab;
  return { api: createAccountApi('/api', f.fetchImpl), store, f, session, local, tab };
}

const pendingOf = (kv: { map: Map<string, string> }) => {
  const raw = kv.map.get(PENDING_KEY);
  return raw === undefined ? undefined : (JSON.parse(raw) as { flowId: string; pollSecret: string });
};

describe('the account carries its Discord link', () => {
  it('parses a linked account, an unlinked one and one from before the field existed; refuses a broken shape', () => {
    expect(parseAccount({ ...ACCOUNT, discord: { id: '123456789012345678', username: 'cool.guy' } })?.discord).toEqual({ id: '123456789012345678', username: 'cool.guy' });
    expect(parseAccount({ ...ACCOUNT, discord: { id: '123456789012345678', username: null } })?.discord).toEqual({ id: '123456789012345678', username: null });
    expect(parseAccount(ACCOUNT)?.discord).toBeNull();
    const { discord: _gone, ...older } = ACCOUNT;
    expect(parseAccount(older)?.discord).toBeNull();
    expect(parseAccount({ ...ACCOUNT, discord: { username: 'no id' } })).toBeNull();
    expect(parseAccount({ ...ACCOUNT, discord: 'linked' })).toBeNull();
  });
});

describe('starting a link', () => {
  it('sends the password with handoff "web" and the session, keeps the poll secret in this tab only, and goes to Discord', async () => {
    const t = setup([started]);
    const out = await startLink(t.api, t.store, t.tab, 'my-own-password');
    expect(out).toEqual({ kind: 'redirect', url: AUTHORIZE });
    expect(t.f.calls).toHaveLength(1);
    expect(t.f.calls[0]).toMatchObject({ method: 'POST', url: '/api/v1/me/discord/link', body: { password: 'my-own-password', handoff: 'web' } });
    expect(t.f.calls[0]?.headers.authorization).toBe(`Bearer ${TOKEN}`);
    expect(pendingOf(t.session)).toEqual({ flowId: FLOW, pollSecret: SECRET, expiresAt: '2026-11-06T12:10:00.000Z' });
    // never in localStorage (it would outlive the tab and reach other tabs)
    expect(t.local.map.has(PENDING_KEY)).toBe(false);
  });

  it('an empty password asks for it without calling the platform', async () => {
    const t = setup([]);
    expect(await startLink(t.api, t.store, t.tab, '')).toEqual({ kind: 'invalid', problems: [{ field: 'password', text: 'Enter your password.' }] });
    expect(t.f.calls).toHaveLength(0);
  });

  it('shows each refusal by its code: wrong password under the field, the rest for the whole form; nothing is stored', async () => {
    const cases: [Answer, string, string | undefined][] = [
      [apiError(401, 'invalid_credentials'), 'That isn’t your password.', 'password'],
      [apiError(429, 'rate_limited', undefined, { 'Retry-After': '90' }), 'Too many attempts. Try again in 2 minutes.', undefined],
      [apiError(409, 'already_has_discord'), 'This account already has a Discord account linked.', undefined],
      [apiError(409, 'password_required'), 'This account has no password yet. Set one under Change password, then link Discord.', undefined],
      [apiError(404, 'discord_disabled'), 'Discord linking isn’t switched on yet. Try again later.', undefined],
      [apiError(503, 'busy'), 'The server is busy checking passwords. Try again in a few seconds.', undefined],
      [{ status: 502, text: '<html>Bad Gateway</html>' }, 'Accounts are offline right now. Try again in a few minutes.', undefined],
    ];
    for (const [answer, text, field] of cases) {
      const t = setup([answer]);
      const out = await startLink(t.api, t.store, t.tab, 'a-password');
      const problem = out.kind === 'invalid' ? out.problems[0] : out.kind === 'error' ? out.problem : null;
      expect(problem, text).toEqual(field ? { field, text } : { text });
      expect(t.session.map.has(PENDING_KEY)).toBe(false);
    }
  });

  it('a wrong password is not a sign-out; an ended session is', async () => {
    const wrong = setup([apiError(401, 'invalid_credentials')]);
    await startLink(wrong.api, wrong.store, wrong.tab, 'x-password');
    expect(wrong.store.get()).toBe(TOKEN);
    const ended = setup([apiError(401, 'unauthorized')]);
    expect(await startLink(ended.api, ended.store, ended.tab, 'x-password')).toEqual({ kind: 'signed-out' });
    expect(ended.store.get()).toBeNull();
  });

  it('never sends the tab anywhere but Discord, and never leaves without somewhere to keep the secret', async () => {
    const elsewhere = setup([{ status: 200, json: { flowId: FLOW, authorizeUrl: 'https://evil.example/oauth2/authorize?x', pollSecret: SECRET, expiresAt: '2026-11-06T12:10:00.000Z' } }]);
    expect((await startLink(elsewhere.api, elsewhere.store, elsewhere.tab, 'pw-123456')).kind).toBe('error');
    expect(elsewhere.session.map.has(PENDING_KEY)).toBe(false);

    const refusing = setup([started], { tab: memoryStorage({ throwOnSet: true }) });
    expect(await startLink(refusing.api, refusing.store, refusing.tab, 'pw-123456')).toEqual({ kind: 'error', problem: { text: NO_STORAGE_TEXT } });
    const none = setup([started], { tab: null });
    expect(await startLink(none.api, none.store, none.tab, 'pw-123456')).toEqual({ kind: 'error', problem: { text: NO_STORAGE_TEXT } });

    expect(isDiscordAuthorizeUrl(AUTHORIZE)).toBe(true);
    for (const bad of ['http://discord.com/oauth2/authorize', 'https://discord.com.evil.example/oauth2/authorize', 'https://discord.com/login', 'javascript:alert(1)', 'not a url']) {
      expect(isDiscordAuthorizeUrl(bad), bad).toBe(false);
    }
  });

  it('a start answer of the wrong shape is a failure, not a half-started link', async () => {
    const t = setup([{ status: 200, json: { flowId: 'nope', authorizeUrl: AUTHORIZE, pollSecret: SECRET, expiresAt: 'x' } }]);
    expect((await startLink(t.api, t.store, t.tab, 'pw-123456')).kind).toBe('error');
    expect(t.session.map.has(PENDING_KEY)).toBe(false);
  });
});

describe('coming back from Discord', () => {
  it('reads the flow and claim from the fragment only, and only in their exact shape', () => {
    expect(readReturn(`#discord_flow=${FLOW}&discord_claim=${CLAIM}`)).toEqual({ flowId: FLOW, claim: CLAIM });
    expect(readReturn(`#discord_claim=${CLAIM}&discord_flow=${FLOW}`)).toEqual({ flowId: FLOW, claim: CLAIM });
    expect(readReturn('')).toBeNull();
    expect(readReturn(`?discord_flow=${FLOW}&discord_claim=${CLAIM}`)).toBeNull();
    expect(readReturn(`#discord_flow=${FLOW}`)).toBeNull();
    expect(readReturn(`#discord_flow=${FLOW}&discord_claim=short`)).toBeNull();
    expect(readReturn(`#discord_flow=../../x&discord_claim=${CLAIM}`)).toBeNull();
    expect(withoutFragment(`https://solar-rp.com/account/#discord_flow=${FLOW}&discord_claim=${CLAIM}`)).toBe('https://solar-rp.com/account/');
    expect(withoutFragment('https://solar-rp.com/account/')).toBe('https://solar-rp.com/account/');
  });

  it('finishes with the flow, this tab\'s poll secret and the claim, under the session; the secret is spent', async () => {
    const t = setup([{ status: 200, json: { status: 'linked' } }]);
    t.session.map.set(PENDING_KEY, JSON.stringify({ flowId: FLOW, pollSecret: SECRET, expiresAt: '2026-11-06T12:10:00.000Z' }));
    expect(await finishLink(t.api, t.store, t.tab, { flowId: FLOW, claim: CLAIM })).toEqual({ kind: 'linked' });
    expect(t.f.calls[0]).toMatchObject({ method: 'POST', url: '/api/v1/me/discord/link/finish', body: { flowId: FLOW, pollSecret: SECRET, claim: CLAIM } });
    expect(t.f.calls[0]?.headers.authorization).toBe(`Bearer ${TOKEN}`);
    expect(t.session.map.has(PENDING_KEY)).toBe(false);
  });

  it('a claim for a flow this tab did not start is refused without asking the platform (somebody else\'s link URL)', async () => {
    const noPending = setup([]);
    const out = await finishLink(noPending.api, noPending.store, noPending.tab, { flowId: FLOW, claim: CLAIM });
    expect(out).toEqual({ kind: 'error', problem: { text: 'That Discord link didn’t finish. Link Discord again from this page.' } });
    expect(noPending.f.calls).toHaveLength(0);

    const other = setup([]);
    other.session.map.set(PENDING_KEY, JSON.stringify({ flowId: '11111111-1111-4111-8111-111111111111', pollSecret: SECRET, expiresAt: 'x' }));
    expect((await finishLink(other.api, other.store, other.tab, { flowId: FLOW, claim: CLAIM })).kind).toBe('error');
    expect(other.f.calls).toHaveLength(0);
  });

  it('says why a link failed, by its code, and spends the secret either way', async () => {
    const cases: [Answer, string][] = [
      [{ status: 200, json: { status: 'failed', code: 'discord_already_linked' } }, 'That Discord account is already linked to another Solar RP account.'],
      [{ status: 200, json: { status: 'failed', code: 'access_denied' } }, 'You cancelled on Discord, so nothing was linked.'],
      [{ status: 200, json: { status: 'failed', code: 'password_changed' } }, 'Your password changed while you were on Discord. Link Discord again.'],
      [{ status: 200, json: { status: 'failed', code: 'already_has_discord' } }, 'This account already has a Discord account linked.'],
      [{ status: 200, json: { status: 'failed', code: 'something_new' } }, LINK_FAILED_DEFAULT],
      [apiError(410, 'flow_expired'), 'The link took longer than 10 minutes. Link Discord again.'],
      [apiError(410, 'flow_used'), 'This Discord link was already finished. If Discord doesn’t show as linked, link it again.'],
      [apiError(404, 'flow_not_found'), 'That Discord link didn’t finish. Link Discord again from this page.'],
      ['network-down', 'Couldn’t reach Solar RP. Check your connection and try again.'],
    ];
    for (const [answer, text] of cases) {
      const t = setup([answer]);
      t.session.map.set(PENDING_KEY, JSON.stringify({ flowId: FLOW, pollSecret: SECRET, expiresAt: 'x' }));
      expect(await finishLink(t.api, t.store, t.tab, { flowId: FLOW, claim: CLAIM }), text).toEqual({ kind: 'error', problem: { text } });
      expect(t.session.map.has(PENDING_KEY)).toBe(false);
    }
  });

  it('an ended session goes to log in', async () => {
    const t = setup([apiError(401, 'unauthorized')]);
    t.session.map.set(PENDING_KEY, JSON.stringify({ flowId: FLOW, pollSecret: SECRET, expiresAt: 'x' }));
    expect(await finishLink(t.api, t.store, t.tab, { flowId: FLOW, claim: CLAIM })).toEqual({ kind: 'signed-out' });
    expect(t.store.get()).toBeNull();
  });
});
