/**
 * In-memory stand-in for the native GAGE-MP.exe host, used ONLY when the page
 * runs outside WebView2 (no `window.chrome.webview`). It mirrors the contract
 * in BRIDGE.md closely enough to develop and preview the UI in a plain browser.
 * All data is fabricated; no network is used; nothing is persisted.
 *
 * Gating lives in bridge.js (dev server, or the `--mode preview` capture
 * build): production builds tree-shake this module out entirely.
 */

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const DEFAULT_SETTINGS = {
  gamePath: 'C:\\Program Files\\Rockstar Games\\GTA V',
  playerName: 'PlayerOne',
  language: 'en-US',
  autoUpdate: true,
  startMinimized: false,
  closeGame: true,
  version: '0.1.3',
};

// Like native, a fetched list carries no pings (-1). Native then pings the
// whole list itself and streams `pings` batches; a `ping` command re-pings
// one row. Each answers after roughly the server's latency, or -1 after the
// 1.2 s timeout.
const LATENCY = {
  '185.22.102.44:22005': 23,
  '45.13.101.7:22005': 51,
  '192.0.2.10:24000': 132,
  '203.0.113.8:22005': 88,
  '10.0.0.55:22005': -1,
  '198.51.100.3:25000': 210,
};

// `?mock=many`: a list long enough to scroll, as real lists run to dozens.
// Documentation-range addresses (RFC 5737), latencies fixed per row.
const MANY_NAMES = ['Vinewood Nights', 'Paleto Bay Life', 'Sandy Shores RP', 'Mirror Park Racing', 'Del Perro Drift', 'Grove Street Family', 'Blaine County Ops', 'Vespucci Freeroam', 'Rockford Hills RP', 'Chumash Cruisers', 'Mount Chiliad Trials', 'Davis Hustle', 'La Mesa Garage', 'Port of LS Cargo', 'Zancudo Air Wars', 'Burton Street Kings', 'Cypress Flats Crew', 'Alamo Sea Fishing', 'Harmony Truckers', 'Strawberry Underground', 'Pillbox Medics', 'Little Seoul Nights', 'Maze Bank Heists', 'Elysian Island Docks'];
function manyServers() {
  return MANY_NAMES.map((name, i) => {
    const host = `198.51.100.${20 + i}`;
    LATENCY[`${host}:22005`] = 18 + ((i * 37) % 180);
    return {
      host,
      port: 22005,
      name,
      players: (i * 13) % 64,
      ping: -1,
      url: `ws://${host}:22005`,
      maxPlayers: [64, 100, 200, 500][i % 4],
      mode: ['roleplay', 'freeroam', 'racing', 'deathmatch'][i % 4],
      lang: ['en', 'ru', 'de', 'pl'][i % 4],
    };
  });
}

// `?names=long`: the names real servers carry (cdn.rage.mp/master, 2026-10-25:
// the busiest 200 run 60 characters at the median, 88 at most), so the
// menu and the loading screen are seen with the text they really hold.
const LONG_NAMES = [
  '🔮 Quant RP | Ми разом! | Halloween Event 🎃 [UKRAINE] [ROLEPLAY] [VOICE] [1.1]',
  '[RolePlay][Voice]⭐ GTA5RP.COM | Blaine | gta5rp.com/discord | X2 [1.1]',
  "🚔 Race 'N' Chase | EN/ES/FR/VI [Copchase][Deathmatch][DM][Freeroam][Voice] [1.1]",
  '🚘 OnlyDrifts Server CustomMaps,Cars,HandlingEditor https://discord.gg/fWAMU6R9HT [1.1]',
  '',
  '💙 Stake RP | COOKING UPDATE | discord.gg/stakegta5 [voice] [ukraine] [roleplay] [1.1]',
];
function longNames() {
  try {
    return new URLSearchParams(window.location.search).get('names') === 'long';
  } catch (e) {
    console.error('mock native: cannot read the names mode', e);
    return false;
  }
}

function makeServers() {
  const list = baseServers();
  return longNames() ? list.map((s, i) => ({ ...s, name: LONG_NAMES[i] })) : list;
}

