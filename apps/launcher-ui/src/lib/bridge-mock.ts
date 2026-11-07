import type { Bridge, InMsg, OutMsg, SettingsMsg } from './bridge';

// A browser stand-in for the native host, for `npm run dev` and headless captures. It answers
// every message the real host answers (including the NEW ones) with the same shapes and timing
// class, keeps state in memory only (never localStorage), and is never used inside SolarLauncher.exe.

export interface MockBridgeOptions {
  /** Pretend a session token was stored by an earlier run (DPAPI on the real host). */
  storedToken?: string | null;
  /** Launch outcome: 'ok', or a BRIDGE.md launchResult code to fail with. */
  launchOutcome?: 'ok' | number;
  /** Milliseconds between launch stages. */
  stageMs?: number;
  /** API origin the host reports in `settings.apiBase`. */
  apiBase?: string;
  /** Called for every message the page sends (tests, the dev console). */
  onSend?: (msg: OutMsg) => void;
  /** The game folder SolarLauncher.exe auto-detected on `ready` (Rockstar registry, Steam libraries, Epic);
   *  an empty string is the "not found automatically" state. */
  gamePath?: string;
  /** A gage-mp:// link "Windows handed over" at start; sent once, after `settings`. */
  deepLink?: string | null;
  /**
   * Preview the forced-update popup (docs/UPDATE-POPUP-REVIEW.md): after `ready` the host "reports" this
   * scenario. One of UPDATE_SCENARIOS; anything else is ignored.
   */
  update?: string | null;
  /** The download progress (0..1) the fixed `downloading` scenario holds at (default 0.42). */
  updateProgress?: number;
}

export const UPDATE_SCENARIOS = ['pending', 'downloading', 'downloading-unknown', 'verifying', 'installing', 'deferred', 'error', 'run', 'noupdate', 'server-behind'] as const;
export type UpdateScenario = (typeof UPDATE_SCENARIOS)[number];

