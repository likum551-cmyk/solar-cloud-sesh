import type { Product, Server } from '../lib/schemas';
import { showroomModelFor, type ShowroomModel } from '../state/shop';

// The client's imagery: muted GTA V trailer loops and GTA V screenshots (ASSETS.md lists every
// file with its source; owner-accepted IP risk). Files are never inlined into index.html: Vite
// serves public/assets in dev; SolarLauncher.exe answers https://app.gage/assets/<path> from
// {app}\launcher-assets\<path> (NATIVE-INTEGRATION.md). Relative URLs resolve against the page.

const base = `${import.meta.env.BASE_URL}assets/`;

/**
 * The background loops. Each is one continuous trailer shot (no montage), slowed with motion
 * interpolation and closed by dissolving its tail into its head: 3-5.4 s, 1600x900 H.264.
 * city: Downtown from the Vinewood Hills with a helicopter; harbor: the refinery at sunset;
 * wild: a biplane over Grand Senora at sunset; night: tuned cars sliding past a neon club on a
 * wet street; vinewood: a penthouse over the city at sunset, a helicopter passing.
 */
export const CLIPS = ['city', 'harbor', 'wild', 'night', 'vinewood'] as const;
export type ClipName = (typeof CLIPS)[number];

/** Screenshot stills: a 1600x900 background each; the product stills also have a card thumbnail. */
export const STILLS = ['downtown', 'vehicle', 'vip', 'outfit', 'item', 'coins', 'settings'] as const;
export type StillName = (typeof STILLS)[number];
/** The stills a card shows (products); the rest are backgrounds only. */
export const CARD_STILLS = ['vehicle', 'vip', 'outfit', 'item', 'coins'] as const satisfies readonly StillName[];

export const clipUrl = (c: ClipName) => `${base}video/${c}.mp4`;
/** The clip's first frame: shown until the video plays, and instead of it under reduced motion. */
export const posterUrl = (c: ClipName) => `${base}video/${c}.webp`;
/** A 640x360 frame of the same shot, for the server card. */
export const clipThumbUrl = (c: ClipName) => `${base}video/thumb/${c}.webp`;
export const stillUrl = (s: StillName) => `${base}stills/${s}.webp`;
export const thumbUrl = (s: StillName) => `${base}stills/thumb/${s}.webp`;

export interface ServerArt { clip: ClipName }

/** Keyword rules over a server's name and tags, most specific first. */
const SERVER_RULES: ReadonlyArray<{ test: RegExp; art: ServerArt }> = [
  { test: /harbou?r|\bport\b|\bdocks?\b|marina|beach|coast/, art: { clip: 'harbor' } },
  { test: /vinewood|hills|boulevard/, art: { clip: 'vinewood' } },
  { test: /rac(e|ing)|drift|drag|street|night|police|cops?\b/, art: { clip: 'night' } },
  { test: /free ?roam|sandbox|county|desert|wild/, art: { clip: 'wild' } },
  { test: /downtown|city|los santos/, art: { clip: 'city' } },
];
/** Rotation for a server no rule matches, by its place in the list. */
const SERVER_ROTATION: readonly ServerArt[] = [
  { clip: 'city' },
  { clip: 'harbor' },
  { clip: 'wild' },
  { clip: 'night' },
  { clip: 'vinewood' },
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

/** Whether this runtime decodes the loops (H.264 High). Windows N without the Media Feature
 *  Pack, and open-source Chromium builds, do not: they get the posters. */
export function canPlayLoops(): boolean {
  if (typeof document === 'undefined') return false;
  return document.createElement('video').canPlayType('video/mp4; codecs="avc1.640029"') !== '';
}