function baseServers() {
  return [
    { host: '185.22.102.44', port: 22005, name: 'GAGE Europe #1', players: 14, ping: -1, url: 'ws://185.22.102.44:22005', maxPlayers: 200, mode: 'freeroam', lang: 'en' },
    { host: '45.13.101.7', port: 22005, name: 'West Coast RP', players: 42, ping: -1, url: 'ws://45.13.101.7:22005', maxPlayers: 500, mode: 'roleplay', lang: 'en' },
    { host: '192.0.2.10', port: 24000, name: 'Crate Kings', players: 5, ping: -1, url: 'ws://192.0.2.10:24000', maxPlayers: 64, mode: 'deathmatch', lang: 'de' },
    { host: '203.0.113.8', port: 22005, name: 'Neon Drift Club', players: 3, ping: -1, url: 'ws://203.0.113.8:22005', maxPlayers: 100, mode: 'racing', lang: 'ru' },
    { host: '10.0.0.55', port: 22005, name: '', players: -1, ping: -1, url: 'ws://10.0.0.55:22005' },
    { host: '198.51.100.3', port: 25000, name: 'Roleplay United', players: 27, ping: -1, url: 'ws://198.51.100.3:25000', maxPlayers: 1000, mode: 'roleplay', lang: 'pl' },
  ];
}

// `?launch=slow`: the store takes a minute to bring the game up, and native
// reports every 5 s meanwhile (its wait loop in DoLaunch).
// `?launch=assets`: the server's files take a minute (6,371 files, as
// blaine.gta5rp.com serves), so the download bar can be looked at.
function launchMode() {
  try {
    return new URLSearchParams(window.location.search).get('launch') || '';
  } catch (e) {
    console.error('mock native: cannot read the launch mode', e);
    return '';
  }
}
const slowLaunch = () => launchMode() === 'slow';

// `?launch=replay`: a real download's progress callbacks, recorded with their
// timing (window.__ASSET_REPLAY = { ticks: [{t, files, total, fromCache,
// bytes, mbps}], from: ms to skip }, set by the capture script), sent at
// their real times as gage_gui.cpp sends them, then its final done tick.
async function replayAssets(emit) {
  const rec = window.__ASSET_REPLAY;
  if (!rec || !Array.isArray(rec.ticks) || !rec.ticks.length) {
    console.error('mock native: ?launch=replay without window.__ASSET_REPLAY');
    return;
  }
  const from = rec.from || 0;
  const start = performance.now();
  for (const k of rec.ticks) {
    if (k.t < from) continue;
    const wait = k.t - from - (performance.now() - start);
    if (wait > 0) await delay(wait);
    // every field native sends (gage_gui.cpp PostAssetProgress), recorded or 0/-1
    emit({
      type: 'assetProgress',
      files: k.files,
      total: k.total,
      fromCache: k.fromCache,
      bytes: k.bytes,
      mbPerSec: k.mbps / 8,
      active: k.active ?? 0,
      activeLeft: k.activeLeft ?? 0,
      bigIndex: k.bigIndex ?? -1,
      bigSize: k.bigSize ?? 0,
      bigDone: k.bigDone ?? 0,
      done: false,
    });
  }
  const last = rec.ticks[rec.ticks.length - 1];
  const files = rec.files ?? last.files;
  emit({ type: 'assetProgress', files, total: files, fromCache: last.fromCache, bytes: rec.bytes ?? last.bytes, mbPerSec: 0, done: true });
  emit({ type: 'status', text: 'Server files ready' });
}

// native's assetProgress ticks (every 250 ms): counting first, then files
// landing, then one `done` message (BRIDGE.md). Three quarters in, native
// finds files past the counted end and recounts for 2 s (total -1 again),
// as client/net/assets.cpp does when a server grew under the count. From 70%
// one 1.2 GB file downloads to the end: the other files are all in by about
// 90%, and the last tenth waits on it alone (the long "99%" a real server
// shows when its largest file comes last).
async function mockAssets(emit, counted, seconds) {
  const ticks = Math.max(2, Math.round((seconds * 1000) / 250));
  const countTicks = Math.min(4, Math.floor(ticks / 3));
  const recountFrom = Math.round(ticks * 0.75);
  const bigFrom = Math.round(ticks * 0.7);
  const bigSize = 1.2 * 1024 * 1024 * 1024;
  const total = counted + 3;
  const bytesTotal = 2.31 * 1024 * 1024 * 1024;
  // the line's speed swings, as real downloads do (big files vs small ones)
  const weight = (i) => 1 + 0.45 * Math.sin(i / 6);
  let weightSum = 0;
  for (let i = 1; i <= ticks + 1; i++) weightSum += weight(i);
  let done = 0;
  for (let i = 1; i <= ticks; i++) {
    await delay(250);
    done += weight(i);
    const share = i / (ticks + 1);
    const files = Math.min(total - 1, Math.round(total * share * 1.12));
    const bytes = Math.round((bytesTotal * done) / weightSum);
    const secs = (i * 250) / 1000;
    const active = Math.max(1, Math.min(16, total - files));
    const big = i >= bigFrom;
    const bigDone = big ? Math.round((bigSize * (i - bigFrom)) / (ticks - bigFrom + 1)) : 1048576;
    const bigLeft = (big ? bigSize : 3145728) - bigDone;
    emit({
      type: 'assetProgress',
      files,
      total: i <= countTicks || (i >= recountFrom && i < recountFrom + 8) ? -1 : i < recountFrom ? counted : total,
      fromCache: 0,
      bytes,
      mbPerSec: bytes / 1e6 / secs,
      active,
      activeLeft: bigLeft + (active - 1) * 1572864,
      bigIndex: big ? 4127 : 3000 + i,
      bigSize: big ? bigSize : 3145728,
      bigDone,
      done: false,
    });
  }
  emit({ type: 'assetProgress', files: total, total, fromCache: 0, bytes: Math.round(bytesTotal), mbPerSec: 0, done: true });
  emit({ type: 'status', text: 'Server files ready' });
}

