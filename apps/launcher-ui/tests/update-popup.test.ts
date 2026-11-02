import { afterEach, describe, expect, it, vi } from 'vitest';
import { launchErrorCopy, parseInMsg, SERVER_BEHIND_TEXT, type InMsg, type UpdateMsg } from '../src/lib/bridge';
import { createMockBridge, UPDATE_SCENARIOS } from '../src/lib/bridge-mock';
import {
  deriveUpdateView, initialUpdateGate, keyAction, nextFocus, percentOf, POPUP_TITLE, scrimClickAction, updateGateReducer,
  type UpdateGate, type UpdateGateEvent,
} from '../src/state/updatePopup';

afterEach(() => { vi.useRealTimers(); });

// BRIDGE.md `update`: current, latest, available, error, state, progress, mandatory, notes.
function upd(over: Partial<UpdateMsg> = {}): UpdateMsg {
  return { type: 'update', current: '0.4.3', latest: '0.4.4', available: true, error: '', state: 'downloading', progress: 0.42, mandatory: true, notes: '', ...over };
}
const feed = (...events: UpdateGateEvent[]): UpdateGate => events.reduce(updateGateReducer, initialUpdateGate);
const U = (over: Partial<UpdateMsg> = {}): UpdateGateEvent => ({ type: 'UPDATE', msg: upd(over) });
const REFUSE14: UpdateGateEvent = { type: 'LAUNCH_RESULT', ok: false, code: 14, error: 'Solar RP was updated. Updating the launcher...' };
const REFUSE15: UpdateGateEvent = { type: 'LAUNCH_RESULT', ok: false, code: 15, error: SERVER_BEHIND_TEXT };
const view = (...events: UpdateGateEvent[]) => deriveUpdateView(feed(...events));

describe('percentOf: the host progress 0..1 as whole percents', () => {
  it('floors, is robust against float error, clamps, and is null when unknown', () => {
    expect(percentOf(0)).toBe(0);
    expect(percentOf(0.42)).toBe(42);
    expect(percentOf(0.29)).toBe(29); // 0.29 * 100 = 28.999999999999996
    expect(percentOf(0.57)).toBe(57); // 56.99999999999999
    expect(percentOf(1)).toBe(100);
    expect(percentOf(1.7)).toBe(100);
    expect(percentOf(-0.2)).toBe(0);
    expect(percentOf(undefined)).toBeNull();
    expect(percentOf(Number.NaN)).toBeNull();
  });
});

