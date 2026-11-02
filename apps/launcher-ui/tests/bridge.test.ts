import { afterEach, describe, expect, it, vi } from 'vitest';
import { createNativeBridge, launchErrorCopy, parseInMsg, request, toBridgeServer, updateBusy, updateStatusText, type InMsg, type OutMsg } from '../src/lib/bridge';
import { createMockBridge } from '../src/lib/bridge-mock';
import { initialLaunch, launchReducer, type LaunchState } from '../src/state/launch';

afterEach(() => { vi.useRealTimers(); });

/** A WebView2 stand-in: records posts and lets the test deliver messages. */
function fakeWebView() {
  const listeners = new Set<(e: { data: unknown }) => void>();
  const posted: unknown[] = [];
  return {
    posted,
    deliver: (data: unknown) => { for (const l of listeners) l({ data }); },
    postMessage: (m: unknown) => { posted.push(m); },
    addEventListener: (_t: 'message', cb: (e: { data: unknown }) => void) => { listeners.add(cb); },
    removeEventListener: (_t: 'message', cb: (e: { data: unknown }) => void) => { listeners.delete(cb); },
  };
}

describe('parseInMsg', () => {
  it('accepts every message the native host sends', () => {
    const samples: unknown[] = [
      { type: 'settings', gamePath: 'C:\\GTA', playerName: '', language: 'en', autoUpdate: true, startMinimized: false, closeGame: false, version: '0.2.0' },
      { type: 'status', text: 'Waiting for GTA V' },
      { type: 'launchResult', ok: false, code: 1, error: 'not found' },
      { type: 'update', current: '0.2.0', latest: '0.2.1', available: true, error: '' },
      { type: 'update', current: '0.2.0', latest: '0.2.1', available: true, error: '', state: 'downloading', progress: 0.42, mandatory: true, notes: 'Fixes.' },
      { type: 'copied', ok: true },
      { type: 'token', token: null },
      { type: 'token', token: 'abc' },
      { type: 'tokenSaved', ok: true },
      { type: 'opened', ok: true, url: 'https://x.example' },
      { type: 'launchStage', stage: 'injecting' },
      { type: 'game', running: false },
      { type: 'window', maximized: false, focused: true, visible: true },
      { type: 'discordCallback' },
      { type: 'deepLink', url: 'gage-mp://auth/verify-email?token=abcdefghijklmnopqrstuvwxyz012345' },
    ];
    for (const s of samples) expect(parseInMsg(s), JSON.stringify(s)).not.toBeNull();
    expect(samples).toHaveLength(15);
  });

  it('update: keeps the new updater fields, accepts the old host form, drops a state it does not know', () => {
    const full = parseInMsg({ type: 'update', current: '0.3.4', latest: '0.3.5', available: true, error: '', state: 'deferred', progress: 1, mandatory: true, notes: 'n' });
    expect(full).toMatchObject({ type: 'update', state: 'deferred', progress: 1, mandatory: true, notes: 'n' });
    const old = parseInMsg({ type: 'update', current: '0.3.4', latest: '0.3.4', available: false, error: '' });
    expect(old).toMatchObject({ type: 'update', available: false });
    expect(old && old.type === 'update' ? old.state : 'x').toBeUndefined();
    const future = parseInMsg({ type: 'update', current: '0.3.4', latest: '0.3.5', available: true, error: '', state: 'teleporting', progress: 7 });
    expect(future).not.toBeNull();
    expect(future && future.type === 'update' ? [future.state, future.progress] : null).toEqual([undefined, undefined]);
  });

  it('updateStatusText / updateBusy: one line per updater state, the old text for an old host', () => {
    const msg = (o: Record<string, unknown>) => {
      const m = parseInMsg({ type: 'update', current: '0.3.4', latest: '0.3.5', available: true, error: '', ...o });
      if (!m || m.type !== 'update') throw new Error('not an update message');
      return m;
    };
    expect(updateStatusText(msg({ state: 'checking', latest: '', available: false }))).toBe('Checking for updates…');
    expect(updateStatusText(msg({ state: 'downloading', progress: 0.47 }))).toBe('Downloading version 0.3.5: 40%.');
    expect(updateStatusText(msg({ state: 'downloading', progress: 0.47, mandatory: true }))).toBe('Downloading version 0.3.5: 40%. This update is required to play.');
    expect(updateStatusText(msg({ state: 'verifying' }))).toBe('Verifying version 0.3.5.');
    expect(updateStatusText(msg({ state: 'installing' }))).toContain('Installing version 0.3.5');
    expect(updateStatusText(msg({ state: 'deferred' }))).toBe('Version 0.3.5 is downloaded. It installs when GTA V is closed.');
    expect(updateStatusText(msg({ state: 'idle' }))).toContain('Turn on');
    expect(updateStatusText(msg({ state: 'idle', available: false, latest: '0.3.4' }))).toBe('You are on the latest version (0.3.4).');
    expect(updateStatusText(msg({ state: 'error', error: 'sha256 mismatch', latest: '' }))).toBe('Update check failed: sha256 mismatch');
    expect(updateStatusText(msg({ state: 'error', error: 'sha256 mismatch' }))).toBe('Update to 0.3.5 failed: sha256 mismatch');
    expect(updateStatusText(msg({ error: 'timed out', latest: '', available: false }))).toBe('Update check failed: timed out');
    // an old host: no state at all
    expect(updateStatusText(msg({}))).toBe('Version 0.3.5 is available. It installs on the next start.');
    expect(updateStatusText(msg({ available: false, latest: '0.3.4' }))).toBe('You are on the latest version (0.3.4).');
    const busy = (['idle', 'checking', 'downloading', 'verifying', 'ready', 'installing', 'deferred', 'error'] as const).map((s) => updateBusy(msg({ state: s })));
    expect(busy).toEqual([false, true, true, true, true, true, false, false]);
    expect(updateBusy(msg({}))).toBe(false);
  });

  it('refuses an oversized deepLink', () => {
    expect(parseInMsg({ type: 'deepLink', url: `gage-mp://auth/verify-email?token=${'a'.repeat(2100)}` })).toBeNull();
    expect(parseInMsg({ type: 'deepLink' })).toBeNull();
  });

  it('fills settings defaults for an old host and keeps the NEW fields when present', () => {
    const old = parseInMsg({ type: 'settings', gamePath: 'C:\\GTA' });
    expect(old).toMatchObject({ type: 'settings', gamePath: 'C:\\GTA', language: 'en', autoUpdate: true, version: '' });
    expect(old && 'apiBase' in old ? old.apiBase : undefined).toBeUndefined();
    const fresh = parseInMsg({ type: 'settings', gamePath: '', apiBase: 'https://api.example', launchOptions: '-windowed' });
    expect(fresh).toMatchObject({ apiBase: 'https://api.example', launchOptions: '-windowed' });
  });

  it('ignores unknown types and malformed known ones', () => {
    expect(parseInMsg({ type: 'somethingNew', x: 1 })).toBeNull();
    expect(parseInMsg({ type: 'launchStage', stage: 'teleporting' })).toBeNull();
    expect(parseInMsg('settings')).toBeNull();
    expect(parseInMsg(null)).toBeNull();
  });
});

