import { describe, expect, it } from 'vitest';
import { createAccountApi, parseQuest, type Quest } from '../src/account/api';
import { describeFailure } from '../src/account/errors';
import { claimQuest, countdown, formatCoins, hostOf, loadQuests, questProgress, questRow, sortForAction, startQuest } from '../src/account/quests';
import { createTokenStore } from '../src/account/store';
import { apiError, memoryStorage, scriptedFetch, TOKEN, type Answer } from './account-helpers';

function setup(answers: Answer[], token: string | null = TOKEN) {
  const f = scriptedFetch(answers);
  const session = memoryStorage();
  if (token) session.map.set('solar.session', token);
  const store = createTokenStore(session, memoryStorage());
  return { api: createAccountApi('/api', f.fetchImpl), store, f, session };
}

// The shapes apps/platform/docs/api.md documents for GET /v1/me/quests.
const LINK = { slug: 'link_discord', kind: 'discord_link', title: 'Link your Discord', description: 'Connect your Discord account.', url: null, reward: { currency: 'coins', amount: 500 }, sort: 0, status: 'locked', lockReason: 'discord_required', startedAt: null, claimableAt: null, waitSeconds: null, claimedAt: null };
const YT = { slug: 'youtube_subscribe', kind: 'visit', title: 'Subscribe on YouTube', description: 'Subscribe to the channel.', url: 'https://www.youtube.example/@solar', reward: { currency: 'coins', amount: 100 }, sort: 20, status: 'available', lockReason: null, startedAt: null, claimableAt: null, waitSeconds: null, claimedAt: null };
const listOk = (over: Record<string, unknown> = {}): Answer => ({ status: 200, json: { discordLinked: false, visitMinSeconds: 30, quests: [LINK, YT], ...over } });
const walletOk = (coins: number): Answer => ({ status: 200, json: { balances: [{ currency: 'coins', balance: coins }] } });

function quest(over: Partial<Quest>): Quest {
  const q = parseQuest({ ...YT, ...over });
  if (!q) throw new Error('fixture does not parse');
  return q;
}

describe('quest API shapes', () => {
  it('parses a documented quest and refuses a changed shape', () => {
    expect(parseQuest(LINK)).toMatchObject({ slug: 'link_discord', status: 'locked', lockReason: 'discord_required', reward: { currency: 'coins', amount: 500 } });
    expect(parseQuest({ ...YT, status: 'done' })).toBeNull();
    expect(parseQuest({ ...YT, reward: { currency: 'coins', amount: -5 } })).toBeNull();
    expect(parseQuest({ ...YT, slug: 'Bad Slug' })).toBeNull();
    expect(parseQuest({ ...YT, lockReason: 'something_new' })).toBeNull();
  });

  it('never keeps a link that is not https', () => {
    expect(parseQuest({ ...YT, url: 'javascript:alert(1)' })?.url).toBeNull();
    expect(parseQuest({ ...YT, url: 'http://www.youtube.example/@solar' })?.url).toBeNull();
    expect(parseQuest(YT)?.url).toBe('https://www.youtube.example/@solar');
  });
});

describe('loading quests', () => {
  it('reads the quests and the coin balance with the session token', async () => {
    const t = setup([listOk(), walletOk(1500)]);
    const out = await loadQuests(t.api, t.store);
    expect(out.kind).toBe('ready');
    if (out.kind !== 'ready') return;
    expect(out.coins).toBe(1500);
    expect(out.list.discordLinked).toBe(false);
    expect(out.list.quests.map((q) => q.slug)).toEqual(['link_discord', 'youtube_subscribe']);
    expect(t.f.calls.map((c) => `${c.method} ${c.url}`)).toEqual(['GET /api/v1/me/quests', 'GET /api/v1/me/wallet']);
    expect(t.f.calls.every((c) => c.headers.authorization === `Bearer ${TOKEN}`)).toBe(true);
  });

  it('a wallet with no coins yet is 0', async () => {
    const t = setup([listOk(), { status: 200, json: { balances: [] } }]);
    const out = await loadQuests(t.api, t.store);
    expect(out.kind === 'ready' && out.coins).toBe(0);
  });

  it('a rejected token signs the page out and clears it', async () => {
    const t = setup([apiError(401, 'unauthorized'), walletOk(0)]);
    expect((await loadQuests(t.api, t.store)).kind).toBe('signed-out');
    expect(t.session.map.size).toBe(0);
  });

  it('a platform without quests yet (the static 404 page) says quests are unavailable', async () => {
    const t = setup([{ status: 404, text: '<!doctype html>' }, walletOk(0)]);
    const out = await loadQuests(t.api, t.store);
    expect(out).toEqual({ kind: 'error', problem: { text: 'Quests aren’t available right now. Try again in a few minutes.' } });
  });

  it('a list with one malformed quest is a failure, not a half list', async () => {
    const t = setup([listOk({ quests: [LINK, { ...YT, status: 'weird' }] }), walletOk(0)]);
    expect((await loadQuests(t.api, t.store)).kind).toBe('error');
  });
});

