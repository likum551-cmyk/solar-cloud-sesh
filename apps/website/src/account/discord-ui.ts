// The Discord block in the wallet column, the link's STATUS: linked (the Discord name and a gold check) or
// not linked (a chip and why). The action is the Discord quest's "Link Discord" (the block's own opener
// shows only when the ledger has no such row); it opens the password form here, which sends the tab to
// Discord. The flows themselves are in discord.ts; all text from the API goes in through textContent.

import type { AccountApi, DiscordLink } from './api';
import { finishLink, startLink } from './discord';
import { discordMark } from './marks';
import type { TokenStore } from './store';
import { clearProblems, need, setBusy, showProblems } from './ui';

type KV = Pick<Storage, 'getItem' | 'setItem' | 'removeItem'>;

function tabStorage(): KV | null {
  try {
    return window.sessionStorage;
  } catch {
    return null;
  }
}

export interface DiscordBlock {
  /** draw the linked / not linked state from the account (GET /v1/me) */
  render(discord: DiscordLink | null): void;
  /** open the password form (the Discord quest's "Link Discord" calls this) */
  open(): void;
  /** finish a link this tab started, with the flow and claim read from the fragment */
  finish(back: { flowId: string; claim: string }): Promise<void>;
}

export function mountDiscord(api: AccountApi, store: TokenStore, onSignedOut: () => void, onLinked: () => Promise<void>): DiscordBlock {
  const box = need<HTMLElement>('.js-discord');
  const linked = need<HTMLElement>('.js-discord-linked', box);
  const name = need<HTMLElement>('.js-discord-name', box);
  const unlinked = need<HTMLElement>('.js-discord-unlinked', box);
  const openBtn = need<HTMLButtonElement>('.js-discord-open', box);
  const form = need<HTMLFormElement>('.js-discord-form', box);
  const password = need<HTMLInputElement>('#dl-password', form);
  const go = need<HTMLButtonElement>('.js-discord-go', form);
  const cancel = need<HTMLButtonElement>('.js-discord-cancel', form);
  const status = need<HTMLElement>('.js-discord-status', box);
  const statusText = need<HTMLElement>('.js-discord-status-text', status);

  linked.prepend(discordMark('dlink__mark'));
  openBtn.prepend(discordMark('dlink__open-mark'));

  /** Every opener of the form: the wallet's own and the Discord quest's row (rebuilt by quests-ui). */
  const openers = () => [...document.querySelectorAll<HTMLButtonElement>('button[aria-controls="discord-form"]')];

  let finishing = false;

  /** `quiet`: said to screen readers only (the linked state on screen already shows it). */
  function say(text: string, error: boolean, quiet = false) {
    statusText.textContent = text;
    status.classList.toggle('is-error', error);
    status.classList.toggle('is-quiet', quiet);
    status.hidden = text === '';
  }

  function setOpen(open: boolean) {
    form.hidden = !open;
    openBtn.hidden = open;
    // the quest row's opener shows as pressed while the form is open (a second click re-focuses the field)
    for (const b of openers()) b.setAttribute('aria-expanded', String(open));
    if (open) {
      say('', false);
      password.focus();
    } else {
      form.reset();
      clearProblems(form);
    }
  }

  function render(d: DiscordLink | null) {
    if (finishing) return; // the finish draws its own outcome, then refreshes
    box.dataset.state = d ? 'linked' : 'unlinked';
    linked.hidden = d === null;
    unlinked.hidden = d !== null;
    // a Discord account whose name Discord did not report still shows as linked (in body type: a
    // stand-in sentence is not a name)
    name.textContent = d ? (d.username ?? 'Your Discord account') : '';
    name.classList.toggle('is-generic', d !== null && d.username === null);
    name.classList.toggle('is-long', (d?.username?.length ?? 0) > 16);
    if (d) setOpen(false);
  }

  function open() {
    if (box.dataset.state !== 'unlinked') return;
    box.scrollIntoView({ block: 'nearest', behavior: window.matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' });
    setOpen(true);
  }

  openBtn.addEventListener('click', () => setOpen(true));
  cancel.addEventListener('click', () => {
    setOpen(false);
    // back to whichever opener is on screen (display:none ones have no boxes)
    openers().find((b) => b.getClientRects().length > 0)?.focus();
  });

  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    if (go.disabled) return;
    clearProblems(form);
    say('', false);
    setBusy(go, true, 'Checking…');
    const out = await startLink(api, store, tabStorage(), password.value);
    if (out.kind === 'signed-out') return onSignedOut();
    if (out.kind === 'redirect') {
      // the tab leaves for Discord's own consent page and comes back to this page
      need<HTMLElement>('.js-label', go).textContent = 'Opening Discord…';
      location.assign(out.url);
      return;
    }
    setBusy(go, false, '');
    if (out.kind === 'invalid') return showProblems(form, out.problems);
    showProblems(form, [out.problem]);
  });

  async function finish(back: { flowId: string; claim: string }) {
    finishing = true;
    box.dataset.state = 'finishing';
    linked.hidden = true;
    unlinked.hidden = true;
    say('Finishing the link with Discord…', false);
    const out = await finishLink(api, store, tabStorage(), back);
    finishing = false;
    if (out.kind === 'signed-out') return onSignedOut();
    if (out.kind === 'error') {
      box.dataset.state = 'unlinked';
      unlinked.hidden = false;
      say(out.problem.text, true);
      await onLinked(); // redraw from the platform: the account may be linked after all
      return;
    }
    await onLinked();
    say('Discord is linked.', false, true);
  }

  return { render, open, finish };
}
