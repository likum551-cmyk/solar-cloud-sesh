import type { Product, Server } from '../lib/schemas';
import { showroomModelFor, type ShowroomModel } from '../state/shop';

// The client's imagery: muted GTA V trailer loops and GTA V screenshots (ASSETS.md lists every
// file with its source; owner-accepted IP risk). Files are never inlined into index.html: Vite
// serves public/assets in dev; SolarLauncher.exe answers https://app.gage/assets/<path> from
// {app}\launcher-assets\<path> (NATIVE-INTEGRATION.md). Relative URLs resolve against the page.

const base = `${import.meta.env.BASE_URL}assets/`;

/**
 * The background footage: eight held GTA V shots, 12 s each, no people, slow camera moves;
 * muted 1600x900 VP9 WebM at 30 fps (ASSETS.md). A screen plays its lead's reel (`reelFor`):
 * two or three shots, each passing to the next through the ground over REEL_FADE_MS, so no shot
 * ever cuts or dissolves into itself.
 * skyline: Downtown at dusk over the beach towns; vinewood: the sign's letters at dusk, the city
 * behind; beach: palms on Vespucci Beach at sunset, the pier lit; canal: the river channel into
 * Downtown at sunset; boulevard: Vinewood Boulevard at golden hour; streets: a top-down drift
 * over a crossroads; pool: a top-down night pool, rising; observatory: Galileo Observatory at
 * night above the lit city.
 */
export const CLIPS = ['skyline', 'vinewood', 'beach', 'canal', 'boulevard', 'streets', 'pool', 'observatory'] as const;
export type ClipName = (typeof CLIPS)[number];

/**
 * The pass from one shot of a reel into the next, through the ground: the outgoing shot sinks to
 * Night Ground over its first 700 ms while the incoming one rises from 300 ms to 1400 ms (the
 * timings live in ui.css `.backdrop__video.is-reel` / `.is-leaving`). Never a cut, and the two
 * shots are never both strong at once, so two horizons never stack into a double exposure.
 */
export const REEL_FADE_MS = 1500;

/** Each server's own shot (SOL, LUNA, TERRA): among the shared reels it plays only in one that it leads, so picking a server changes the whole window (Quests' own reel, with no server on screen, borrows SOL's beach). */
export const SIGNATURE_CLIPS = ['beach', 'observatory', 'streets'] as const satisfies readonly ClipName[];

/**
 * The reels, curated by horizon and light, so a pass reads as one camera moving on: the dusk city
 * shots together, golden street level with the sunset canal, top-down with top-down. A reel
 * never borrows another server's signature shot.
 */
const REELS: { readonly [C in ClipName]: readonly [C, ...ClipName[]] } = {
  // Sign-in and the Play fallback: Downtown at dusk from above, then down the river to it. Not the
  // Vinewood letters: from behind, with the V under the login card, they read as scaffolding.
  skyline: ['skyline', 'canal'],
  vinewood: ['vinewood', 'skyline', 'boulevard'],
  // SOL: two low dusk horizons with their lights on.
  beach: ['beach', 'skyline'],
  canal: ['canal', 'boulevard', 'skyline'],
  boulevard: ['boulevard', 'canal', 'skyline'],
  // LUNA: the night city, then the dusk city's lights.
  observatory: ['observatory', 'skyline'],
  streets: ['streets', 'pool'],
  // The one exception to matching framing: the only other overhead shot is TERRA's, so a party
  // server's reel turns to the canal. No current server leads with the pool.
  pool: ['pool', 'canal', 'skyline'],
};

/**
 * Quests' own reel (a reading screen, no server on it): the two low dusk horizons with their lights
 * on, the city and then the beach. Not the shared skyline reel: the Vinewood letters' girders and
 * the canal's haze sit behind the title and the list, and the boulevard's billboards stay in frame.
 * The beach is SOL's shot, which suits the screen that pays for backing Solar RP.
 */
export const QUESTS_REEL = ['skyline', 'beach'] as const satisfies readonly [ClipName, ...ClipName[]];

/** The shots a screen plays, in order, starting with `lead`; the reel then wraps to its start. */
export function reelFor(lead: ClipName): readonly ClipName[] {
  return REELS[lead];
}