// `?mock=offline|empty|loading|many` previews the list's other states.
function listMode() {
  try {
    return new URLSearchParams(window.location.search).get('mock') || '';
  } catch (e) {
    console.error('mock native: cannot read the preview mode', e);
    return '';
  }
}

export function createMockNative() {
  const settings = { ...DEFAULT_SETTINGS };
  const mode = listMode();
  // what native would post once a fetch settles, per preview mode
  const settled = () =>
    mode === 'offline'
      ? { type: 'servers', status: 'error', servers: [], error: 'Could not reach the server list (connection refused).' }
      : { type: 'servers', status: 'ok', servers: mode === 'empty' ? [] : servers, error: '' };
  // what a fetch returns in this preview mode
  const fetchList = () => (mode === 'many' ? [...makeServers(), ...manyServers()] : makeServers());
  let servers = fetchList();
  let favorites = [servers[0], servers[2]];
  let recents = [];
  const listeners = [];

  const emit = (data) => {
    const evt = { data };
    for (const l of listeners) l(evt);
  };
  const key = (s) => `${s.host}:${s.port}`;
  let gen = 0; // bumped by every fetch: a reply for an older list is dropped, as native does

  const measure = (s) => {
    const base = LATENCY[key(s)] ?? -1;
    return base < 0 ? -1 : Math.max(8, base + Math.round((Math.random() - 0.5) * 12));
  };
  const replyDelay = (ms) => (ms < 0 ? 1200 : 40 + ms * 2);

  // native's list pinger: everything at once, results handed over every 40 ms
  function pingAll() {
    const mine = gen;
    let batch = [];
    let flush = 0;
    servers.forEach((s, row) => {
      const ms = measure(s);
      window.setTimeout(() => {
        if (gen !== mine) return;
        servers = servers.map((sv, i) => (i === row ? { ...sv, ping: ms } : sv));
        batch.push({ row, ms });
        if (!flush)
          flush = window.setTimeout(() => {
            flush = 0;
            if (gen === mine && batch.length) emit({ type: 'pings', results: batch });
            batch = [];
          }, 40);
      }, replyDelay(ms));
    });
  }

  function settle() {
    servers = fetchList();
    gen++;
    emit(settled());
    if (mode !== 'offline' && mode !== 'empty') pingAll();
  }

  // Capture-only handles (preview builds; this module never ships in
  // GAGE-MP.exe): replace the list as a refetch would, slow the clicked
  // re-ping, read what was copied.
  const hooks = {
    pingDelayMs: undefined,
    copied: [],
    setServers(filter) {
      gen++;
      servers = fetchList().filter(filter);
      emit(settled());
      pingAll(); // as native does after every fetch
    },
  };
  window.__gageMock = hooks;

  async function handle(cmd) {
    switch (cmd.type) {
      case 'ready':
        emit({ type: 'settings', ...settings });
        emit({ type: 'favorites', servers: favorites });
        emit({ type: 'recents', servers: recents });
        gen++;
        emit({ type: 'servers', status: 'loading', servers: [], error: '' });
        if (mode === 'loading') break;
        await delay(500);
        settle();
        break;

      case 'fetch':
      case 'refresh': {
        gen++;
        emit({ type: 'servers', status: 'loading', servers: [], error: '' });
        if (mode === 'loading') break;
        await delay(600);
        settle();
        break;
      }

      case 'ping': {
        const target = servers[cmd.row];
        if (!target) {
          emit({ type: 'ping', row: cmd.row, ms: -1 });
          break;
        }
        const ms = measure(target);
        await delay(hooks.pingDelayMs ?? replyDelay(ms));
        // Native drops a reply whose list was refetched meanwhile.
        const row = servers.findIndex((s) => key(s) === key(target));
        if (row !== cmd.row) break;
        servers = servers.map((s, i) => (i === row ? { ...s, ping: ms } : s));
        emit({ type: 'ping', row, ms });
        break;
      }

      case 'copyText':
        // native's clipboard write (BRIDGE.md copyText)
        hooks.copied.push(cmd.text);
        emit({ type: 'copied', ok: typeof cmd.text === 'string' && cmd.text.length > 0 && cmd.text.length <= 512 });
        break;

      case 'launch': {
        emit({ type: 'status', text: 'Starting GTA V…' });
        await delay(500);
        if (!cmd.gamePath) {
          // native's code 1: no game exe found anywhere
          emit({ type: 'launchResult', ok: false, code: 1, error: 'Could not find GTA5.exe. Set the game folder in Settings.' });
          break;
        }
        // native downloads the server's own files before the game starts
        emit({ type: 'status', text: 'Checking server files…' });
        if (launchMode() === 'replay') await replayAssets(emit);
        else await mockAssets(emit, 6371, launchMode() === 'assets' ? 60 : 1.5);
        await delay(300);
        emit({ type: 'status', text: 'Starting GTA V through Steam…' });
        await delay(500);
        if (slowLaunch()) {
          for (let s = 5; s <= 60; s += 5) {
            await delay(5000);
            emit({ type: 'status', text: `Waiting for GTA V to start (${s} s)…` });
          }
        }
        emit({ type: 'status', text: 'Loading GAGE-MP into the game…' });
        await delay(500);
        // Mirrors the native host: launchResult.error is empty on success,
        // and nothing claims a join (the engine does not join servers yet).
        emit({ type: 'status', text: 'Game started.' });
        emit({ type: 'launchResult', ok: true, code: 0, error: '' });
        break;
      }

      case 'browseGamePath':
        settings.gamePath = 'C:\\Games\\Grand Theft Auto V';
        emit({ type: 'settings', ...settings });
        break;

      case 'getSettings':
        emit({ type: 'settings', ...settings });
        break;

      case 'saveSetting': {
        const map = {
          player_name: 'playerName',
          playerName: 'playerName',
          gamePath: 'gamePath',
          language: 'language',
          autoUpdate: 'autoUpdate',
          startMinimized: 'startMinimized',
          closeGame: 'closeGame',
        };
        settings[map[cmd.key] || cmd.key] = cmd.value;
        emit({ type: 'settings', ...settings });
        break;
      }

      case 'addFavorite':
        if (!favorites.some((s) => key(s) === key(cmd.server))) {
          favorites = [cmd.server, ...favorites];
        }
        emit({ type: 'favorites', servers: favorites });
        break;

      case 'removeFavorite':
        favorites = favorites.filter((s) => key(s) !== cmd.key);
        emit({ type: 'favorites', servers: favorites });
        break;

      case 'clearFavorites':
        favorites = [];
        emit({ type: 'favorites', servers: favorites });
        break;

      case 'addRecent':
        recents = [
          cmd.server,
          ...recents.filter((s) => key(s) !== key(cmd.server)),
        ].slice(0, 8);
        emit({ type: 'recents', servers: recents });
        break;

      case 'clearRecents':
        recents = [];
        emit({ type: 'recents', servers: recents });
        break;

      case 'checkUpdate':
        await delay(400);
        emit({
          type: 'update',
          current: settings.version,
          latest: '0.2.0',
          available: true,
          error: '',
        });
        break;

      case 'minimize':
        emit({ type: 'status', text: '(mock) window minimized' });
        break;
      case 'close':
        emit({ type: 'status', text: '(mock) window hidden to tray' });
        break;
      case 'quit':
        emit({ type: 'status', text: '(mock) quitting…' });
        break;

      default:
        break;
    }
  }

  return {
    postMessage(cmd) {
      handle(cmd).catch((e) => console.error('mock native:', cmd && cmd.type, e));
    },
    addEventListener(type, handler) {
      if (type === 'message') listeners.push(handler);
    },
    removeEventListener(type, handler) {
      const i = listeners.indexOf(handler);
      if (type === 'message' && i >= 0) listeners.splice(i, 1);
    },
  };
}
