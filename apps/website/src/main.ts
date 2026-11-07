import './base.css';
import './styles.css';
import { formatIssued, formatSize, loadRelease, splitNotes, type Release } from './release';
import { describeAge, describeStatus, findServer, loadFraction, loadServers, STATIC_STATUS, type ServerInfo } from './servers';
import { browserTokenStore } from './account/store';
import { wireAccountLink } from './account/ui';

// GI, 2026-11-07: "Launcher product page" (like fivem.net / alt:V). This script fills in the live facts: the
// release behind every download button, and the hero's server list, which is the launcher's own list,
// refreshed every 20 s while the tab is visible. Plus the account links, the bar's solid state and the
// one-time row reveals. No video, no WebGL; reduced motion runs no animation.
const SITE_HOST = 'solar-rp.com';
const $ = <T extends Element>(sel: string, el: ParentNode = document) => el.querySelector<T>(sel);
const $$ = <T extends Element>(sel: string, el: ParentNode = document) => Array.from(el.querySelectorAll<T>(sel));
const MOTION = !window.matchMedia('(prefers-reduced-motion: reduce)').matches;

/* ---------------------------------------------------------------- reveals */

if (MOTION && 'IntersectionObserver' in window) {
  // .rv hides the rows below the fold until they scroll in; set here, so a page whose script never ran
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

/* ---------------------------------------------------------------- release / download */

const NEWS_SHOWN = 5;

function showRelease(r: Release | null) {
  const buttons = $$<HTMLAnchorElement>('.js-download');
  const status = $<HTMLElement>('.js-release-status');
  const label = (a: HTMLElement) => $<HTMLElement>('.js-download-label', a) ?? a;
  const text = (a: HTMLElement) => (a.classList.contains('btn--sm') ? 'Download' : 'Download for Windows');
  if (!r) {
    // The release feed did not answer or did not check out (releases ARE published: this is a fetch or
    // feed problem, never "no build yet"). Every control leads to the download section, which says so.
    for (const a of buttons) {
      a.href = '#download';
      a.removeAttribute('download');
      label(a).textContent = text(a);
      a.classList.add('is-soon'); // outlined, not gold: there is no file to hand out right now
    }
    $<HTMLAnchorElement>('.js-download-main')?.setAttribute('aria-disabled', 'true');
    for (const m of $$<HTMLElement>('.js-release-meta')) m.textContent = 'Windows 10 and 11 · free download';
    if (status) status.textContent = 'The download link could not be loaded. Reload the page to try again.';
    return;
  }
  for (const a of buttons) {
    a.href = r.url;
    a.setAttribute('download', '');
    a.removeAttribute('aria-disabled');
    label(a).textContent = text(a);
    a.classList.remove('is-soon');
  }
  for (const m of $$<HTMLElement>('.js-release-meta')) m.textContent = `Windows 10 and 11 · v${r.version} · ${formatSize(r.size)} · free`;
  for (const v of $$<HTMLElement>('.js-release-version')) v.textContent = `v${r.version}`;
  const hash = $<HTMLElement>('.js-release-hash');
  const sha = $<HTMLElement>('.js-sha');
  if (hash && sha) {
    sha.textContent = r.sha256;
    hash.hidden = false;
  }
  // the signed release notes, the same text the launcher shows; textContent only (never parsed as markup)
  // as a list: the first NEWS_SHOWN items, the rest behind "N more", so long notes never stretch the panel
  const news = $<HTMLElement>('.js-news');
  const items = splitNotes(r.notes);
  if (news && items.length > 0) {
    const title = $<HTMLElement>('.js-news-title', news);
    const date = $<HTMLTimeElement>('.js-news-date', news);
    if (title) title.textContent = `What's new in v${r.version}`;
    if (date) {
      date.textContent = r.issued ? formatIssued(r.issued) : '';
      if (r.issued) date.dateTime = r.issued.toISOString();
    }
    const fill = (ul: HTMLElement | null, list: string[]) => {
      if (!ul) return;
      ul.replaceChildren(...list.map((t) => Object.assign(document.createElement('li'), { textContent: t })));
    };
    fill($<HTMLElement>('.js-news-list', news), items.slice(0, NEWS_SHOWN));
    const rest = items.slice(NEWS_SHOWN);
    const more = $<HTMLElement>('.js-news-more', news);
    if (more) {
      more.hidden = rest.length === 0;
      fill($<HTMLElement>('.js-news-rest', news), rest);
      const label = $<HTMLElement>('.js-news-more-label', news);
      if (label) label.textContent = `${rest.length} more`;
    }
    news.hidden = false;
  }
  if (status) status.textContent = '';
}

loadRelease(SITE_HOST)
  .then(showRelease)
  .catch(() => showRelease(null));

/* ---------------------------------------------------------------- the live server list */

const row = $<HTMLElement>('.js-status');
const age = $<HTMLElement>('.js-srv-age');
const key = row?.dataset.server ?? 'sol';
const POLL_MS = 20_000;
let checkedAt = 0; // when the list last answered (0 = never)

function showServer(s: ServerInfo | undefined, answered: boolean) {
  if (!row) return;
  const v = answered ? describeStatus(s) : STATIC_STATUS;
  const set = (sel: string, text: string, el: ParentNode = document) => {
    const n = $<HTMLElement>(sel, el);
    if (n) n.textContent = text;
  };
  $<HTMLElement>('.js-status-state', row)?.setAttribute('data-tone', v.tone);
  set('.js-status-label', v.label, row);
  set('.js-status-note', v.note);
  // the Players fact exists only while the server reports counts; nothing stands in for a number. The big
  // "N playing right now" line and its capacity bar show only once someone is on (a big "0 / 1024" under an
  // H1 that says everyone is a player read as a ghost town)
  const players = $<HTMLElement>('.js-status-players');
  const frac = answered ? loadFraction(s) : null;
  if (players) players.hidden = v.count === null;
  set('.js-status-count', v.count ?? '');
  const busy = $<HTMLElement>('.js-status-busy');
  if (busy) busy.hidden = frac === null || frac === 0 || !s;
  set('.js-status-busy-count', s?.players != null ? String(s.players) : '');
  $<HTMLElement>('.js-status-fill')?.style.setProperty('--load', frac === null ? '0' : String(Math.max(frac, 0.015)));
  if (answered) checkedAt = Date.now();
  tickAge();
}

function tickAge() {
  if (age) age.textContent = checkedAt ? describeAge(Date.now() - checkedAt) : 'Live status';
}

async function refresh() {
  try {
    const list = await loadServers();
    showServer(list ? findServer(list, key) : undefined, list !== null);
  } catch {
    showServer(undefined, false);
  }
}

let poll = 0;
function schedule() {
  window.clearInterval(poll);
  if (document.visibilityState === 'visible') poll = window.setInterval(refresh, POLL_MS);
}
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible' && Date.now() - checkedAt > POLL_MS) void refresh();
  schedule();
});
void refresh();
schedule();
window.setInterval(tickAge, 1000);

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