/** Create the mock bridge. Messages are delivered asynchronously, as WebView2 does. */
export function createMockBridge(opts: MockBridgeOptions = {}): Bridge & { emit(msg: InMsg): void; sent: OutMsg[] } {
  const handlers = new Set<(m: InMsg) => void>();
  const sent: OutMsg[] = [];
  let token: string | null = opts.storedToken ?? null;
  let launching = false;
  /** The server a pending launch waits on a join token for. */
  let awaitingToken: string | null = null;
  const stageMs = opts.stageMs ?? 900;
  const settings: SettingsMsg = {
    type: 'settings',
    gamePath: opts.gamePath ?? 'C:\\Program Files\\Rockstar Games\\Grand Theft Auto V',
    playerName: '',
    language: 'en',
    autoUpdate: true,
    startMinimized: false,
    closeGame: false,
    version: '0.2.0-demo',
    launchOptions: '',
    apiBase: opts.apiBase ?? 'mock://platform',
  };

  const emit = (msg: InMsg, delay = 0) => {
    setTimeout(() => { for (const h of [...handlers]) h(msg); }, delay);
  };

  // --- forced-update popup previews ---------------------------------------------------------
  const scenario = (UPDATE_SCENARIOS as readonly string[]).includes(opts.update ?? '') ? (opts.update as UpdateScenario) : null;
  const heldProgress = Math.min(1, Math.max(0, Number.isFinite(opts.updateProgress) ? (opts.updateProgress as number) : 0.42));
  const newest = '0.4.4';
  let errorShown = false;
  const upd = (over: Partial<Extract<InMsg, { type: 'update' }>>, delay = 0) =>
    emit({ type: 'update', current: settings.version, latest: newest, available: true, error: '', state: 'downloading', mandatory: true, notes: '', ...over }, delay);
  const refuse = (code: number, error: string, delay = 0) => emit({ type: 'launchResult', ok: false, code, error }, delay);
  /** The whole update, as the host walks it: 0..100% in 2% steps, verifying, installing (the real launcher then exits). */
  const runUpdate = () => {
    for (let i = 0; i <= 50; i++) upd({ state: 'downloading', progress: i / 50 }, i * 180);
    upd({ state: 'verifying' }, 51 * 180);
    upd({ state: 'ready' }, 51 * 180 + 1400);
    upd({ state: 'installing' }, 51 * 180 + 2200);
  };
  /** What the host says for the scenario: on `ready`, and again (where the pass stands) on `checkUpdate`. */
  const playUpdateScenario = (fromCheck: boolean) => {
    switch (scenario) {
      case 'pending':
        if (!fromCheck) refuse(14, 'Solar RP was updated. Updating the launcher...', 150);
        else upd({ state: 'checking', available: false, mandatory: false, latest: '' }, 300);
        break;
      case 'downloading': upd({ state: 'downloading', progress: heldProgress }, 150); break;
      case 'downloading-unknown': upd({ state: 'downloading' }, 150); break;
      case 'verifying': upd({ state: 'verifying' }, 150); break;
      case 'installing': upd({ state: 'installing' }, 150); break;
      case 'deferred': upd({ state: 'deferred' }, 150); break;
      case 'error': {
        const fail = () => upd({ state: 'error', error: 'The download stopped: the update server did not answer.' }, 150);
        if (!fromCheck) { fail(); setTimeout(() => { errorShown = true; }, 400); } // `ready` and the boot check come first
        else if (!errorShown) fail();
        else runUpdate(); // Try again: this time it goes through
        break;
      }
      case 'run': if (!fromCheck) runUpdate(); break;
      case 'noupdate':
        if (!fromCheck) refuse(14, 'Solar RP was updated. Updating the launcher...', 150);
        else upd({ state: 'idle', available: false, mandatory: false, latest: '' }, 600);
        break;
      case 'server-behind':
        if (!fromCheck) refuse(15, 'This server has not been updated yet. Try again in a minute.', 150);
        break;
      case null: break;
    }
  };

  function send(msg: OutMsg) {
    sent.push(msg);
    opts.onSend?.(msg);
    switch (msg.type) {
      case 'ready':
        emit({ ...settings });
        playUpdateScenario(false);
        if (opts.deepLink) emit({ type: 'deepLink', url: opts.deepLink }, 50);
        break;
      case 'getSettings':
        emit({ ...settings });
        break;
      case 'saveSetting':
        (settings as unknown as Record<string, unknown>)[msg.key] = typeof msg.value === 'string' ? msg.value.trim() : msg.value;
        emit({ ...settings });
        break;
      case 'browseGamePath':
        settings.gamePath = 'E:\\SteamLibrary\\steamapps\\common\\Grand Theft Auto V';
        emit({ ...settings }, 400);
        break;
      case 'checkUpdate':
        if (scenario) playUpdateScenario(true);
        else emit({ type: 'update', current: settings.version, latest: settings.version, available: false, error: '' }, 600);
        break;
      case 'copyText':
        emit({ type: 'copied', ok: true });
        break;
      case 'tokenGet':
        emit({ type: 'token', token }, 30);
        break;
      case 'tokenSet':
        token = msg.token;
        emit({ type: 'tokenSaved', ok: true });
        break;
      case 'tokenClear':
        token = null;
        emit({ type: 'tokenSaved', ok: true });
        break;
      case 'openExternal':
        emit({ type: 'opened', ok: /^https:\/\//.test(msg.url), url: msg.url });
        break;
      case 'launch': {
        if (launching) break;
        launching = true;
        const outcome = opts.launchOutcome ?? 'ok';
        const steps: Array<[number, InMsg]> = [
          [0, { type: 'launchStage', stage: 'preflight' }],
          [1, { type: 'status', text: 'Checking Solar RP files' }],
          [2, { type: 'launchStage', stage: 'starting_store' }],
          [2, { type: 'status', text: 'Asking Steam to start GTA V' }],
          [3, { type: 'launchStage', stage: 'waiting_for_game' }],
          [3, { type: 'status', text: 'Waiting for GTA V to start' }],
        ];
        if (outcome === 'ok') {
          // The real host asks for the join token right before it injects; the launch waits.
          steps.push([5, { type: 'launchStage', stage: 'injecting' }], [5, { type: 'status', text: 'Loading Solar RP into the game' }]);
          steps.push([5, { type: 'joinTokenNeeded', serverId: msg.serverId }]);
          awaitingToken = msg.serverId;
        } else {
          steps.push([4, { type: 'launchResult', ok: false, code: outcome, error: '' }]);
          setTimeout(() => { launching = false; }, 4 * stageMs);
        }
        for (const [i, m] of steps) emit(m, i * stageMs);
        break;
      }
      case 'joinToken': {
        if (awaitingToken === null || msg.serverId !== awaitingToken) break;
        awaitingToken = null;
        if (msg.token) {
          emit({ type: 'launchStage', stage: 'injected' }, stageMs);
          emit({ type: 'launchResult', ok: true, code: 0, error: '' }, stageMs);
          emit({ type: 'game', running: true }, 2 * stageMs);
        } else {
          emit({ type: 'launchResult', ok: false, code: 12, error: msg.error }, 0);
        }
        setTimeout(() => { launching = false; }, 2 * stageMs);
        break;
      }
      case 'minimize':
      case 'close':
      case 'quit':
      case 'toggleMaximize':
      case 'queryServers': // a browser has no UDP: the demo list keeps its labelled demo status
        break;
    }
  }

  return {
    kind: 'mock',
    send,
    on(handler) {
      handlers.add(handler);
      return () => handlers.delete(handler);
    },
    emit: (m) => emit(m),
    sent,
  };
}
