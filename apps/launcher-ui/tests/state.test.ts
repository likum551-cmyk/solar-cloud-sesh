import { describe, expect, it } from 'vitest';
import type { Account, OwnedItem, Product } from '../src/lib/schemas';
import { authReducer, initialAuth, isLocked, signedOut, type AuthState } from '../src/state/auth';
import { initialLaunch, isBusy, launchReducer, stageLabel, STAGES, type LaunchState } from '../src/state/launch';
import { formatMoney, showroomModelFor, spentByCurrency, stampFor, vipWindow } from '../src/state/shop';
import { renderActive } from '../src/ui/hooks';
import { createQueryServersSync, MAX_QUERY_TARGETS, mergeServerStatus, overlayNativeStatus, queryTargets, type NativeServerStatus } from '../src/state/serverStatus';
import type { Server } from '../src/lib/schemas';

const account: Account = {
  id: 'a1', username: 'nightshift', email: 'n@example.com', emailVerified: true, roles: ['player'],
  hasPassword: true, discord: null, ban: null, createdAt: '2026-01-02T10:00:00.000Z',
};
const T0 = Date.parse('2026-10-29T12:00:00.000Z');
const DAY = 86_400_000;

function out(s: AuthState) {
  if (s.status !== 'signedOut') throw new Error(`expected signedOut, got ${s.status}`);
  return s;
}

describe('auth reducer', () => {
  it('boots to the login frame or straight into a session', () => {
    expect(authReducer(initialAuth, { type: 'BOOT_NONE' })).toEqual(signedOut());
    expect(authReducer(initialAuth, { type: 'SIGNED_IN', account, token: 't' })).toEqual({ status: 'signedIn', account, token: 't' });
    const signedIn: AuthState = { status: 'signedIn', account, token: 't' };
    expect(authReducer(signedIn, { type: 'BOOT_NONE' })).toBe(signedIn);
  });

  it('a submit while busy is ignored, a failure clears busy and shows the error', () => {
    let s = authReducer(signedOut(), { type: 'SUBMIT', now: T0 });
    expect(out(s).busy).toBe(true);
    expect(authReducer(s, { type: 'SUBMIT', now: T0 })).toBe(s);
    s = authReducer(s, { type: 'FAIL', error: 'That email or username and password do not match.', now: T0 });
    expect(out(s)).toMatchObject({ busy: false, error: 'That email or username and password do not match.', lockedUntil: null });
  });

  it('a 429 locks login until Retry-After, and the lock survives a view change', () => {
    let s = authReducer(signedOut(), { type: 'SUBMIT', now: T0 });
    s = authReducer(s, { type: 'FAIL', error: 'Too many attempts.', retryAfterSeconds: 540, now: T0 });
    expect(out(s).lockedUntil).toBe(T0 + 540_000);
    expect(isLocked(s, T0 + 539_999)).toBe(true);
    expect(isLocked(s, T0 + 540_000)).toBe(false);
    expect(authReducer(s, { type: 'SUBMIT', now: T0 + 1000 })).toBe(s);
    s = authReducer(s, { type: 'SHOW', view: 'forgot' });
    s = authReducer(s, { type: 'SHOW', view: 'login' });
    expect(isLocked(s, T0 + 2000)).toBe(true);
    expect(out(authReducer(s, { type: 'SUBMIT', now: T0 + 541_000 })).busy).toBe(true);
  });

  it('the lock only gates login: forgot password still submits', () => {
    let s = authReducer(signedOut(), { type: 'FAIL', error: 'x', retryAfterSeconds: 60, now: T0 });
    s = authReducer(s, { type: 'SHOW', view: 'forgot' });
    expect(out(authReducer(s, { type: 'SUBMIT', now: T0 })).busy).toBe(true);
  });

  it('forgot -> sent carries the email, reset -> done', () => {
    expect(out(authReducer(signedOut('forgot'), { type: 'FORGOT_SENT', email: 'a@b.c' }))).toMatchObject({ view: 'forgotSent', email: 'a@b.c' });
    expect(out(authReducer(signedOut('reset'), { type: 'RESET_DONE' })).view).toBe('resetDone');
  });

  it('discord: start, needs username keeps the flow, leaving the wait drops it', () => {
    let s = authReducer(signedOut(), { type: 'DISCORD_STARTED', flowId: 'f1', pollSecret: 'p'.repeat(16) });
    expect(out(s)).toMatchObject({ view: 'discord', discord: { flowId: 'f1' } });
    s = authReducer(s, { type: 'DISCORD_NEEDS_USERNAME', suggestedUsername: 'night' });
    expect(out(s)).toMatchObject({ view: 'discordUsername', discord: { flowId: 'f1', suggestedUsername: 'night' } });
    expect(out(authReducer(s, { type: 'SHOW', view: 'login' })).discord).toBeNull();
    expect(authReducer(signedOut(), { type: 'DISCORD_NEEDS_USERNAME', suggestedUsername: 'x' })).toEqual(signedOut());
  });

  it('signing out carries the reason once; the next view change clears it', () => {
    const s = authReducer({ status: 'signedIn', account, token: 't' }, { type: 'SIGNED_OUT', reason: 'Your session ended. Log in again.' });
    expect(out(s).reason).toBe('Your session ended. Log in again.');
    expect(out(authReducer(s, { type: 'SHOW', view: 'forgot' })).reason).toBeNull();
  });

  it('ACCOUNT refreshes only a signed-in state', () => {
    const verified = { ...account, emailVerified: false };
    expect(authReducer({ status: 'signedIn', account, token: 't' }, { type: 'ACCOUNT', account: verified })).toMatchObject({ account: verified });
    expect(authReducer(signedOut(), { type: 'ACCOUNT', account })).toEqual(signedOut());
  });

  it('NOTICE (an email link handled) returns to login with the line and keeps the lockout', () => {
    let s = authReducer(signedOut('reset'), { type: 'FAIL', error: 'x', retryAfterSeconds: 60, now: T0 });
    s = authReducer(s, { type: 'NOTICE', reason: 'Email verified. Log in to play.' });
    expect(out(s)).toMatchObject({ view: 'login', error: null, reason: 'Email verified. Log in to play.', lockedUntil: T0 + 60_000 });
    const signedIn: AuthState = { status: 'signedIn', account, token: 't' };
    expect(authReducer(signedIn, { type: 'NOTICE', reason: 'x' })).toBe(signedIn);
  });
});