describe('update popup: code 14 and the update messages', () => {
  it('(a) code 14 with nothing known shows the pending popup, indeterminate, not dismissable', () => {
    const v = view(REFUSE14);
    expect(v).toMatchObject({ open: true, kind: 'pending', mandatory: true, dismissable: false, title: POPUP_TITLE, progress: null, percent: null, valueNow: null });
    expect(v.title).toBe('Updating Solar RP...');
    expect(v.steps).toEqual(['now', 'next', 'next']);
    expect(v.actions).toEqual({ retry: false, close: false });
  });

  it('(a) a check in progress after code 14 keeps it pending', () => {
    expect(view(REFUSE14, U({ state: 'checking', available: false, mandatory: false, latest: '' })).kind).toBe('pending');
  });

  it('(b) downloading shows the percent, a determinate bar and announces in 10% steps', () => {
    const v = view(U({ state: 'downloading', progress: 0.42 }));
    expect(v).toMatchObject({ open: true, kind: 'downloading', percent: 42, progress: 0.42, valueNow: 42, dismissable: false, version: '0.4.4' });
    expect(v.steps).toEqual(['now', 'next', 'next']);
    expect(v.announce).toBe('Downloading version 0.4.4: 40%.');
    expect(view(U({ progress: 0.29 })).percent).toBe(29);
    expect(view(U({ progress: 0.07 })).announce).toBe('Downloading version 0.4.4: 0%.');
    expect(view(U({ progress: 1 })).percent).toBe(100);
  });

  it('(b) downloading without a progress field is indeterminate (a host that predates it, or 0 not yet known)', () => {
    const { progress: _p, ...noProgress } = upd({ state: 'downloading' });
    void _p;
    const v = view({ type: 'UPDATE', msg: noProgress as UpdateMsg });
    expect(v).toMatchObject({ kind: 'downloading', progress: null, percent: null, valueNow: null });
    expect(v.announce).toBe('Downloading version 0.4.4.');
  });

  it('(c) verifying, ready/installing and deferred map to their own steps', () => {
    expect(view(U({ state: 'verifying' }))).toMatchObject({ kind: 'verifying', steps: ['done', 'now', 'next'], percent: null, dismissable: false });
    for (const state of ['ready', 'installing'] as const) {
      expect(view(U({ state }))).toMatchObject({ kind: 'installing', steps: ['done', 'done', 'now'], valueNow: 100, dismissable: false });
    }
    const w = view(U({ state: 'deferred' }));
    expect(w).toMatchObject({ kind: 'waiting', steps: ['done', 'done', 'next'], dismissable: false, title: 'Update ready' });
    expect(w.body).toContain('GTA V');
  });

  it('(d) an updater error with the update pending is a failure with Try again, still not dismissable', () => {
    const v = view(U({ state: 'error', error: 'The download stopped: the update server did not answer.' }));
    expect(v).toMatchObject({ kind: 'failed', mandatory: true, dismissable: false, actions: { retry: true, close: false }, title: 'Update failed' });
    expect(v.detail).toBe('The download stopped: the update server did not answer.');
    // an `error` text alone (state missing, an older host) is a failure too
    expect(view(U({ state: undefined, error: 'verify failed' })).kind).toBe('failed');
  });

  it('(d) Try again marks the popup busy until the host answers, then the answer replaces it', () => {
    const failed = feed(U({ state: 'error', error: 'x' }));
    expect(deriveUpdateView(failed).busy).toBe(false);
    const retrying = updateGateReducer(failed, { type: 'RETRY' });
    expect(deriveUpdateView(retrying).busy).toBe(true);
    expect(deriveUpdateView(updateGateReducer(retrying, { type: 'RETRY_SETTLED' })).busy).toBe(false);
    const answered = updateGateReducer(retrying, U({ state: 'downloading', progress: 0.01 }));
    expect(deriveUpdateView(answered)).toMatchObject({ kind: 'downloading', busy: false });
  });

  it('a long or multi-line host error is cut to one short line', () => {
    const v = view(U({ state: 'error', error: `line one\n${'x'.repeat(400)}` }));
    expect(v.detail).not.toContain('\n');
    expect(v.detail!.length).toBeLessThanOrEqual(160);
  });
});

describe('update popup: mandatory handling', () => {
  it('every available update is mandatory, whatever the mandatory flag says', () => {
    for (const mandatory of [true, false, undefined]) {
      const v = view(U({ mandatory, state: 'downloading' }));
      expect(v).toMatchObject({ open: true, mandatory: true, dismissable: false });
    }
    // available but idle (the host about to start it): pending, mandatory
    expect(view(U({ state: 'idle', mandatory: false }))).toMatchObject({ kind: 'pending', mandatory: true, dismissable: false });
  });

  it('an installer in flight is mandatory even if the message says available:false', () => {
    expect(view(U({ available: false, mandatory: false, state: 'installing' }))).toMatchObject({ kind: 'installing', dismissable: false });
  });

  it('DISMISS does nothing while mandatory (same state object back)', () => {
    for (const events of [[U()], [U({ state: 'verifying' })], [U({ state: 'error', error: 'x' })], [REFUSE14], [U({ state: 'deferred' })], [U({ mandatory: false })]]) {
      const g = feed(...events);
      expect(updateGateReducer(g, { type: 'DISMISS' })).toBe(g);
      expect(deriveUpdateView(updateGateReducer(g, { type: 'DISMISS' })).open).toBe(true);
    }
  });

  it('Esc and a click outside do nothing while mandatory (Esc is swallowed), and dismiss otherwise', () => {
    const mandatory = view(U());
    expect(keyAction('Escape', mandatory)).toBe('swallow');
    expect(scrimClickAction(mandatory)).toBe('none');
    expect(keyAction('Enter', mandatory)).toBe('none');
    const open15 = view(REFUSE15);
    expect(keyAction('Escape', open15)).toBe('dismiss');
    expect(scrimClickAction(open15)).toBe('dismiss');
    expect(keyAction('Escape', view())).toBe('none'); // hidden: nothing to swallow
  });

  it('nextFocus wraps both ways and brings stray focus back in', () => {
    expect(nextFocus(0, 2, false)).toBe(1);
    expect(nextFocus(1, 2, false)).toBe(0);
    expect(nextFocus(0, 2, true)).toBe(1);
    expect(nextFocus(1, 2, true)).toBe(0);
    expect(nextFocus(-1, 3, false)).toBe(0);
    expect(nextFocus(-1, 3, true)).toBe(2);
    expect(nextFocus(0, 0, false)).toBe(-1); // no control: the panel itself holds focus
  });
});

