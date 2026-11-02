import { useEffect, useRef, useState } from 'react';
import { Lockup } from './Brand.jsx';
import { splashSequence } from './gta.jsx';
import { Spinner } from './ui.jsx';

// Long enough to read as a loading screen, never long enough to be in the way.
const MIN_MS = 2200;
const MAX_MS = 5000;
// once the list is in, wait this long at most for its pings before lifting
const PING_GRACE_MS = 1600;
const PINGED_ENOUGH = 0.9;
// One painting holds for a full push (a fast start sees only it); a slow one
// reaches the next. Never lifts mid-crossfade: two half-faded leads would
// double-expose on the way out.
const SLIDE_MS = 3400;
const FADE_MS = 900; // styles.css .splash-art transition
const TIP_MS = 2600;
const LEAVE_MS = 480; // styles.css .splash: 200 ms to black, then 260 ms

// The game's loading screens carry a hint under the title; these are the
// launcher's own, each a thing it really does.
const TIPS = [
  'Press F on a server to keep it in Favorites.',
  'Right-click a server to connect, favorite it or copy its address.',
  'Ctrl+F searches the server list by name, address, game mode or language.',
  'Q and E move between Servers, Favorites, Recents and Settings.',
  'Double-click a server, or press Enter on it, to join.',
  'Know the address? Direct connect takes host:port.',
  'Closing the window keeps GAGE-MP running in the tray.',
];

const fmt = (n) => n.toLocaleString('en-US');

// a number that counts up to `to` once it is known (the game's cash counter)
function useCountUp(to, ms = 650) {
  const [v, setV] = useState(0);
  useEffect(() => {
    if (!(to > 0)) return undefined;
    const reduce = window.matchMedia?.('(prefers-reduced-motion: reduce)').matches;
    if (reduce) {
      setV(to);
      return undefined;
    }
    let raf = 0;
    const t0 = performance.now();
    const step = (t) => {
      const k = Math.min(1, (t - t0) / ms);
      setV(Math.round(to * (1 - Math.pow(1 - k, 3))));
      if (k < 1) raf = requestAnimationFrame(step);
    };
    raf = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf);
  }, [to, ms]);
  return v;
}

/**
 * The start-up loading screen, as the game's: the three leads' key art
 * crossfading with a slow push, the lockup wiping in with a hint under it
 * (hints rotate), and bottom right what it is really doing: loading the
 * list, then measuring ping, then the totals. A thin bar on the bottom edge
 * follows the real ping progress. Lifts once most pings are in (or after
 * MAX_MS); any key or click lifts it at once. The window buttons stay above
 * it, so the window can always be moved or closed.
 *
 * `list`: { status: 'loading'|'ok'|'error', total, players, pinged, toPing }
 */
