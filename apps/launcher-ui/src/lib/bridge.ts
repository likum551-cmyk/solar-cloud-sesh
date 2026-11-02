import { z } from 'zod';

// The launcher UI <-> native host bridge (WebView2 postMessage). The existing contract is
// apps/launcher/BRIDGE.md; the messages marked NEW are specified in NATIVE-INTEGRATION.md and
// are what the native side must add for this UI. Incoming messages are validated; unknown types
// are ignored so an older or newer host never breaks the page.

/** The legacy server object `launch` still carries (BRIDGE.md). */
export interface BridgeServerObj {
  host: string;
  port: number;
  name: string;
  players: number;
  ping: number;
  url: string;
  maxPlayers: number;
  mode: string;
  lang: string;
}

/** Local settings the native host persists (BRIDGE.md `saveSetting`; `launchOptions` is NEW). */
export type SettingKey = 'gamePath' | 'playerName' | 'language' | 'autoUpdate' | 'startMinimized' | 'closeGame' | 'launchOptions';

/** JS -> native. */
export type OutMsg =
  | { type: 'ready' }
  | { type: 'getSettings' }
  | { type: 'saveSetting'; key: SettingKey; value: string | number | boolean }
  | { type: 'browseGamePath' }
  | { type: 'checkUpdate' }
  | { type: 'minimize' }
  | { type: 'close' }
  | { type: 'quit' }
  | { type: 'copyText'; text: string }
  /** BRIDGE.md `launch` plus the NEW `serverId` (platform id) and `launchOptions`. */
  | { type: 'launch'; server: BridgeServerObj; gamePath: string; playerName: string; serverId: string; launchOptions: string }
  /**
   * NEW: the reply to `joinTokenNeeded`. A join token lives 60 s and a cold GTA V start takes
   * minutes, so the token is minted when native is about to inject, never at PLAY.
   * `token` null = no token (`error` says why); native then fails the launch with code 12.
   */
  | { type: 'joinToken'; serverId: string; token: string | null; expiresAt: string | null; error: string }
  /** NEW: DPAPI-backed session token storage. */
  | { type: 'tokenGet' }
  | { type: 'tokenSet'; token: string }
  | { type: 'tokenClear' }
  /** NEW: open an https URL in the default browser (checkout, Discord authorize). */
  | { type: 'openExternal'; url: string }
  /** NEW: maximize / restore (double-click on the drag region already does this natively). */
  | { type: 'toggleMaximize' }
  /** NEW: an uncaught UI error or console.error, written to gage.log by native. */
  | { type: 'log'; level: 'error' | 'warn'; message: string };

/** Where the native updater stands (BRIDGE.md `update`). */
export const UPDATE_STATES = ['idle', 'checking', 'downloading', 'verifying', 'ready', 'installing', 'deferred', 'error'] as const;

const settingsMsg = z.object({
  type: z.literal('settings'),
  gamePath: z.string().catch(''),
  playerName: z.string().catch(''),
  language: z.string().catch('en'),
  autoUpdate: z.boolean().catch(true),
  startMinimized: z.boolean().catch(false),
  closeGame: z.boolean().catch(false),
  version: z.string().catch(''),
  /** NEW */
  launchOptions: z.string().optional().catch(undefined),
  /** NEW: platform API origin; absent on an old host. */
  apiBase: z.string().optional().catch(undefined),
});