describe('update popup: code 15, the server is behind', () => {
  it('(e) has its own copy, is not an update of this client, and is dismissable', () => {
    const v = view(REFUSE15);
    expect(v).toMatchObject({ open: true, kind: 'serverBehind', mandatory: false, dismissable: true, title: 'Server not updated yet', actions: { retry: false, close: true } });
    expect(v.body).toBe('This server has not been updated yet. Try again in a minute.');
    expect(v.title).not.toBe(POPUP_TITLE);
    expect(v.steps).toBeNull();
    expect(v.progress).toBeNull();
  });

  it('shows the fixed sentence even if the host text differs, and differs from the code 14 copy', () => {
    const v = view({ type: 'LAUNCH_RESULT', ok: false, code: 15, error: 'something else' });
    expect(v.body).toBe(SERVER_BEHIND_TEXT);
    expect(v.body).not.toBe(view(REFUSE14).body);
  });

  it('DISMISS closes it and the spent refusal does not come back with a later update message', () => {
    const g = feed(REFUSE15);
    const after = updateGateReducer(g, { type: 'DISMISS' });
    expect(deriveUpdateView(after).open).toBe(false);
    expect(deriveUpdateView(updateGateReducer(after, U({ state: 'idle', available: false, mandatory: false, latest: '' }))).open).toBe(false);
  });

  it('a pending update outranks code 15 (the update is the cure for this launcher)', () => {
    expect(view(REFUSE15, U()).kind).toBe('downloading');
  });

  it('launchErrorCopy knows code 15 (the launch overlay shows it, not the generic "Launch failed")', () => {
    expect(launchErrorCopy(15, '')).toEqual({ title: 'Server not updated yet', body: 'This server has not been updated yet. Try again in a minute.' });
    expect(launchErrorCopy(15, 'x').title).not.toBe('Launch failed');
  });
});

describe('update popup: code 14 when the check finds nothing', () => {
  it('a finished check without an update resolves pending into a dismissable "No update yet"', () => {
    const v = view(REFUSE14, U({ state: 'idle', available: false, mandatory: false, latest: '' }));
    expect(v).toMatchObject({ open: true, kind: 'noUpdate', mandatory: false, dismissable: true, actions: { retry: false, close: true } });
    expect(deriveUpdateView(updateGateReducer(feed(REFUSE14, U({ state: 'idle', available: false, mandatory: false, latest: '' })), { type: 'DISMISS' })).open).toBe(false);
  });

  it('an answer from before the refusal is forgotten (it says nothing about this refusal)', () => {
    const stale = U({ state: 'idle', available: false, mandatory: false, latest: '' });
    expect(view(stale, REFUSE14).kind).toBe('pending');
    // ...but a pending update survives the refusal
    expect(view(U({ progress: 0.5 }), REFUSE14)).toMatchObject({ kind: 'downloading', percent: 50 });
  });

  it('a failed check with nothing pending is a dismissable failure with Try again', () => {
    const v = view(REFUSE14, U({ state: 'error', available: false, mandatory: false, latest: '', error: 'offline' }));
    expect(v).toMatchObject({ kind: 'failed', mandatory: false, dismissable: true, actions: { retry: true, close: true } });
  });

  it('another launch outcome (success, or a different code) clears the refusal', () => {
    expect(view(REFUSE14, { type: 'LAUNCH_RESULT', ok: true, code: 0, error: '' }).open).toBe(false);
    expect(view(REFUSE15, { type: 'LAUNCH_RESULT', ok: false, code: 7, error: '' }).open).toBe(false);
  });
});

describe('update popup: a background check alone never opens it', () => {
  it('idle, checking and a failed check with nothing pending stay hidden', () => {
    expect(view(U({ state: 'idle', available: false, mandatory: false, latest: '' })).open).toBe(false);
    expect(view(U({ state: 'checking', available: false, mandatory: false, latest: '' })).open).toBe(false);
    expect(view(U({ state: 'error', available: false, mandatory: false, latest: '', error: 'offline' })).open).toBe(false);
    expect(view().open).toBe(false);
    // an old host that sends the first five fields only
    expect(view({ type: 'UPDATE', msg: { type: 'update', current: '0.4.3', latest: '', available: false, error: '' } }).open).toBe(false);
  });

  it('the popup opens when the pending update arrives and goes with the host (no update left)', () => {
    expect(view(U({ state: 'idle', available: false, mandatory: false, latest: '' }), U()).open).toBe(true);
  });
});