describe('createNativeBridge + request', () => {
  it('posts outgoing messages and delivers only valid incoming ones', () => {
    const wv = fakeWebView();
    const bridge = createNativeBridge(wv);
    const got: InMsg[] = [];
    const off = bridge.on((m) => got.push(m));
    bridge.send({ type: 'minimize' });
    wv.deliver({ type: 'game', running: true });
    wv.deliver({ type: 'bogus' });
    off();
    wv.deliver({ type: 'game', running: false });
    expect(wv.posted).toEqual([{ type: 'minimize' }]);
    expect(got).toEqual([{ type: 'game', running: true }]);
  });

  it('request resolves with the first matching answer', async () => {
    const wv = fakeWebView();
    const bridge = createNativeBridge(wv);
    const p = request(bridge, { type: 'tokenGet' }, (m) => (m.type === 'token' ? m.token : undefined));
    wv.deliver({ type: 'status', text: 'unrelated' });
    wv.deliver({ type: 'token', token: 'stored-token' });
    await expect(p).resolves.toBe('stored-token');
    expect(wv.posted).toEqual([{ type: 'tokenGet' }]);
  });

  it('request resolves null when an older host never answers', async () => {
    vi.useFakeTimers();
    const bridge = createNativeBridge(fakeWebView());
    const p = request(bridge, { type: 'tokenGet' }, (m) => (m.type === 'token' ? m.token : undefined), 1500);
    await vi.advanceTimersByTimeAsync(1499);
    let settled = false;
    void p.then(() => { settled = true; });
    await Promise.resolve();
    expect(settled).toBe(false);
    await vi.advanceTimersByTimeAsync(1);
    await expect(p).resolves.toBeNull();
  });

  it('request resolves a null token (signed out) as null, not as a timeout', async () => {
    const wv = fakeWebView();
    const bridge = createNativeBridge(wv);
    const p = request(bridge, { type: 'tokenGet' }, (m) => (m.type === 'token' ? m.token : undefined));
    wv.deliver({ type: 'token', token: null });
    await expect(p).resolves.toBeNull();
  });
});

