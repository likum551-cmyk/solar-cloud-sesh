import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { readResponse } from '../src/lib/api';
import { ApiFailure } from '../src/lib/errors';
import { questListSchema, type Quest } from '../src/lib/schemas';
import { ACCOUNT_URL, SIGNUP_URL } from '../src/lib/site';
import { QuestRow } from '../src/screens/QuestsScreen';
import { coinsEarned, coinsLeft, deadlineFor, questAction, questFailureCopy, questOrder, refusalNeedsReload, waitFromFailure, type QuestAction } from '../src/state/quests';

// The quest rows read the platform's status (apps/platform src/quests/service.ts) and add only the
// local countdown; these pin every status and refusal the platform documents (docs/api.md "Quests").

const T = Date.parse('2026-11-06T12:00:00.000Z');

const quest = (over: Partial<Quest> = {}): Quest => ({
  slug: 'youtube_subscribe', kind: 'visit', title: 'Subscribe on YouTube', description: 'Subscribe to the Solar RP YouTube channel.',
  url: 'https://www.youtube.com/@solar-rp', reward: { currency: 'coins', amount: 100 }, sort: 20,
  status: 'available', lockReason: null, startedAt: null, claimableAt: null, waitSeconds: null, claimedAt: null, ...over,
});

const refused = (code: string, opts: { retryAfterSeconds?: number; details?: unknown } = {}) =>
  new ApiFailure('refused', `refused ${code}`, { status: 409, code, ...opts });

describe('questAction', () => {
  it('maps every platform status to the row step', () => {
    expect(questAction(quest(), null, T, 30)).toEqual({ kind: 'start' });
    expect(questAction(quest({ status: 'claimable' }), null, T, 30)).toEqual({ kind: 'claim' });
    expect(questAction(quest({ status: 'claimed', claimedAt: '2026-11-05T10:00:00.000Z' }), null, T, 30)).toEqual({ kind: 'claimed', at: '2026-11-05T10:00:00.000Z' });
    expect(questAction(quest({ status: 'locked', lockReason: 'discord_required' }), null, T, 30)).toEqual({ kind: 'linkDiscord' });
    // The Discord quest's own step, unlinked, is the link itself (never "needs Discord").
    expect(questAction(quest({ kind: 'discord_link', status: 'locked', lockReason: 'discord_required' }), null, T, 30)).toEqual({ kind: 'link' });
    expect(questAction(quest({ status: 'locked', lockReason: 'not_available' }), null, T, 30)).toEqual({ kind: 'soon' });
    expect(questAction(quest({ status: 'locked', lockReason: 'discord_already_claimed' }), null, T, 30)).toMatchObject({ kind: 'locked', why: expect.stringMatching(/another Solar RP account/) });
    expect(questAction(quest({ status: 'locked', lockReason: 'something_new' }), null, T, 30)).toEqual({ kind: 'locked', why: 'Not available right now.' });
    expect(questAction(quest({ status: 'paused' }), null, T, 30)).toEqual({ kind: 'unknown', status: 'paused' });
  });

  it('counts a started quest down on the local clock and offers Claim at zero', () => {
    const q = quest({ status: 'started', waitSeconds: 30 });
    const deadline = T + 30_000;
    expect(questAction(q, deadline, T, 30)).toEqual({ kind: 'wait', deadline, secondsLeft: 30, total: 30 });
    expect(questAction(q, deadline, T + 12_100, 30)).toMatchObject({ kind: 'wait', secondsLeft: 18 });
    expect(questAction(q, deadline, T + 29_999, 30)).toMatchObject({ kind: 'wait', secondsLeft: 1 });
    expect(questAction(q, deadline, T + 30_000, 30)).toEqual({ kind: 'claim' });
    // No deadline known (no wait in the answer): the server said started, the claim will say how long.
    expect(questAction(q, null, T, 30)).toEqual({ kind: 'claim' });
  });
});