export const inMsgSchema = z.discriminatedUnion('type', [
  settingsMsg,
  z.object({ type: z.literal('status'), text: z.string() }),
  z.object({ type: z.literal('launchResult'), ok: z.boolean(), code: z.number().int(), error: z.string().catch('') }),
  z.object({
    type: z.literal('update'),
    current: z.string(),
    latest: z.string(),
    available: z.boolean(),
    error: z.string().catch(''),
    /** NEW (signed auto-update): where the native updater stands. Absent on an old host. */
    state: z.enum(UPDATE_STATES).optional().catch(undefined),
    /** NEW: 0..1 while state is 'downloading'. */
    progress: z.number().min(0).max(1).optional().catch(undefined),
    /** NEW: the pending update is mandatory: PLAY is refused (launchResult code 14) until it is installed. */
    mandatory: z.boolean().optional().catch(undefined),
    /** NEW: release notes from the signed manifest. */
    notes: z.string().optional().catch(undefined),
  }),
  z.object({ type: z.literal('copied'), ok: z.boolean() }),
  // NEW
  z.object({ type: z.literal('token'), token: z.string().nullable() }),
  z.object({ type: z.literal('tokenSaved'), ok: z.boolean() }),
  z.object({ type: z.literal('opened'), ok: z.boolean(), url: z.string().optional() }),
  z.object({ type: z.literal('launchStage'), stage: z.enum(['preflight', 'starting_store', 'waiting_for_game', 'injecting', 'injected']) }),
  z.object({ type: z.literal('game'), running: z.boolean() }),
  z.object({ type: z.literal('window'), maximized: z.boolean(), focused: z.boolean(), visible: z.boolean() }),
  z.object({ type: z.literal('discordCallback'), flowId: z.string().optional() }),
  /** NEW: native is about to inject and needs a fresh join token for this server. */
  z.object({ type: z.literal('joinTokenNeeded'), serverId: z.string().min(1).max(128) }),
  /** NEW: a `gage-mp://` link Windows handed to SolarLauncher.exe (email verification, password reset). */
  z.object({ type: z.literal('deepLink'), url: z.string().max(2048) }),
  /**
   * NEW: real GAGE server status from a native UDP query (phase P2). In demo mode the native
   * host polls the live GAGE server and sends this; the UI overlays it onto the matching server
   * (by host:port), replacing the fabricated demo counts. `pingMs`/`players`/`maxPlayers` are -1
   * when the server did not answer (`online` false).
   */
  z.object({
    type: z.literal('serverStatus'),
    servers: z.array(z.object({
      host: z.string(),
      port: z.number().int(),
      online: z.boolean(),
      pingMs: z.number().int().catch(-1),
      players: z.number().int().catch(-1),
      maxPlayers: z.number().int().catch(-1),
      protocol: z.number().int().optional().catch(undefined),
    })).catch([]),
  }),
]);
export type InMsg = z.infer<typeof inMsgSchema>;
export type SettingsMsg = z.infer<typeof settingsMsg>;
export type UpdateMsg = Extract<InMsg, { type: 'update' }>;

/** True while the native updater is working on something, so the check button shows busy. */
export function updateBusy(m: UpdateMsg): boolean {
  return m.state === 'checking' || m.state === 'downloading' || m.state === 'verifying' || m.state === 'ready' || m.state === 'installing';
}

/**
 * The one-line status the Settings screen shows for an `update` message. The download percentage
 * moves in 10% steps so a screen reader is not read a hundred updates. An old host sends no
 * `state`; it gets the text it always had.
 */
export function updateStatusText(m: UpdateMsg): string {
  if (m.error || m.state === 'error') {
    return `${m.latest ? `Update to ${m.latest} failed` : 'Update check failed'}${m.error ? `: ${m.error}` : '.'}`;
  }
  const required = m.mandatory ? ' This update is required to play.' : '';
  switch (m.state) {
    case 'checking': return 'Checking for updates…';
    case 'downloading': return `Downloading version ${m.latest}: ${Math.floor((m.progress ?? 0) * 10) * 10}%.${required}`;
    case 'verifying': return `Verifying version ${m.latest}.${required}`;
    case 'ready':
    case 'installing': return `Installing version ${m.latest}. Solar Launcher closes and starts again by itself.${required}`;
    case 'deferred': return `Version ${m.latest} is downloaded. It installs when GTA V is closed.${required}`;
    case 'idle': return m.available
      ? `Version ${m.latest} is available. Turn on “Install updates automatically” to install it.`
      : `You are on the latest version (${m.current}).`;
    default: return m.available
      ? `Version ${m.latest} is available. It installs on the next start.`
      : `You are on the latest version (${m.current}).`;
  }
}

export interface Bridge {
  /** 'native' inside SolarLauncher.exe, 'mock' in a browser (dev, captures). */
  readonly kind: 'native' | 'mock';
  send(msg: OutMsg): void;
  /** Subscribe to validated incoming messages. Returns the unsubscribe function. */
  on(handler: (msg: InMsg) => void): () => void;
}

interface WebViewLike {
  postMessage(msg: unknown): void;
  addEventListener(type: 'message', cb: (e: { data: unknown }) => void): void;
  removeEventListener(type: 'message', cb: (e: { data: unknown }) => void): void;
}

