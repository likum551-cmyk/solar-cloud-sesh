import 'lenis/dist/lenis.css';
import './base.css';
import './styles.css';
import { gsap } from 'gsap';
import { ScrollTrigger } from 'gsap/ScrollTrigger';
import { SplitText } from 'gsap/SplitText';
import Lenis from 'lenis';
import { formatSize, loadRelease, type Release } from './release';
import type { Look, Mark3D, Pose } from './mark3d';
import { browserTokenStore } from './account/store';
import { wireAccountLink } from './account/ui';

// One scroll clock: Lenis smooths the wheel and drives GSAP's ticker; every beat is a ScrollTrigger
// timeline scrubbed by it (SHOTS.md). Without motion (reduced-motion users) nothing pins or scrubs:
// the page is the plain sequence of framed shots the stylesheet lays out, and the mark is drawn once.

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
  if (v.classList.contains('card__video')) continue; // driven by the cards timeline (wireMotion)
  vio.observe(v);
}
// the hero shot is the first thing seen: fetch it now (it ships preload=none so reduced/save-data never do)
const heroVideo = $<HTMLVideoElement>('.hero__video');
if (heroVideo && !reduced && !saveData) heroVideo.preload = 'auto';

/* ---------------------------------------------------------------- shared geometry */

const bar = $<HTMLElement>('.bar');
// the top bar is solid as soon as the page has moved at all: once the letterbox opens, the S, the
// slab and the footage all pass under it, and the nav must never sit on any of them
if (bar) {
  const solid = () => bar.classList.toggle('is-solid', window.scrollY > 0);
  window.addEventListener('scroll', solid, { passive: true });
  solid();
}
const isPhone = () => window.matchMedia('(max-width: 760px)').matches;
const lerp = (a: number, b: number, t: number) => a + (b - a) * t;
const ease = (t: number) => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);
const clamp01 = (t: number) => Math.min(1, Math.max(0, t));

/** The gold slab cut: a band `sw` wide at 45 degrees crosses the frame right to left as p goes 0..1.
 *  Left of the band is the outgoing shot, under it the slab, right of it the incoming shot. */
function cutAt(p: number, W: number, H: number, sw: number) {
  const x = lerp(W + H, -sw, p); // the band's left edge at the top; the bottom runs H behind it
  return {
    outgoing: `polygon(-2px 0, ${x}px 0, ${x - H}px ${H}px, -2px ${H}px)`,
    incoming: `polygon(${x}px 0, ${W + 2}px 0, ${W + 2}px ${H}px, ${x - H}px ${H}px)`,
    slab: `polygon(${x}px 0, ${x + sw}px 0, ${x + sw - H}px ${H}px, ${x - H}px ${H}px)`,
  };
}
const slabW = (W: number) => W * 0.26;

/** Reveals `el` (with its own `.slab`) by the cut at p. */
function revealBySlab(el: HTMLElement, p: number) {
  const W = el.clientWidth;
  const H = el.clientHeight;
  const c = cutAt(p, W, H, slabW(W));
  el.style.clipPath = p >= 1 ? 'none' : c.incoming;
  const s = $<HTMLElement>(':scope > .slab', el);
  if (s) s.style.clipPath = p > 0 && p < 1 ? c.slab : 'polygon(0 0, 0 0, 0 0)';
}

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

// The hero timeline runs HERO_LEN screen heights: the mark's flight and the letterbox take the first
// one, the gold slab hands over to the cards in the rest.
const HERO_LEN = 1.55;
const MARK_GONE = 0.58; // timeline time after which the S is fully faded and stops rendering
const HERO_CUT_AT = 0.72; // the gold slab starts here and takes 0.7: was 1.1, which left half a screen of bare footage
const BAR_H = 64; // --bar-h

/** The hero pose at hero timeline time u. As the copy lifts away the S rises into the haze sun
 *  (SHOTS.md beat 1: lifts toward the sun, about half its size, fades), one long eased move that
 *  starts with the first pixel of scroll; it no longer flies at the camera, which filled the frame
 *  with a half-transparent face (GI, 2026-11-06: fewer, more fluid moves). */