describe('questOrder', () => {
  it('puts what can be done now first and claimed quests last, the platform sort breaking ties', () => {
    const list = [
      quest({ slug: 'claimed_a', sort: 1, status: 'claimed' }),
      quest({ slug: 'soon_b', sort: 2, status: 'locked', lockReason: 'not_available' }),
      quest({ slug: 'start_c', sort: 3 }),
      quest({ slug: 'wait_d', sort: 4, status: 'started' }),
      quest({ slug: 'claim_e', sort: 5, status: 'claimable' }),
      quest({ slug: 'start_f', sort: 0 }),
      quest({ slug: 'link_g', sort: 6, status: 'locked', lockReason: 'discord_required' }),
      quest({ slug: 'discord_h', kind: 'discord_link', sort: 7, status: 'locked', lockReason: 'discord_required' }),
    ];
    // Linking unlocks every claim, so the Discord quest's link leads.
    expect(questOrder(list, { wait_d: T + 9_000 }, T, 30)).toEqual(['discord_h', 'claim_e', 'wait_d', 'start_f', 'start_c', 'link_g', 'soon_b', 'claimed_a']);
    // A started quest whose wait is over is claimable now, so it leads.
    expect(questOrder(list, { wait_d: T - 1 }, T, 30).slice(1, 3)).toEqual(['wait_d', 'claim_e']);
  });
});

describe('deadlineFor', () => {
  it('counts waitSeconds from when the answer arrived, not from claimableAt (the PC clock may be off)', () => {
    // claimableAt is an hour in the "past" on a PC whose clock runs fast; waitSeconds still wins.
    expect(deadlineFor({ waitSeconds: 12, claimableAt: '2026-11-06T11:00:00.000Z' }, T)).toBe(T + 12_000);
    expect(deadlineFor({ waitSeconds: null, claimableAt: '2026-11-06T12:00:30.000Z' }, T)).toBe(T + 30_000);
    expect(deadlineFor({ waitSeconds: null, claimableAt: null }, T)).toBeNull();
  });
});

describe('coinsLeft', () => {
  it('sums what can still be earned: not claimed, not permanently locked', () => {
    const list = [
      quest({ slug: 'link_discord', kind: 'discord_link', status: 'locked', lockReason: 'discord_required', reward: { currency: 'coins', amount: 500 } }),
      quest({ slug: 'a', status: 'available' }),
      quest({ slug: 'b', status: 'started' }),
      quest({ slug: 'c', status: 'claimed' }),
      quest({ slug: 'd', status: 'locked', lockReason: 'not_available' }),
      quest({ slug: 'e', status: 'locked', lockReason: 'discord_already_claimed' }),
    ];
    expect(coinsLeft(list)).toBe(700);
    expect(coinsLeft([])).toBe(0);
    expect(coinsEarned(list)).toBe(100);
    expect(coinsEarned([])).toBe(0);
  });
});

describe('quest refusals', () => {
  it('reads the wait from Retry-After, else from details.waitSeconds', () => {
    expect(waitFromFailure(refused('quest_wait', { retryAfterSeconds: 17 }))).toBe(17);
    expect(waitFromFailure(refused('quest_wait', { details: { waitSeconds: 9, claimableAt: '2026-11-06T12:00:09.000Z' } }))).toBe(9);
    expect(waitFromFailure(refused('already_claimed'))).toBeNull();
    expect(waitFromFailure(new Error('x'))).toBeNull();
  });

  it('a real 409 quest_wait response keeps Retry-After on the failure', async () => {
    const res = new Response(JSON.stringify({ error: { code: 'quest_wait', message: 'You can claim this in 21 s.', requestId: 'r', details: { waitSeconds: 21, claimableAt: '2026-11-06T12:00:21.000Z' } } }), { status: 409, headers: { 'Retry-After': '21' } });
    const e = await readResponse(res, questListSchema).then(() => null, (x: unknown) => x);
    expect(e).toBeInstanceOf(ApiFailure);
    expect(e).toMatchObject({ kind: 'refused', code: 'quest_wait', retryAfterSeconds: 21 });
    expect(waitFromFailure(e)).toBe(21);
  });

  it('names every documented refusal in the player’s words', () => {
    for (const code of ['quest_wait', 'discord_required', 'discord_already_claimed', 'already_claimed', 'quest_inactive', 'quest_not_found', 'quest_not_started', 'quest_unavailable', 'not_a_visit_quest', 'account_banned']) {
      expect(questFailureCopy(refused(code)), code).toBeTruthy();
    }
    expect(questFailureCopy(refused('quest_wait', { retryAfterSeconds: 4 }))).toBe('Not yet: you can claim this in 4 s.');
    expect(questFailureCopy(refused('something_else'))).toBeNull();
    expect(questFailureCopy(new ApiFailure('offline', 'down'))).toBeNull();
  });

  it('reads the list again after a refusal that means it is stale, not after a wait', () => {
    expect(refusalNeedsReload(refused('already_claimed'))).toBe(true);
    expect(refusalNeedsReload(refused('quest_inactive'))).toBe(true);
    expect(refusalNeedsReload(refused('discord_required'))).toBe(true);
    expect(refusalNeedsReload(refused('quest_wait'))).toBe(false);
    expect(refusalNeedsReload(new ApiFailure('offline', 'down'))).toBe(false);
  });
});

