import type { Server } from '../lib/schemas';

// The ping and status the server list shows, from two sources: the fetched list (the platform API, or the demo
// data) and the native host's own UDP query of each server (bridge `serverStatus`, keyed by host:port).
//
// The ping a player sees must be measured from the player's PC. The platform's `status.pingMs` is measured from
// the VPS to a game server on the same VPS (it reads 0-3 ms for everybody), so in platform mode it is never shown:
// the native measurement is the only ping. Pure functions, so tests/state.test.ts pins every rule.

/**
 * Where the list came from. `platform`: the platform API (its state and counts are the truth; the native query
 * supplies only the ping). `demo`: the labelled demo data (a native answer replaces the fabricated counts).
 */
export type StatusMode = 'platform' | 'demo';

/** One server's real status from a native UDP query (bridge `serverStatus`), keyed by host:port. */
export type NativeServerStatus = { host: string; port: number; online: boolean; pingMs: number; players: number; maxPlayers: number; protocol?: number };

/** A server the native host should ask by UDP query (bridge `queryServers`). */
export type QueryTarget = { host: string; port: number };

/** The most servers one `queryServers` may name (BRIDGE.md; native drops the rest). */
export const MAX_QUERY_TARGETS = 16;

export function statusKey(host: string, port: number): string {
  return `${host}:${port}`;
}

/**
 * A server's status with the native measurement `r` (undefined: none yet) applied for `mode`.
 * - platform: state, players, maxPlayers and protocol stay the API's; `pingMs` is the native ping, null until
 *   one arrives and null after a failed query (the player may block UDP while the server is up: not offline).
 * - demo: no measurement keeps the demo status; an answer replaces the demo counts and marks them `live`
 *   (a field the server did not report, -1, keeps its prior value); a failed query goes offline.
 */
export function overlayNativeStatus(mode: StatusMode, prev: Server['status'], r: NativeServerStatus | undefined): Server['status'] {
  switch (mode) {
    case 'platform':
      return { ...prev, pingMs: r && r.online && r.pingMs >= 0 ? r.pingMs : null };
    case 'demo':
      if (!r) return prev;
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
  const unknownMode: never = mode;
  throw new Error(`unknown status mode ${String(unknownMode)}`);
}

/** The list the screens read: every server's status with its native measurement (by host:port) applied for `mode`. */
export function mergeServerStatus(mode: StatusMode, servers: Server[], native: Readonly<Record<string, NativeServerStatus>>): Server[] {
  return servers.map((s) => {
    const r = s.host ? native[statusKey(s.host, s.port)] : undefined;
    const status = overlayNativeStatus(mode, s.status, r);
    return status === s.status ? s : { ...s, status };
  });
}

/** The servers to ask: every listed server with a host and a valid port, each host:port once, sorted, at most 16. */
export function queryTargets(servers: readonly Pick<Server, 'host' | 'port'>[]): QueryTarget[] {
  const byKey = new Map<string, QueryTarget>();
  for (const s of servers) {
    if (!s.host || !Number.isInteger(s.port) || s.port < 1 || s.port > 65535) continue;
    byKey.set(statusKey(s.host, s.port), { host: s.host, port: s.port });
  }
  return [...byKey.keys()].sort().slice(0, MAX_QUERY_TARGETS).map((k) => byKey.get(k)!);
}

/**
 * Sends `queryServers` when the list's host:port set differs from the last one sent (a refetch of the same servers
 * sends nothing; an empty list after a non-empty one sends [] so native stops asking). Nothing is sent before the
 * first non-empty list.
 */
export function createQueryServersSync(send: (msg: { type: 'queryServers'; servers: QueryTarget[] }) => void): (servers: readonly Pick<Server, 'host' | 'port'>[]) => void {
  let lastKey = '';
  return (servers) => {
    const targets = queryTargets(servers);
    const key = targets.map((t) => statusKey(t.host, t.port)).join(',');
    if (key === lastKey) return;
    lastKey = key;
    send({ type: 'queryServers', servers: targets });
  };
}
