import { useEffect, useMemo, useRef, useState } from 'react';
import { getBridge } from './bridge.js';
import { displayName, matchesQuery, modeKey, modeLabel, parseDirectConnect, serverKey } from './lib/format.js';
import { pingStateOf, usePingQueue } from './lib/usePingQueue.js';
import TopBar, { TABS } from './components/TopBar.jsx';
import ServerTable from './components/ServerTable.jsx';
import SidePanel, { failureText } from './components/SidePanel.jsx';
import ServerListCard from './components/ServerListCard.jsx';
import SettingsView from './components/SettingsView.jsx';
import BridgeMissing from './components/BridgeMissing.jsx';
import Splash from './components/Splash.jsx';
import Footer from './components/Footer.jsx';
import { AlertScreen, LaunchScreen } from './components/Overlays.jsx';
import { BACKDROP_ART, nextSplash } from './components/gta.jsx';

// If native goes quiet during a launch (no `status`, no `launchResult`) for
// this long, release the UI. Native reports every few seconds while the
// store brings the game up, which can take minutes.
const LAUNCH_TIMEOUT_MS = 45000;
// the download speed shown is measured over this much of the latest ticks
const RATE_WINDOW_MS = 3000;
// and re-measured this often, ticks or not, so a stall shows as a falling speed
const RATE_EVERY_MS = 500;

/**
 * MiB/s over about the last RATE_WINDOW_MS of `samples` ({t, bytes}, oldest
 * first), or null with under a second of them. Native ticks only while bytes
 * arrive, so after a second without one the window runs to `now`.
 */
function speedOf(samples, now) {
  if (samples.length < 2) return null;
  const last = samples[samples.length - 1];
  const end = now - last.t < 1000 ? last.t : now;
  let base = samples[0];
  for (const s of samples) {
    if (end - s.t < RATE_WINDOW_MS) break;
    base = s;
  }
  const span = (end - base.t) / 1000;
  if (span < 1) return null;
  return Math.max(0, (last.bytes - base.bytes) / 1048576 / span);
}

export default function App() {
  const bridge = getBridge();
  // Production build outside WebView2: block, never fake data (bridge.js).
  if (bridge.missing) return <BridgeMissing />;
  return <Launcher bridge={bridge} />;
}

// keys typed into a field, or pressed on a control that owns them, are not menu keys
function ownsKeys(target) {
  return target instanceof Element && !!target.closest('input, textarea, select, [contenteditable="true"]');
}

