import { Environment, Lightformer } from '@react-three/drei';
import { Canvas, useFrame, useThree } from '@react-three/fiber';
import { useEffect, useLayoutEffect, useRef, useState, type MutableRefObject } from 'react';
import * as THREE from 'three';
import { useEmblemSlot } from '../ui/emblemSlot';
import { Emblem } from './Emblem';
import { envScene, syncEnv } from './env';

// The only real-time 3D in the client: the chrome G emblem, drawn on one small transparent
// canvas laid over the footage and kept on top of whichever <EmblemSlot> is mounted. Chrome is
// all reflection, so it is lit by an environment of studio strips (no HDRI, no lights): a cool
// key band above, side strips that run along the 45-degree chamfers, and a low warm bounce that
// picks up the dusk footage. The loop runs only while the slot is shown and renderActive allows
// it; with reduced motion it renders on demand (a still emblem).

/** Normalised pointer (-1..1), shared by every emblem: it leans a little toward the cursor. */
const pointer = { current: { x: 0, y: 0 } };
if (typeof window !== 'undefined') {
  window.addEventListener('pointermove', (e) => {
    pointer.current.x = (e.clientX / window.innerWidth) * 2 - 1;
    pointer.current.y = (e.clientY / window.innerHeight) * 2 - 1;
  }, { passive: true });
}

// The emblem is turned 0.36 rad toward its copy, so its polished face mirrors a direction about
// 40 degrees left of the lens and a little below level. The main boxes sit there: a cool softbox
// above a dark horizon line and a dimmer warm one below it. Perspective spreads the face's
// reflection over about 20 degrees, so the line crosses the face and moves as it sways, which is
// what makes flat chrome read as chrome. Thin strips at the sides and top run along the
// 45-degree chamfers; a graphite ground keeps the dark parts metal, not void.
const FACE_AZ = -0.7; // radians, around the face's mirror direction
const at = (az: number, y: number, r = 6): [number, number, number] => [r * Math.sin(az), y, r * Math.cos(az)];

function Strips() {
  return (
    <>
      <color attach="background" args={['#15181d']} />
      <Lightformer form="rect" intensity={2.2} color="#eef3fb" position={at(FACE_AZ, 1.2)} scale={[7.5, 3.2, 1]} />
      <Lightformer form="rect" intensity={0.55} color="#f0d4bd" position={at(FACE_AZ, -2.2)} scale={[7.5, 2.5, 1]} />
      {/* a narrow fill behind the lens for the chamfers that face the camera */}
      <Lightformer form="rect" intensity={1.1} color="#e8eef8" position={[0, 2.4, 6]} scale={[5, 0.6, 1]} />
      <Lightformer form="rect" intensity={3} position={[0, 4, -6]} scale={[10, 0.35, 1]} />
      <Lightformer form="rect" intensity={2.6} position={[-6, 1.2, 1]} rotation={[0, Math.PI / 2, 0]} scale={[9, 0.3, 1]} />
      <Lightformer form="rect" intensity={2.2} position={[6, 1.6, -1]} rotation={[0, -Math.PI / 2, 0]} scale={[9, 0.24, 1]} />
      <Lightformer form="rect" intensity={1.4} position={[0, 6, 1]} rotation={[Math.PI / 2, 0, 0]} scale={[7, 2.2, 1]} />
      {/* Low and warm: the dusk bounce off the street. */}
      <Lightformer form="rect" intensity={0.9} color="#ffb07a" position={[0, -4, 3]} rotation={[-Math.PI / 2.6, 0, 0]} scale={[8, 1.6, 1]} />
    </>
  );
}