describe('legacy helpers', () => {
  it('toBridgeServer fills BRIDGE.md unknowns with -1', () => {
    expect(toBridgeServer({ name: 'Downtown', host: '203.0.113.5', port: 22005, players: null, maxPlayers: 300, tags: ['roleplay'] }))
      .toEqual({ host: '203.0.113.5', port: 22005, name: 'Downtown', players: -1, ping: -1, url: '', maxPlayers: 300, mode: 'roleplay', lang: '' });
  });

  it('launchErrorCopy: code 1 asks for the game folder; unknown codes keep the host error', () => {
    expect(launchErrorCopy(1, '')).toMatchObject({ title: 'GTA V not found', action: 'gameFolder' });
    for (const c of [2, 3, 4, 5, 6, 7, 8, 9, 11, 14]) expect(launchErrorCopy(c, '').action).toBeUndefined();
    expect(launchErrorCopy(14, 'Solar Launcher 0.3.5 is required to play. It is being installed now.')).toMatchObject({ title: 'Solar Launcher is updating', body: 'Solar Launcher 0.3.5 is required to play. It is being installed now.' });
    expect(launchErrorCopy(14, '').body).toContain('update');
    expect(launchErrorCopy(42, 'mutex busy').body).toBe('mutex busy');
    expect(launchErrorCopy(42, '').body).toBe('The launcher stopped with code 42.');
  });
});

describe('mock bridge', () => {
  it('answers ready with settings including the NEW apiBase', async () => {
    const b = createMockBridge({ apiBase: 'mock://platform' });
    const s = await request(b, { type: 'ready' }, (m) => (m.type === 'settings' ? m : undefined));
    expect(s).toMatchObject({ type: 'settings', apiBase: 'mock://platform' });
  });

  it('stores, returns and clears the token in memory only', async () => {
    const b = createMockBridge();
    const get = () => request(b, { type: 'tokenGet' }, (m) => (m.type === 'token' ? m.token : undefined));
    expect(await get()).toBeNull();
    await request(b, { type: 'tokenSet', token: 'session-token-1' }, (m) => (m.type === 'tokenSaved' ? m.ok : undefined));
    expect(await get()).toBe('session-token-1');
    await request(b, { type: 'tokenClear' }, (m) => (m.type === 'tokenSaved' ? m.ok : undefined));
    expect(await get()).toBeNull();
  });

  it('refuses to open a non-https URL', async () => {
    const b = createMockBridge();
    const open = (url: string) => request(b, { type: 'openExternal', url }, (m) => (m.type === 'opened' ? m.ok : undefined));
    expect(await open('https://pay.example.com/x')).toBe(true);
    expect(await open('file:///C:/Windows/System32/calc.exe')).toBe(false);
  });

  it('records what the page sent', () => {
    const seen: OutMsg[] = [];
    const b = createMockBridge({ onSend: (m) => seen.push(m) });
    b.send({ type: 'saveSetting', key: 'closeGame', value: true });
    expect(b.sent).toEqual(seen);
    expect(seen).toEqual([{ type: 'saveSetting', key: 'closeGame', value: true }]);
  });
});

