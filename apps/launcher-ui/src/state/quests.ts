import { ApiFailure } from '../lib/errors';
import type { Quest } from '../lib/schemas';

// What each quest row offers, as pure functions over the platform's own status (apps/platform
// src/quests/service.ts questView decides it; the launcher only reads it). The one thing the
// client adds is the clock: a started visit quest's wait counts down locally from the
// `waitSeconds` the server sent with the response (never from `claimableAt` against the PC's
// clock, which may be off), and the row offers Claim once it reaches zero.

/** The row's next step. */
export type QuestAction =
  /** A visit quest not started: open its page and start the wait. */
  | { kind: 'start' }
  /** Started: the claim opens at `deadline` (epoch ms on this PC's clock). */
  | { kind: 'wait'; deadline: number; secondsLeft: number; total: number }
  | { kind: 'claim' }
  /** The Discord quest while no Discord is linked: its step is linking (on the website), so its row holds the Link Discord plate. */
  | { kind: 'link' }
  /** Another quest that needs a linked Discord account before it can be claimed. */
  | { kind: 'linkDiscord' }
  | { kind: 'claimed'; at: string | null }
  /** Not open yet (the platform's `not_available`): shown as coming soon, no action. */
  | { kind: 'soon' }
  /** Cannot be done by this account: `why` is the player-facing line. */
  | { kind: 'locked'; why: string }
  /** A status this launcher does not know: logged, shown as unavailable, no action. */
  | { kind: 'unknown'; status: string };

/** Where a row stands in the list: what can be done now first (linking unlocks every claim), what is done last. */
const ROW_ORDER: { [K in QuestAction['kind']]: number } = {
  link: 0, claim: 1, wait: 2, start: 3, linkDiscord: 4, soon: 5, locked: 6, unknown: 7, claimed: 8,
};

/**
 * The list order, by each row's step and then the platform's `sort`. Taken once per read of the
 * list (not on every tick), so a row the player just claimed stays where they clicked it.
 */
export function questOrder(quests: readonly Quest[], deadlines: Readonly<Record<string, number>>, now: number, visitMinSeconds: number): string[] {
  return quests
    .map((q) => ({ q, rank: ROW_ORDER[questAction(q, deadlines[q.slug] ?? null, now, visitMinSeconds).kind] }))
    .sort((a, b) => a.rank - b.rank || a.q.sort - b.q.sort)
    .map((x) => x.q.slug);
}

/** When the claim of a started quest opens, on this PC's clock: the server's `waitSeconds` counted from when the answer arrived. */
export function deadlineFor(q: Pick<Quest, 'waitSeconds' | 'claimableAt'>, receivedAt: number): number | null {
  if (q.waitSeconds !== null) return receivedAt + q.waitSeconds * 1000;
  return q.claimableAt ? Date.parse(q.claimableAt) : null;
}

const LOCK_COPY: Record<string, string> = {
  discord_already_claimed: 'Your Discord account already claimed this on another Solar RP account.',
};

/**
 * The action for one quest. `deadline` is the row's local claim time (deadlineFor), `now` the
 * current time, `visitMinSeconds` the full wait (for the countdown bar).
 */
export function questAction(q: Quest, deadline: number | null, now: number, visitMinSeconds: number): QuestAction {
  switch (q.status) {
    case 'claimed': return { kind: 'claimed', at: q.claimedAt };
    case 'claimable': return { kind: 'claim' };
    case 'available': return { kind: 'start' };
    case 'started': {
      const at = deadline ?? now;
      if (at <= now) return { kind: 'claim' };
      return { kind: 'wait', deadline: at, secondsLeft: Math.ceil((at - now) / 1000), total: visitMinSeconds };
    }
    case 'locked':
      if (q.lockReason === 'discord_required') return q.kind === 'discord_link' ? { kind: 'link' } : { kind: 'linkDiscord' };
      if (q.lockReason === 'not_available') return { kind: 'soon' };
      return { kind: 'locked', why: LOCK_COPY[q.lockReason ?? ''] ?? 'Not available right now.' };
    default: return { kind: 'unknown', status: q.status };
  }
}

/** Coins still to earn: every quest not claimed and not permanently locked for this account. */
export function coinsLeft(quests: readonly Quest[]): number {
  return quests
    .filter((q) => q.status !== 'claimed' && !(q.status === 'locked' && q.lockReason !== 'discord_required'))
    .reduce((n, q) => n + q.reward.amount, 0);
}

/** Coins already earned here: the rewards of claimed quests (the wallet bar's filled part, of earned + left). */
export function coinsEarned(quests: readonly Quest[]): number {
  return quests.filter((q) => q.status === 'claimed').reduce((n, q) => n + q.reward.amount, 0);
}

/** The platform's claim and start refusals, in the player's words (apps/platform docs/api.md "Quests"). */
export function questFailureCopy(e: unknown): string | null {
  if (!(e instanceof ApiFailure) || e.kind !== 'refused') return null;
  switch (e.code) {
    case 'quest_wait': return e.retryAfterSeconds ? `Not yet: you can claim this in ${e.retryAfterSeconds} s.` : 'Not yet: the claim opens 30 s after you start the quest.';
    case 'discord_required': return 'Link your Discord account first, then claim.';
    case 'discord_already_claimed': return LOCK_COPY.discord_already_claimed!;
    case 'already_claimed': return 'You already claimed this one.';
    case 'quest_inactive': return 'This quest is not running any more.';
    case 'quest_not_found': return 'This quest is gone. The list was refreshed.';
    case 'quest_not_started': return 'Start the quest first.';
    case 'quest_unavailable': return 'This quest cannot be completed yet.';
    case 'not_a_visit_quest': return 'This quest is claimed directly; there is nothing to start.';
    case 'account_banned': return e.message;
    default: return null;
  }
}

/** The wait a quest_wait refusal names, in seconds: Retry-After, else details.waitSeconds. */
export function waitFromFailure(e: unknown): number | null {
  if (!(e instanceof ApiFailure) || e.code !== 'quest_wait') return null;
  if (e.retryAfterSeconds) return e.retryAfterSeconds;
  const d = e.details as { waitSeconds?: unknown } | undefined;
  return typeof d?.waitSeconds === 'number' && d.waitSeconds >= 0 ? d.waitSeconds : null;
}

/** Refusals after which the list is stale and is read again. */
export function refusalNeedsReload(e: unknown): boolean {
  return e instanceof ApiFailure && e.kind === 'refused' && ['already_claimed', 'quest_inactive', 'quest_not_found', 'discord_required', 'discord_already_claimed', 'quest_unavailable', 'quest_not_started'].includes(e.code ?? '');
}
