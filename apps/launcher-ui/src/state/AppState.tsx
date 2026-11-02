import { createContext, useCallback, useContext, useEffect, useMemo, useReducer, useRef, useState, type ReactNode } from 'react';
import { createApi, type Api } from '../lib/api';
import { createMockBridge } from '../lib/bridge-mock';
import { createNativeBridge, nativeWebView, request, toBridgeServer, type Bridge, type SettingKey, type SettingsMsg } from '../lib/bridge';
import { describeFailure, isFailure } from '../lib/errors';
import { parseAuthLink, type AuthLink } from '../lib/links';
import { isValidInGameName, randomInGameName } from '../lib/names';
import { createMockFetch, scenarioFromQuery } from '../lib/mock-api';
import type { Server } from '../lib/schemas';
import { authReducer, initialAuth, type AuthState, type AuthEvent } from './auth';
import { initialLaunch, isBusy, launchReducer, type LaunchState } from './launch';

// The app's single state owner: bridge, API client, auth, launch, local settings, window state
// and the server list. Screens read it through useApp(); every side effect lives here.

export type Screen = 'play' | 'shop' | 'profile' | 'settings';

export interface WindowState {
  focused: boolean;
  visible: boolean;
  maximized: boolean;
}

export interface ServersState {
  status: 'loading' | 'ok' | 'error';
  servers: Server[];
  error: string | null;
  offline: boolean;
  fetchedAt: number | null;
}

interface AppValue {
  bridge: Bridge;
  api: Api;
  demo: boolean;
  auth: AuthState;
  authDispatch: (e: AuthEvent) => void;
  launch: LaunchState;
  screen: Screen;
  setScreen: (s: Screen) => void;
  settings: SettingsMsg | null;
  saveSetting: (key: SettingKey, value: string | boolean) => void;
  windowState: WindowState;
  gameRunning: boolean;
  servers: ServersState;
  refreshServers: () => void;
  selectedServerId: string | null;
  selectServer: (id: string) => void;
  play: () => void;
  dismissLaunch: () => void;
  signIn: (token: string, account: import('../lib/schemas').Account) => Promise<void>;
  signOut: (reason?: string | null, callApi?: boolean) => Promise<void>;
  retryBoot: () => void;
  openExternal: (url: string) => void;
  /** Redeem an email verification token; resolves with the line to show. */
  verifyEmail: (token: string) => Promise<string>;
  /** A reset token that arrived by deep link, to prefill the reset frame. */
  resetToken: string | null;
  /** One line for the profile from a handled email link; cleared once shown. */
  notice: string | null;
  clearNotice: () => void;
}

const Ctx = createContext<AppValue | null>(null);

/** The app state. Throws outside <AppStateProvider>. */
export function useApp(): AppValue {
  const v = useContext(Ctx);
  if (!v) throw new Error('useApp outside AppStateProvider');
  return v;
}

const DEMO_TOKEN = 'demo-session-token-0123456789abcdef';

/** One server's real status from a native UDP query (bridge `serverStatus`), keyed by host:port. */
export type NativeServerStatus = { host: string; port: number; online: boolean; pingMs: number; players: number; maxPlayers: number; protocol?: number };

/**
 * Overlay a real native measurement onto a server's status, replacing the fabricated demo counts.
 * `live: true` marks the numbers as really measured so the UI drops the "Demo counts" chip. A
 * field the server did not report (-1) keeps its prior value; an offline result clears the counts.
 */
export function overlayNativeStatus(prev: Server['status'], r: NativeServerStatus): Server['status'] {
  if (!r.online) {
    return { ...prev, state: 'offline', pingMs: null, players: null, maxPlayers: null, protocol: null, checkedAt: new Date().toISOString(), detail: 'timeout', live: true };
  }
  return {
    ...prev,
    state: 'online',
    pingMs: r.pingMs >= 0 ? r.pingMs : null,
    players: r.players >= 0 ? r.players : prev.players,
    maxPlayers: r.maxPlayers >= 0 ? r.maxPlayers : prev.maxPlayers,
    protocol: r.protocol ?? prev.protocol,
    checkedAt: new Date().toISOString(),
    live: true,
  };
}

function makeBridge(search: string): Bridge {
  const wv = nativeWebView();
  if (wv) return createNativeBridge(wv);
  const q = new URLSearchParams(search);
  const launch = q.get('launch');
  return createMockBridge({
    storedToken: q.get('signedIn') === '1' ? DEMO_TOKEN : null,
    launchOutcome: launch && /^\d+$/.test(launch) ? Number(launch) : 'ok',
    stageMs: Number(q.get('stageMs') ?? '900') || 900,
    deepLink: q.get('deepLink'),
    // ?game=none: the exe found no GTA V install on `ready`.
    ...(q.get('game') === 'none' ? { gamePath: '' } : {}),
  });
}

