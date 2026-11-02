// A line-by-line TypeScript port of the server-list parsing in libs/net/serverfinder.cpp
// (ParseServerEntry, CollectServers, ParseServerList and their helpers), used as the oracle
// for "the launcher reads GET /servers unchanged". It is a port, not the real code: when
// serverfinder.cpp changes, change this file with it. Byte-length rules (ReadLabel caps) are
// UTF-8 bytes, as in C++ std::string::size().

export const LAUNCHER_DEFAULT_PORT = 7777; // kDefaultPort in serverfinder.cpp (a RAGE:MP leftover)
export const LAUNCHER_MAX_SERVERS = 2000; // kMaxServers

export interface LauncherServer {
  host: string;
  port: number;
  name: string;
  url: string;
  players: number; // -1 = unknown
  maxPlayers: number; // -1 = unknown
  mode: string;
  lang: string;
}

type Obj = Record<string, unknown>;
const isObj = (v: unknown): v is Obj => typeof v === 'object' && v !== null && !Array.isArray(v);

const trimA = (s: string): string => s.replace(/^[ \t\r\n]+|[ \t\r\n]+$/g, '');

function parsePortText(s: string): number | null {
  if (s.length === 0 || s.length > 5) return null;
  if (!/^[0-9]+$/.test(s)) return null;
  const v = Number(s);
  return v < 1 || v > 65535 ? null : v;
}

function validHost(h: string): boolean {
  return h.length > 0 && h.length <= 253 && /^[A-Za-z0-9.\-_:%]+$/.test(h);
}