function heroPose(u: number): Pose {
  const t = ease(clamp01((u - 0.04) / 0.5));
  const opacity = 1 - clamp01((u - 0.3) / 0.26);
  const sweep = lerp(-12, 40, clamp01(u / 0.56));
  if (isPhone()) {
    // ends clear of the nav bar (0.66 parked the S's top 12 px under it)
    // starts at 0.28: in front of the towers, overlapping their crowns by a clear margin (0.36, 0.38 and 0.45
    // each left its foot just touching a roof as the drone moves, which read as an accident)
    return { x: lerp(0, 0.14, t), y: lerp(0.28, 0.5, t), height: lerp(0.3, 0.16, t), yaw: lerp(-11, 10, t), pitch: lerp(3, -2, t), opacity, sweep };
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
  // the haze sun sits at 62% / 18% of the frame (.haze): in stage units about x 0.24, y 0.64
  return {
    x: lerp(portrait ? 0.4 : 0.48, 0.3, t),
    y: lerp(portrait ? 0.2 : 0.06 * frame - centre, 0.56, t),
    height: lerp(portrait ? 0.36 : 0.6 * frame, portrait ? 0.2 : 0.33 * frame, t),
    // at rest turned to -11 (MAX_TURN is 12): a side wall and the chamfer catch light, still an S
    yaw: lerp(-11, 10, t),
    pitch: lerp(3, -2, t),
    opacity,
    sweep,
  };
}

/** The closing pose at progress q (0 as the section enters, 1 when it is centred): above the title. */
function closePose(q: number): Pose {
  const t = ease(clamp01(q));
  const opacity = clamp01(q * 1.6);
  const sweep = lerp(40, 4, t);
  if (isPhone()) return { x: 0, y: lerp(0.54, 0.6, t), height: lerp(0.11, 0.14, t), yaw: lerp(10, 0, t), pitch: lerp(-6, 0, t), opacity, sweep };
  return { x: 0, y: lerp(0.47, 0.53, t), height: lerp(0.14, 0.17, t), yaw: lerp(10, 0, t), pitch: lerp(-6, 0, t), opacity, sweep };
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
  if (reduced) {
    mark.setPose(heroPose(0), true);
    mark.setActive(true);
    // one settled frame is enough; stop once the environment has loaded
    setTimeout(() => mark?.setActive(false), 1500);
    return;
  }
  // entrance: the S rises into its hero pose on the same curve as the title (ENTER), from wherever the
  // scroll already is, so arriving and scrolling are one continuous move
  mark.setPose(withEntry(heroPose(0)), true);
  mark.setActive(true);
  wireMarkScroll();
  openEntrance();
  gsap.to(markEntry, { v: 1, duration: ENTER.duration, ease: ENTER.ease, delay: ENTER.delay, onUpdate: () => placeMark?.() });
}

/** The one load move on the page: the hero copy and the S both arrive on it. */
const ENTER = { duration: 1.8, ease: 'expo.out', delay: 0.05 } as const;
/** Opens when the S is ready (or cannot be), at the latest after ENTRANCE_WAIT_MS, so the copy and the
 *  S start their arrival together instead of as two events seconds apart. */
const ENTRANCE_WAIT_MS = 900;
let openEntrance: () => void = () => {};
const entranceOpen = new Promise<void>((resolve) => { openEntrance = resolve; });
window.setTimeout(() => openEntrance(), ENTRANCE_WAIT_MS);
const markEntry = { v: 0 };
let placeMark: (() => void) | null = null;
/** The hero pose, held back by the entrance: lower, turned a little further, faded. */
function withEntry(p: Pose): Pose {
  const k = 1 - markEntry.v;
  return { ...p, y: p.y - 0.08 * k, yaw: p.yaw - 8 * k, opacity: p.opacity * markEntry.v };
}

/* ---------------------------------------------------------------- motion */

function wireMarkScroll() {
  if (!mark || !heroStage || !closeStage) return;
  const m = mark;
  let where: 'hero' | 'close' | 'none' | 'unset' = 'unset'; // 'unset': the first place() always applies
  // Placement is derived from both triggers' progress on every update, never from enter/leave
  // toggles: a jump past a whole range (End key, a reload that restores the scroll, an anchor)
  // changes progress without toggling, and left the mark stranded in the hero.
  let hero: ScrollTrigger | null = null;
  let close: ScrollTrigger | null = null;
  let out: ScrollTrigger | null = null;
  const closeAt = (cp: number): Pose => {
    const pose = closePose(cp);
    // past centre the S leaves with its section: faded out before it can climb into the top bar
    return { ...pose, opacity: pose.opacity * (1 - clamp01((out?.progress ?? 0) * 2)) };
  };
  const place = () => {
    if (!hero || !close) return; // create() can refresh before both exist
    const u = hero.progress * HERO_LEN;
    const cp = close.progress;
    const want = cp > 0 ? 'close' : u < MARK_GONE ? 'hero' : 'none';
    if (want !== where) {
      where = want;
      // a stopped renderer leaves its last frame on the canvas: hide every stage the S is not in
      heroStage.style.visibility = want === 'hero' ? '' : 'hidden';
      closeStage.style.visibility = want === 'close' ? '' : 'hidden';
      if (want === 'hero') { m.attach(heroStage, 'hero'); m.setPose(withEntry(heroPose(u)), true); }
      if (want === 'close') { m.attach(closeStage, 'close'); m.setPose(closeAt(cp), true); }
      m.setActive(want !== 'none');
    }
    if (where === 'hero') m.setPose(withEntry(heroPose(u)));
    else if (where === 'close') m.setPose(closeAt(cp));
  };
  placeMark = place;
  hero = ScrollTrigger.create({
    trigger: '.hero',
    start: 'top top',
    end: () => `+=${window.innerHeight * HERO_LEN}`,
    onUpdate: place,
    onRefresh: place,
  });
  close = ScrollTrigger.create({ trigger: '.close', start: 'top bottom', end: 'center center', onUpdate: place, onRefresh: place });
  // the page ends before the close's centre can reach the top: fade out over whatever scroll is left
  out = ScrollTrigger.create({ trigger: '.close', start: 'center center', end: 'max', onUpdate: place, onRefresh: place });
  place();
}

function wireMotion() {
  gsap.registerPlugin(ScrollTrigger, SplitText);
  root.classList.add('motion');

  const lenis = new Lenis({ lerp: 0.09, smoothWheel: true, anchors: true });
  lenis.on('scroll', ScrollTrigger.update);
  gsap.ticker.add((time) => lenis.raf(time * 1000));
  gsap.ticker.lagSmoothing(0);

  /* 1. cold open: letterbox opens, footage pushes in, copy leaves, the S flies past the camera
        (wireMarkScroll); then a gold slab cuts to the beach, which IS the first card's shot */
  const front = $<HTMLElement>('.hero__front')!;
  const heroSlab = $<HTMLElement>('.slab--hero')!;
  const heroNext = $<HTMLElement>('.hero__next')!;
  const heroCut = { p: 0 };
  const drawHeroCut = () => {
    const W = front.clientWidth;
    const H = front.clientHeight;
    const c = cutAt(heroCut.p, W, H, slabW(W));
    front.style.clipPath = heroCut.p <= 0 ? 'none' : c.outgoing;
    heroSlab.style.clipPath = heroCut.p > 0 && heroCut.p < 1 ? c.slab : 'polygon(0 0, 0 0, 0 0)';
  };
  // GI, 2026-11-06: fewer animations, more fluid. The hero has exactly two moves: one arrival on load
  // (copy and S on the same expo-out curve, ENTER) and one continuous scroll move (footage drifts in,
  // the copy lifts away as one block, the bars open), all scrubbed on Lenis' smoothed scroll.
  const hero = gsap.timeline({
    defaults: { ease: 'none' },
    scrollTrigger: {
      trigger: '.hero',
      start: 'top top',
      end: () => `+=${window.innerHeight * HERO_LEN}`,
      scrub: true,
      pin: '.hero__stage',
      anticipatePin: 1,
      invalidateOnRefresh: true,
      onRefresh: drawHeroCut,
      // the cards stage lies exactly under the finished hero: the still steps aside for the live shot
      onLeave: () => gsap.set(heroNext, { autoAlpha: 0 }),
      onEnterBack: () => gsap.set(heroNext, { autoAlpha: 1 }),
    },
  });
  hero
    // the footage drifts in and up over the whole hero: one slow push, never a stop
    .fromTo('.hero__video', { scale: 1, yPercent: 0 }, { scale: 1.1, yPercent: -2.5, duration: HERO_CUT_AT + 0.7, ease: 'none' }, 0)
    // the copy lifts away as one block (the parent: the load move owns its children), gone before the
    // S heads for the camera so nothing ghosts across it
    .to('.hero__copy', { y: () => -0.14 * window.innerHeight, autoAlpha: 0, duration: 0.3, ease: 'power1.in' }, 0.02)
    // the credits live on the bottom bar: they leave before the shrinking bar can uncover them
    .to('.hero__credits', { autoAlpha: 0, duration: 0.12, ease: 'power1.out' }, 0)
    // the bars open with it, before the S moves: an empty bottom bar cropped the gold hook
    .to('.letterbox--top, .letterbox--bottom', { scaleY: 0, duration: 0.34, ease: 'power1.inOut' }, 0)
    .to('.hero__shade', { opacity: 0.35, duration: 0.7, ease: 'none' }, 0.3)
    // the slab cut starts as the S finishes fading (MARK_GONE), so no screen of bare footage waits between
    .to(heroCut, { p: 1, duration: 0.7, ease: 'power2.inOut', onUpdate: drawHeroCut }, HERO_CUT_AT)
    .fromTo(heroNext, { scale: 1.14 }, { scale: 1.06, duration: 0.8, ease: 'power2.out' }, HERO_CUT_AT)
    .to({}, { duration: HERO_LEN - HERO_CUT_AT - 0.7 }, HERO_CUT_AT + 0.7);

  // the load move: the title rises and sharpens on the S's curve and clock (ENTER, entranceOpen); the
  // letterbox and the credits band (lede, download plate) are simply there from the first frame, so the
  // action is never a blur
  const heroTitle = '.hero__title';
  gsap.set(heroTitle, { y: 36, autoAlpha: 0, filter: 'blur(10px)' });
  void entranceOpen.then(() =>
    gsap.to(heroTitle, { y: 0, autoAlpha: 1, filter: 'blur(0px)', duration: ENTER.duration, ease: ENTER.ease, delay: ENTER.delay, clearProps: 'filter' }),
  );

  /* 2. title cards: the beach is already on screen; each next shot is cut in by the gold slab */
  const cards = $$<HTMLElement>('.card');
  const titleSplits = cards.map((c) => new SplitText($('.card__title', c)!, { type: 'words,chars', mask: 'words', aria: 'auto' }));
  // one card video at a time: the top revealed card plays while the cards stage is pinned
  const cardVideos = cards.map((c) => $<HTMLVideoElement>('.card__video', c));
  const shown: number[] = cards.map((_, i) => (i === 0 ? 1 : 0));
  let cardsOn = false;
  const syncCardVideos = () => {
    let top = 0;
    shown.forEach((p, i) => { if (p > 0.5) top = i; });
    cardVideos.forEach((v, i) => {
      if (!v || saveData) return;
      if (cardsOn && i === top) { if (v.paused) { v.preload = 'auto'; v.play().catch(() => {}); } }
      else if (!v.paused) v.pause();
    });
  };
  const cardsTl = gsap.timeline({
    defaults: { ease: 'none' },
    scrollTrigger: {
      trigger: '.cards',
      start: 'top top',
      end: () => `+=${window.innerHeight * 2.95}`,
      scrub: 0.6,
      pin: '.cards__stage',
      anticipatePin: 1,
      onToggle: (st) => { cardsOn = st.isActive; syncCardVideos(); },
      onRefresh: () => cards.forEach((c, i) => { if (i > 0) revealBySlab(c, shown[i] ?? 0); }),
    },
  });
  cards.forEach((card, i) => {
    const at = i === 0 ? 0 : 0.75 + (i - 1) * 1.1; // card 1 (the main claim) holds readable first
    const video = $('.card__video', card);
    if (i > 0) {
      const st = { p: 0 };
      revealBySlab(card, 0);
      cardsTl.to(st, { p: 1, duration: 0.6, ease: 'power2.inOut', onUpdate: () => { revealBySlab(card, st.p); shown[i] = st.p; syncCardVideos(); } }, at);
      cardsTl.fromTo(video, { scale: 1.18 }, { scale: 1, duration: 0.9, ease: 'power2.out' }, at);
    } else {
      // continues the hero still's push (1.06), so the cut from still to footage does not jump
      cardsTl.fromTo(video, { scale: 1.06 }, { scale: 1, duration: 0.6 }, 0);
    }
    const split = titleSplits[i]!;
    cardsTl.from(split.chars, { yPercent: 115, duration: 0.3, stagger: 0.012, ease: 'power3.out' }, at + (i === 0 ? 0 : 0.36));
    cardsTl.from($('.card__fact', card), { y: 30, autoAlpha: 0, duration: 0.25, ease: 'power2.out' }, at + (i === 0 ? 0.12 : 0.5));
  });
  cardsTl.to({}, { duration: 0.35 }); // hold the last card

  /* 3. cast: each key-art panel opens as a window on the face, then fills the screen; the panel
        before it dims and its line leaves first, so two lines never share the frame */
  const panels = $$<HTMLElement>('.panel');
  /** data-fx/fy is the face as a fraction of the key art; object-fit: cover crops it differently on
   *  every screen shape, so map it through the crop to panel pixels. */
  const focusOf = (panel: HTMLElement, img: HTMLImageElement) => {
    const W = panel.clientWidth;
    const H = panel.clientHeight;
    const nw = img.naturalWidth || Number(img.getAttribute('width')) || W;
    const nh = img.naturalHeight || Number(img.getAttribute('height')) || H;
    const s = Math.max(W / nw, H / nh);
    const [px, py] = getComputedStyle(img).objectPosition.split(' ').map((v) => (v.endsWith('%') ? parseFloat(v) / 100 : 0.5));
    return {
      x: (W - nw * s) * (px ?? 0.5) + Number(panel.dataset.fx ?? 0.5) * nw * s,
      y: (H - nh * s) * (py ?? 0.5) + Number(panel.dataset.fy ?? 0.4) * nh * s,
    };
  };
  const frameOf = (panel: HTMLElement, g: number) => {
    const W = panel.clientWidth;
    const H = panel.clientHeight;
    const img = $<HTMLImageElement>('img', panel)!;
    const { x: fx, y: fy } = focusOf(panel, img);
    img.style.transformOrigin = `${(fx / W) * 100}% ${(fy / H) * 100}%`;
    const fw = Math.min(W * (isPhone() ? 0.78 : 0.4), 640);
    const fh = Math.min((H - BAR_H) * 0.66, 680);
    // the window frames the head in its upper third, clear of the top bar and inside the frame
    const l0 = Math.min(Math.max(fx - fw / 2, 0), W - fw);
    const t0 = Math.min(Math.max(fy - fh * 0.33, BAR_H), H - fh);
    const l = lerp(l0, 0, g);
    const r = lerp(l0 + fw, W, g);
    const t = lerp(t0, 0, g);
    const b = lerp(t0 + fh, H, g);
    const c = lerp(36, 0, g);
    // Only the art is clipped (the line stays on the black around the window). The art also pushes
    // out from 1.3 around the face, and clip-path applies before the transform, so the window is
    // mapped back into the image's own, unscaled coordinates.
    const s = 1 + 0.3 * (1 - g);
    img.style.transform = `scale(${s})`;
    const lx = (x: number) => fx + (x - fx) / s;
    const ly = (y: number) => fy + (y - fy) / s;
    img.style.clipPath = g >= 1 ? 'none'
      : `polygon(${lx(l)}px ${ly(t)}px, ${lx(r - c)}px ${ly(t)}px, ${lx(r)}px ${ly(t + c)}px, ${lx(r)}px ${ly(b)}px, ${lx(l)}px ${ly(b)}px)`;
  };
  const grown: number[] = panels.map(() => 0);
  const castTl = gsap.timeline({
    defaults: { ease: 'none' },
    scrollTrigger: {
      trigger: '.cast',
      start: 'top top',
      end: () => `+=${window.innerHeight * 2.8}`,
      scrub: 0.6,
      pin: '.cast__stage',
      anticipatePin: 1,
      onRefresh: () => panels.forEach((p, i) => frameOf(p, grown[i] ?? 0)),
    },
  });
  panels.forEach((panel, i) => {
    const at = i * 1.0;
    const st = { g: 0 };
    frameOf(panel, 0);
    const img = $<HTMLImageElement>('img', panel)!;
    if (!img.complete) img.addEventListener('load', () => frameOf(panel, grown[i] ?? 0), { once: true });
    const line = new SplitText($('.panel__line', panel)!, { type: 'words,chars', mask: 'words', aria: 'auto' });
    const prev = panels[i - 1];
    if (prev) {
      castTl.to($('.panel__line', prev), { autoAlpha: 0, y: -24, duration: 0.14, ease: 'power2.in' }, at - 0.12);
      // the previous panel goes once the new window is half open, so bodies never stack
      castTl.to(prev, { autoAlpha: 0, duration: 0.2, ease: 'power1.out' }, at + 0.3);
      castTl.set(panel, { visibility: 'visible' }, at);
      castTl.fromTo(panel, { autoAlpha: 0 }, { autoAlpha: 1, duration: 0.1, ease: 'power2.out' }, at);
    }
    castTl.to(st, { g: 1, duration: 0.55, ease: 'power3.inOut', onUpdate: () => { grown[i] = st.g; frameOf(panel, st.g); } }, at + 0.12);
    // the line arrives with the window, so the black around it always carries type
    castTl.from(line.chars, { yPercent: 115, duration: 0.28, stagger: 0.01, ease: 'power3.out' }, at + 0.04);
  });
  castTl.to({}, { duration: 0.3 });

  /* 4. servers arrive */
  gsap.from('.server', { y: 60, autoAlpha: 0, duration: 1, ease: 'expo.out', stagger: 0.08, scrollTrigger: { trigger: '.servers__grid', start: 'top 82%' } });

  /* 5. get in: on wide screens one pinned launcher window whose screen the slab cuts from step to step */
  const mm = gsap.matchMedia();
  mm.add('(min-width: 901px) and (min-height: 680px)', () => {
    const steps = $$<HTMLElement>('.step');
    const figs = steps.map((s) => $<HTMLElement>('.step__fig', s)!);
    const added: HTMLElement[] = [];
    const cut: number[] = figs.map((_, i) => (i === 0 ? 1 : 0));
    let on = -1;
    const setOn = (k: number) => {
      if (k === on) return;
      on = k;
      steps.forEach((s, i) => s.classList.toggle('is-on', i === k));
      const n = $('.step__n', steps[k]!);
      if (n) gsap.fromTo(n, { yPercent: 22, opacity: 0 }, { yPercent: 0, opacity: 1, duration: 0.6, ease: 'expo.out', overwrite: true });
    };
    figs.forEach((fig, i) => {
      if (i === 0) return;
      const s = document.createElement('div');
      s.className = 'slab';
      s.setAttribute('aria-hidden', 'true');
      fig.append(s);
      added.push(s);
      revealBySlab(fig, 0);
    });
    const tl = gsap.timeline({
      defaults: { ease: 'none' },
      scrollTrigger: {
        trigger: '.getin__stage',
        start: 'top top',
        end: () => `+=${window.innerHeight * 2.2}`,
        scrub: 0.6,
        pin: true,
        anticipatePin: 1,
        onUpdate: (st) => setOn(Math.min(steps.length - 1, Math.floor(st.progress * 3.0 + 0.02))),
        onRefresh: (st) => { figs.forEach((f, i) => { if (i > 0) revealBySlab(f, cut[i] ?? 0); }); setOn(Math.min(steps.length - 1, Math.floor(st.progress * 3.0 + 0.02))); },
      },
    });
    figs.forEach((fig, i) => {
      if (i === 0) return;
      const st = { p: 0 };
      tl.to(st, { p: 1, duration: 0.45, ease: 'power2.inOut', onUpdate: () => { cut[i] = st.p; revealBySlab(fig, st.p); } }, i - 0.22);
    });
    tl.to({}, { duration: 0.01 }, 3.0);
    return () => {
      added.forEach((s) => s.remove());
      figs.forEach((f) => { f.style.clipPath = ''; });
      steps.forEach((s) => s.classList.remove('is-on'));
    };
  });
  mm.add('(max-width: 900px), (max-height: 679px)', () => {
    for (const fig of $$<HTMLElement>('.step__fig')) {
      gsap.from(fig, { y: 50, autoAlpha: 0, duration: 1, ease: 'expo.out', scrollTrigger: { trigger: fig, start: 'top 88%' } });
    }
  });

  /* 6. close: the title rises */
  const closeSplit = new SplitText('.close__title', { type: 'words,chars', mask: 'words', aria: 'auto' });
  gsap.from(closeSplit.chars, { yPercent: 115, duration: 1, ease: 'expo.out', stagger: 0.02, scrollTrigger: { trigger: '.close', start: 'top 55%' } });

  window.addEventListener('load', () => ScrollTrigger.refresh());
  document.fonts?.ready.then(() => ScrollTrigger.refresh());
}

if (!reduced) wireMotion();
// The S starts at once (it used to wait up to 2.5 s for the hero video's first frame): the copy now waits
// for it, at most ENTRANCE_WAIT_MS, so both arrive as one move. The 3D chunk (about 130 kB gzipped)
// loads beside the video, which has its poster on screen meanwhile.
void startMark();