describe('launch reducer', () => {
  const play = (s: LaunchState = initialLaunch) => launchReducer(s, { type: 'PLAY', serverId: 's1', serverName: 'Downtown Roleplay' });

  it('PLAY is ignored while a launch is busy', () => {
    const joining = play();
    expect(isBusy(joining)).toBe(true);
    expect(play(joining)).toBe(joining);
    const launching = launchReducer(joining, { type: 'JOINED' });
    expect(isBusy(launching)).toBe(true);
    expect(play(launching)).toBe(launching);
  });

  it('a join refusal fails before native launch and can ask for email verification', () => {
    const s = launchReducer(play(), { type: 'JOIN_FAILED', title: 'Verify your email', body: 'x', action: 'verifyEmail' });
    expect(s).toMatchObject({ phase: 'failed', action: 'verifyEmail' });
    expect(isBusy(s)).toBe(false);
  });

  it('stage and status only apply while launching', () => {
    expect(launchReducer(play(), { type: 'STAGE', stage: 'injecting' })).toEqual(play());
    const s = launchReducer(launchReducer(play(), { type: 'JOINED' }), { type: 'STAGE', stage: 'waiting_for_game' });
    expect(s).toMatchObject({ phase: 'launching', stage: 'waiting_for_game' });
    expect(launchReducer(s, { type: 'STATUS', text: 'Waiting' })).toMatchObject({ statusText: 'Waiting' });
  });

  it('game exit after launch returns to idle; dismiss closes launched or failed only', () => {
    const launched = launchReducer(launchReducer(play(), { type: 'JOINED' }), { type: 'RESULT', ok: true, code: 0, error: '' });
    expect(launched).toMatchObject({ phase: 'launched', gameRunning: true });
    expect(launchReducer(launched, { type: 'GAME', running: false })).toEqual(initialLaunch);
    expect(launchReducer(launched, { type: 'DISMISS' })).toEqual(initialLaunch);
    const joining = play();
    expect(launchReducer(joining, { type: 'DISMISS' })).toBe(joining);
  });

  it('a late RESULT after dismiss is ignored', () => {
    expect(launchReducer(initialLaunch, { type: 'RESULT', ok: true, code: 0, error: '' })).toBe(initialLaunch);
  });

  it('every stage has a label', () => {
    for (const st of STAGES) expect(stageLabel(st)).not.toBe('');
    expect(stageLabel(null)).toBe('Handing over to the launcher');
  });
});