describe('QuestRow', () => {
  const render = (q: Quest, action: QuestAction, extra: { error?: string | null; primary?: boolean } = {}) =>
    renderToStaticMarkup(
      <QuestRow quest={q} action={action} primary={extra.primary ?? true} busy={false} locked={false} error={extra.error ?? null}
        onStart={() => {}} onClaim={() => {}} onReopen={() => {}} />,
    );

  it('renders each step with its one control, and nothing undefined', () => {
    const start = render(quest(), { kind: 'start' });
    expect(start).toContain('Start');
    expect(start).toContain('+100');
    expect(start).not.toContain(quest().description);   // the title says it; the platform's line repeats it

    // The countdown lives in the Claim plate, which fills toward the moment it opens.
    const wait = render(quest({ status: 'started' }), { kind: 'wait', deadline: T + 18_000, secondsLeft: 18, total: 30 });
    expect(wait).toContain('quest__wait');
    expect(wait).toContain('0:18');
    expect(wait).toContain('Claim opens in 18 seconds');
    expect(wait).toMatch(/--fill:0\.4/);   // 12 of 30 s gone
    expect(wait).toContain('disabled');    // Claim shows, closed, while the wait runs
    // Reopening the page is a quiet icon beside the plate, so the row stays one line.
    expect(wait).toContain('quest__reopen');
    expect(wait).toContain('Open the Subscribe on YouTube page again');
    expect(wait).not.toContain('quest__hint');
    expect(wait).toMatch(/<span>Claim · <span class="quest__clock">0:18<\/span><\/span>/);
    expect(render(quest({ status: 'started' }), { kind: 'wait', deadline: T + 75_000, secondsLeft: 75, total: 90 })).toContain('1:15');

    const claim = render(quest({ status: 'claimable' }), { kind: 'claim' });
    expect(claim).toContain('plate--primary');
    expect(claim).toContain('Claim');
    // Gold is rationed: a claimable row that is not the first one gets the ghost plate.
    const second = render(quest({ status: 'claimable' }), { kind: 'claim' }, { primary: false });
    expect(second).toContain('plate--ghost');
    expect(second).not.toContain('plate--primary');

    // The Discord quest, unlinked, offers the link itself, as the gold.
    const link = render(quest({ slug: 'link_discord', kind: 'discord_link', url: null, status: 'locked', lockReason: 'discord_required', reward: { currency: 'coins', amount: 500 } }), { kind: 'link' });
    expect(link).toContain('Link Discord');
    expect(link).toContain('plate--primary');
    expect(link).toContain('+500');
    expect(link).not.toContain('Needs Discord');
    // Any other quest that needs Discord only says so: no control.
    const needs = render(quest({ status: 'locked', lockReason: 'discord_required' }), { kind: 'linkDiscord' });
    expect(needs).toContain('Needs Discord');
    expect(needs).not.toContain('<button');

    const odd = render(quest({ status: 'paused' }), { kind: 'unknown', status: 'paused' });
    expect(odd).toContain('Unavailable');
    expect(odd).not.toContain('paused');

    const done = render(quest({ status: 'claimed', claimedAt: '2026-11-03T09:00:00.000Z' }), { kind: 'claimed', at: '2026-11-03T09:00:00.000Z' });
    expect(done).toContain('Claimed');
    expect(done).toContain('03 NOV 2026');
    expect(done).not.toContain('+100');   // folded: title, date, chip

    const soon = render(quest({ status: 'locked', lockReason: 'not_available' }), { kind: 'soon' });
    expect(soon).toContain('Coming soon');
    expect(soon).not.toContain('<button');

    const err = render(quest({ status: 'claimable' }), { kind: 'claim' }, { error: 'You already claimed this one.' });
    expect(err).toContain('role="alert"');

    for (const html of [start, wait, claim, link, done, soon, err]) expect(html).not.toMatch(/undefined|NaN|&gt;\}|\}&lt;/);
  });
});

describe('website links', () => {
  it('sign-up and account pages are https on solar-rp.com (openExternal refuses anything else)', () => {
    expect(SIGNUP_URL).toBe('https://solar-rp.com/signup/');
    expect(ACCOUNT_URL).toBe('https://solar-rp.com/account/');
  });
});
