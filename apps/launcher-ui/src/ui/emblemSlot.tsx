import { useLayoutEffect, useRef, useSyncExternalStore } from 'react';
import { EMBLEM_3D } from '../brand/brand';

// Where the 3D emblem stands. Screens place an <EmblemSlot> in their layout (beside the server
// name, over the sign-in lockup); the one persistent emblem canvas (three/EmblemLayer.tsx)
// follows the most recently mounted slot that is still mounted, so moving between screens never
// creates a second WebGL context or recompiles its shaders. The launch overlay mounts its own slot
// over a Play screen that stays mounted underneath; when the overlay goes, the emblem returns to
// the Play slot. No slot mounted: the emblem hides and stops rendering.

let stack: HTMLElement[] = [];
let current: HTMLElement | null = null;
const listeners = new Set<() => void>();

function publish() {
  const top = stack[stack.length - 1] ?? null;
  if (current === top) return;
  current = top;
  for (const l of listeners) l();
}

function subscribe(l: () => void) {
  listeners.add(l);
  return () => { listeners.delete(l); };
}

/** The mounted slot element, or null. */
export function useEmblemSlot(): HTMLElement | null {
  return useSyncExternalStore(subscribe, () => current, () => null);
}

/** An empty box the emblem is drawn over. Its size comes from CSS (`className`). */
export function EmblemSlot({ className = '' }: { className?: string }) {
  const ref = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    const el = ref.current;
    if (!el) return;
    stack = [...stack.filter((s) => s !== el), el];
    publish();
    return () => { stack = stack.filter((s) => s !== el); publish(); };
  }, []);
  // The 3D emblem is off (brand.ts): no box, so the title closes up to the edge.
  if (!EMBLEM_3D) return null;
  return <div ref={ref} className={`emblem-slot ${className}`} aria-hidden />;
}
