import { CODE_SERVER_BEHIND, CODE_UPDATE_REQUIRED, SERVER_BEHIND_TEXT, type UpdateMsg } from '../lib/bridge';

// The forced-update popup as a pure reducer plus a pure view. Two inputs feed it, both from the native
// host (apps/launcher/BRIDGE.md): the `update` message (state, progress, available, mandatory, error)
// and `launchResult` with code 14 ("an update stands in the way") or 15 ("the server is behind this
// launcher"). The component only draws the `UpdateView` this module derives; every rule about what is
// shown, whether it can be dismissed and what the progress means lives here, with tests.
//
// The rules (BRIDGE.md, version_gate.h):
//  - EVERY available update is mandatory. The host reports `mandatory: true` for all of them, but the
//    UI does not lean on the flag alone: `available`, or an installer already in flight, is mandatory too.
//  - A mandatory popup cannot be dismissed: not by Esc, not by a click outside, not by any button.
//  - Code 14 with nothing known yet shows "pending" (indeterminate) until the host's `update` messages say
//    more; the host started a check, and the UI asks for one too (`checkUpdate` is never rate-limited).
//  - Code 15 is not an update of this client: its own copy, dismissable.
//  - A background check on its own (no refusal, nothing pending) never opens the popup.

export type UpdateKind =
  | 'hidden'
  | 'pending'      // an update is required, nothing known about its progress yet
  | 'downloading'  // percent, or indeterminate when the host sends no progress
  | 'verifying'    // SHA-256 of the finished download
  | 'installing'   // ready / installing: setup starts, the launcher exits and restarts by itself
  | 'waiting'      // deferred: verified installer in hand, GTA V is running with Solar RP loaded
  | 'failed'       // the updater (or the check) reported an error; Try again re-requests the check
  | 'noUpdate'     // code 14 but the check found nothing newer: the server is ahead of what is published
  | 'serverBehind'; // code 15

export type StepState = 'done' | 'now' | 'next';

export interface UpdateView {
  open: boolean;
  kind: UpdateKind;
  /** The update must be installed before PLAY works: the popup cannot be dismissed. */
  mandatory: boolean;
  dismissable: boolean;
  title: string;
  body: string;
  /** The line beside the percent figure. */
  status: string | null;
  /** The host's own error text (failed), cut to one short line. */
  detail: string | null;
  /** The version being installed, when a verified manifest named one. */
  version: string | null;
  chip: { tone: 'plain' | 'void' | 'warn'; label: string } | null;
  /** 0..1 while the download has a known progress; null = indeterminate (or not a download). */
  progress: number | null;
  /** Whole percent 0..100 for the figure; null when progress is unknown or not a download. */
  percent: number | null;
  /** Download, Verify, Install. Null when the popup has no steps (failed, noUpdate, serverBehind). */
  steps: [StepState, StepState, StepState] | null;
  /** aria-valuenow of the progress bar; null = indeterminate. */
  valueNow: number | null;
  /** What a screen reader hears: 10% steps while downloading, so it is not read a hundred updates. */
  announce: string;
  actions: { retry: boolean; close: boolean };
  /** Try again was pressed and the host has not answered yet. */
  busy: boolean;
}

export interface UpdateGate {
  /** The last `update` message that still counts. */
  update: UpdateMsg | null;
  /** The launch refusal that opened (or keeps open) the popup. */
  refusal: { code: 14 | 15; error: string } | null;
  retrying: boolean;
}

export const initialUpdateGate: UpdateGate = { update: null, refusal: null, retrying: false };

export type UpdateGateEvent =
  | { type: 'UPDATE'; msg: UpdateMsg }
  | { type: 'LAUNCH_RESULT'; ok: boolean; code: number; error: string }
  | { type: 'RETRY' }
  | { type: 'RETRY_SETTLED' }
  | { type: 'DISMISS' };

export const POPUP_TITLE = 'Updating Solar RP...';

const IN_FLIGHT: ReadonlySet<string> = new Set(['downloading', 'verifying', 'ready', 'installing', 'deferred']);

