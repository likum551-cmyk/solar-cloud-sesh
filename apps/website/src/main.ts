import 'lenis/dist/lenis.css';
import './base.css';
import './styles.css';
import { gsap } from 'gsap';
import { ScrollTrigger } from 'gsap/ScrollTrigger';
import Lenis from 'lenis';
import { formatSize, loadRelease, type Release } from './release';
import type { Look, Mark3D, Pose } from './mark3d';
import { browserTokenStore } from './account/store';
import { wireAccountLink } from './account/ui';

// GI, 2026-11-06: "too animated, make it more professional". The page scrolls as a page: nothing pins,
// nothing is scrubbed, no wipes, no per-letter reveals. The one authored moment is the arrival (the hero
// title and the 3D S on one curve); after that the footage is the only thing that moves. Lenis only
// smooths the wheel. The S stands still in its two shots (hero, close) and turns a little with the pointer.
const SITE_HOST = 'solar-rp.com';
const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
const root = document.documentElement;
const $ = <T extends Element>(sel: string, el: ParentNode = document) => el.querySelector<T>(sel);
const $$ = <T extends Element>(sel: string, el: ParentNode = document) => Array.from(el.querySelectorAll<T>(sel));

/* ---------------------------------------------------------------- release / download */

function showRelease(r: Release | null) {
  const plates = $$<HTMLAnchorElement>('.js-download');
  const status = $<HTMLElement>('.js-release-status');
  const label = (a: HTMLElement) => $<HTMLElement>('.js-download-label', a) ?? a;
  if (!r) {
    // The release feed did not answer or did not check out (releases ARE published: this is a fetch or
    // feed problem, never "no build yet"). Every control leads to the closing section, which says so.
    for (const a of plates) {
      a.href = '#download';
      a.removeAttribute('download');
      label(a).textContent = a.classList.contains('plate--sm') ? 'Download' : 'Download Solar Launcher';
      a.classList.add('is-soon'); // outlined, not gold: there is no file to hand out right now
    }
    $<HTMLAnchorElement>('.js-download-main')?.setAttribute('aria-disabled', 'true');
    for (const m of $$<HTMLElement>('.js-release-meta')) m.textContent = 'Windows 10 and 11 · free download';
    if (status) status.textContent = 'The download link could not be loaded. Reload the page to try again.';
    return;
  }
  for (const a of plates) {
    a.href = r.url;
    a.setAttribute('download', '');
    a.removeAttribute('aria-disabled');
    label(a).textContent = a.classList.contains('plate--sm') ? 'Download' : 'Download Solar Launcher';
    a.classList.remove('is-soon');
  }
  for (const m of $$<HTMLElement>('.js-release-meta')) m.textContent = `Windows 10 and 11 · v${r.version} · ${formatSize(r.size)}`;
  const hash = $<HTMLElement>('.js-release-hash');
  const sha = $<HTMLElement>('.js-sha');
  if (hash && sha) {
    sha.textContent = r.sha256;
    hash.hidden = false;
  }
  if (status) status.textContent = '';
}

loadRelease(SITE_HOST)
  .then(showRelease)
  .catch(() => showRelease(null));

// "Log in" for visitors, "Account" when this browser holds a session (src/account/store.ts)
wireAccountLink(browserTokenStore());

/* ---------------------------------------------------------------- videos play only on screen */

const vio = new IntersectionObserver(
  (entries) => {
    for (const e of entries) {
      const v = e.target as HTMLVideoElement;
      if (e.isIntersecting && !reduced) v.play().catch(() => {});
      else v.pause();
    }
  },
  { rootMargin: '10% 0px' },
);
const saveData = (navigator as Navigator & { connection?: { saveData?: boolean } }).connection?.saveData === true;
for (const v of $$<HTMLVideoElement>('.js-loop')) {
  if (reduced || saveData) { v.pause(); v.preload = 'none'; continue; } // the poster is the shot
  vio.observe(v);
}
// the hero shot is the first thing seen: fetch it now (it ships preload=none so reduced/save-data never do)
const heroVideo = $<HTMLVideoElement>('.hero__video');
if (heroVideo && !reduced && !saveData) heroVideo.preload = 'auto';

/* ---------------------------------------------------------------- shared geometry */

const bar = $<HTMLElement>('.bar');
// the top bar turns solid as soon as the page has moved, so the nav never sits on footage or the S
if (bar) {
  const solid = () => bar.classList.toggle('is-solid', window.scrollY > 0);
  window.addEventListener('scroll', solid, { passive: true });
  solid();
}
const isPhone = () => window.matchMedia('(max-width: 760px)').matches;

/* ---------------------------------------------------------------- 3D mark */

let mark: Mark3D | null = null;
const heroStage = $<HTMLElement>('[data-stage="hero"]');
const closeStage = $<HTMLElement>('[data-stage="close"]');

