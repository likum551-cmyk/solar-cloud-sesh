import Lenis from 'lenis';
import { useCallback, useEffect, useRef } from 'react';

/**
 * Smooth wheel scrolling (Lenis) for one overflow container. Returns a callback ref; attach it
 * to the scrolling element. Skipped under prefers-reduced-motion (native scrolling stays).
 * Its rAF loop runs only while the element is mounted.
 */
export function useLenis<T extends HTMLElement>(): (el: T | null) => void {
  const inst = useRef<{ lenis: Lenis; raf: number } | null>(null);
  const stop = () => {
    if (!inst.current) return;
    cancelAnimationFrame(inst.current.raf);
    inst.current.lenis.destroy();
    inst.current = null;
  };
  useEffect(() => stop, []);
  return useCallback((el: T | null) => {
    stop();
    if (!el || matchMedia('(prefers-reduced-motion: reduce)').matches) return;
    const lenis = new Lenis({ wrapper: el, content: el, lerp: 0.14, wheelMultiplier: 0.9, smoothWheel: true });
    const loop = (t: number) => {
      lenis.raf(t);
      if (inst.current) inst.current.raf = requestAnimationFrame(loop);
    };
    inst.current = { lenis, raf: requestAnimationFrame(loop) };
  }, []);
}