describe('order stamps', () => {
  const at = (status: string, expiresInMs = DAY) => ({ status, expiresAt: new Date(T0 + expiresInMs).toISOString() });

  it('maps every known status', () => {
    expect(stampFor(at('pending'), T0)).toEqual({ label: 'AWAITING PAYMENT', tone: 'pending' });
    expect(stampFor(at('pending', -1), T0)).toEqual({ label: 'EXPIRED', tone: 'void' });
    expect(stampFor(at('paid'), T0)).toEqual({ label: 'PAID', tone: 'stamp' });
    expect(stampFor(at('fulfilled'), T0)).toEqual({ label: 'DELIVERED', tone: 'stamp' });
    expect(stampFor(at('refunded'), T0)).toEqual({ label: 'REFUNDED', tone: 'plain' });
    expect(stampFor(at('failed'), T0)).toEqual({ label: 'FAILED', tone: 'void' });
    expect(stampFor(at('chargeback'), T0)).toEqual({ label: 'REVERSED', tone: 'void' });
  });

  it('shows an unknown future status verbatim, pencilled', () => {
    expect(stampFor(at('partially_refunded'), T0)).toEqual({ label: 'PARTIALLY_REFUNDED', tone: 'pending' });
  });

  it('only a pending order expires', () => {
    expect(stampFor(at('paid', -DAY), T0).label).toBe('PAID');
  });
});

describe('money', () => {
  it('formats minor units by the currency exponent', () => {
    expect(formatMoney(1999, 'EUR')).toBe('€19.99');
    expect(formatMoney(500, 'JPY')).toBe('¥500');
    expect(formatMoney(0, 'USD')).toBe('$0.00');
  });

  it('does not throw on a code Intl rejects', () => {
    expect(formatMoney(1234, '€€€')).toBe('12.34 €€€');
  });

  it('counts only paid and delivered orders as spent, per currency', () => {
    const o = (status: string, totalMinor: number, currency = 'EUR') => ({ status, totalMinor, currency });
    expect(spentByCurrency([o('fulfilled', 1999), o('paid', 799), o('refunded', 499), o('pending', 999), o('failed', 299), o('chargeback', 100), o('fulfilled', 500, 'USD')]))
      .toEqual([{ currency: 'EUR', totalMinor: 2798 }, { currency: 'USD', totalMinor: 500 }]);
    expect(spentByCurrency([o('refunded', 499), o('pending', 999)])).toEqual([]);
  });
});

describe('VIP window', () => {
  const vip = (createdAt: number, durationDays: number | undefined, quantity = 1, type = 'entitlement'): OwnedItem => ({
    id: `i${createdAt}`, sku: 'vip.30d', type, quantity, orderId: null, delivered: true,
    payload: durationDays === undefined ? { key: 'vip' } : { key: 'vip', durationDays }, createdAt: new Date(createdAt).toISOString(),
  });

  it('none without grants, or once the last one ran out', () => {
    expect(vipWindow([], T0)).toEqual({ kind: 'none' });
    expect(vipWindow([vip(T0 - 40 * DAY, 30)], T0)).toEqual({ kind: 'none' });
  });

  it('a grant without a duration is permanent', () => {
    expect(vipWindow([vip(T0 - DAY, 30), vip(T0, undefined)], T0)).toEqual({ kind: 'permanent' });
  });

  it('stacks back-to-back grants and multiplies by quantity', () => {
    const w = vipWindow([vip(T0 - 10 * DAY, 30), vip(T0 - 5 * DAY, 30, 2)], T0);
    expect(w).toEqual({ kind: 'timed', startedAt: T0 - 10 * DAY, endsAt: T0 + 80 * DAY, remainingMs: 80 * DAY, totalMs: 90 * DAY });
  });

  it('starts a new window after a gap', () => {
    const w = vipWindow([vip(T0 - 100 * DAY, 30), vip(T0 - 2 * DAY, 30)], T0);
    expect(w).toMatchObject({ kind: 'timed', startedAt: T0 - 2 * DAY, endsAt: T0 + 28 * DAY });
  });

  it('ignores items that are not the vip entitlement', () => {
    expect(vipWindow([vip(T0, 30, 1, 'item')], T0)).toEqual({ kind: 'none' });
  });
});

describe('showroom subject', () => {
  const p = (kind: string, grants: Product['grants']) => ({ kind, grants });
  it('maps by item key prefix, then by kind', () => {
    expect(showroomModelFor(p('cosmetic', [{ type: 'item', key: 'vehicle.vanta-gt' }]))).toBe('car');
    expect(showroomModelFor(p('cosmetic', [{ type: 'item', key: 'outfit.field-jacket' }]))).toBe('outfit');
    expect(showroomModelFor(p('cosmetic', [{ type: 'item', key: 'cosmetic.steel-digital' }]))).toBe('watch');
    expect(showroomModelFor(p('vip', [{ type: 'entitlement', key: 'vip', durationDays: 30 }]))).toBe('emblem');
    expect(showroomModelFor(p('currency_pack', [{ type: 'wallet_credit', currency: 'coins', amount: 5000 }]))).toBe('coin');
  });
});