/** The WebView2 transport, when this page runs inside SolarLauncher.exe. */
export function nativeWebView(): WebViewLike | null {
  const w = globalThis as unknown as { chrome?: { webview?: WebViewLike } };
  return w.chrome?.webview ?? null;
}

/** Parse one raw message from native; null when it is not a message this UI understands. */
export function parseInMsg(raw: unknown): InMsg | null {
  const r = inMsgSchema.safeParse(raw);
  return r.success ? r.data : null;
}

/** Bridge over a WebView2-like transport. */
export function createNativeBridge(wv: WebViewLike): Bridge {
  const handlers = new Set<(m: InMsg) => void>();
  const listener = (e: { data: unknown }) => {
    const msg = parseInMsg(e.data);
    if (!msg) return;
    for (const h of handlers) h(msg);
  };
  wv.addEventListener('message', listener);
  return {
    kind: 'native',
    send: (msg) => wv.postMessage(msg),
    on(handler) {
      handlers.add(handler);
      return () => handlers.delete(handler);
    },
  };
}

/**
 * Send `msg` and resolve with the first incoming message `pick` accepts, or null after
 * `timeoutMs` (an older host that does not know the NEW message never answers).
 */
export function request<T>(bridge: Bridge, msg: OutMsg, pick: (m: InMsg) => T | undefined, timeoutMs = 1500): Promise<T | null> {
  return new Promise((resolve) => {
    let done = false;
    const off = bridge.on((m) => {
      const v = pick(m);
      if (v === undefined || done) return;
      done = true;
      off();
      clearTimeout(timer);
      resolve(v);
    });
    const timer = setTimeout(() => {
      if (done) return;
      done = true;
      off();
      resolve(null);
    }, timeoutMs);
    bridge.send(msg);
  });
}

/** Legacy `launch.server` object for a join (counts unknown = -1, as BRIDGE.md defines). */
export function toBridgeServer(s: { name: string; host: string; port: number; players: number | null; maxPlayers: number | null; tags: string[] }): BridgeServerObj {
  return {
    host: s.host,
    port: s.port,
    name: s.name,
    players: s.players ?? -1,
    ping: -1,
    url: '',
    maxPlayers: s.maxPlayers ?? -1,
    mode: s.tags[0] ?? '',
    lang: '',
  };
}

/** Human copy for BRIDGE.md launchResult codes. */
export function launchErrorCopy(code: number, error: string): { title: string; body: string; action?: 'gameFolder' } {
  switch (code) {
    case 1: return { title: 'GTA V not found', body: 'Point Solar Launcher at your GTA V folder, then press PLAY again.', action: 'gameFolder' };
    case 2: return { title: 'The game could not be opened', body: 'Close GTA V if it is stuck, then try again.' };
    case 3: return { title: 'A Solar RP file is missing', body: 'gageinjector.dll is missing. Reinstall Solar RP.' };
    case 4: return { title: 'Solar RP could not load into the game', body: 'Restart GTA V and try again. If it keeps failing, reinstall Solar RP.' };
    case 5: return { title: 'This server address is invalid', body: 'Refresh the server list and pick the server again.' };
    case 6: return { title: 'Settings could not be written', body: 'Windows refused the registry write. Run Solar Launcher as your normal user and try again.' };
    case 7: return { title: 'BattlEye is running', body: 'Solar RP never loads into a BattlEye session. Close GTA V and launch from here.' };
    case 8: return { title: 'Your game store did not answer', body: 'Open Steam, Epic or the Rockstar Games Launcher, then try again.' };
    case 9: return { title: 'GTA V did not start in time', body: 'The game took longer than 3 minutes to start. Try again.' };
    case 11: return { title: 'Solar RP files do not match this build', body: 'One of Solar RP’s own files is missing or changed. Reinstall Solar RP.' };
    case 12: return { title: 'No join ticket', body: `${error ? `${error} ` : 'The Solar RP network did not hand out a join ticket. '}GTA V stays open: press PLAY again to join.` };
    case 13: return { title: 'Demo mode', body: 'These servers are demo data: no Solar RP server is set up yet, so PLAY does not start the game.' };
    case 14: return { title: 'Solar Launcher is updating', body: error || 'A Solar Launcher update is being installed. Try again in a moment.' };
    default: return { title: 'Launch failed', body: error || `The launcher stopped with code ${code}.` };
  }
}