/** True when the message says an update is known and must be installed (every update is mandatory). */
export function updatePending(u: UpdateMsg | null): boolean {
  if (!u) return false;
  return u.available || u.mandatory === true || (u.state !== undefined && IN_FLIGHT.has(u.state));
}

/** Whole percent from the host's 0..1 progress; null when it is missing or not a number. */
export function percentOf(progress: number | undefined | null): number | null {
  if (typeof progress !== 'number' || !Number.isFinite(progress)) return null;
  // + 1e-6: 0.29 * 100 is 28.999999999999996, and the host sends whole percents.
  return Math.min(100, Math.max(0, Math.floor(progress * 100 + 1e-6)));
}

function cleanDetail(s: string | undefined): string | null {
  const t = (s ?? '').replace(/\s+/g, ' ').trim();
  if (!t) return null;
  return t.length > 160 ? `${t.slice(0, 159)}…` : t;
}

const HIDDEN: UpdateView = {
  open: false, kind: 'hidden', mandatory: false, dismissable: false, title: '', body: '', status: null, detail: null,
  version: null, chip: null, progress: null, percent: null, steps: null, valueNow: null, announce: '',
  actions: { retry: false, close: false }, busy: false,
};

const ACTIVE_BODY = 'A new version of Solar RP is required to play. Solar Launcher restarts by itself when it is installed.';

function active(kind: 'pending' | 'downloading' | 'verifying' | 'installing' | 'waiting', over: Partial<UpdateView>): UpdateView {
  return { ...HIDDEN, open: true, kind, mandatory: true, dismissable: false, title: POPUP_TITLE, body: ACTIVE_BODY, chip: { tone: 'plain', label: 'Update required' }, ...over };
}

/** What the popup shows for the gate's state. Pure: the component draws exactly this. */
export function deriveUpdateView(g: UpdateGate): UpdateView {
  const u = g.update;
  const r = g.refusal;
  const pending = updatePending(u);
  const version = u && u.latest ? u.latest : null;
  const ver = version ? `version ${version}` : 'the new version';

  // 1. A known pending update drives the popup, whatever launched it.
  if (u && pending) {
    if (u.state === 'error' || u.error) {
      return {
        ...HIDDEN, open: true, kind: 'failed', mandatory: true, dismissable: false, title: 'Update failed',
        body: 'Solar RP needs this update before you can play. Check your connection and try again.',
        detail: cleanDetail(u.error), version, chip: { tone: 'void', label: 'Stopped' },
        announce: 'Update failed. Solar RP needs this update before you can play. Try again.',
        actions: { retry: true, close: false }, busy: g.retrying,
      };
    }
    switch (u.state) {
      case 'downloading': {
        const percent = percentOf(u.progress);
        const tenths = percent === null ? null : Math.floor(percent / 10) * 10;
        return active('downloading', {
          status: `Downloading ${ver}`, version, progress: percent === null ? null : percent / 100, percent,
          steps: ['now', 'next', 'next'], valueNow: percent,
          announce: tenths === null ? `Downloading ${ver}.` : `Downloading ${ver}: ${tenths}%.`,
        });
      }
      case 'verifying':
        return active('verifying', { status: 'Checking the download', version, steps: ['done', 'now', 'next'], valueNow: null, announce: `Checking the download of ${ver}.` });
      case 'ready':
      case 'installing':
        return active('installing', {
          body: 'Solar Launcher is closing and will start again by itself.', status: `Installing ${ver}`, version,
          steps: ['done', 'done', 'now'], valueNow: 100, announce: `Installing ${ver}. Solar Launcher closes and starts again by itself.`,
        });
      case 'deferred':
        return active('waiting', {
          title: 'Update ready', body: `${version ? `Version ${version}` : 'The update'} is downloaded. It installs as soon as GTA V is closed.`,
          status: 'Waiting for GTA V to close', version, steps: ['done', 'done', 'next'], valueNow: 100,
          announce: 'The update is downloaded. It installs as soon as GTA V is closed.',
        });
      default: // checking, idle (about to start), or a host without `state`
        return active('pending', { status: 'Getting the update ready', version, steps: ['now', 'next', 'next'], valueNow: null, announce: 'A required update is being prepared.' });
    }
  }

  // 2. No update is known. Only a launch refusal opens the popup.
  if (!r) return HIDDEN;

  if (r.code === CODE_SERVER_BEHIND) {
    return {
      ...HIDDEN, open: true, kind: 'serverBehind', mandatory: false, dismissable: true, title: 'Server not updated yet', body: SERVER_BEHIND_TEXT,
      chip: { tone: 'warn', label: 'Try again soon' }, announce: `Server not updated yet. ${SERVER_BEHIND_TEXT}`, actions: { retry: false, close: true },
    };
  }

  // code 14, no update known (yet)
  if (!u || u.state === 'checking') {
    return active('pending', { status: 'Getting the update ready', steps: ['now', 'next', 'next'], valueNow: null, announce: 'A required update is being prepared.' });
  }
  if (u.state === 'error' || u.error) {
    return {
      ...HIDDEN, open: true, kind: 'failed', mandatory: false, dismissable: true, title: 'Update check failed',
      body: 'Solar RP could not look for the update this server needs. Check your connection and try again.',
      detail: cleanDetail(u.error), chip: { tone: 'void', label: 'Check failed' },
      announce: 'Update check failed. Check your connection and try again.', actions: { retry: true, close: true }, busy: g.retrying,
    };
  }
  return {
    ...HIDDEN, open: true, kind: 'noUpdate', mandatory: false, dismissable: true, title: 'No update yet',
    body: 'This server asks for a newer version of Solar RP than has been published so far. Try again in a minute.',
    chip: { tone: 'warn', label: 'Try again soon' },
    announce: 'No update yet. This server asks for a newer version than has been published so far. Try again in a minute.',
    actions: { retry: false, close: true },
  };
}