/** Drive the launch reducer from a mock bridge launch, as AppState does. */
async function runLaunch(outcome: 'ok' | number, mint: 'ok' | 'refused' = 'ok'): Promise<{ states: LaunchState[]; final: LaunchState; asked: string[] }> {
  vi.useFakeTimers();
  const b = createMockBridge({ launchOutcome: outcome, stageMs: 100 });
  let s = launchReducer(initialLaunch, { type: 'PLAY', serverId: 's1', serverName: 'Downtown Roleplay' });
  s = launchReducer(s, { type: 'JOINED' });
  const states: LaunchState[] = [s];
  const asked: string[] = [];
  b.on((m) => {
    if (m.type === 'joinTokenNeeded') {
      asked.push(m.serverId);
      b.send(mint === 'ok'
        ? { type: 'joinToken', serverId: m.serverId, token: 'gmj_test', expiresAt: '2026-10-29T10:01:00.000Z', error: '' }
        : { type: 'joinToken', serverId: m.serverId, token: null, expiresAt: null, error: 'This server is in maintenance.' });
      return;
    }
    if (m.type === 'launchStage') s = launchReducer(s, { type: 'STAGE', stage: m.stage });
    else if (m.type === 'status') s = launchReducer(s, { type: 'STATUS', text: m.text });
    else if (m.type === 'launchResult') s = launchReducer(s, { type: 'RESULT', ok: m.ok, code: m.code, error: m.error });
    else if (m.type === 'game') s = launchReducer(s, { type: 'GAME', running: m.running });
    else return;
    states.push(s);
  });
  b.send({ type: 'launch', server: toBridgeServer({ name: 'Downtown Roleplay', host: '203.0.113.5', port: 22005, players: 1, maxPlayers: 300, tags: [] }), gamePath: 'C:\\GTA', playerName: 'nightshift', serverId: 's1', launchOptions: '' });
  await vi.advanceTimersByTimeAsync(1000);
  return { states, final: s, asked };
}

describe('mock bridge launch drives the launch state machine', () => {
  it('ok: asks for the join token at injection, then walks to launched with the game running', async () => {
    const { states, final, asked } = await runLaunch('ok');
    expect(asked).toEqual(['s1']);
    const stages = states.flatMap((x) => (x.phase === 'launching' && x.stage ? [x.stage] : []));
    expect([...new Set(stages)]).toEqual(['preflight', 'starting_store', 'waiting_for_game', 'injecting', 'injected']);
    expect(final).toEqual({ phase: 'launched', serverId: 's1', serverName: 'Downtown Roleplay', gameRunning: true });
  });

  it('no token at injection: fails with code 12 and carries the refusal', async () => {
    const { final, asked } = await runLaunch('ok', 'refused');
    expect(asked).toEqual(['s1']);
    expect(final).toMatchObject({ phase: 'failed', title: 'No join ticket' });
    expect(final.phase === 'failed' && final.body).toContain('This server is in maintenance.');
  });

  it('code 1: stops at failed with the game-folder action and never asks for a token', async () => {
    const { final, asked } = await runLaunch(1);
    expect(asked).toEqual([]);
    expect(final).toMatchObject({ phase: 'failed', title: 'GTA V not found', action: 'gameFolder' });
  });
});