describe('start and claim', () => {
  it('start posts to the quest and returns its new state', async () => {
    const t = setup([{ status: 200, json: { quest: { ...YT, status: 'started', startedAt: '2026-11-06T12:00:00.000Z', waitSeconds: 30 } } }]);
    const out = await startQuest(t.api, t.store, 'youtube_subscribe');
    expect(t.f.calls[0]).toMatchObject({ method: 'POST', url: '/api/v1/me/quests/youtube_subscribe/start' });
    expect(out.kind === 'done' && out.quest).toMatchObject({ status: 'started', waitSeconds: 30 });
  });

  it('claim returns the paid amount and the new balance', async () => {
    const t = setup([{ status: 200, json: { quest: { ...LINK, status: 'claimed', lockReason: null, claimedAt: '2026-11-06T12:00:00.000Z' }, credited: { currency: 'coins', amount: 500 }, balance: 2000 } }]);
    const out = await claimQuest(t.api, t.store, 'link_discord');
    expect(t.f.calls[0]).toMatchObject({ method: 'POST', url: '/api/v1/me/quests/link_discord/claim' });
    expect(out).toMatchObject({ kind: 'done', amount: 500, coins: 2000, quest: { status: 'claimed' } });
  });

  it('every refusal is said in words, with its code kept for the page', async () => {
    const cases: [Answer, string][] = [
      [apiError(403, 'discord_required'), 'Claiming quests needs a linked Discord account.'],
      [apiError(409, 'already_claimed'), 'You already claimed this quest.'],
      [apiError(409, 'quest_wait', { waitSeconds: 12, claimableAt: '2026-11-06T12:00:30.000Z' }, { 'Retry-After': '12' }), 'Not yet: you can claim this in 12 seconds.'],
      [apiError(409, 'quest_not_started'), 'Start the quest first, then claim it.'],
      [apiError(409, 'quest_inactive'), 'This quest has ended.'],
      [apiError(409, 'discord_already_claimed'), 'Your Discord account already claimed this quest on another Solar RP account.'],
      [apiError(409, 'quest_unavailable'), 'This quest can’t be completed yet.'],
    ];
    for (const [answer, text] of cases) {
      const t = setup([answer]);
      const out = await claimQuest(t.api, t.store, 'youtube_subscribe');
      expect(out).toMatchObject({ kind: 'error', problem: { text } });
    }
  });

  it('a claim with no token never calls the platform', async () => {
    const t = setup([], null);
    expect((await claimQuest(t.api, t.store, 'link_discord')).kind).toBe('signed-out');
    expect(t.f.calls).toHaveLength(0);
  });
});