function Launcher({ bridge }) {
  // All state is owned by native and arrives via bridge events (BRIDGE.md).
  // The SPA is deliberately stateless with respect to persistence.
  const [settings, setSettings] = useState(null);
  const [serverState, setServerState] = useState({ status: 'loading', servers: [], error: '' });
  const [favorites, setFavorites] = useState([]);
  const [recents, setRecents] = useState([]);
  const [statusText, setStatusText] = useState('');
  // this launch's server-asset download (BRIDGE.md assetProgress), or null
  const [assets, setAssets] = useState(null);
  const [launchResult, setLaunchResult] = useState(null);
  const [launching, setLaunching] = useState(false);
  const [updateInfo, setUpdateInfo] = useState(null);
  const pings = usePingQueue(bridge.send);

  // Client-side only
  const [tab, setTab] = useState('browse');
  const [query, setQuery] = useState('');
  // busiest first, like the game's own job lists
  const [sort, setSort] = useState({ key: 'players', dir: 'desc' });
  const [selectedKey, setSelectedKey] = useState(null);
  // a server row's right-click menu is open (the footer shows its keys)
  const [menuOpen, setMenuOpen] = useState(false);
  // an overview standout ("Lowest ping") has focus (the footer offers Enter)
  const [pickFocus, setPickFocus] = useState(false);
  // the server list's game-mode filter ('' = all modes)
  const [modeFilter, setModeFilter] = useState('');
  const [selectedFallback, setSelectedFallback] = useState(null);
  // bumped when a failure sends the player to the game-folder field
  const [callGamePath, setCallGamePath] = useState(0);
  // bumped when an empty or offline list sends the player to direct connect
  const [dcFocus, setDcFocus] = useState(0);
  // the overlays the player dismissed, by the result they belong to
  const [alertSeen, setAlertSeen] = useState(null);
  const [launchSeen, setLaunchSeen] = useState(null);
  // this start's loading screen
  const [art] = useState(nextSplash);

  const started = useRef(false);
  // `launching` state lags a render behind; the ref is the synchronous guard
  // that makes a second Connect in the same tick a no-op.
  const launchingRef = useRef(false);
  const launchTimer = useRef(null);
  // this launch's download ticks inside RATE_WINDOW_MS ({t, bytes})
  const rateSamples = useRef([]);

  useEffect(() => () => window.clearTimeout(launchTimer.current), []);

  useEffect(() => {
    const pingQueue = pings.queue;
    const unsubscribe = bridge.onMessage((d) => {
      switch (d.type) {
        case 'settings':
          setSettings({ ...d });
          break;
        case 'servers':
          pingQueue.onServers(d);
          setServerState({
            status: d.status,
            servers: Array.isArray(d.servers) ? d.servers : [],
            error: d.error || '',
          });
          break;
        case 'pings': {
          // native's list pinger: many rows per message, one render
          pingQueue.onPings(d);
          const byRow = new Map((Array.isArray(d.results) ? d.results : []).map((r) => [r.row, r.ms]));
          setServerState((s) => ({
            ...s,
            servers: s.servers.map((sv, i) => (byRow.has(i) ? { ...sv, ping: byRow.get(i) } : sv)),
          }));
          break;
        }
        case 'ping':
          pingQueue.onPing(d);
          setServerState((s) => ({
            ...s,
            servers: s.servers.map((sv, i) => (i === d.row ? { ...sv, ping: d.ms } : sv)),
          }));
          break;
        case 'favorites':
          setFavorites(Array.isArray(d.servers) ? d.servers : []);
          break;
        case 'recents':
          setRecents(Array.isArray(d.servers) ? d.servers : []);
          break;
        case 'launchResult':
          window.clearTimeout(launchTimer.current);
          launchingRef.current = false;
          setLaunching(false);
          setLaunchResult(d);
          break;
        case 'status':
          setStatusText(d.text || '');
          if (launchingRef.current) armLaunchTimer();
          break;
        case 'assetProgress': {
          if (!launchingRef.current) break; // a late tick after the result
          // the speed over the last few seconds, so the number moves with the
          // real rate (native's mbPerSec is the average since the start, and
          // in decimal megabytes; this screen counts binary ones throughout)
          const now = Date.now();
          const samples = rateSamples.current.filter((s) => now - s.t <= RATE_WINDOW_MS + 2000);
          samples.push({ t: now, bytes: d.bytes });
          rateSamples.current = samples;
          const rate = speedOf(samples, now);
          // native recounts (total -1 again) when it finds a file past the
          // counted end: keep the last total so the bar does not jump back
          setAssets((prev) => {
            const recount = !d.done && d.total < 0 && !!prev && prev.total >= 0;
            const total = recount ? Math.max(prev.total, d.files) : d.total;
            return { ...d, total, recount, rate };
          });
          armLaunchTimer();
          break;
        }
        case 'update':
          setUpdateInfo(d);
          break;
        default:
          break;
      }
    });

    if (!started.current) {
      started.current = true;
      bridge.send({ type: 'ready' });
      bridge.send({ type: 'getSettings' });
    }
    return () => {
      unsubscribe();
      pingQueue.dispose();
    };
  }, [bridge, pings.queue]);

  // While files download, the speed is re-measured between ticks too: native
  // only ticks while bytes arrive, so without this a stall would leave the
  // last speed standing.
  const downloading = launching && !!assets && !assets.done;
  useEffect(() => {
    if (!downloading) return undefined;
    const id = window.setInterval(() => {
      const rate = speedOf(rateSamples.current, Date.now());
      setAssets((prev) => (prev && !prev.done && prev.rate !== rate ? { ...prev, rate } : prev));
    }, RATE_EVERY_MS);
    return () => window.clearInterval(id);
  }, [downloading]);

  // A successful launch holds "Launched" (no second launch mid-boot) until the
  // player comes back: the window regains focus after the game took it, or a
  // minute passes. Then Connect re-arms.
  useEffect(() => {
    if (!launchResult || !launchResult.ok) return undefined;
    let left = !document.hasFocus();
    const onBlur = () => {
      left = true;
    };
    const onFocus = () => {
      if (left) setLaunchResult(null);
    };
    const t = window.setTimeout(() => setLaunchResult(null), 60000);
    window.addEventListener('blur', onBlur);
    window.addEventListener('focus', onFocus);
    return () => {
      window.clearTimeout(t);
      window.removeEventListener('blur', onBlur);
      window.removeEventListener('focus', onFocus);
    };
  }, [launchResult]);

  const favoriteKeys = useMemo(() => new Set(favorites.map(serverKey)), [favorites]);

  // Selection is a key, resolved against the freshest data (the live list
  // carries new pings), so a re-sent list never strands a stale object.
  const selected = useMemo(() => {
    if (!selectedKey) return null;
    return (
      serverState.servers.find((s) => serverKey(s) === selectedKey) ||
      favorites.find((s) => serverKey(s) === selectedKey) ||
      recents.find((s) => serverKey(s) === selectedKey) ||
      (selectedFallback && serverKey(selectedFallback) === selectedKey ? selectedFallback : null)
    );
  }, [selectedKey, selectedFallback, serverState.servers, favorites, recents]);

  const send = (msg) => bridge.send(msg);

  function select(server) {
    setSelectedKey(serverKey(server));
    setSelectedFallback(server);
    // a picked standout's button leaves with the overview, and removal sends no blur
    setPickFocus(false);
    if (!launchingRef.current) setLaunchResult(null);
  }

  // (Re)start the no-response timer; refs and setters only, so the message
  // handler's first-render copy of this function stays correct.
  function armLaunchTimer() {
    window.clearTimeout(launchTimer.current);
    launchTimer.current = window.setTimeout(() => {
      launchingRef.current = false;
      setLaunching(false);
      // Drop the stale progress line beside the error.
      setStatusText('');
      setLaunchResult({ type: 'launchResult', ok: false, code: -1, error: 'No response from the launcher for 45 s.' });
    }, LAUNCH_TIMEOUT_MS);
  }

  function handleConnect(server) {
    if (launchingRef.current || (launchResult && launchResult.ok)) return;
    launchingRef.current = true;
    select(server);
    setLaunching(true);
    setLaunchResult(null);
    setStatusText('');
    setAssets(null);
    rateSamples.current = [];
    armLaunchTimer();
    send({ type: 'addRecent', server });
    send({
      type: 'launch',
      server,
      gamePath: (settings && settings.gamePath) || '',
      playerName: (settings && settings.playerName) || '',
    });
  }

  function handleDirectConnect(value) {
    const parsed = parseDirectConnect(value);
    if (!parsed) return false;
    handleConnect({ ...parsed, name: parsed.host, players: -1, ping: -1, url: '' });
    return true;
  }

  function handleToggleFavorite(s) {
    const key = serverKey(s);
    if (favoriteKeys.has(key)) send({ type: 'removeFavorite', key });
    else send({ type: 'addFavorite', server: s });
  }

  function handleSort(key) {
    // a new column starts where it is useful: most players first, else A-Z / fastest
    setSort((s) => (s.key === key ? { key, dir: s.dir === 'asc' ? 'desc' : 'asc' } : { key, dir: key === 'players' ? 'desc' : 'asc' }));
  }

  function openGameFolder() {
    setAlertSeen(launchResult);
    setTab('settings');
    setCallGamePath((n) => n + 1);
  }

  function stepTab(step) {
    const i = TABS.findIndex((t) => t.id === tab);
    setTab(TABS[(i + step + TABS.length) % TABS.length].id);
  }

  const phase = launching ? 'launching' : launchResult ? (launchResult.ok ? 'done' : 'failed') : 'idle';
  // every Connect control stays locked from launch until the player is back
  const launchLocked = launching || phase === 'done';
  const showAlert = phase === 'failed' && alertSeen !== launchResult;
  const showLaunch = !!selected && (launching || (phase === 'done' && launchSeen !== launchResult));

  // The card, Enter and F act on the picked server only while it is in the
  // list on screen; switching back to a list that holds it shows it again.
  const viewList = useMemo(() => {
    if (tab === 'browse') return serverState.status === 'ok' ? serverState.servers.filter((s) => matchesQuery(s, query)) : [];
    return tab === 'favorites' ? favorites : tab === 'recents' ? recents : [];
  }, [tab, query, serverState, favorites, recents]);
  const shown = useMemo(() => {
    if (!selected) return null;
    const key = serverKey(selected);
    return viewList.some((s) => serverKey(s) === key) ? selected : null;
  }, [selected, viewList]);
  // with no list to pick from, direct connect is the way in
  const promote = !shown && tab === 'browse' && (serverState.status === 'error' || (serverState.status === 'ok' && serverState.servers.length === 0));
  const isFav = shown ? favoriteKeys.has(serverKey(shown)) : false;

  // The game's PC menu keys: Q and E walk the tabs, F stars the picked server,
  // Enter with nothing focused connects it, Esc leaves Settings.
  const keyCtx = useRef({});
  keyCtx.current = { stepTab, shown, tab, launchLocked, overlay: showAlert || showLaunch, handleToggleFavorite, handleConnect };
  useEffect(() => {
    const onKey = (e) => {
      const c = keyCtx.current;
      if (c.overlay || e.ctrlKey || e.altKey || e.metaKey || ownsKeys(e.target)) return;
      const k = e.key.toLowerCase();
      if (k === 'q' || k === 'e') {
        e.preventDefault();
        c.stepTab(k === 'q' ? -1 : 1);
      } else if (k === 'f' && c.shown) {
        e.preventDefault();
        c.handleToggleFavorite(c.shown);
      } else if (e.key === 'Escape' && c.tab === 'settings') {
        e.preventDefault();
        setTab('browse');
      } else if (e.key === 'Enter' && c.shown && !c.launchLocked && (e.target === document.body || !e.target)) {
        e.preventDefault();
        c.handleConnect(c.shown);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  const name = selected ? displayName(selected) : '';
  // The foot speaks for the launch only; the list's loading and offline
  // states are said once, in the list's own panel. In Settings the player
  // is already at the fix, so the failure is not repeated there.
  const status = launching
    ? { icon: 'busy', text: statusText || `Launching ${name}` }
    : phase === 'done'
      ? { icon: 'ok', text: 'Game started. Joining servers is not in this build yet.' }
      : phase === 'failed' && tab !== 'settings'
        ? { icon: 'stop', text: `Launch failed: ${failureText(launchResult)}` }
        : null;

  // the session's totals (the loading screen's line), whatever the filter
  const totals = useMemo(() => {
    const list = serverState.status === 'ok' ? serverState.servers : [];
    return { servers: list.length, players: list.reduce((n, s) => n + (s.players > 0 ? s.players : 0), 0) };
  }, [serverState]);
  // nothing picked: the overview card's numbers, from the list already
  // fetched, over the game mode the list is filtered to (on Servers)
  const overview = useMemo(() => {
    const all = serverState.status === 'ok' ? serverState.servers : [];
    const filtered = tab === 'browse' && modeFilter ? all.filter((s) => modeKey(s) === modeFilter) : [];
    const list = filtered.length ? filtered : all;
    if (list.length === 0) return null;
    const players = list.reduce((n, s) => n + (s.players > 0 ? s.players : 0), 0);
    const best = list.reduce((b, s) => (s.ping >= 0 && (!b || s.ping < b.ping) ? s : b), null);
    const busiest = list.reduce((b, s) => (s.players > 0 && (!b || s.players > b.players) ? s : b), null);
    return { servers: list.length, players, best, busiest, mode: filtered.length ? modeLabel(modeFilter) : '' };
  }, [serverState, tab, modeFilter]);
  const listLive = serverState.status === 'ok' && serverState.servers.length > 0;

  // the instructional buttons for what can be done here, in the game's order
  const hints = [];
  const focusIn = (sel) => () => document.querySelector(sel)?.focus();
  if (menuOpen && tab !== 'settings') {
    // a row's right-click menu holds the keys while it is open (RowMenu.jsx)
    hints.push({ keys: ['↑', '↓'], label: 'Select' });
    hints.push({ keys: ['Enter'], label: 'Choose' });
    hints.push({ keys: ['F'], label: 'Favorite' });
    hints.push({ keys: ['Esc'], label: 'Back' });
  } else if (tab === 'settings') {
    hints.push({ keys: ['↑', '↓'], label: 'Select', onClick: focusIn('.opts .opt input, .opts button.opt') });
    hints.push({ keys: ['←', '→'], label: 'Change' });
    hints.push({ keys: ['Esc'], label: 'Back', onClick: () => setTab('browse') });
  } else {
    // an overview standout has focus: Enter picks it
    if (pickFocus && !shown) hints.push({ keys: ['Enter'], label: 'Select' });
    if (viewList.length > 0) {
      hints.push({ keys: ['↑', '↓'], label: 'Select', onClick: focusIn('.schedule tbody tr[tabindex="0"]') });
    }
    if (shown) {
      // as in the game, a button that does nothing right now is not shown
      if (!launchLocked) {
        hints.push({ keys: ['Enter'], label: phase === 'failed' ? 'Retry' : 'Connect', onClick: () => handleConnect(shown) });
      }
      hints.push({ keys: ['F'], label: isFav ? 'Unfavorite' : 'Favorite', onClick: () => handleToggleFavorite(shown) });
    }
    // with no list, the panel's own message offers Retry / Refresh
    if (tab === 'browse' && listLive) {
      hints.push({ keys: ['Ctrl', 'F'], label: 'Search', onClick: focusIn('.search input') });
      hints.push({ keys: ['F5'], label: 'Refresh', onClick: () => send({ type: 'refresh' }) });
    }
  }
  if (!menuOpen || tab === 'settings') hints.push({ keys: ['Q', 'E'], label: 'Tabs', onClick: () => stepTab(1) });

  // what Favorites and Recents share with the server list
  const listProps = {
    selectedKey,
    onSelect: select,
    onConnect: handleConnect,
    launching: launchLocked,
    liveServers: serverState.servers,
    pings,
    favoriteKeys,
    onToggleFavorite: handleToggleFavorite,
    onMenuChange: setMenuOpen,
  };

  return (
    <div className="shell">
      <div className="backdrop" aria-hidden="true">
        <img src={BACKDROP_ART} alt="" decoding="async" />
      </div>
      <TopBar
        isMock={bridge.isMock}
        tab={tab}
        onTab={setTab}
        counts={{ favorites: favorites.length, recents: recents.length }}
        playerName={settings && settings.playerName}
      />

      {/* one fixed frame: tabs swap what is inside it, never its size */}
      <div className={`deck ${tab === 'settings' ? 'is-single' : ''}`}>
        {tab === 'settings' ? (
          <SettingsView
            settings={settings}
            updateInfo={updateInfo}
            send={send}
            callGamePath={callGamePath}
            favoritesCount={favorites.length}
            recentsCount={recents.length}
          />
        ) : (
          <>
            <main className="main">
              {tab === 'browse' && (
                <ServerTable
                  servers={serverState.servers}
                  status={serverState.status}
                  error={serverState.error}
                  query={query}
                  onQuery={setQuery}
                  sort={sort}
                  onSort={handleSort}
                  favoriteKeys={favoriteKeys}
                  selectedKey={selectedKey}
                  onSelect={select}
                  onConnect={handleConnect}
                  onToggleFavorite={handleToggleFavorite}
                  onPing={(s) => pings.queue.request(s)}
                  pings={pings}
                  onRetry={() => send({ type: 'refresh' })}
                  onDirect={() => setDcFocus((n) => n + 1)}
                  launching={launchLocked}
                  onMenuChange={setMenuOpen}
                  mode={modeFilter}
                  onMode={setModeFilter}
                />
              )}
              {tab === 'favorites' && (
                <ServerListCard
                  title="Favorites"
                  servers={favorites}
                  emptyTitle="No favorites yet"
                  emptyHint="Press F on a server, or its star, to keep it here."
                  {...listProps}
                />
              )}
              {tab === 'recents' && (
                <ServerListCard
                  title="Recents"
                  servers={recents}
                  emptyTitle="No recent servers"
                  emptyHint="Servers you connect to show up here."
                  {...listProps}
                />
              )}
            </main>

            <SidePanel
              selected={shown}
              launching={launching}
              launchResult={launchResult}
              promote={promote}
              overview={overview}
              listState={serverState.status === 'ok' && serverState.servers.length === 0 ? 'empty' : serverState.status}
              dcFocus={dcFocus}
              isFavorite={isFav}
              pingState={shown ? pingStateOf(serverKey(shown), pings) : null}
              onToggleFavorite={handleToggleFavorite}
              onLaunch={handleConnect}
              onDirectConnect={handleDirectConnect}
              onOpenSettings={openGameFolder}
              onPick={select}
              onPickFocus={setPickFocus}
            />
          </>
        )}
      </div>

      <Footer status={status} hints={hints} />

      {showLaunch && <LaunchScreen server={selected} phase={phase} statusText={statusText} assets={assets} onBack={() => setLaunchSeen(launchResult)} />}
      {showAlert && (
        <AlertScreen
          server={selected}
          result={launchResult}
          onRetry={() => selected && handleConnect(selected)}
          onFix={openGameFolder}
          onBack={() => setAlertSeen(launchResult)}
        />
      )}
      <Splash
        art={art}
        list={{
          status: serverState.status,
          total: totals.servers,
          players: totals.players,
          pinged: pings.auto.total - pings.auto.left,
          toPing: pings.auto.total,
        }}
      />
    </div>
  );
}