describe('render-pause policy', () => {
  const base = { visible: true, focused: true, idle: false, launchBusy: false, gameRunning: false, launchedOverlay: false };
  it('renders only when visible, attended and not handing the GPU to the game', () => {
    expect(renderActive(base)).toBe(true);
    expect(renderActive({ ...base, visible: false })).toBe(false);
    expect(renderActive({ ...base, focused: false })).toBe(true);
    expect(renderActive({ ...base, focused: false, idle: true })).toBe(false);
    expect(renderActive({ ...base, idle: true })).toBe(true);
    expect(renderActive({ ...base, launchBusy: true })).toBe(false);
    expect(renderActive({ ...base, gameRunning: true })).toBe(false);
    expect(renderActive({ ...base, launchedOverlay: true })).toBe(false);
  });
});

describe('server status overlay, demo mode (real native UDP query over demo counts; unchanged by 0.5.6)', () => {
  // The fabricated demo status the mock gives the live server.
  const demo: Server['status'] = { state: 'online', players: 128, maxPlayers: 300, pingMs: 24, protocol: 3, checkedAt: '2026-10-29T12:00:00.000Z' };
  const base: NativeServerStatus = { host: '5.175.253.187', port: 24800, online: true, pingMs: 42, players: 1, maxPlayers: 8, protocol: 3 };

  it('replaces the demo counts with the measured ones and marks them live (chip hides)', () => {
    const s = overlayNativeStatus('demo', demo, base);
    expect(s).toMatchObject({ state: 'online', pingMs: 42, players: 1, maxPlayers: 8, protocol: 3, live: true });
    expect(demo.live).toBeUndefined(); // the demo status is still demo until overlaid
  });

  it('an empty server (0 players) is a real count, not "unknown"', () => {
    const s = overlayNativeStatus('demo', demo, { ...base, players: 0, maxPlayers: 8 });
    expect(s.players).toBe(0);
    expect(s.live).toBe(true);
  });

  it('an offline result clears the counts and goes offline, still marked measured', () => {
    const s = overlayNativeStatus('demo', demo, { ...base, online: false, pingMs: -1, players: -1, maxPlayers: -1 });
    expect(s).toMatchObject({ state: 'offline', pingMs: null, players: null, maxPlayers: null, live: true });
  });

  it('a field the server did not report (-1) keeps the prior value', () => {
    const s = overlayNativeStatus('demo', demo, { ...base, players: -1, pingMs: -1 });
    expect(s.players).toBe(128); // kept from prev
    expect(s.pingMs).toBeNull(); // online but ping not measured -> null, not a stale demo 24
  });

  it('(d) with no native measurement the demo status (and its demo ping) is shown as it was', () => {
    expect(overlayNativeStatus('demo', demo, undefined)).toBe(demo);
    const list = [server('sol', '5.175.253.187', 24800, demo)];
    expect(mergeServerStatus('demo', list, {})[0]!.status).toEqual(demo);
    expect(mergeServerStatus('demo', list, { '5.175.253.187:24800': base })[0]!.status).toMatchObject({ pingMs: 42, players: 1, maxPlayers: 8, live: true });
  });
});

/** A listed server with the given status (the rest of the shape does not matter here). */
function server(id: string, host: string, port: number, status: Server['status']): Server {
  return { id, name: id.toUpperCase(), region: 'EU', host, port, tags: [], sort: 0, maintenance: { enabled: false, message: null }, comingSoon: false, status };
}

