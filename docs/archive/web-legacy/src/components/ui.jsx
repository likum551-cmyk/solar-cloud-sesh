import { useEffect, useRef, useState } from 'react';

/** The game's busy spinner: an open white ring turning in place. */
export function Spinner({ className = 'h-4 w-4' }) {
  return (
    <svg className={`spin ${className}`} viewBox="0 0 24 24" fill="none" aria-hidden="true">
      <circle cx="12" cy="12" r="9" stroke="currentColor" strokeOpacity="0.28" strokeWidth="3" />
      <path d="M12 3a9 9 0 0 1 9 9" stroke="currentColor" strokeWidth="3" strokeLinecap="round" />
    </svg>
  );
}

/** A keyboard glyph, as the game draws its PC prompts. */
export function Key({ children }) {
  return <kbd className="key">{children}</kbd>;
}

// ping tone, shared by the bars and the number
export function pingTone(ms) {
  if (ms == null || ms < 0) return 'none';
  return ms < 60 ? 'good' : ms < 120 ? 'fair' : 'poor';
}

/**
 * The connection-strength icon from GTA Online's player list: four rising
 * bars, as many lit as the ping deserves. `state` comes from the ping queue:
 * 'pending' (queued or in flight) or 'failed' (no reply); a value always wins.
 */
export function PingCell({ ms, state = null }) {
  const tone = pingTone(ms);
  const lit = tone === 'good' ? 4 : tone === 'fair' ? 3 : tone === 'poor' ? 1 : 0;
  let label;
  if (tone !== 'none') label = `${ms === 0 ? '<1' : ms} ms`;
  else if (state === 'failed') label = 'no reply';
  else if (state === 'pending') label = 'pinging';
  else label = '—';
  return (
    <span className={`ping is-${tone} ${state === 'pending' && tone === 'none' ? 'is-pending' : ''}`}>
      <svg viewBox="0 0 16 12" className="ping-bars" aria-hidden="true">
        {[0, 1, 2, 3].map((i) => (
          <rect key={i} x={i * 4} y={9 - i * 3} width="3" height={3 + i * 3} className={i < lit ? 'is-lit' : ''} />
        ))}
      </svg>
      {/* native measures whole milliseconds: 0 is a sub-millisecond (LAN) reply */}
      <span className="ping-ms">{label}</span>
    </span>
  );
}

/**
 * A menu list sized to whole rows, as the game's: returns the list's height
 * in CSS px, or 0 while all `count` rows fit in `avail()` (no foot, nothing
 * clipped). Rows sit at a fixed `pitch` below a `head`; `gap` is the trailing
 * space the last row does not carry; `foot` is the list foot's height. The
 * layout APIs report CSS px inside the shell's zoom, so `avail()` can read
 * clientHeight directly. Re-measures whenever an element in `watch` resizes.
 */
export function useWholeRows(watch, avail, count, { head = 0, pitch, gap = 0, foot = 32 }) {
  const [height, setHeight] = useState(0);
  const read = useRef(avail);
  read.current = avail;
  useEffect(() => {
    const els = watch.map((r) => r.current).filter(Boolean);
    if (!els.length) return undefined;
    const measure = () => {
      const room = read.current();
      if (head + pitch * count - gap <= room) return setHeight(0);
      const n = Math.max(1, Math.floor((room - foot - head + gap) / pitch));
      setHeight(head + pitch * n - gap);
    };
    measure();
    const ro = new ResizeObserver(measure);
    els.forEach((el) => ro.observe(el));
    return () => ro.disconnect();
    // `watch` holds refs, stable for the component's life
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [count, head, pitch, gap, foot]);
  return height;
}

/**
 * The foot of a menu list that runs past its panel, as the game draws it:
 * up and down arrows (each dims at its end) and, when a row is picked, its
 * place in the list. Absent while everything fits. `step` is one page in
 * CSS px (whole rows); without it a page is the view less a line.
 */
export function ListFoot({ scrollRef, index = 0, total = 0, step = 0 }) {
  const [edges, setEdges] = useState(null);
  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return undefined;
    const measure = () => {
      const over = el.scrollHeight - el.clientHeight > 1;
      setEdges(over ? { up: el.scrollTop > 1, down: el.scrollTop < el.scrollHeight - el.clientHeight - 1 } : null);
    };
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    if (el.firstElementChild) ro.observe(el.firstElementChild);
    el.addEventListener('scroll', measure, { passive: true });
    return () => {
      ro.disconnect();
      el.removeEventListener('scroll', measure);
    };
  }, [scrollRef]);
  if (!edges) return null;
  const page = (dir) => {
    const el = scrollRef.current;
    if (el) el.scrollBy({ top: dir * (step || el.clientHeight - 40) });
  };
  return (
    <div className="list-foot">
      <span className="list-foot-arrows">
        <button type="button" tabIndex={-1} aria-label="Scroll up" disabled={!edges.up} onClick={() => page(-1)}>
          <svg viewBox="0 0 12 8" aria-hidden="true">
            <path d="M1 7 6 1.5 11 7z" fill="currentColor" />
          </svg>
        </button>
        <button type="button" tabIndex={-1} aria-label="Scroll down" disabled={!edges.down} onClick={() => page(1)}>
          <svg viewBox="0 0 12 8" aria-hidden="true">
            <path d="M1 1 6 6.5 11 1z" fill="currentColor" />
          </svg>
        </button>
      </span>
      {/* nothing picked yet: the length alone, so the foot never sits bare */}
      {total > 0 && <span className="list-foot-count">{index > 0 ? `${index}/${total}` : `–/${total}`}</span>}
    </div>
  );
}

export function Chevron({ flip = false }) {
  return (
    <svg viewBox="0 0 8 12" className={`h-3 w-2 ${flip ? 'rotate-180' : ''}`} aria-hidden="true">
      <path d="M6 1 1.5 6 6 11" fill="none" stroke="currentColor" strokeWidth="2" strokeLinejoin="round" />
    </svg>
  );
}

/**
 * An on/off setting as a pause-menu option row: label left, `‹ On ›` right.
 * Click flips it; Left/Right flip it too, as in the game's menus.
 */
export function Toggle({ label, checked, onChange, onFocusRow, current = false }) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      onClick={() => onChange(!checked)}
      onFocus={onFocusRow}
      onMouseEnter={onFocusRow}
      onKeyDown={(e) => {
        if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
          e.preventDefault();
          onChange(!checked);
        }
      }}
      className={`opt ${current ? 'is-current' : ''}`}
    >
      <span className="opt-label">{label}</span>
      <span className="opt-value" aria-hidden="true">
        <Chevron />
        <span className="w-[2.6em] text-center">{checked ? 'On' : 'Off'}</span>
        <Chevron flip />
      </span>
    </button>
  );
}