export function updateGateReducer(s: UpdateGate, e: UpdateGateEvent): UpdateGate {
  switch (e.type) {
    case 'UPDATE':
      return { ...s, update: e.msg, retrying: false };
    case 'LAUNCH_RESULT': {
      if (e.ok) return s.refusal ? { ...s, refusal: null } : s;
      if (e.code === CODE_UPDATE_REQUIRED) {
        // An answer from before this refusal (a finished check that found nothing) says nothing about it:
        // forget it unless an update is really pending. The UI asks for a fresh check.
        return { ...s, refusal: { code: 14, error: e.error }, update: updatePending(s.update) ? s.update : null };
      }
      if (e.code === CODE_SERVER_BEHIND) return { ...s, refusal: { code: 15, error: e.error } };
      return s.refusal ? { ...s, refusal: null } : s; // the launch moved on to some other outcome
    }
    case 'RETRY':
      return { ...s, retrying: true };
    case 'RETRY_SETTLED':
      return s.retrying ? { ...s, retrying: false } : s;
    case 'DISMISS':
      // Only what is not mandatory can be dismissed; the refusal it was showing is then spent.
      if (!deriveUpdateView(s).dismissable) return s;
      return { ...s, refusal: null };
  }
}

// ---- input policy: what Esc and a click outside do ----------------------------------------------

/** Escape: dismiss a dismissable popup; swallow it (do nothing, and let nothing behind react) while mandatory. */
export function keyAction(key: string, v: UpdateView): 'dismiss' | 'swallow' | 'none' {
  if (!v.open || key !== 'Escape') return 'none';
  return v.dismissable ? 'dismiss' : 'swallow';
}

/** A click on the scrim outside the panel. */
export function scrimClickAction(v: UpdateView): 'dismiss' | 'none' {
  return v.open && v.dismissable ? 'dismiss' : 'none';
}

/**
 * The next focus index for Tab inside the panel's `count` focusable controls. `index` is the control
 * that has focus, or -1 when focus is outside the panel (it comes back in at the first/last).
 */
export function nextFocus(index: number, count: number, shift: boolean): number {
  if (count <= 0) return -1;
  if (index < 0) return shift ? count - 1 : 0;
  return shift ? (index - 1 + count) % count : (index + 1) % count;
}