// each stage lights the S from its own shot (its poster): the hero's golden skyline at dusk, the close's
// observatory at night. Each reel is ONE shot, so the S never stands in a plate it was not lit from.
const LOOKS: Record<string, Look> = {
  hero: {
    env: '/media/hero.webp',
    fog: 0xb39a7e,
    // from the side and above: the front faces take less of it than the tops and chamfers, so the
    // cream enamel holds a tone instead of clipping to white
    key: { color: 0xffd7a0, intensity: 1.25, position: [3.5, 4, 1.2] },
    rim: { color: 0xffb45a, intensity: 2, position: [-3, 2, -4] },
  },
  close: {
    env: '/media/close.webp',
    // the observatory at night over the lit city: warm lamp light, never a violet rim (NOON)
    fog: 0x2a2219,
    key: { color: 0xffc890, intensity: 1.2, position: [4, 1, 3] },
    rim: { color: 0xffa64d, intensity: 1.5, position: [-3, 3, -4] },
  },
};

const BAR_H = 64; // --bar-h

/** The S at rest in the hero: in the clear sky right of the title, turned -11 degrees (MAX_TURN is 12) so
 *  a side wall and the chamfer catch light and it still reads as an S. */
function heroPose(): Pose {
  const sweep = -12;
  if (isPhone()) {
    // in front of the towers, overlapping their crowns by a clear margin (0.36, 0.38 and 0.45 each left
    // its foot just touching a roof as the drone moves, which read as an accident)
    return { x: 0, y: 0.28, height: 0.3, yaw: -11, pitch: 3, opacity: 1, sweep };
  }
  // the letterbox exists only where the stylesheet draws it (landscape, wider than a phone); its top
  // bar is only as tall as the nav bar, so the frame sits low of centre
  const boxed = window.matchMedia('(min-aspect-ratio: 16/10) and (min-width: 761px)').matches;
  const H = window.innerHeight;
  const lb = boxed ? Math.max(0, (H - window.innerWidth / 2.39) / 2) : 0;
  const top = Math.min(lb, BAR_H);
  const frame = (H - top - lb) / H; // the letterboxed frame's share
  const centre = (lb - top) / H; // the frame's centre, in the stage's -1..1 (y up)
  const portrait = H > window.innerWidth;
  return {
    x: portrait ? 0.4 : 0.48,
    y: portrait ? 0.2 : 0.06 * frame - centre,
    // 0.48 of the frame: below the title's weight now that it stands still (0.6 matched it)
    height: portrait ? 0.36 : 0.48 * frame,
    yaw: -11,
    pitch: 3,
    opacity: 1,
    sweep,
  };
}

/** The S above the closing title. It does not move; it fades in once, the first time the section comes on
 *  screen (closeFade), and is simply there after that. */
const closeFade = { v: reduced ? 1 : 0 };
const closeTitle = $<HTMLElement>('.close__title');
/** Placed from the title's real position (the copy is laid out with svh padding, so a fixed stage y put
 *  the S under the nav bar at max scroll): its foot 32 px above the title, its height a share of the
 *  window, not of the section. Stage units: y -1..1 (up), height a fraction of the stage height. */
function closePose(): Pose {
  const opacity = closeFade.v;
  const hPx = window.innerHeight * (isPhone() ? 0.14 : 0.17);
  const stage = closeStage?.getBoundingClientRect();
  const title = closeTitle?.getBoundingClientRect();
  if (!stage || !title || stage.height <= 0) return { x: 0, y: 0.5, height: 0.17, yaw: 0, pitch: 0, opacity, sweep: 4 };
  const centrePx = title.top - stage.top - 32 - hPx / 2;
  return { x: 0, y: 1 - (2 * centrePx) / stage.height, height: hPx / stage.height, yaw: 0, pitch: 0, opacity, sweep: 4 };
}

async function startMark() {
  const { createMark3D, webglAvailable } = await import('./mark3d');
  if (!webglAvailable() || !heroStage) {
    root.classList.add('no-webgl');
    openEntrance();
    return;
  }
  try {
    mark = createMark3D(reduced, LOOKS);
  } catch (err) {
    console.warn('3D mark unavailable, showing the flat mark', err);
    root.classList.add('no-webgl');
    openEntrance();
    return;
  }
  mark.attach(heroStage, 'hero');
  // reduced motion: the same placement (hero, then close), with no arrival and no fades
  if (reduced) markEntry.v = 1;
  mark.setPose(withEntry(heroPose()), true);
  mark.setActive(true);
  wireMarkScroll();
  openEntrance();
  // entrance: the S settles into its pose on the same curve as the title (ENTER)
  if (!reduced) gsap.to(markEntry, { v: 1, duration: ENTER.duration, ease: ENTER.ease, delay: ENTER.delay, onUpdate: () => placeMark?.() });
}