/** Screenshot stills: a 1600x900 background each; the product stills also have a card thumbnail. */
export const STILLS = ['downtown', 'vehicle', 'vip', 'outfit', 'item', 'coins', 'settings'] as const;
export type StillName = (typeof STILLS)[number];
/** The stills a card shows (products); the rest are backgrounds only. */
export const CARD_STILLS = ['vehicle', 'vip', 'outfit', 'item', 'coins'] as const satisfies readonly StillName[];

export const clipUrl = (c: ClipName) => `${base}video/${c}.webm`;
/** The clip's first frame: shown until the video plays, and instead of it under reduced motion. */
export const posterUrl = (c: ClipName) => `${base}video/${c}.webp`;
/** A 640x360 frame from the middle of the same shot, for the server card. */
export const clipThumbUrl = (c: ClipName) => `${base}video/thumb/${c}.webp`;
export const stillUrl = (s: StillName) => `${base}stills/${s}.webp`;
export const thumbUrl = (s: StillName) => `${base}stills/thumb/${s}.webp`;

export interface ServerArt { clip: ClipName }

/** Keyword rules over a server's name and tags, most specific first. */
// Solar RP's own servers come first: SOL (the sun) gets the sun setting over Vespucci Beach,
// LUNA (the moon) the observatory at night, TERRA (the earth) the streets seen from above. (The
// boulevard is not SOL's: a legible "for lease" billboard sits in it, right under the rail.)
const SERVER_RULES: ReadonlyArray<{ test: RegExp; art: ServerArt }> = [
  { test: /\bsol\b|\bsun/, art: { clip: 'beach' } },
  { test: /\bluna\b|\bmoon|night|observatory/, art: { clip: 'observatory' } },
  { test: /\bterra\b|rac(e|ing)|drift|drag|street|police|cops?\b/, art: { clip: 'streets' } },
  { test: /harbou?r|\bport\b|\bdocks?\b|marina|beach|coast/, art: { clip: 'beach' } },
  { test: /vinewood|hills/, art: { clip: 'vinewood' } },
  { test: /boulevard|roleplay/, art: { clip: 'boulevard' } },
  { test: /canal|river|free ?roam|sandbox|county|wild/, art: { clip: 'canal' } },
  { test: /party|club|pool/, art: { clip: 'pool' } },
  { test: /downtown|city|los santos/, art: { clip: 'skyline' } },
];
/** Rotation for a server no rule matches, by its place in the list: never a Solar RP server's own shot. */
const SERVER_ROTATION: readonly ServerArt[] = [
  { clip: 'skyline' },
  { clip: 'canal' },
  { clip: 'vinewood' },
  { clip: 'boulevard' },
];

/**
 * The footage and card art for a server. The platform has no per-server art field yet
 * (docs/api/launcher-ui.md), so the name and tags pick it; anything else rotates by `index`.
 */
export function serverArt(s: Pick<Server, 'name' | 'tags'>, index: number): ServerArt {
  const text = `${s.name} ${s.tags.join(' ')}`.toLowerCase();
  for (const r of SERVER_RULES) if (r.test.test(text)) return r.art;
  const n = SERVER_ROTATION.length;
  return SERVER_ROTATION[((index % n) + n) % n]!;
}

const PRODUCT_STILL: Record<ShowroomModel, StillName> = {
  car: 'vehicle',
  outfit: 'outfit',
  // any other item (the 3D era called it the watch): light-strip trim on bikes and riding suits
  watch: 'item',
  emblem: 'vip',
  coin: 'coins',
  none: 'coins',
};

/** The still behind a product: by its grant (vehicle.*, outfit.*, other items), then by kind. */
export function productStill(p: Pick<Product, 'kind' | 'grants'>): StillName {
  return PRODUCT_STILL[showroomModelFor(p)];
}

/** Whether this runtime decodes the footage (VP9 profile 0, 8 bit). Chromium decodes VP9 itself
 *  (libvpx, or the GPU), so WebView2 plays it on Windows N too; anything else gets the posters. */
export function canPlayLoops(): boolean {
  if (typeof document === 'undefined') return false;
  return document.createElement('video').canPlayType('video/webm; codecs="vp09.00.40.08"') !== '';
}