describe('update popup: the bridge messages parse as the contract says', () => {
  it('update messages with and without the optional fields, and launchResult 14 / 15', () => {
    const full = parseInMsg({ type: 'update', current: '0.4.3', latest: '0.4.4', available: true, error: '', state: 'downloading', progress: 0.42, mandatory: true, notes: 'n' });
    expect(full).toMatchObject({ type: 'update', state: 'downloading', progress: 0.42, mandatory: true });
    const old = parseInMsg({ type: 'update', current: '0.4.3', latest: '', available: false, error: '' }) as UpdateMsg;
    expect(old.state).toBeUndefined();
    expect(old.progress).toBeUndefined();
    expect(parseInMsg({ type: 'launchResult', ok: false, code: 15, error: SERVER_BEHIND_TEXT })).toMatchObject({ code: 15 });
  });
});

describe('mock bridge update previews (?update=...)', () => {
  /** Collect what the mock host sends in `ms` after the page sends `ready`. */
  async function collect(update: string, ms: number, progress?: number): Promise<InMsg[]> {
    vi.useFakeTimers();
    const b = createMockBridge({ update, updateProgress: progress });
    const got: InMsg[] = [];
    b.on((m) => got.push(m));
    b.send({ type: 'ready' });
    await vi.advanceTimersByTimeAsync(ms);
    return got;
  }
  const updates = (ms: InMsg[]) => ms.filter((m): m is UpdateMsg => m.type === 'update');

  it('every scenario name is listed and unknown names are ignored', async () => {
    expect(UPDATE_SCENARIOS).toContain('server-behind');
    expect((await collect('nope', 2000)).filter((m) => m.type === 'update' || m.type === 'launchResult')).toEqual([]);
  });

  it('downloading holds at the progress given; server-behind and pending refuse with 15 and 14', async () => {
    expect(updates(await collect('downloading', 500, 0.73)).at(-1)).toMatchObject({ state: 'downloading', progress: 0.73, mandatory: true, available: true });
    expect((await collect('server-behind', 500)).find((m) => m.type === 'launchResult')).toMatchObject({ ok: false, code: 15 });
    expect((await collect('pending', 500)).find((m) => m.type === 'launchResult')).toMatchObject({ ok: false, code: 14 });
  });

  it('through the real reducer: each scenario lands in its state', async () => {
    const run = async (name: string, ms: number, progress?: number) => {
      const got = await collect(name, ms, progress);
      let g = initialUpdateGate;
      for (const m of got) {
        if (m.type === 'update') g = updateGateReducer(g, { type: 'UPDATE', msg: m });
        if (m.type === 'launchResult') g = updateGateReducer(g, { type: 'LAUNCH_RESULT', ok: m.ok, code: m.code, error: m.error });
      }
      return deriveUpdateView(g);
    };
    expect(await run('downloading', 500, 0.42)).toMatchObject({ kind: 'downloading', percent: 42 });
    expect(await run('downloading-unknown', 500)).toMatchObject({ kind: 'downloading', progress: null });
    expect((await run('verifying', 500)).kind).toBe('verifying');
    expect((await run('installing', 500)).kind).toBe('installing');
    expect((await run('deferred', 500)).kind).toBe('waiting');
    expect((await run('error', 500)).kind).toBe('failed');
    expect((await run('server-behind', 500)).kind).toBe('serverBehind');
    expect((await run('run', 4000)).kind).toBe('downloading');
    expect((await run('run', 60_000)).kind).toBe('installing');
  });

  it('error: Try again (checkUpdate) restarts the update; pending: the answer to checkUpdate stays "checking"', async () => {
    vi.useFakeTimers();
    const b = createMockBridge({ update: 'error' });
    const got: InMsg[] = [];
    b.on((m) => got.push(m));
    b.send({ type: 'ready' });
    b.send({ type: 'checkUpdate' }); // the boot check comes before the error is shown
    await vi.advanceTimersByTimeAsync(1000);
    expect(updates(got).every((m) => m.state === 'error')).toBe(true);
    got.length = 0;
    b.send({ type: 'checkUpdate' }); // Try again
    await vi.advanceTimersByTimeAsync(2000);
    expect(updates(got).some((m) => m.state === 'downloading')).toBe(true);
  });
});