export default function Splash({ art, list }) {
  const [phase, setPhase] = useState('in'); // in -> leaving -> gone
  const [minDone, setMinDone] = useState(false);
  const [slides] = useState(() => splashSequence(art));
  const [slide, setSlide] = useState(0);
  const [tip, setTip] = useState(() => Math.floor(Math.random() * TIPS.length));
  const [listAt, setListAt] = useState(0); // when the list settled
  const [graceDone, setGraceDone] = useState(false);
  const [fading, setFading] = useState(false);
  const leave = useRef(() => {});
  leave.current = () => setPhase((p) => (p === 'in' ? 'leaving' : p));

  const settled = list.status !== 'loading';
  const pingFrac = list.toPing > 0 ? list.pinged / list.toPing : 1;

  useEffect(() => {
    const min = window.setTimeout(() => setMinDone(true), MIN_MS);
    const max = window.setTimeout(() => leave.current(), MAX_MS);
    const skip = () => leave.current();
    window.addEventListener('keydown', skip);
    window.addEventListener('pointerdown', skip);
    return () => {
      window.clearTimeout(min);
      window.clearTimeout(max);
      window.removeEventListener('keydown', skip);
      window.removeEventListener('pointerdown', skip);
    };
  }, []);

  // key art and hints keep moving while it waits (the art holds still for
  // reduced motion: a hard cut every two seconds is worse than a fade)
  useEffect(() => {
    const still = window.matchMedia?.('(prefers-reduced-motion: reduce)').matches;
    const s = still ? 0 : window.setInterval(() => setSlide((i) => (i + 1) % slides.length), SLIDE_MS);
    const t = window.setInterval(() => setTip((i) => (i + 1) % TIPS.length), TIP_MS);
    return () => {
      window.clearInterval(s);
      window.clearInterval(t);
    };
  }, [slides.length]);

  useEffect(() => {
    if (slide === 0) return undefined;
    setFading(true);
    const f = window.setTimeout(() => setFading(false), FADE_MS);
    return () => window.clearTimeout(f);
  }, [slide]);

  useEffect(() => {
    if (!settled || listAt) return undefined;
    setListAt(performance.now());
    const g = window.setTimeout(() => setGraceDone(true), PING_GRACE_MS);
    return () => window.clearTimeout(g);
  }, [settled, listAt]);

  // lift with the pings mostly in, so the list opens measured (a key, a
  // click or MAX_MS still lift it at once)
  useEffect(() => {
    if (minDone && settled && !fading && (list.status !== 'ok' || pingFrac >= PINGED_ENOUGH || graceDone)) leave.current();
  }, [minDone, settled, fading, list.status, pingFrac, graceDone]);

  // the title bar's own chrome (tabs, wordmark, HUD) steps back while the
  // loading screen is up; only the window buttons stay
  useEffect(() => {
    const root = document.documentElement;
    if (phase === 'in') root.dataset.splash = '';
    else delete root.dataset.splash;
    return () => {
      delete root.dataset.splash;
    };
  }, [phase]);

  useEffect(() => {
    if (phase !== 'leaving') return undefined;
    const t = window.setTimeout(() => setPhase('gone'), LEAVE_MS);
    return () => window.clearTimeout(t);
  }, [phase]);

  const players = useCountUp(list.status === 'ok' ? list.players : 0);

  if (phase === 'gone') return null;

  // what it is doing right now, in the game's spinner corner
  let status;
  let busy = true;
  if (list.status === 'loading') status = <span>Loading servers</span>;
  else if (list.status === 'error') {
    status = <span>Server list unavailable</span>;
    busy = false;
  } else if (pingFrac < 1 && list.toPing > 0)
    status = (
      <span>
        Measuring ping <span className="splash-num">{`${list.pinged}/${list.toPing}`}</span>
      </span>
    );
  else {
    status = <span>Ready</span>;
    busy = false;
  }

  // bar: indeterminate while the list loads, then the share of pings in
  const bar = list.status === 'loading' ? null : list.status === 'ok' ? Math.max(0.08, pingFrac) : 1;

  return (
    // decorative: the list panel says it is loading, for assistive tech too
    <div className={`splash ${phase === 'leaving' ? 'is-leaving' : ''}`} data-who={slides[slide].who} aria-hidden="true">
      {slides.map((s, i) => (
        <img
          key={s.who}
          className={`splash-art ${i === slide ? 'is-on' : ''} ${i % 2 ? 'is-alt' : ''}`}
          src={s.src}
          alt=""
          decoding="async"
        />
      ))}
      <div className="splash-shade" />
      <div className="splash-foot">
        <div className="splash-title">
          <div className="splash-lockup">
            <Lockup height={80} />
          </div>
          <p key={tip} className="splash-tip">
            {TIPS[tip]}
          </p>
        </div>
        {/* it says what it is waiting for until the moment it lifts */}
        <div className="splash-status">
          {list.status === 'ok' && list.total > 0 && (
            <p className="splash-totals">
              {/* the server total is fixed at once: it is the ping count's denominator */}
              <span className="splash-num">{fmt(list.total)}</span> servers
              <span className="splash-dot" />
              <span className="splash-num">{fmt(players)}</span> players online
            </p>
          )}
          <p className="splash-load">
            {status}
            {busy && <Spinner className="h-[18px] w-[18px]" />}
          </p>
        </div>
      </div>
      <div className={`splash-bar ${bar == null ? 'is-busy' : ''}`}>
        <span style={bar == null ? undefined : { transform: `scaleX(${bar})` }} />
      </div>
    </div>
  );
}