/** Binds the environment once it exists, and writes window.__gagePerf for the perf budget. */
function Probe({ demand }: { demand: boolean }) {
  const invalidate = useThree((s) => s.invalidate);
  const samples = useRef<number[]>([]);
  // On demand, the environment renders on the first frames: ask for a few more so the still
  // emblem shows its reflections (a demand loop draws nothing it is not asked for).
  useEffect(() => {
    if (!demand) return;
    const ts = [0, 120, 400, 1000].map((ms) => setTimeout(invalidate, ms));
    return () => { for (const t of ts) clearTimeout(t); };
  }, [demand, invalidate]);
  useFrame((_, dt) => {
    syncEnv();
    const s = samples.current;
    s.push(dt * 1000);
    if (s.length >= 120) {
      const sorted = [...s].sort((a, b) => a - b);
      const avg = s.reduce((a, b) => a + b, 0) / s.length;
      (window as unknown as { __gagePerf?: unknown }).__gagePerf = { avgMs: +avg.toFixed(2), p95Ms: +sorted[Math.floor(s.length * 0.95)]!.toFixed(2), frames: s.length, at: Date.now() };
      s.length = 0;
    }
  });
  return null;
}

export interface EmblemLayerProps {
  /** 0..1 PLAY charge, driven by GSAP (read every frame). */
  charge: MutableRefObject<number>;
  active: boolean;
  reducedMotion: boolean;
}

export default function EmblemLayer({ charge, active, reducedMotion }: EmblemLayerProps) {
  const slot = useEmblemSlot();
  const box = useRef<HTMLDivElement>(null);
  const [placed, setPlaced] = useState(false);

  // Follow the slot: every frame for a second after it mounts (screens rise in), then whenever
  // the slot or any box around it changes size. The slot moves without resizing when content
  // above or beside it changes (a maintenance banner, a two-line name), and that always resizes
  // one of its ancestors.
  useLayoutEffect(() => {
    const el = box.current;
    if (!el || !slot) { setPlaced(false); return; }
    let w = -1;
    let h = -1;
    const place = () => {
      const r = slot.getBoundingClientRect();
      el.style.transform = `translate3d(${Math.round(r.left)}px, ${Math.round(r.top)}px, 0)`;
      // Size only when it changes: the canvas resizes itself on its own observer.
      if (Math.round(r.width) !== w || Math.round(r.height) !== h) {
        w = Math.round(r.width);
        h = Math.round(r.height);
        el.style.width = `${w}px`;
        el.style.height = `${h}px`;
      }
    };
    const until = performance.now() + 1100;
    let raf = 0;
    const tick = () => {
      place();
      if (performance.now() < until) raf = requestAnimationFrame(tick);
    };
    tick();
    setPlaced(true);
    const ro = new ResizeObserver(place);
    for (let n: Element | null = slot; n; n = n.parentElement) ro.observe(n);
    window.addEventListener('resize', place);
    return () => {
      cancelAnimationFrame(raf);
      ro.disconnect();
      window.removeEventListener('resize', place);
    };
  }, [slot]);

  const shown = slot !== null && placed;
  const frameloop = !shown ? 'never' : reducedMotion ? 'demand' : active ? 'always' : 'never';
  return (
    <div ref={box} className={`emblem-layer ${shown ? 'is-on' : ''}`} aria-hidden>
      <Canvas
        frameloop={frameloop}
        // R3F gives its wrapper pointer-events:auto inline; the layer floats over the screen
        // (and stays where it was when no slot is mounted), so it must never take a click.
        style={{ pointerEvents: 'none' }}
        dpr={[1, 2]}
        gl={{ alpha: true, antialias: true, stencil: false, powerPreference: 'high-performance' }}
        camera={{ fov: 30, near: 0.1, far: 20, position: [0, 0, 2.7] }}
        onCreated={({ gl }) => {
          gl.setClearColor(0x000000, 0);
          gl.toneMapping = THREE.ACESFilmicToneMapping;
          gl.toneMappingExposure = 1.05;
        }}
      >
        <Environment scene={envScene} resolution={256}>
          <Strips />
        </Environment>
        <Probe demand={frameloop === 'demand'} />
        {/* Turned toward the copy on its right, so the chamfer and the depth read as an object. */}
        <Emblem size={1} rotation={[0.05, -0.36, 0]} charge={charge} pointer={reducedMotion ? null : pointer} castShadow={false} reflect={1.15} sway={!reducedMotion} />
      </Canvas>
    </div>
  );
}
