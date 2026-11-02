export const DEFAULT_PORT = 22005;

export function serverKey(s) {
  return `${s.host}:${s.port}`;
}

// list names arrive padded (" GALAXY DM"); a blank one falls back to the host
export function displayName(s) {
  if (!s) return '—';
  return (s.name || '').trim() || s.host || '—';
}

/** "roleplay" -> "Roleplay"; "" stays "". */
export function modeLabel(mode) {
  const m = (mode || '').trim();
  return m ? m.charAt(0).toUpperCase() + m.slice(1) : '';
}

/** A server's game mode as a grouping key ("Roleplay " and "roleplay" are one mode); '' when unlisted. */
export function modeKey(s) {
  return (s?.mode || '').trim().toLowerCase();
}

/** A list row's short tags: { mode: "Roleplay", lang: "RU" }, each '' when the list leaves it out. */
export function modeTag(s) {
  return { mode: modeLabel(s?.mode), lang: (s?.lang || '').trim().toUpperCase() };
}

let langNames = null;
/** A list's language code as a name ("ru" -> "Russian"); an unknown code is shown as-is, uppercased. */
export function langName(code) {
  const c = (code || '').trim();
  if (!c) return '';
  try {
    langNames = langNames || new Intl.DisplayNames(['en'], { type: 'language' });
    const n = langNames.of(c.toLowerCase());
    if (n && n.toLowerCase() !== c.toLowerCase()) return n;
  } catch (err) {
    // not a well-formed code ("gb", "xx-1"): the raw code says enough
    if (!(err instanceof RangeError)) throw err;
  }
  return c.toUpperCase();
}

/** The server list's search: part of a name, a host, host:port, a game mode or a language, any case. */
export function matchesQuery(s, query) {
  const q = query.trim().toLowerCase();
  if (!q) return true;
  return (
    (s.name || '').toLowerCase().includes(q) ||
    `${s.host}:${s.port}`.toLowerCase().includes(q) ||
    (s.mode || '').toLowerCase().includes(q) ||
    (!!s.lang && (s.lang.toLowerCase() === q || langName(s.lang).toLowerCase().includes(q)))
  );
}

/**
 * Parse a direct-connect input: `host:port`, `ip:port`, or a full
 * `ws://` / `wss://` / `http(s)://` URL. Returns `{ host, port }` or null.
 * Falls back to the launcher's default port when none is given.
 */
export function parseDirectConnect(input) {
  if (!input) return null;
  let s = String(input).trim();
  if (!s) return null;

  if (/^[a-z][a-z0-9+.-]*:\/\//i.test(s)) {
    try {
      const u = new URL(s);
      if (!u.hostname) return null;
      const port = u.port ? parseInt(u.port, 10) : DEFAULT_PORT;
      if (Number.isFinite(port) && port > 0 && port < 65536) {
        return { host: u.hostname, port };
      }
    } catch {
      /* fall through to raw parse */
    }
  }

  s = s.replace(/^[a-z][a-z0-9+.-]*:\/\//i, '');
  s = s.split(/[/?#]/)[0];

  let host;
  let port = DEFAULT_PORT;
  const idx = s.lastIndexOf(':');
  if (idx > 0) {
    host = s.slice(0, idx);
    const p = parseInt(s.slice(idx + 1), 10);
    if (Number.isFinite(p) && p > 0 && p < 65536) port = p;
  } else {
    host = s;
  }
  host = host.trim();
  if (!host) return null;
  return { host, port };
}