describe('what a row shows', () => {
  it('an available visit quest offers Start with its link', () => {
    expect(questRow(quest({}), false, 0)).toEqual({ kind: 'start', url: 'https://www.youtube.example/@solar' });
  });

  it('a started quest counts down on this browser clock, then offers the claim', () => {
    const q = quest({ status: 'started', waitSeconds: 30 });
    expect(questRow(q, true, 0)).toEqual({ kind: 'wait', seconds: 30 });
    expect(questRow(q, true, 29_001)).toEqual({ kind: 'wait', seconds: 1 });
    expect(questRow(q, true, 30_000)).toEqual({ kind: 'claim' });
  });

  it('at the end of the wait an account without Discord is told to link it, not offered a claim', () => {
    expect(questRow(quest({ status: 'started', waitSeconds: 5 }), false, 6000)).toEqual({ kind: 'locked', text: 'Needs Discord' });
  });

  it('locks, claimed and a quest with no usable link', () => {
    expect(questRow(quest({ status: 'locked', lockReason: 'discord_required' }), false, 0)).toEqual({ kind: 'locked', text: 'Needs Discord' });
    expect(questRow(quest({ status: 'locked', lockReason: 'discord_already_claimed' }), true, 0)).toEqual({ kind: 'locked', text: 'Claimed on another account' });
    expect(questRow(quest({ status: 'claimed', claimedAt: '2026-11-06T12:00:00.000Z' }), true, 0)).toEqual({ kind: 'claimed' });
    expect(questRow(quest({ url: 'ftp://nope' }), true, 0)).toEqual({ kind: 'no-link' });
  });

  it('the Discord quest before linking offers the link itself, not "Needs Discord"; once linked it is a claim', () => {
    const link = { slug: 'link_discord', kind: 'discord_link' as const, url: null };
    expect(questRow(quest({ ...link, status: 'locked', lockReason: 'discord_required' }), false, 0)).toEqual({ kind: 'link' });
    expect(questRow(quest({ ...link, status: 'claimable' }), true, 0)).toEqual({ kind: 'claim' });
    // another quest locked on Discord stays a plain lock (only the Discord quest carries the control)
    expect(questRow(quest({ status: 'locked', lockReason: 'discord_required' }), false, 0)).toEqual({ kind: 'locked', text: 'Needs Discord' });
    // and the link sorts with the things to do now, ahead of the locks
    const locked = quest({ slug: 'locked_one', status: 'locked', lockReason: 'not_available' });
    expect(sortForAction([locked, quest({ ...link, status: 'locked', lockReason: 'discord_required' })], false).map((q) => q.slug)).toEqual(['link_discord', 'locked_one']);
  });

  it('sorts what can be done now first, keeping the API order within a group', () => {
    const claimed = quest({ slug: 'done_one', status: 'claimed', claimedAt: '2026-11-06T12:00:00.000Z' });
    const locked = quest({ slug: 'locked_one', status: 'locked', lockReason: 'not_available' });
    const startA = quest({ slug: 'start_a' });
    const startB = quest({ slug: 'start_b' });
    const waiting = quest({ slug: 'waiting', status: 'started', waitSeconds: 12 });
    const ready = quest({ slug: 'ready', status: 'claimable' });
    const order = sortForAction([claimed, locked, startA, waiting, startB, ready], true).map((q) => q.slug);
    expect(order).toEqual(['ready', 'waiting', 'start_a', 'start_b', 'locked_one', 'done_one']);
    // a started quest whose wait is over, on an account without Discord, is locked, not claimable
    const over = quest({ slug: 'over', status: 'started', waitSeconds: 0 });
    expect(sortForAction([over, startA], false).map((q) => q.slug)).toEqual(['start_a', 'over']);
  });

  it('counts progress and the coins still to earn', () => {
    const qs = [quest({ slug: 'q_a', status: 'claimed', claimedAt: '2026-11-06T12:00:00.000Z', reward: { currency: 'coins', amount: 500 } }), quest({ slug: 'q_b' }), quest({ slug: 'q_c', status: 'started', waitSeconds: 3 })];
    expect(questProgress(qs)).toEqual({ done: 1, total: 3, left: 200 });
    expect(questProgress([])).toEqual({ done: 0, total: 0, left: 0 });
  });

  it('names a link by its host, without www.', () => {
    expect(hostOf('https://www.youtube.example/@solar')).toBe('youtube.example');
    expect(hostOf('https://x.example/solar')).toBe('x.example');
    expect(hostOf(null)).toBeNull();
    expect(hostOf('not a url')).toBeNull();
  });

  it('formats coins and countdowns without the browser locale', () => {
    expect(formatCoins(0)).toBe('0');
    expect(formatCoins(500)).toBe('500');
    expect(formatCoins(1500)).toBe('1,500');
    expect(formatCoins(1234567)).toBe('1,234,567');
    expect(countdown(30)).toBe('0:30');
    expect(countdown(5)).toBe('0:05');
    expect(countdown(90)).toBe('1:30');
  });

  it('quest_wait falls back to Retry-After when details are missing', () => {
    expect(describeFailure({ kind: 'api', status: 409, code: 'quest_wait', message: '', requestId: null, details: null, retryAfterSeconds: 7 }, 'quests').text).toBe('Not yet: you can claim this in 7 seconds.');
  });
});
