// SOL's live status for the landing page, from the platform's public server list (GET /api/v1/servers,
// apps/platform/src/http/routes/player.ts). The platform probes each server over the GAGE UDP query and
// caches the answer about 5 s. Its pingMs is measured from the platform host, not from the visitor, so
// the page never shows it. Anything the list does not say (no SOL row, a failed fetch, a server not
// probed yet) falls back to the page's static claim, "Open", with no player count (the panel hides that row).

export type ServerState = 'online' | 'offline' | 'unknown';

export interface ServerInfo {
  id: string;
  name: string;
  state: ServerState;
  players: number | null;
  maxPlayers: number | null;
  /** the maintenance message when maintenance is on, else null */
  maintenance: string | null;
}

const STATES: readonly ServerState[] = ['online', 'offline', 'unknown'];
const count = (v: unknown): number | null => (typeof v === 'number' && Number.isSafeInteger(v) && v >= 0 && v <= 100_000 ? v : null);

function parseOne(v: unknown): ServerInfo | null {
  if (typeof v !== 'object' || v === null) return null;
  const o = v as Record<string, unknown>;
  if (typeof o.id !== 'string' || typeof o.name !== 'string') return null;
  const st = (typeof o.status === 'object' && o.status !== null ? o.status : {}) as Record<string, unknown>;
  const state = STATES.includes(st.state as ServerState) ? (st.state as ServerState) : 'unknown';
  const m = (typeof o.maintenance === 'object' && o.maintenance !== null ? o.maintenance : {}) as Record<string, unknown>;
  const maintenance = m.enabled === true ? (typeof m.message === 'string' && m.message.trim() ? m.message.trim().slice(0, 200) : '') : null;
  let players = count(st.players);
  let maxPlayers = count(st.maxPlayers);
  if (state !== 'online' || players === null || maxPlayers === null || maxPlayers === 0 || players > maxPlayers) {
    players = null;
    maxPlayers = null;
  }
  return { id: o.id, name: o.name, state, players, maxPlayers, maintenance };
}

/** The servers in a /v1/servers body, or null when the body is not that shape. Bad rows are skipped. */
export function parseServers(json: unknown): ServerInfo[] | null {
  if (typeof json !== 'object' || json === null) return null;
  const list = (json as { servers?: unknown }).servers;
  if (!Array.isArray(list)) return null;
  return list.map(parseOne).filter((s): s is ServerInfo => s !== null);
}

/** The row for `key` (matched on id or name, any case), if the list has one. */
export function findServer(list: readonly ServerInfo[], key: string): ServerInfo | undefined {
  const k = key.toLowerCase();
  return list.find((s) => s.id.toLowerCase() === k || s.name.toLowerCase() === k);
}

export type Tone = 'open' | 'online' | 'offline' | 'maintenance';

export interface StatusView {
  tone: Tone;
  label: string;
  /** "12 / 128" when the server reported both counts, else null (the page hides the Players row) */
  count: string | null;
  note: string;
}

export const STATIC_STATUS: StatusView = {
  tone: 'open',
  label: 'Open',
  count: null,
  note: 'Pick SOL in Solar Launcher and press PLAY.',
};

/** What the status panel shows for a server row (or for no row: the static claim). */
export function describeStatus(s: ServerInfo | undefined): StatusView {
  if (!s) return STATIC_STATUS;
  if (s.maintenance !== null) {
    return { tone: 'maintenance', label: 'Maintenance', count: null, note: s.maintenance || 'The server is down for maintenance. The launcher shows when it is back.' };
  }
  if (s.state === 'offline') {
    return { tone: 'offline', label: 'Offline right now', count: null, note: 'The server is not answering. The launcher shows when it is back.' };
  }
  if (s.state === 'online' && s.players !== null && s.maxPlayers !== null) {
    return { tone: 'online', label: 'Online', count: `${s.players} / ${s.maxPlayers}`, note: STATIC_STATUS.note };
  }
  return STATIC_STATUS;
}

/** How full a server is, 0..1, for the capacity bar; null when it reported no counts (the bar is hidden). */
export function loadFraction(s: ServerInfo | undefined): number | null {
  if (!s || s.maintenance !== null || s.state !== 'online' || s.players === null || s.maxPlayers === null) return null;
  return Math.min(1, s.players / s.maxPlayers);
}

/** "updated 12 s ago" for the server panel's age line (whole seconds, then whole minutes). */
export function describeAge(ms: number): string {
  const s = Math.max(0, Math.floor(ms / 1000));
  if (s < 5) return 'Updated just now';
  if (s < 60) return `Updated ${s} s ago`;
  return `Updated ${Math.floor(s / 60)} min ago`;
}

/** Fetches the list; null on any failure (the page then keeps its static claim). */
export async function loadServers(signal?: AbortSignal): Promise<ServerInfo[] | null> {
  const res = await fetch('/api/v1/servers', { cache: 'no-store', signal, headers: { accept: 'application/json' } });
  if (!res.ok) return null;
  return parseServers(await res.json());
}