/** The one load move on the page: the hero title and the S both arrive on it. */
const ENTER = { duration: 1.4, ease: 'expo.out', delay: 0.05 } as const;
/** Opens when the S is ready (or cannot be), at the latest after ENTRANCE_WAIT_MS, so the title and the
 *  S arrive together instead of as two events seconds apart. */
const ENTRANCE_WAIT_MS = 500;
let openEntrance: () => void = () => {};
const entranceOpen = new Promise<void>((resolve) => { openEntrance = resolve; });
window.setTimeout(() => openEntrance(), ENTRANCE_WAIT_MS);
const markEntry = { v: 0 };
let placeMark: (() => void) | null = null;
/** The hero pose, held back by the entrance: a little lower, a few degrees further turned, faded. */
function withEntry(p: Pose): Pose {
  const k = 1 - markEntry.v;
  return { ...p, y: p.y - 0.03 * k, yaw: p.yaw - 4 * k, opacity: p.opacity * markEntry.v };
}

/* ---------------------------------------------------------------- motion */

function wireMarkScroll() {
  if (!mark || !heroStage || !closeStage) return;
  const m = mark;
  let where: 'hero' | 'close' | 'none' | 'unset' = 'unset'; // 'unset': the first place() always applies
  // Placement is derived from the triggers' progress on every update, never from enter/leave toggles:
  // a jump past a whole range (End key, a reload that restores the scroll, an anchor) changes progress
  // without toggling, and left the mark stranded in the hero.
  let hero: ScrollTrigger | null = null;
  let close: ScrollTrigger | null = null;
  const place = () => {
    if (!hero || !close) return; // create() can refresh before both exist
    // the S renders only while one of its shots is on screen (a section that is on screen has progress
    // strictly between 0 and 1; the close ends the page, so its progress never reaches 1)
    const want = close.progress > 0 && close.progress < 1 ? 'close' : hero.progress < 1 ? 'hero' : 'none';
    if (want !== where) {
      where = want;
      // a stopped renderer leaves its last frame on the canvas: hide every stage the S is not in
      heroStage.style.visibility = want === 'hero' ? '' : 'hidden';
      closeStage.style.visibility = want === 'close' ? '' : 'hidden';
      if (want === 'hero') { m.attach(heroStage, 'hero'); m.setPose(withEntry(heroPose()), true); }
      if (want === 'close') {
        m.attach(closeStage, 'close');
        m.setPose(closePose(), true);
        // the one fade, the first time the close comes on screen
        if (closeFade.v === 0) gsap.to(closeFade, { v: 1, duration: 1.2, ease: 'expo.out', onUpdate: place });
      }
      m.setActive(want !== 'none');
    }
    if (where === 'hero') m.setPose(withEntry(heroPose()));
    else if (where === 'close') m.setPose(closePose());
  };
  placeMark = place;
  hero = ScrollTrigger.create({ trigger: '.hero', start: 'top top', end: 'bottom top', onUpdate: place, onRefresh: place });
  close = ScrollTrigger.create({ trigger: '.close', start: 'top bottom', end: 'bottom top', onUpdate: place, onRefresh: place });
  place();
}

// the S's placement uses ScrollTrigger with or without motion (wireMarkScroll)
gsap.registerPlugin(ScrollTrigger);

function wireMotion() {

  const lenis = new Lenis({ lerp: 0.12, smoothWheel: true, anchors: true });
  lenis.on('scroll', ScrollTrigger.update);
  gsap.ticker.add((time) => lenis.raf(time * 1000));
  gsap.ticker.lagSmoothing(0);

  // the arrival: the title rises 16 px and fades in on the S's curve and clock (ENTER, entranceOpen);
  // the lede and the download plate are there from the first frame, so the action is never hidden. The
  // start state is painted from the first frame by .enter (public/enter.js); GSAP takes it over in the
  // same task that drops the class, so nothing paints in between.
  // If this script arrived late and the stylesheet's failsafe has already begun showing the title, it
  // stays shown: hiding it again to replay the arrival would flicker.
  const heroTitle = $<HTMLElement>('.hero__title');
  const stillHidden = heroTitle !== null && root.classList.contains('enter') && getComputedStyle(heroTitle).opacity === '0';
  if (heroTitle && stillHidden) {
    gsap.set(heroTitle, { y: 16, autoAlpha: 0 });
    root.classList.remove('enter');
    void entranceOpen.then(() => gsap.to(heroTitle, { y: 0, autoAlpha: 1, duration: ENTER.duration, ease: ENTER.ease, delay: ENTER.delay }));
  }
  // otherwise .enter stays: the failsafe animation finishes and holds the title at full opacity

  window.addEventListener('load', () => ScrollTrigger.refresh());
  document.fonts?.ready.then(() => ScrollTrigger.refresh());
}

if (!reduced) wireMotion();
// The S starts at once: the title waits for it, at most ENTRANCE_WAIT_MS, so both arrive as one move.
// The 3D chunk (about 130 kB gzipped) loads beside the video, which has its poster on screen meanwhile.
void startMark();
