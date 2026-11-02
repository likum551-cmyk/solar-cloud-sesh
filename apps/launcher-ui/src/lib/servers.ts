// Solar RP's own server lineup — the real, curated list the launcher shows. One live server (SOL,
// the Solar RP network's official roleplay server, on the VPS) and the upcoming servers as "coming soon".
//
// This is the baked default so the launcher shows the real network even when no master/platform
// API is configured (demo mode). When a platform API IS configured, the list comes from it
// instead (see AppState.fetchServers); the platform can mark its own entries comingSoon too.
//
// NOTE: the #2 and #3 entries (LUNA, TERRA) are not live; their regions and tags are placeholders —
// set them when those servers go live. The identifiers below (GAGE_*, GageServerSeed) are internal
// names and stay as they are.

/** The live Solar RP server, SOL (the Hetzner VPS). PLAY on #1 hands off to this host:port. */
export const GAGE_LIVE_HOST = '5.175.253.187';
export const GAGE_LIVE_PORT = 24800;

export interface GageServerSeed {
  id: string;
  name: string;
  region: string;
  /** Empty for a not-yet-live server (it is never joined). */
  host: string;
  port: number;
  tags: string[];
  comingSoon: boolean;
}

/** The canonical lineup, in display order. */
export const GAGE_SERVER_SEEDS: readonly GageServerSeed[] = [
  { id: 'sol',   name: 'SOL',   region: 'EU', host: GAGE_LIVE_HOST, port: GAGE_LIVE_PORT, tags: ['roleplay'], comingSoon: false },
  { id: 'luna',  name: 'LUNA',  region: 'EU', host: '', port: 0, tags: ['freeroam'], comingSoon: true },
  { id: 'terra', name: 'TERRA', region: 'EU', host: '', port: 0, tags: ['racing'],   comingSoon: true },
];
