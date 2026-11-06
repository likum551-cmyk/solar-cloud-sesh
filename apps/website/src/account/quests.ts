// Quests on the account page, free of the DOM: the calls (list, start, claim) and the rule that turns
// a quest and the clock into what its row shows. Coins are paid only by the platform (it checks every
// rule again on a claim); this module never decides that something was paid.

import type { AccountApi, ApiFailure, Quest, QuestList } from './api';
import { describeFailure, type Problem } from './errors';
import type { TokenStore } from './store';

export const COINS = 'coins';

const isUnauthorized = (f: ApiFailure) => f.kind === 'api' && f.code === 'unauthorized';

export type QuestsLoad =
  | { kind: 'ready'; list: QuestList; coins: number }
  | { kind: 'signed-out' }
  | { kind: 'error'; problem: Problem };

/** The quests and the coin balance, together (the balance is what a claim adds to). */
export async function loadQuests(api: AccountApi, store: TokenStore): Promise<QuestsLoad> {
  const token = store.get();
  if (token === null) return { kind: 'signed-out' };
  const [list, wallet] = await Promise.all([api.quests(token), api.wallet(token)]);
  for (const r of [list, wallet]) {
    if (!r.ok && isUnauthorized(r.failure)) {
      store.clear();
      return { kind: 'signed-out' };
    }
  }
  if (!list.ok) return { kind: 'error', problem: describeFailure(list.failure, 'quests') };
  if (!wallet.ok) return { kind: 'error', problem: describeFailure(wallet.failure, 'quests') };
  return { kind: 'ready', list: list.value, coins: wallet.value.find((b) => b.currency === COINS)?.balance ?? 0 };
}

export type QuestAction<T> = ({ kind: 'done' } & T) | { kind: 'signed-out' } | { kind: 'error'; problem: Problem; code: string | null };

function failed<T>(store: TokenStore, f: ApiFailure): QuestAction<T> {
  if (isUnauthorized(f)) {
    store.clear();
    return { kind: 'signed-out' };
  }
  return { kind: 'error', problem: describeFailure(f, 'quests'), code: f.kind === 'api' ? f.code : null };
}

/** Record the start of a visit quest. The page opens the quest's link itself, in the same click. */
export async function startQuest(api: AccountApi, store: TokenStore, slug: string): Promise<QuestAction<{ quest: Quest }>> {
  const token = store.get();
  if (token === null) return { kind: 'signed-out' };
  const r = await api.startQuest(token, slug);
  return r.ok ? { kind: 'done', quest: r.value } : failed(store, r.failure);
}

export async function claimQuest(api: AccountApi, store: TokenStore, slug: string): Promise<QuestAction<{ quest: Quest; amount: number; coins: number }>> {
  const token = store.get();
  if (token === null) return { kind: 'signed-out' };
  const r = await api.claimQuest(token, slug);
  if (!r.ok) return failed(store, r.failure);
  return { kind: 'done', quest: r.value.quest, amount: r.value.credited.amount, coins: r.value.balance };
}

/* ---------------------------------------------------------------- what a row shows */

export type QuestRowView =
  /** a visit quest not started: a Start link that opens its page */
  | { kind: 'start'; url: string }
  /** a visit quest without a page yet (the API never lists one, but a quest must never get a dead link) */
  | { kind: 'no-link' }
  | { kind: 'wait'; seconds: number }
  | { kind: 'claim' }
  | { kind: 'claimed' }
  /** the Discord quest before Discord is linked: its action is the link itself */
  | { kind: 'link' }
  | { kind: 'locked'; text: string };

export const LOCK_TEXT = {
  discord_required: 'Needs Discord',
  discord_already_claimed: 'Claimed on another account',
  not_available: 'Not available yet',
} as const;

/**
 * The row for `quest`, `elapsed` ms after the list was loaded. The wait counts down on this browser's
 * clock from the server's waitSeconds (never from the server's timestamps, so a wrong PC clock cannot
 * skew it); when it ends the row offers the claim only if a Discord account is linked.
 */
export function questRow(quest: Quest, discordLinked: boolean, elapsed: number): QuestRowView {
  switch (quest.status) {
    case 'claimed':
      return { kind: 'claimed' };
    case 'available':
      return quest.url ? { kind: 'start', url: quest.url } : { kind: 'no-link' };
    case 'locked':
      // the Discord quest itself is not "needs Discord": it simply is not done yet, and its action is the link
      if (quest.kind === 'discord_link' && quest.lockReason === 'discord_required') return { kind: 'link' };
      return { kind: 'locked', text: LOCK_TEXT[quest.lockReason ?? 'not_available'] };
    case 'started': {
      const left = Math.ceil(((quest.waitSeconds ?? 0) * 1000 - elapsed) / 1000);
      if (left > 0) return { kind: 'wait', seconds: left };
      return discordLinked ? { kind: 'claim' } : { kind: 'locked', text: LOCK_TEXT.discord_required };
    }
    case 'claimable':
      return { kind: 'claim' };
  }
}

/** What can be done now first: claim, then waiting, then link and start, then locked, then done. Stable within a group. */
export function sortForAction(quests: Quest[], discordLinked: boolean): Quest[] {
  const rank: Record<QuestRowView['kind'], number> = { claim: 0, wait: 1, link: 2, start: 2, 'no-link': 3, locked: 4, claimed: 5 };
  return quests.map((q, i) => ({ q, i, r: rank[questRow(q, discordLinked, 0).kind] })).sort((a, b) => a.r - b.r || a.i - b.i).map((x) => x.q);
}

/** "2 of 4 done", and the coins still to earn (rewards of the quests not claimed yet). */
export function questProgress(quests: Quest[]): { done: number; total: number; left: number } {
  const open = quests.filter((q) => q.status !== 'claimed');
  return { done: quests.length - open.length, total: quests.length, left: open.reduce((s, q) => s + q.reward.amount, 0) };
}

/** "youtube.com" for a visit quest's link (www. dropped); null when there is no usable link. */
export function hostOf(url: string | null): string | null {
  if (url === null) return null;
  try {
    return new URL(url).hostname.replace(/^www\./, '');
  } catch {
    return null;
  }
}

/** "1,500": grouped by thousands, built by hand (the page never uses the browser's locale strings). */
export function formatCoins(n: number): string {
  const sign = n < 0 ? '-' : '';
  return sign + String(Math.abs(Math.trunc(n))).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

/** "0:24" for a countdown. */
export function countdown(seconds: number): string {
  const s = Math.max(0, Math.ceil(seconds));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}