export function AppStateProvider({ children }: { children: ReactNode }) {
  const search = typeof location === 'undefined' ? '' : location.search;
  const bridge = useMemo(() => makeBridge(search), [search]);
  // Demo data: in a browser (mock bridge), or in the exe while no platform API is
  // configured (the host navigates to ?demo=1 and keeps the real bridge).
  const demo = bridge.kind === 'mock' || new URLSearchParams(search).get('demo') === '1';
  const tokenRef = useRef<string | null>(null);
  const [settings, setSettings] = useState<SettingsMsg | null>(null);
  const apiBase = demo ? 'mock://platform' : settings?.apiBase || (import.meta.env.VITE_API_BASE as string | undefined) || '';
  const scenarioRef = useRef(scenarioFromQuery(search));
  const api = useMemo(
    () => createApi({ baseUrl: apiBase, getToken: () => tokenRef.current, ...(demo ? { fetchImpl: createMockFetch(() => scenarioRef.current) } : {}) }),
    [apiBase, demo],
  );

  const [auth, authDispatch] = useReducer(authReducer, initialAuth);
  const [launch, launchDispatch] = useReducer(launchReducer, initialLaunch);
  const [screen, setScreen] = useState<Screen>(() => {
    const s = new URLSearchParams(search).get('screen');
    return demo && (s === 'shop' || s === 'profile' || s === 'settings') ? s : 'play';
  });
  const [windowState, setWindowState] = useState<WindowState>({ focused: true, visible: true, maximized: false });
  const [gameRunning, setGameRunning] = useState(false);
  const [servers, setServers] = useState<ServersState>({ status: 'loading', servers: [], error: null, offline: false, fetchedAt: null });
  // Real server status from native UDP queries (demo mode), overlaid onto the list by host:port.
  const [nativeStatus, setNativeStatus] = useState<Record<string, NativeServerStatus>>({});
  const [selectedServerId, setSelectedServerId] = useState<string | null>(null);
  const [bootNonce, setBootNonce] = useState(0);
  const [authLink, setAuthLink] = useState<AuthLink | null>(null);
  const [resetToken, setResetToken] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);

  // --- bridge events -------------------------------------------------------------------------
  useEffect(() => {
    const off = bridge.on((m) => {
      switch (m.type) {
        case 'settings': setSettings(m); break;
        case 'status': launchDispatch({ type: 'STATUS', text: m.text }); break;
        case 'launchStage': launchDispatch({ type: 'STAGE', stage: m.stage }); break;
        case 'launchResult': launchDispatch({ type: 'RESULT', ok: m.ok, code: m.code, error: m.error }); break;
        case 'game': setGameRunning(m.running); launchDispatch({ type: 'GAME', running: m.running }); break;
        case 'window': setWindowState({ focused: m.focused, visible: m.visible, maximized: m.maximized }); break;
        case 'serverStatus': {
          if (m.servers.length === 0) break;
          setNativeStatus((prev) => {
            const next = { ...prev };
            for (const r of m.servers) next[`${r.host}:${r.port}`] = r;
            return next;
          });
          break;
        }
        case 'deepLink': {
          // Only gage-mp:// links; anything else the host forwards is ignored.
          const link = parseAuthLink(m.url, ['gage-mp:']);
          if (link) setAuthLink(link);
          break;
        }
        default: break;
      }
    });
    bridge.send({ type: 'ready' });
    return off;
  }, [bridge]);

  // Browser focus/visibility also feed the window state (WebView2 reports both natively too).
  useEffect(() => {
    const upd = () => setWindowState((w) => ({ ...w, focused: document.hasFocus(), visible: document.visibilityState === 'visible' }));
    window.addEventListener('focus', upd);
    window.addEventListener('blur', upd);
    document.addEventListener('visibilitychange', upd);
    return () => {
      window.removeEventListener('focus', upd);
      window.removeEventListener('blur', upd);
      document.removeEventListener('visibilitychange', upd);
    };
  }, []);

  // --- boot: restore the stored session --------------------------------------------------------
  useEffect(() => {
    if (!demo && !settings) return; // wait for the host's settings (apiBase) before calling the API
    let cancelled = false;
    (async () => {
      const stored = await request(bridge, { type: 'tokenGet' }, (m) => (m.type === 'token' ? m.token : undefined));
      if (cancelled) return;
      if (!stored) { authDispatch({ type: 'BOOT_NONE' }); return; }
      tokenRef.current = stored;
      try {
        const { account } = await api.me();
        if (!cancelled) authDispatch({ type: 'SIGNED_IN', account, token: stored });
      } catch (e) {
        if (cancelled) return;
        if (isFailure(e, 'unauthorized')) {
          tokenRef.current = null;
          bridge.send({ type: 'tokenClear' });
          authDispatch({ type: 'SIGNED_OUT', reason: 'Your session ended. Log in again.' });
        } else {
          authDispatch({ type: 'BOOT_NONE' });
          authDispatch({ type: 'FAIL', error: describeFailure(e), now: Date.now() });
        }
      }
    })();
    return () => { cancelled = true; };
  }, [api, bridge, demo, settings === null, bootNonce]); // eslint-disable-line react-hooks/exhaustive-deps

  const signIn = useCallback(async (token: string, account: import('../lib/schemas').Account) => {
    tokenRef.current = token;
    bridge.send({ type: 'tokenSet', token });
    authDispatch({ type: 'SIGNED_IN', account, token });
  }, [bridge]);

  const signOut = useCallback(async (reason: string | null = null, callApi = true) => {
    if (callApi && tokenRef.current) {
      try { await api.logout(); } catch { /* the token is dropped locally either way; a dead session needs no revoke */ }
    }
    tokenRef.current = null;
    bridge.send({ type: 'tokenClear' });
    authDispatch({ type: 'SIGNED_OUT', reason });
    setScreen('play');
  }, [api, bridge]);

  // --- servers ---------------------------------------------------------------------------------
  const fetchServers = useCallback(async () => {
    try {
      const { servers: list } = await api.servers();
      const sorted = [...list].sort((a, b) => a.sort - b.sort || a.name.localeCompare(b.name));
      setServers({ status: 'ok', servers: sorted, error: null, offline: false, fetchedAt: Date.now() });
    } catch (e) {
      setServers((s) => ({ ...s, status: 'error', error: describeFailure(e), offline: isFailure(e, 'offline', 'timeout', 'not_configured') }));
    }
  }, [api]);

  const signedIn = auth.status === 'signedIn';
  const pollServers = signedIn && windowState.visible && !gameRunning && !isBusy(launch);
  useEffect(() => {
    if (!pollServers) return;
    void fetchServers();
    const t = setInterval(() => void fetchServers(), 15_000);
    return () => clearInterval(t);
  }, [pollServers, fetchServers]);

  // Keep a valid selection: the first online, non-maintenance server.
  useEffect(() => {
    if (servers.servers.length === 0) return;
    if (selectedServerId && servers.servers.some((s) => s.id === selectedServerId)) return;
    const best = servers.servers.find((s) => s.status.state === 'online' && !s.maintenance.enabled) ?? servers.servers[0]!;
    setSelectedServerId(best.id);
  }, [servers.servers, selectedServerId]);

  // --- launch ----------------------------------------------------------------------------------
  const play = useCallback(() => {
    const s = servers.servers.find((x) => x.id === selectedServerId);
    if (!s || isBusy(launch) || auth.status !== 'signedIn') return;
    launchDispatch({ type: 'PLAY', serverId: s.id, serverName: s.name });
    (async () => {
      try {
        // The gate: the platform refuses here (unverified email, ban, maintenance) before GTA V
        // spends minutes starting. This token is not the one the game uses: native asks for a
        // fresh one at injection (`joinTokenNeeded`), which voids this one.
        const join = await api.join(s.id);
        launchDispatch({ type: 'JOINED' });
        // A real account's username is the in-game name. In demo mode every install signs in as the
        // same demo account, and the server refuses a second player with a name already in use, so
        // the name is this PC's own: the one chosen in Settings, else a random one saved on first play.
        // The demo account's own username does not count as chosen: earlier builds wrote it into every
        // install's settings, so it is the one name all of them share.
        let playerName = auth.account.username;
        if (demo) {
          const chosen = (settings?.playerName ?? '').trim();
          const own = isValidInGameName(chosen) && chosen.toLowerCase() !== auth.account.username.toLowerCase();
          playerName = own ? chosen : randomInGameName();
          if (playerName !== chosen) bridge.send({ type: 'saveSetting', key: 'playerName', value: playerName });
        }
        bridge.send({
          type: 'launch',
          server: toBridgeServer({ name: join.server.name, host: join.server.host, port: join.server.port, players: s.status.players, maxPlayers: s.status.maxPlayers, tags: s.tags }),
          gamePath: settings?.gamePath ?? '',
          playerName,
          serverId: join.server.id,
          launchOptions: settings?.launchOptions ?? '',
        });
      } catch (e) {
        if (isFailure(e, 'unauthorized')) {
          launchDispatch({ type: 'JOIN_FAILED', title: 'Session ended', body: 'Log in again to join.' });
          void signOut('Your session ended. Log in again.', false);
          return;
        }
        const notVerified = isFailure(e, 'refused') && e.code === 'email_not_verified';
        launchDispatch({ type: 'JOIN_FAILED', title: notVerified ? 'Verify your email' : 'Could not join', body: describeFailure(e), ...(notVerified ? { action: 'verifyEmail' as const } : {}) });
      }
    })();
  }, [servers.servers, selectedServerId, launch, auth, api, bridge, settings, signOut, demo]);

  // Native is about to inject: mint the join token now (it lives 60 s; the game took minutes).
  useEffect(() => bridge.on((m) => {
    if (m.type !== 'joinTokenNeeded') return;
    const serverId = m.serverId;
    void (async () => {
      try {
        const join = await api.join(serverId);
        bridge.send({ type: 'joinToken', serverId, token: join.token, expiresAt: join.expiresAt, error: '' });
      } catch (e) {
        bridge.send({ type: 'joinToken', serverId, token: null, expiresAt: null, error: describeFailure(e) });
      }
    })();
  }), [bridge, api]);

  const saveSetting = useCallback((key: SettingKey, value: string | boolean) => {
    bridge.send({ type: 'saveSetting', key, value });
  }, [bridge]);

  // --- email links (deep link or pasted) -------------------------------------------------------
  const verifyEmail = useCallback(async (token: string) => {
    const { account } = await api.verifyEmail(token);
    authDispatch({ type: 'ACCOUNT', account }); // no-op while signed out
    return 'Email verified. Joining and buying are open.';
  }, [api]);

  // A link can arrive before the host's settings (apiBase) or while the stored session is still
  // being restored (Windows started GAGE-MP for it): hold it until both have settled.
  useEffect(() => {
    if (!authLink || auth.status === 'booting' || (!demo && !settings)) return;
    const link = authLink;
    const signedInNow = auth.status === 'signedIn';
    setAuthLink(null);
    if (link.kind === 'resetPassword') {
      if (signedInNow) {
        setScreen('profile');
        setNotice('That was a password reset link. You are logged in, so change your password in Settings instead.');
        return;
      }
      setResetToken(link.token);
      authDispatch({ type: 'SHOW', view: 'reset' });
      return;
    }
    void (async () => {
      try {
        const text = await verifyEmail(link.token);
        if (signedInNow) { setScreen('profile'); setNotice(text); } else authDispatch({ type: 'NOTICE', reason: 'Email verified. Log in to play.' });
      } catch (e) {
        const text = `The verification link did not work: ${describeFailure(e)}`;
        if (signedInNow) { setScreen('profile'); setNotice(text); } else authDispatch({ type: 'NOTICE', reason: text });
      }
    })();
  }, [authLink, auth.status, demo, settings, verifyEmail]);

  const openExternal = useCallback((url: string) => {
    if (!/^https:\/\//i.test(url)) return;
    bridge.send({ type: 'openExternal', url });
  }, [bridge]);

  // The list screens read: the fetched servers with any real native status overlaid by host:port
  // (demo mode). Nothing to overlay (no native host, or a coming-soon server with no host) is a
  // no-op, so platform mode and the browser are unaffected.
  const mergedServers = useMemo(() => {
    if (Object.keys(nativeStatus).length === 0) return servers.servers;
    return servers.servers.map((s) => {
      if (!s.host) return s;
      const r = nativeStatus[`${s.host}:${s.port}`];
      return r ? { ...s, status: overlayNativeStatus(s.status, r) } : s;
    });
  }, [servers.servers, nativeStatus]);
  const mergedServersState = useMemo<ServersState>(() => ({ ...servers, servers: mergedServers }), [servers, mergedServers]);

  const value: AppValue = {
    bridge, api, demo, auth, authDispatch, launch, screen, setScreen, settings, saveSetting, windowState, gameRunning,
    servers: mergedServersState, refreshServers: () => void fetchServers(), selectedServerId, selectServer: setSelectedServerId,
    play, dismissLaunch: () => launchDispatch({ type: 'DISMISS' }), signIn, signOut,
    retryBoot: () => setBootNonce((n) => n + 1), openExternal,
    verifyEmail, resetToken, notice, clearNotice: () => setNotice(null),
  };
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}
