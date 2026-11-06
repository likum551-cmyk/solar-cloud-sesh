import './base.css';
import './styles.css';
import { formatSize, loadRelease, type Release } from './release';
import { describeStatus, findServer, loadServers, STATIC_STATUS, type StatusView } from './servers';
import { browserTokenStore } from './account/store';
import { wireAccountLink } from './account/ui';

// GI, 2026-11-06, after three rounds of a trailer page: "FULL REDESIGN, remove this disgusting 3D S". The
// landing page is a plain community hub now: no video, no 3D, no scroll-driven motion. This script fills
// in the two live facts (the release and SOL's status), the account links and the bar's solid state.
// The only motion is the hero's arrival, done in CSS (styles.css, .enter).
const SITE_HOST = 'solar-rp.com';
const $ = <T extends Element>(sel: string, el: ParentNode = document) => el.querySelector<T>(sel);
const $$ = <T extends Element>(sel: string, el: ParentNode = document) => Array.from(el.querySelectorAll<T>(sel));

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
  for (const v of $$<HTMLElement>('.js-release-version')) v.textContent = `Solar RP ${r.version}`;
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