function splitHostPort(specIn: string, allowScheme: boolean): { host: string; port: number; hasPort: boolean } | null {
  let s = trimA(specIn);
  const sc = s.indexOf('://');
  if (sc !== -1) {
    if (!allowScheme) return null;
    s = s.slice(sc + 3);
  }
  const cut = s.search(/[/?#]/);
  if (cut !== -1) s = s.slice(0, cut);
  if (s.length === 0 || s.length > 260 || s.includes('@')) return null;

  let host: string;
  let port = 0;
  let hasPort = false;
  if (s.startsWith('[')) {
    const close = s.indexOf(']');
    if (close === -1) return null;
    host = s.slice(1, close);
    if (!host.includes(':')) return null;
    const rest = s.slice(close + 1);
    if (rest.length > 0) {
      if (rest[0] !== ':') return null;
      const p = parsePortText(rest.slice(1));
      if (p === null) return null;
      port = p;
      hasPort = true;
    }
  } else {
    const c1 = s.indexOf(':');
    if (c1 === -1) host = s;
    else if (s.indexOf(':', c1 + 1) === -1) {
      host = s.slice(0, c1);
      const p = parsePortText(s.slice(c1 + 1));
      if (p === null) return null;
      port = p;
      hasPort = true;
    } else host = s; // bare IPv6 literal
  }
  return validHost(host) ? { host, port, hasPort } : null;
}

function firstString(o: Obj, keys: string[]): string | null {
  for (const k of keys) {
    const v = o[k];
    if (typeof v !== 'string') continue;
    const t = trimA(v);
    if (t.length === 0) continue;
    return t;
  }
  return null;
}

function readPort(v: unknown): { kind: 'absent' } | { kind: 'ok'; port: number } | { kind: 'bad' } {
  if (v === undefined || v === null) return { kind: 'absent' };
  if (typeof v === 'number') {
    return v >= 1 && v <= 65535 && v === Math.floor(v) ? { kind: 'ok', port: v } : { kind: 'bad' };
  }
  if (typeof v === 'string') {
    const t = trimA(v);
    if (t.length === 0) return { kind: 'absent' };
    const p = parsePortText(t);
    return p === null ? { kind: 'bad' } : { kind: 'ok', port: p };
  }
  return { kind: 'bad' };
}

function readCount(o: Obj, keys: string[]): number {
  for (const k of keys) {
    const v = o[k];
    if (v === undefined) continue;
    if (typeof v === 'number') {
      if (v >= 0 && v <= 10_000_000) return Math.floor(v + 0.5);
      continue;
    }
    if (typeof v === 'string') {
      const t = trimA(v);
      if (t.length === 0 || t.length > 8) continue;
      if (/^[0-9]+$/.test(t)) return Number(t);
    }
  }
  return -1;
}

function readLabel(o: Obj, keys: string[], capBytes: number): string {
  const s = firstString(o, keys);
  if (s === null) return '';
  return Buffer.byteLength(s, 'utf8') > capBytes ? '' : s;
}

function parseServerEntry(o: unknown, key: string | null): LauncherServer | null {
  if (!isObj(o)) return null;

  let host = '';
  let port = 0;
  let haveHost = false;
  let havePort = false;

  for (const k of ['ip', 'host', 'address', 'server']) {
    if (haveHost) break;
    const v = o[k];
    if (typeof v !== 'string') continue;
    const sp = splitHostPort(v, false);
    if (!sp) continue;
    host = sp.host;
    haveHost = true;
    if (sp.hasPort) {
      port = sp.port;
      havePort = true;
    }
  }

  for (const k of ['port', 'port2']) {
    const f = readPort(o[k]);
    if (f.kind === 'absent') continue;
    if (f.kind === 'bad') return null;
    port = f.port;
    havePort = true;
    break;
  }

  const fallback = (spec: string, allowScheme: boolean): void => {
    const sp = splitHostPort(spec, allowScheme);
    if (!sp || !sp.hasPort) return;
    if (!haveHost) {
      host = sp.host;
      haveHost = true;
      if (!havePort) {
        port = sp.port;
        havePort = true;
      }
    } else if (!havePort && sp.host.toLowerCase() === host.toLowerCase()) {
      port = sp.port;
      havePort = true;
    }
  };
  if (key !== null) fallback(key, false);
  let urlText = '';
  const uv = o['url'];
  if (typeof uv === 'string') {
    urlText = trimA(uv);
    if (urlText.length > 2048) urlText = '';
    const sc = urlText.indexOf('://');
    let connectable = sc === -1;
    if (!connectable) {
      const scheme = urlText.slice(0, sc).toLowerCase();
      connectable = scheme === 'ws' || scheme === 'wss';
    }
    if (connectable && urlText.length > 0) fallback(urlText, true);
  }

  if (!haveHost) return null;
  if (!havePort) port = LAUNCHER_DEFAULT_PORT;

  let name = firstString(o, ['name', 'label']) ?? '';
  if (name === '') name = `${host}:${port}`;

  return {
    host,
    port,
    name,
    url: urlText,
    players: readCount(o, ['players', 'player_count', 'current', 'numPlayers']),
    maxPlayers: readCount(o, ['maxplayers', 'max_players', 'maxPlayers', 'slots']),
    mode: readLabel(o, ['gamemode', 'mode'], 32),
    lang: readLabel(o, ['lang', 'language'], 8),
  };
}

export interface LauncherParse {
  servers: LauncherServer[];
  dropped: number;
}

/** ParseServerList(body): null when the launcher would report an error (invalid JSON or no servers). */
export function launcherParseServerList(body: string): LauncherParse | null {
  let root: unknown;
  try {
    root = JSON.parse(body);
  } catch {
    return null; // "invalid JSON in server list"
  }
  const servers: LauncherServer[] = [];
  const seen = new Set<string>();
  let dropped = 0;
  let full = false;

  const add = (o: unknown, key: string | null, probe = false): boolean => {
    if (full) return false;
    const info = parseServerEntry(o, key);
    if (!info) {
      if (!probe) dropped++;
      return false;
    }
    const id = `${info.host.toLowerCase()}:${info.port}`;
    if (seen.has(id)) return false;
    seen.add(id);
    if (servers.length >= LAUNCHER_MAX_SERVERS) {
      full = true;
      return false;
    }
    servers.push(info);
    return true;
  };

  const collect = (v: unknown, level: number): void => {
    if (full) return;
    if (Array.isArray(v)) {
      for (const e of v) if (isObj(e)) add(e, null);
      return;
    }
    if (!isObj(v)) return;
    if (add(v, null, true)) return;
    if (level < 3) {
      for (const w of ['servers', 'list', 'result', 'data']) {
        const m = v[w];
        if (!(Array.isArray(m) || isObj(m))) continue;
        const before = servers.length;
        collect(m, level + 1);
        if (servers.length > before) return;
      }
    }
    for (const [k, m] of Object.entries(v)) {
      if (full) break;
      if (isObj(m)) add(m, k);
    }
  };
  collect(root, 0);

  return servers.length === 0 ? null : { servers, dropped };
}
