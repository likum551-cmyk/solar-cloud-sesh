import './base.css';
import './styles.css';
import { formatSize, loadRelease, type Release } from './release';
import { describeStatus, findServer, loadServers, STATIC_STATUS, type StatusView } from './servers';
import { browserTokenStore } from './account/store';
import { wireAccountLink } from './account/ui';

// GI, 2026-11-06: the trailer page ("remove this disgusting 3D S") and then the quiet hub ("grim asf", "it
// needs more anim") are gone; the page is a warm "big RP project" home that moves. This script fills in the
// live facts (the release and SOL's status), the account links and the bar's solid state, and runs the
// motion that needs a script: reveals as blocks scroll in, ticking numbers, the hero stack following the
// pointer. Everything else moves in CSS (styles.css). No video, no WebGL; reduced motion runs none of it.
const SITE_HOST = 'solar-rp.com';
const $ = <T extends Element>(sel: string, el: ParentNode = document) => el.querySelector<T>(sel);
const $$ = <T extends Element>(sel: string, el: ParentNode = document) => Array.from(el.querySelectorAll<T>(sel));
const MOTION = !window.matchMedia('(prefers-reduced-motion: reduce)').matches;

/* ---------------------------------------------------------------- numbers that tick up */

function showCount(el: HTMLElement, to: number) {
  const dp = Number(el.dataset.dp ?? 0);
  if (!MOTION) {
    el.textContent = to.toFixed(dp);
    return;
  }
  const start = performance.now();
  const ms = 1600;
  const step = (now: number) => {
    const t = Math.min(1, (now - start) / ms);
    const eased = 1 - Math.pow(1 - t, 4);
    el.textContent = (to * eased).toFixed(dp);
    if (t < 1) requestAnimationFrame(step);
  };
  requestAnimationFrame(step);
}

// counters tick when they come into view (the markup already holds the final value, so a page without
// this script, or one that never scrolls there, still reads right)
const counted = new WeakSet<Element>();
function countWhenSeen(els: HTMLElement[]) {
  if (!MOTION || !('IntersectionObserver' in window)) return;
  const io = new IntersectionObserver((entries) => {
    for (const e of entries) {
      if (!e.isIntersecting || counted.has(e.target)) continue;
      counted.add(e.target);
      io.unobserve(e.target);
      const el = e.target as HTMLElement;
      showCount(el, Number(el.dataset.to ?? 0));
    }
  }, { threshold: 0.6 });
  for (const el of els) io.observe(el);
}
countWhenSeen($$<HTMLElement>('.js-count'));

/* ---------------------------------------------------------------- reveals */

if (MOTION && 'IntersectionObserver' in window) {
  // .rv hides the blocks below the fold until they scroll in; set here, so a page whose script never ran
  // keeps everything visible
  document.documentElement.classList.add('rv');
  const io = new IntersectionObserver((entries) => {
    for (const e of entries) {
      if (!e.isIntersecting) continue;
      e.target.classList.add('in');
      io.unobserve(e.target);
    }
  }, { threshold: 0.04, rootMargin: '0px 0px -8% 0px' });
  for (const el of $$('[data-reveal]')) io.observe(el);
}

/* ---------------------------------------------------------------- hero stack follows the pointer */

const stack = $<HTMLElement>('.stack');
if (stack && MOTION && window.matchMedia('(pointer: fine)').matches) {
  let raf = 0;
  let px = 0;
  let py = 0;
  window.addEventListener('pointermove', (ev) => {
    px = (ev.clientX / window.innerWidth - 0.5) * 2;
    py = (ev.clientY / window.innerHeight - 0.5) * 2;
    if (raf) return;
    raf = requestAnimationFrame(() => {
      raf = 0;
      if (window.scrollY > window.innerHeight) return; // the hero is off screen
      stack.style.setProperty('--px', px.toFixed(3));
      stack.style.setProperty('--py', py.toFixed(3));
    });
  }, { passive: true });
}

/* ---------------------------------------------------------------- release / download */

function showRelease(r: Release | null) {
  const plates = $$<HTMLAnchorElement>('.js-download');
  const status = $<HTMLElement>('.js-release-status');
  const label = (a: HTMLElement) => $<HTMLElement>('.js-download-label', a) ?? a;
  if (!r) {
    // The release feed did not answer or did not check out (releases ARE published: this is a fetch or
    // feed problem, never "no build yet"). Every control leads to the download section, which says so.
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
  for (const v of $$<HTMLElement>('.js-release-version')) v.textContent = `Solar RP ${r.version}`;  const hash = $<HTMLElement>('.js-release-hash');
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

/* ---------------------------------------------------------------- SOL's live status */

function showStatus(v: StatusView) {
  const panel = $<HTMLElement>('.js-status');
  if (!panel) return;
  $<HTMLElement>('.js-status-state', panel)?.setAttribute('data-tone', v.tone);
  const set = (sel: string, text: string, el: ParentNode = panel) => {
    const n = $<HTMLElement>(sel, el);
    if (n) n.textContent = text;
  };
  set('.js-status-label', v.label);
  set('.js-status-note', v.note);
  // the Players row exists only while the server reports a count; nothing stands in for a number
  const players = $<HTMLElement>('.js-status-players', panel);
  if (players) players.hidden = v.count === null;
  set('.js-status-count', v.count ?? '');
  set('.js-srv-state', v.count !== null ? `Online, ${v.count} players` : v.label, document);
  $<HTMLElement>('.js-srv-state')?.parentElement?.setAttribute('data-tone', v.tone);
}

const key = $<HTMLElement>('.js-status')?.dataset.server ?? 'sol';
loadServers()
  .then((list) => showStatus(list ? describeStatus(findServer(list, key)) : STATIC_STATUS))
  .catch(() => showStatus(STATIC_STATUS));

/* ---------------------------------------------------------------- account links, bar */

// "Log in" for visitors, "Account" when this browser holds a session (src/account/store.ts); the
// guest-only and member-only links (.js-foot-guest / .js-foot-member) follow the same session
wireAccountLink(browserTokenStore());

// the top bar turns solid as soon as the page has moved, so the nav never sits on a busy picture
const bar = $<HTMLElement>('.bar');
if (bar) {
  const solid = () => bar.classList.toggle('is-solid', window.scrollY > 8);
  window.addEventListener('scroll', solid, { passive: true });
  solid();
}
