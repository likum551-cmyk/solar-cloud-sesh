import { useEffect, useState } from 'react';

/** Epoch ms, re-rendering every `intervalMs`. */
export function useNow(intervalMs = 1000): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), intervalMs);
    return () => clearInterval(t);
  }, [intervalMs]);
  return now;
}

/** prefers-reduced-motion, live. */
export function useReducedMotion(): boolean {
  const [reduced, setReduced] = useState(() => typeof matchMedia !== 'undefined' && matchMedia('(prefers-reduced-motion: reduce)').matches);
  useEffect(() => {
    const mq = matchMedia('(prefers-reduced-motion: reduce)');
    const on = () => setReduced(mq.matches);
    mq.addEventListener('change', on);
    return () => mq.removeEventListener('change', on);
  }, []);
  return reduced;
}

/** True once there has been no pointer or key input for `ms`. */
export function useIdle(ms: number): boolean {
  const [idle, setIdle] = useState(false);
  useEffect(() => {
    let t = setTimeout(() => setIdle(true), ms);
    const kick = () => {
      setIdle(false);
      clearTimeout(t);
      t = setTimeout(() => setIdle(true), ms);
    };
    const evs = ['pointermove', 'pointerdown', 'keydown', 'wheel'] as const;
    for (const e of evs) window.addEventListener(e, kick, { passive: true });
    return () => {
      clearTimeout(t);
      for (const e of evs) window.removeEventListener(e, kick);
    };
  }, [ms]);
  return idle;
}

/**
 * The render-pause policy: the 3D loop runs only while the window is visible, not
 * unfocused-and-idle for 10 s, no launch owns the window and the game is not running.
 */
export function renderActive(p: { visible: boolean; focused: boolean; idle: boolean; launchBusy: boolean; gameRunning: boolean; launchedOverlay: boolean }): boolean {
  if (!p.visible) return false;
  if (!p.focused && p.idle) return false;
  if (p.launchBusy || p.gameRunning || p.launchedOverlay) return false;
  return true;
}
