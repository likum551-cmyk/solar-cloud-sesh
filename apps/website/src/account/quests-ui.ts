// The Quests section of the account page: a ledger of quests in the page's left column (Start opens the
// quest's page in a new tab and records the start, then a countdown, then Claim) and the wallet in its
// right column (balance, progress, the Discord link: discord-ui.ts). All text from the API goes in through
// textContent; links only ever carry an https URL (parseQuest drops anything else).

import type { AccountApi, Quest, QuestList } from './api';
import { claimQuest, countdown, formatCoins, hostOf, loadQuests, questProgress, questRow, sortForAction, startQuest, type QuestRowView } from './quests';
import { formatDate } from './format';
import { discordMark } from './marks';
import type { TokenStore } from './store';
import { need } from './ui';

const SVG = 'http://www.w3.org/2000/svg';

/** A small stroked icon, drawn in the page's own 16px grid (no glyphs standing in for icons). */
function icon(paths: string[], cls: string): SVGSVGElement {
  const svg = document.createElementNS(SVG, 'svg');
  svg.setAttribute('viewBox', '0 0 16 16');
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('class', cls);
  for (const d of paths) {
    const p = document.createElementNS(SVG, 'path');
    p.setAttribute('d', d);
    svg.append(p);
  }
  return svg;
}
const ICON_OUT = ['M6 3H3v10h10v-3', 'M9 2.5h4.5V7', 'M13.5 2.5L7.5 8.5'];
const ICON_CHECK = ['M2.5 8.5l3.5 3.5 7.5-8'];
const ICON_LOCK = ['M4 7.5h8v6H4z', 'M5.75 7.5V5.25a2.25 2.25 0 0 1 4.5 0V7.5'];

function el<K extends keyof HTMLElementTagNameMap>(tag: K, cls: string, text?: string): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

/** "Solar RP" never splits across two lines (a no-break space, built in code). */
const NBSP = String.fromCharCode(0xa0);
const keepBrand = (s: string) => s.replace(/Solar RP/g, () => `Solar${NBSP}RP`);

const reducedMotion = () => window.matchMedia('(prefers-reduced-motion: reduce)').matches;

/**
 * `onLinkDiscord` opens the wallet's Discord link control (the Discord quest's own action, so the link
 * is where the quest is).
 */
