// Linking Discord from the account page, free of the DOM (apps/platform/docs/api.md, "Discord login and
// linking", handoff "web"). Two secrets, two channels, the platform's own design:
//
//   1. startLink: the player re-enters their password; the platform answers with the Discord URL and a
//      poll secret. The secret is kept in THIS TAB's sessionStorage (never localStorage, never the URL),
//      then the tab goes to Discord.
//   2. Discord sends the browser back through the platform, which redirects to
//      /account/#discord_flow=<id>&discord_claim=<claim>. The claim is in the fragment, so it never
//      reaches a server log; the page reads it once and removes it from the address bar.
//   3. finishLink: flow id + the tab's poll secret + the claim, with this account's session.
//
// Whoever sends their own Discord URL to somebody else gets nothing: the claim lands in the other
// person's browser, which has neither the poll secret nor the sender's session.

import type { AccountApi, ApiFailure } from './api';
import { describeFailure, linkFailedText, type Problem } from './errors';
import type { TokenStore } from './store';

export const PENDING_KEY = 'solar.discordLink';

type KV = Pick<Storage, 'getItem' | 'setItem' | 'removeItem'>;

export interface PendingLink {
  flowId: string;
  pollSecret: string;
  expiresAt: string;
}

const FLOW_ID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const CLAIM = /^[A-Za-z0-9_-]{43}$/;
const SECRET = /^[A-Za-z0-9_-]{16,200}$/;

const isUnauthorized = (f: ApiFailure) => f.kind === 'api' && f.code === 'unauthorized';

/** Only Discord's own consent page is ever opened (https://discord.com/...). */
export function isDiscordAuthorizeUrl(url: string): boolean {
  try {
    const u = new URL(url);
    return u.protocol === 'https:' && u.hostname === 'discord.com' && u.pathname.startsWith('/oauth2/authorize');
  } catch {
    return false;
  }
}

/** The flow and claim the platform put in the account page's fragment, or null when the fragment is not that. */
export function readReturn(hash: string): { flowId: string; claim: string } | null {
  if (!hash.startsWith('#')) return null;
  const p = new URLSearchParams(hash.slice(1));
  const flowId = p.get('discord_flow');
  const claim = p.get('discord_claim');
  if (flowId === null || claim === null || !FLOW_ID.test(flowId) || !CLAIM.test(claim)) return null;
  return { flowId, claim };
}

function loadPending(kv: KV | null): PendingLink | null {
  let raw: string | null = null;
  try {
    raw = kv?.getItem(PENDING_KEY) ?? null;
  } catch {
    return null;
  }
  if (raw === null) return null;
  try {
    const v = JSON.parse(raw) as Partial<PendingLink>;
    if (typeof v.flowId === 'string' && FLOW_ID.test(v.flowId) && typeof v.pollSecret === 'string' && SECRET.test(v.pollSecret) && typeof v.expiresAt === 'string') {
      return { flowId: v.flowId, pollSecret: v.pollSecret, expiresAt: v.expiresAt };
    }
  } catch {
    /* a damaged entry is no entry */
  }
  return null;
}

function dropPending(kv: KV | null): void {
  try {
    kv?.removeItem(PENDING_KEY);
  } catch {
    /* nothing stored where nothing can be stored */
  }
}

export type StartOutcome =
  /** go to Discord: the poll secret is stored */
  | { kind: 'redirect'; url: string }
  | { kind: 'invalid'; problems: Problem[] }
  | { kind: 'error'; problem: Problem }
  | { kind: 'signed-out' };

export const NO_STORAGE_TEXT = 'This browser won’t let the site remember the link while you’re on Discord. Allow site data for solar-rp.com, then try again.';

/** Check the password with the platform and get the Discord URL; keeps the poll secret in `tab` (sessionStorage). */
export async function startLink(api: AccountApi, store: TokenStore, tab: KV | null, password: string): Promise<StartOutcome> {
  const token = store.get();
  if (token === null) return { kind: 'signed-out' };
  if (password.length === 0) return { kind: 'invalid', problems: [{ field: 'password', text: 'Enter your password.' }] };
  const r = await api.discordLinkStart(token, password);
  if (!r.ok) {
    if (isUnauthorized(r.failure)) {
      store.clear();
      return { kind: 'signed-out' };
    }
    const p = describeFailure(r.failure, 'discord');
    return p.field ? { kind: 'invalid', problems: [p] } : { kind: 'error', problem: p };
  }
  if (!isDiscordAuthorizeUrl(r.value.authorizeUrl)) return { kind: 'error', problem: { text: 'Something went wrong on our side. Try again in a moment.' } };
  const pending: PendingLink = { flowId: r.value.flowId, pollSecret: r.value.pollSecret, expiresAt: r.value.expiresAt };
  try {
    if (tab === null) throw new Error('no sessionStorage');
    tab.setItem(PENDING_KEY, JSON.stringify(pending));
    // read it back: some privacy modes accept a write and keep nothing
    if (loadPending(tab)?.flowId !== pending.flowId) throw new Error('sessionStorage kept nothing');
  } catch {
    // without the secret this tab could never finish the link: do not send the player to Discord for nothing
    return { kind: 'error', problem: { text: NO_STORAGE_TEXT } };
  }
  return { kind: 'redirect', url: r.value.authorizeUrl };
}

export type FinishOutcome =
  | { kind: 'linked' }
  | { kind: 'error'; problem: Problem }
  | { kind: 'signed-out' };

/**
 * Finish the link this tab started, with the flow and claim from the fragment. The stored secret is
 * single use: it is removed whatever the answer (a retry starts a new link).
 */
export async function finishLink(api: AccountApi, store: TokenStore, tab: KV | null, back: { flowId: string; claim: string }): Promise<FinishOutcome> {
  const pending = loadPending(tab);
  dropPending(tab);
  if (pending === null || pending.flowId !== back.flowId) {
    return { kind: 'error', problem: { text: 'That Discord link didn’t finish. Link Discord again from this page.' } };
  }
  const token = store.get();
  if (token === null) return { kind: 'signed-out' };
  const r = await api.discordLinkFinish(token, { flowId: back.flowId, pollSecret: pending.pollSecret, claim: back.claim });
  if (!r.ok) {
    if (isUnauthorized(r.failure)) {
      store.clear();
      return { kind: 'signed-out' };
    }
    return { kind: 'error', problem: describeFailure(r.failure, 'discord') };
  }
  if (r.value.status === 'linked') return { kind: 'linked' };
  return { kind: 'error', problem: { text: linkFailedText(r.value.code) } };
}

/** The address without the fragment (what the page puts back in the address bar once it has read the claim). */
export function withoutFragment(href: string): string {
  const i = href.indexOf('#');
  return i === -1 ? href : href.slice(0, i);
}
