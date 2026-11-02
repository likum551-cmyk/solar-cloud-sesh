import { PRICEDOWN } from '../lib/pricedown.js';
import { serverKey } from '../lib/format.js';
import coverFranklinChop from '../assets/gta/cover-franklin-chop.webp';
import coverFranklinHills from '../assets/gta/cover-franklin-hills.webp';
import coverFranklinBandana from '../assets/gta/cover-franklin-bandana.webp';
import coverMichaelCity from '../assets/gta/cover-michael-city.webp';
import coverMichaelVangelico from '../assets/gta/cover-michael-vangelico.webp';
import coverTrevorAtv from '../assets/gta/cover-trevor-atv.webp';
import coverTrevorJack from '../assets/gta/cover-trevor-jack.webp';
import coverTrevorGas from '../assets/gta/cover-trevor-gas.webp';
import splashMichael from '../assets/gta/splash-michael.webp';
import splashFranklin from '../assets/gta/splash-franklin.webp';
import splashTrevor from '../assets/gta/splash-trevor.webp';
import backdropCity from '../assets/gta/backdrop-city.webp';

/**
 * Pricedown lettering, pre-outlined at build time (lib/pricedown.js): white
 * fill under a black outline, like the game's own titles. `size` is the em
 * size in px, so every string shares one scale. Decorative: pair it with
 * real text for assistive tech.
 */
export function Pricedown({ k, size, className = '' }) {
  const g = PRICEDOWN[k];
  if (!g) throw new Error(`no Pricedown outline for "${k}"`);
  return (
    <svg
      className={`pd ${className}`}
      viewBox={`0 0 ${g.w} ${g.h}`}
      width={(g.w / 100) * size}
      height={(g.h / 100) * size}
      aria-hidden="true"
      focusable="false"
    >
      <path d={g.d} />
    </svg>
  );
}

// Key art by character (`who` also picks the joining screen's art). Every
// title bar stays GTA Online's freemode blue whichever lead is shown.
// `focus` is the lead's face (x% y%, measured on the 960x480 covers), used as
// object-position so a short card strip crops around the face, not the crown.
const COVERS = [
  { src: coverFranklinChop, who: 'franklin', focus: '60% 25%' },
  { src: coverMichaelCity, who: 'michael', focus: '55% 30%' },
  { src: coverTrevorAtv, who: 'trevor', focus: '40% 21%' },
  { src: coverFranklinHills, who: 'franklin', focus: '50% 22%' },
  { src: coverMichaelVangelico, who: 'michael', focus: '50% 28%' },
  { src: coverTrevorJack, who: 'trevor', focus: '55% 22%' },
  { src: coverFranklinBandana, who: 'franklin', focus: '45% 28%' },
  { src: coverTrevorGas, who: 'trevor', focus: '50% 23%' },
];

/** A server's cover is fixed by its address, so it never shuffles between visits. */
export function coverFor(server) {
  const key = serverKey(server);
  let h = 2166136261;
  for (let i = 0; i < key.length; i++) {
    h ^= key.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return COVERS[(h >>> 0) % COVERS.length];
}

const SPLASHES = [
  { src: splashMichael, who: 'michael' },
  { src: splashFranklin, who: 'franklin' },
  { src: splashTrevor, who: 'trevor' },
];

/** Loading screens rotate per start, like the game's; a fresh profile starts on Michael. */
export function nextSplash() {
  let i = 0;
  try {
    i = (Number(window.localStorage.getItem('gage.splash')) || 0) % SPLASHES.length;
    window.localStorage.setItem('gage.splash', String(i + 1));
  } catch (err) {
    // storage blocked: always the first screen, nothing else depends on it
    console.warn('splash rotation unavailable', err);
  }
  return SPLASHES[i];
}

/** All three loading screens, starting from `first` (the start-up splash cycles them). */
export function splashSequence(first) {
  const i = Math.max(0, SPLASHES.findIndex((s) => s.who === first?.who));
  return [...SPLASHES.slice(i), ...SPLASHES.slice(0, i)];
}

/** The loading screen for a server: its cover's lead, full length. */
export function splashFor(server) {
  const who = coverFor(server).who;
  return SPLASHES.find((s) => s.who === who) || SPLASHES[0];
}

// The paused world behind the menu: downtown Los Santos, no character (a
// blurred figure behind the panels reads as a smudge, not a place).
export const BACKDROP_ART = backdropCity;