export function mountQuests(api: AccountApi, store: TokenStore, onSignedOut: () => void, onLinkDiscord: () => void): { refresh: () => Promise<void> } {
  const section = need<HTMLElement>('.sec--quests');
  const listEl = need<HTMLUListElement>('.js-quests', section);
  const coinsEl = need<HTMLElement>('.js-coins', section);
  const coinsBox = need<HTMLElement>('.coins', section);
  const status = need<HTMLElement>('.js-quests-status', section);
  const empty = need<HTMLElement>('.js-quests-empty', section);
  const progressBox = need<HTMLElement>('.js-quests-progress', section);
  const progressText = need<HTMLElement>('.js-quests-progress-text', section);
  const bar = need<HTMLElement>('.js-quests-bar', section);

  let list: QuestList | null = null;
  let coins = 0;
  /** when this browser received each quest's current state (performance.now()), for its countdown */
  const receivedAt = new Map<string, number>();
  const rows = new Map<string, HTMLLIElement>();
  let timer: number | undefined;
  let countUp = 0;

  const viewOf = (q: Quest): QuestRowView => questRow(q, list?.discordLinked ?? false, performance.now() - (receivedAt.get(q.slug) ?? performance.now()));

  /** Errors stay on screen; a success is said to screen readers only (the row and the balance show it). */
  function say(text: string, ok: boolean) {
    status.classList.toggle('is-ok', ok);
    status.textContent = text;
  }

  function setCoins(n: number) {
    coins = n;
    coinsEl.textContent = formatCoins(n);
  }

  /** The claim's moment in the wallet: the balance counts up to its new value and holds gold a beat. */
  function gainCoins(to: number) {
    const from = coins;
    coins = to;
    cancelAnimationFrame(countUp);
    coinsBox.classList.remove('is-gain');
    void coinsBox.offsetWidth; // restart the moment if two claims land close together
    coinsBox.classList.add('is-gain');
    if (reducedMotion()) {
      coinsEl.textContent = formatCoins(to);
      return;
    }
    const t0 = performance.now();
    const step = (t: number) => {
      const k = Math.min(1, (t - t0) / 500);
      const eased = 1 - (1 - k) ** 3;
      coinsEl.textContent = formatCoins(Math.round(from + (to - from) * eased));
      if (k < 1) countUp = requestAnimationFrame(step);
    };
    countUp = requestAnimationFrame(step);
  }

  function renderProgress() {
    const quests = list?.quests ?? [];
    progressBox.hidden = quests.length === 0;
    const p = questProgress(quests);
    progressText.textContent = p.left > 0 ? `${p.done} of ${p.total} done · ${formatCoins(p.left)} coins left to earn` : `All ${p.total} done`;
    // one hard-edged segment per quest, as wide as its reward (the bar counts coins, like the line
    // above it); the claimed ones fill gold from the left
    // claimed in the order they were paid, so a new claim extends the gold run instead of reshuffling it
    const paid = quests.filter((q) => q.status === 'claimed').sort((a, b) => (a.claimedAt ?? '').localeCompare(b.claimedAt ?? ''));
    const segs = [...paid, ...quests.filter((q) => q.status !== 'claimed')].map((q) => {
      const seg = el('span', q.status === 'claimed' ? 'wallet__seg is-done' : 'wallet__seg');
      seg.style.flexGrow = String(q.reward.amount);
      return seg;
    });
    bar.replaceChildren(...segs);
  }

  function action(q: Quest, v: QuestRowView): HTMLElement {
    switch (v.kind) {
      case 'start': {
        const a = el('a', 'plate plate--ghost plate--xs quest__start');
        a.href = v.url;
        a.target = '_blank';
        a.rel = 'noopener noreferrer';
        a.append(el('span', '', 'Start'), icon(ICON_OUT, 'quest__out'));
        a.setAttribute('aria-label', `Start: ${q.title} (opens in a new tab)`);
        // The link opens by itself (a popup opened after an await would be blocked); the start is
        // recorded alongside it. A middle click opens it too, so it counts as a start as well.
        a.addEventListener('click', () => void begin(q));
        a.addEventListener('auxclick', (e) => {
          if (e.button === 1) void begin(q);
        });
        return a;
      }
      case 'wait': {
        const b = el('button', 'plate plate--xs quest__claim');
        b.type = 'button';
        b.disabled = true;
        // the visible label is the accessible name, so the time read out is the time shown
        b.append(el('span', 'js-label', `Claim in ${countdown(v.seconds)}`), el('span', 'visually-hidden', `: ${q.title}`));
        return b;
      }
      case 'claim': {
        const b = el('button', 'plate plate--xs quest__claim');
        b.type = 'button';
        b.append(el('span', 'js-label', `Claim +${formatCoins(q.reward.amount)}`));
        b.setAttribute('aria-label', `Claim ${formatCoins(q.reward.amount)} coins for: ${q.title}`);
        b.addEventListener('click', () => void claim(q, b));
        return b;
      }
      case 'claimed': {
        const c = el('span', 'chip chip--ok quest__chip');
        c.append(icon(ICON_CHECK, 'quest__chip-icon'), document.createTextNode('Claimed'));
        return c;
      }
      case 'link': {
        // the page's one "Link Discord" while this row shows (the wallet's own opener steps aside, CSS)
        const b = el('button', 'plate plate--ghost plate--xs quest__link');
        b.type = 'button';
        b.setAttribute('aria-controls', 'discord-form');
        b.setAttribute('aria-expanded', 'false');
        b.append(discordMark('quest__mark'), el('span', 'js-label', 'Link Discord'));
        b.addEventListener('click', () => onLinkDiscord());
        return b;
      }
      case 'locked': {
        const c = el('span', 'chip quest__chip');
        c.append(icon(ICON_LOCK, 'quest__chip-icon'), document.createTextNode(v.text));
        return c;
      }
      case 'no-link':
        return el('span', 'chip quest__chip', 'Opens soon');
    }
  }

  /** A meta line that breaks only between its parts (never inside "30 seconds" or a host). */
  function meta(parts: string[]): HTMLParagraphElement {
    const p = el('p', 'quest__meta');
    parts.forEach((part, i) => {
      if (i > 0) p.append(document.createTextNode(' · '));
      p.append(el('span', 'quest__meta-part', part));
    });
    return p;
  }

  function row(q: Quest): HTMLLIElement {
    const v = viewOf(q);
    const li = el('li', 'quest');
    li.dataset.slug = q.slug;
    li.dataset.state = v.kind;
    const reward = el('p', 'quest__reward');
    reward.append(el('span', 'quest__amount', `+${formatCoins(q.reward.amount)}`), el('span', 'quest__unit', q.reward.currency));
    const text = el('div', 'quest__text');
    text.append(el('p', 'quest__title', keepBrand(q.title)));
    if (q.description) text.append(el('p', 'quest__desc', keepBrand(q.description)));
    const host = hostOf(q.url);
    if (q.kind === 'visit' && v.kind === 'start' && host) {
      // what the quest asks, before it is started: the countdown after it is no surprise
      text.append(meta([`Opens ${host}`, `claim ${list?.visitMinSeconds ?? 30} seconds after you start`]));
    } else if (q.kind === 'visit' && v.kind === 'locked' && q.waitSeconds !== null && (q.status !== 'locked' || q.lockReason === 'discord_required')) {
      // (waitSeconds is non-null exactly when a visit quest was started)
      // started, the wait is over, but no Discord yet: say the start is kept
      text.append(meta(['Started', 'claim once Discord is linked']));
    } else if (v.kind === 'claimed' && q.claimedAt) {
      text.append(meta([`Paid ${formatDate(q.claimedAt)}`]));
    }
    // a started visit quest keeps its page one click away (the first tab may be closed)
    if (q.kind === 'visit' && q.url && q.status !== 'available' && q.status !== 'claimed') {
      const again = el('a', 'quest__again', 'Open the page again');
      again.href = q.url;
      again.target = '_blank';
      again.rel = 'noopener noreferrer';
      text.append(again);
    }
    const act = el('div', 'quest__act');
    act.append(action(q, v));
    li.append(reward, text, act);
    return li;
  }

  function put(q: Quest): HTMLLIElement {
    const fresh = row(q);
    const old = rows.get(q.slug);
    // keep keyboard focus in the row when it is redrawn
    const hadFocus = old?.contains(document.activeElement) ?? false;
    if (old) old.replaceWith(fresh);
    else listEl.append(fresh);
    rows.set(q.slug, fresh);
    if (hadFocus) {
      // a row whose new state has no control (claimed, locked, waiting) keeps focus on its title
      const next = fresh.querySelector<HTMLElement>('.quest__act a, .quest__act button:not(:disabled)');
      if (next) next.focus();
      else {
        const title = need<HTMLElement>('.quest__title', fresh);
        title.tabIndex = -1;
        title.focus();
      }
    }
    schedule();
    return fresh;
  }

  function replace(q: Quest): HTMLLIElement | null {
    if (!list) return null;
    list = { ...list, quests: list.quests.map((x) => (x.slug === q.slug ? q : x)) };
    receivedAt.set(q.slug, performance.now());
    const li = put(q);
    renderProgress();
    return li;
  }

  /** One timer for every countdown; it stops when none is left. */
  function schedule() {
    const waiting = list?.quests.some((q) => viewOf(q).kind === 'wait') ?? false;
    if (waiting && timer === undefined) timer = window.setInterval(tick, 1000);
    if (!waiting && timer !== undefined) {
      window.clearInterval(timer);
      timer = undefined;
    }
  }

  function tick() {
    for (const q of list?.quests ?? []) {
      const li = rows.get(q.slug);
      if (!li || li.dataset.state !== 'wait') continue;
      const v = viewOf(q);
      if (v.kind === 'wait') {
        const label = li.querySelector<HTMLElement>('.js-label');
        if (label) label.textContent = `Claim in ${countdown(v.seconds)}`;
      } else {
        put(q);
        if (v.kind === 'claim') say(`${q.title}: ready to claim.`, true);
      }
    }
    schedule();
  }

  async function begin(q: Quest) {
    say('', true);
    const out = await startQuest(api, store, q.slug);
    if (out.kind === 'signed-out') return onSignedOut();
    if (out.kind === 'error') {
      say(`${q.title}: ${out.problem.text}`, false);
      if (out.code === 'quest_inactive' || out.code === 'quest_not_found') await refresh(false);
      return;
    }
    replace(out.quest);
  }

  async function claim(q: Quest, button: HTMLButtonElement) {
    say('', true);
    button.disabled = true;
    button.setAttribute('aria-busy', 'true');
    const label = button.querySelector<HTMLElement>('.js-label');
    if (label) label.textContent = 'Claiming…';
    const out = await claimQuest(api, store, q.slug);
    if (out.kind === 'signed-out') return onSignedOut();
    if (out.kind === 'error') {
      say(`${q.title}: ${out.problem.text}`, false);
      // the platform knows better than this page: redraw from its answer (keeping this message)
      await refresh(false);
      return;
    }
    const li = replace(out.quest);
    if (li) {
      li.classList.add('is-paid'); // the moment at the row: the amount goes gold, then settles
      if (!reducedMotion()) {
        // the brand slash crosses the row once and is removed, so nothing of it stays
        const wipe = el('span', 'quest__wipe');
        wipe.setAttribute('aria-hidden', 'true');
        const barEl = el('span', 'quest__wipe-bar');
        barEl.addEventListener('animationend', () => wipe.remove(), { once: true });
        barEl.addEventListener('animationcancel', () => wipe.remove(), { once: true });
        wipe.append(barEl);
        li.append(wipe);
      }
    }
    gainCoins(out.coins);
    say(`+${formatCoins(out.amount)} coins for “${q.title}”. Balance: ${formatCoins(out.coins)}.`, true);
  }

  async function refresh(clearStatus = true): Promise<void> {
    const out = await loadQuests(api, store);
    if (out.kind === 'signed-out') return onSignedOut();
    listEl.removeAttribute('aria-busy');
    if (out.kind === 'error') {
      if (list === null) {
        listEl.replaceChildren();
        coinsEl.textContent = '–';
      }
      say(out.problem.text, false);
      return;
    }
    if (clearStatus) say('', true);
    // what can be done now comes first; only a reload re-sorts, so no row moves under the pointer
    list = { ...out.list, quests: sortForAction(out.list.quests, out.list.discordLinked) };
    const now = performance.now();
    receivedAt.clear();
    for (const q of list.quests) receivedAt.set(q.slug, now);
    rows.clear();
    listEl.replaceChildren();
    for (const q of list.quests) put(q);
    empty.hidden = list.quests.length > 0;
    setCoins(out.coins);
    renderProgress();
    schedule();
  }

  return { refresh: () => refresh() };
}