describe('server status, platform mode (0.5.6: the ping is the player\'s own, never the API\'s)', () => {
  // What GET /api/v1/servers returns live: the platform pings the game server from the same VPS.
  const api: Server['status'] = { state: 'online', players: 5, maxPlayers: 1024, pingMs: 3, protocol: 8, checkedAt: '2026-11-07T10:00:00.000Z', live: true };
  const sol = server('sol', '5.175.253.187', 24800, api);
  const luna = server('luna', 'luna.solar-rp.com', 24801, { ...api, pingMs: 0 });
  const soon = server('terra', '', 0, { state: 'unknown', players: null, maxPlayers: null, pingMs: 7, protocol: null, checkedAt: null });
  const measured: NativeServerStatus = { host: '5.175.253.187', port: 24800, online: true, pingMs: 61, players: 4, maxPlayers: 300, protocol: 8 };

  it('(a) the API pingMs is never shown: null until the launcher measured it, for every server', () => {
    const out = mergeServerStatus('platform', [sol, luna, soon], {});
    expect(out.map((s) => s.status.pingMs)).toEqual([null, null, null]);
    // A measurement for one server does not leak the API ping into the others.
    const one = mergeServerStatus('platform', [sol, luna], { '5.175.253.187:24800': measured });
    expect(one.map((s) => s.status.pingMs)).toEqual([61, null]);
    expect(overlayNativeStatus('platform', api, undefined).pingMs).toBeNull();
  });

  it('(b) a native result sets the ping; state, players and maxPlayers stay the API\'s', () => {
    const s = overlayNativeStatus('platform', api, measured);
    expect(s).toMatchObject({ state: 'online', pingMs: 61, players: 5, maxPlayers: 1024, protocol: 8 });
    const out = mergeServerStatus('platform', [sol], { '5.175.253.187:24800': measured });
    expect(out[0]!.status).toMatchObject({ state: 'online', pingMs: 61, players: 5, maxPlayers: 1024 });
    // An answer with no round trip (-1) is no ping, not a stale API one.
    expect(overlayNativeStatus('platform', api, { ...measured, pingMs: -1 }).pingMs).toBeNull();
  });

  it('(c) a failed native query keeps the API\'s state and counts and only nulls the ping', () => {
    const failed: NativeServerStatus = { ...measured, online: false, pingMs: -1, players: -1, maxPlayers: -1, protocol: -1 };
    const s = overlayNativeStatus('platform', api, failed);
    expect(s).toEqual({ ...api, pingMs: null });
    const offlineApi: Server['status'] = { ...api, state: 'offline', players: null, maxPlayers: null };
    expect(overlayNativeStatus('platform', offlineApi, measured)).toEqual({ ...offlineApi, pingMs: 61 }); // the API's offline stands too
  });
});

describe('queryServers (0.5.6: which servers the launcher pings, sent only when the set changes)', () => {
  function sink() {
    const sent: Array<{ type: 'queryServers'; servers: Array<{ host: string; port: number }> }> = [];
    return { sent, sync: createQueryServersSync((m) => sent.push(m)) };
  }
  const api: Server['status'] = { state: 'online', players: 5, maxPlayers: 1024, pingMs: 3, protocol: 8, checkedAt: null };

  it('(e) sends the list\'s host:port pairs (no host, no send; each pair once)', () => {
    const { sent, sync } = sink();
    sync([server('sol', '5.175.253.187', 24800, api), server('luna', '', 0, api), server('terra', 'terra.solar-rp.com', 24802, api), server('sol2', '5.175.253.187', 24800, api)]);
    expect(sent).toEqual([{ type: 'queryServers', servers: [{ host: '5.175.253.187', port: 24800 }, { host: 'terra.solar-rp.com', port: 24802 }] }]);
  });

  it('(e) re-sends only when the host:port set changes, not on every refetch or reorder', () => {
    const { sent, sync } = sink();
    sync([]); // the list before the first fetch
    expect(sent).toHaveLength(0);
    const a = server('sol', '5.175.253.187', 24800, api);
    const b = server('luna', 'luna.solar-rp.com', 24801, api);
    sync([a]);
    sync([server('sol', '5.175.253.187', 24800, { ...api, players: 9 })]); // a refetch: same server, new objects and counts
    sync([a]);
    expect(sent).toHaveLength(1);
    sync([b, a]);            // LUNA goes live
    sync([a, b]);            // the same set in another order
    expect(sent).toHaveLength(2);
    expect(sent[1]!.servers).toEqual([{ host: '5.175.253.187', port: 24800 }, { host: 'luna.solar-rp.com', port: 24801 }]);
    sync([{ ...b, port: 24811 }, a]); // a port change is a change
    expect(sent).toHaveLength(3);
    sync([]);                // the list emptied: native stops asking
    expect(sent).toHaveLength(4);
    expect(sent[3]!.servers).toEqual([]);
  });

  it('caps the list at 16 and skips a port native would refuse', () => {
    const many = Array.from({ length: 20 }, (_, i) => server(`s${i}`, `s${String(i).padStart(2, '0')}.solar-rp.com`, 24800, api));
    expect(queryTargets(many)).toHaveLength(MAX_QUERY_TARGETS);
    expect(queryTargets([server('x', 'x.com', 0, api), server('y', 'y.com', 70000, api)])).toEqual([]);
  });
});
