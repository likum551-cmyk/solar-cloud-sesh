import { Environment, Lightformer } from '@react-three/drei';
import { Canvas, useFrame, useThree } from '@react-three/fiber';
import { Bloom, EffectComposer, Noise, ToneMapping, Vignette } from '@react-three/postprocessing';
import gsap from 'gsap';
import { BlendFunction, ToneMappingMode, type EffectComposer as PPComposer } from 'postprocessing';
import { Component, Suspense, useEffect, useRef, type MutableRefObject, type ReactNode } from 'react';
import * as THREE from 'three';
import type { ShowroomModel } from '../state/shop';
import { ASSET, preloadShowroom, preloadStreet } from './assets';

// Fetch the whole street set in parallel as soon as this chunk evaluates.
preloadStreet();
import { Emblem } from './Emblem';
import { envScene, syncEnv } from './env';
import { cropRect, registerSnapshot, SUBJECT_CROP, type Crop } from './prints';
import { PRINT_CENTRE_Y, PRINT_KINDS, PrintRack, RACK_STEP, RACK_X0, Showroom, SHOWROOM_ORIGIN } from './Showroom';
import { CAR_POS, COVERED_POS, PLAYER, PROPS_POS, Street, WALL_Z } from './Street';

// The one WebGL canvas behind the whole client, lit like a flash photograph: one hard key on a
// bracket beside the lens, inverse-square falloff into black, a hard shadow on the wall, no fill.
// Reflections (chrome, paint, wet road) come from a side environment each material opts into
// (env.ts). The render loop runs only while `active`; with reduced motion it renders on demand.

export type Shot = 'auth' | 'play' | 'profile' | 'settings' | 'shop';

type V3 = [number, number, number];
/**
 * pos/target frame the shot; `aim` is where the photographer points the flash (the subject);
 * `frame` is what its print is centred on (default: the aim).
 */
interface Key { pos: V3; target: V3; aim: V3; fov: number; frame?: V3 }
const SX = SHOWROOM_ORIGIN[0];
/** The chrome GAGE mark on the wall, just behind the player's near shoulder: in the hero framings it
 *  sits at left-centre with the figure (the contract's first viewport). */
const EMBLEM_POS: V3 = [-1.37, 1.6, WALL_Z + 0.28];
/** Turned 10 degrees to the right: the street cameras stand 28-37 degrees right of it and the
 *  settings camera 21 degrees left, so no camera (with its flash) sits within 18 degrees of the
 *  face's normal and the flash never mirrors straight back and blows it to white (measured
 *  on captures at +24 and -17 degrees: both blew out). */
const EMBLEM_YAW = 0.17;

/**
 * A framing around the player: the camera sits `dist` from the target on bearing `theta`
 * (radians from +z toward +x); the target is shifted `shift` to camera-right so the player
 * stands in the left of the frame and the right third stays unlit for the copy.
 */
function orbit(theta: number, dist: number, camY: number, targetY: number, shift: number, fov: number, aimY: number): Key {
  const [cx, , cz] = PLAYER;
  const rx = Math.cos(theta);
  const rz = -Math.sin(theta);
  const tx = cx + rx * shift;
  const tz = cz + rz * shift;
  return {
    pos: [tx + Math.sin(theta) * dist, camY, tz + Math.cos(theta) * dist],
    target: [tx, targetY, tz],
    aim: [cx, aimY, cz],
    fov,
    frame: [cx, 1.3, cz],
  };
}

/**
 * The play framings, one per server frame (index modulo length): each server is a different
 * corner of the same street, so the contact strip reads as five photographs, not one. Each
 * strip frame is a print of its framing; choosing a frame cuts the hero to it with a flash pop.
 * The character (a MakeHuman base, ASSETS.md) is never framed closer than full length, so his
 * face stays under about 80 px. Every framing keeps its subject in the left 60% (the copy
 * column is the right third).
 */
export const PLAY_FRAMES: readonly Key[] = [
  // Downtown: the player at full length against the wall, the mark at his shoulder.
  orbit(0.4, 4.6, 1.3, 1.08, 0.75, 30, 1.3),
  // Harbor: the street lamp from low down with a wide lens, its lantern against the lit wall.
  { pos: [PROPS_POS[0] + 2.45, 0.55, 3.9], target: [PROPS_POS[0] + 0.84, 1.9, -0.3], aim: [PROPS_POS[0], 1.8, PROPS_POS[2]], fov: 46, frame: [PROPS_POS[0], 2.7, PROPS_POS[2]] },
  // Freeroam: the covered car from the rear three-quarter on a long lens, a manhole in front of it.
  { pos: [COVERED_POS[0] - 8.03, 1.0, COVERED_POS[2] + 8.92], target: [COVERED_POS[0] + 0.18, 0.6, COVERED_POS[2] + 1.6], aim: [COVERED_POS[0], 0.7, COVERED_POS[2] + 0.1], fov: 26, frame: [COVERED_POS[0] - 0.3, 0.6, COVERED_POS[2] + 0.6] },
  // Night Racing: the black car, front three-quarter, low, on a long lens from across the street.
  { pos: [CAR_POS[0] - 7.72, 1.0, CAR_POS[2] + 9.95], target: [CAR_POS[0] + 0.63, 0.6, CAR_POS[2] + 1.33], aim: [CAR_POS[0] - 0.2, 0.6, CAR_POS[2] + 0.1], fov: 26, frame: [CAR_POS[0] - 0.2, 0.55, CAR_POS[2] + 0.2] },
  // Vinewood: straight down from a first-floor window: the top of his head, his shadow, the drain.
  { pos: [0.9, 4.6, 4.2], target: [0.1, 0.2, 0.5], aim: [PLAYER[0], 1.0, PLAYER[2]], fov: 34, frame: [-0.3, 0.9, 0.3] },
];

/** Camera keys per shot. `play` is replaced by the chosen frame's framing. */
export const SHOTS: Record<Shot, Key> = {
  // Full length from low down: he stands over the sign-in.
  auth: orbit(0.34, 4.5, 0.95, 1.15, 0.72, 30, 1.3),
  play: PLAY_FRAMES[0]!,
  profile: orbit(0.2, 4.8, 1.55, 1.05, 0.85, 30, 1.3),
  // From left of the mark, 2.4 m out, yawed right: the mark sits in the left third at a
  // three-quarter (the bevel shows), and parallax puts the player, 0.8 m to its right, behind the
  // opaque settings panel at every window size (about u 0.68; the panel starts at u 0.44-0.56).
  settings: { pos: [-2.2, 1.66, 2.0], target: [-0.72, 1.6, -0.4], aim: EMBLEM_POS, fov: 32 },
  shop: { pos: [SX + 3.2, 1.45, 5.3], target: [SX + 1.05, 0.38, 0], aim: [SX, 0.55, 0], fov: 30 },
};

/** Shop prints: how far the showroom camera moves toward each subject (1 = the showroom shot). */
const PRINT_DOLLY: Record<(typeof PRINT_KINDS)[number], number> = { car: 1.3, outfit: 1.3, watch: 0.6, emblem: 0.8, coin: 0.95 };

function keyFor(shot: Shot, playFrame: number): Key {
  if (shot !== 'play') return SHOTS[shot];
  return PLAY_FRAMES[((playFrame % PLAY_FRAMES.length) + PLAY_FRAMES.length) % PLAY_FRAMES.length]!;
}

/** The flash aim, written by the camera rig (tweened with the dolly) and read by the flash. */
const flashAim = new THREE.Vector3(...SHOTS.play.aim);
/** What the current shot's print centres on (the launch print), written by the camera rig. */
const frameAim = new THREE.Vector3(...(SHOTS.play.frame ?? SHOTS.play.aim));
const _ndc = new THREE.Vector3();
const _centre = new THREE.Vector3();

/** Where `p` lands on the canvas, as fractions (0..1) from the top-left. */
function onCanvas(cam: THREE.PerspectiveCamera, p: THREE.Vector3): [number, number] {
  cam.updateMatrixWorld();
  _ndc.copy(p).project(cam);
  return [(_ndc.x + 1) / 2, (1 - _ndc.y) / 2];
}
/** The flash light and the scene (for its fog), for the print pass (it renders outside the frame loop). */
const flash: { light: THREE.SpotLight | null; scene: THREE.Scene | null } = { light: null, scene: null };
/**
 * The fog is the flash's falloff: it starts just in front of the subject and reaches black
 * this far behind it, whatever the subject's distance (a long-lens car frame and a close
 * portrait fall off the same way behind the subject).
 */
const FOG_DEPTH = 6.5;

/** Flash power at the subject (inverse-square compensated) and the PLAY surge on top. */
const FLASH_BASE = 4.6;
const FLASH_SURGE = 34;
const _right = new THREE.Vector3();
const _up = new THREE.Vector3();

/** Put the flash on its bracket: a hand left of the lens and above it, aimed at the subject. */
function placeFlash(l: THREE.SpotLight, cam: THREE.Camera, charge: number) {
  cam.updateMatrixWorld();
  _right.setFromMatrixColumn(cam.matrixWorld, 0);
  _up.setFromMatrixColumn(cam.matrixWorld, 1);
  l.position.copy(cam.position).addScaledVector(_right, -0.25).addScaledVector(_up, 0.3);
  l.target.position.copy(flashAim);
  l.target.updateMatrixWorld();
  const d2 = l.position.distanceToSquared(flashAim);
  l.intensity = (FLASH_BASE + charge * charge * FLASH_SURGE) * d2;
  const fog = flash.scene?.fog;
  if (fog && (fog as THREE.Fog).isFog) {
    const f = fog as THREE.Fog;
    f.near = Math.max(0.5, cam.position.distanceTo(flashAim) - 0.5);
    f.far = f.near + FOG_DEPTH;
  }
}

export interface StageProps {
  shot: Shot;
  /** Which play framing (the chosen server's frame index). */
  playFrame: number;
  active: boolean;
  reducedMotion: boolean;
  /** 0..1 PLAY charge (emblem enamel + flash surge), driven by GSAP. */
  charge: MutableRefObject<number>;
  showroomModel: ShowroomModel;
  /** Turntable angle (drag adds to it). */
  tableAngle: MutableRefObject<number>;
  /** The contact prints of PLAY_FRAMES, once, after the set has loaded. */
  onPrints?: (urls: (string | null)[]) => void;
  /** One print per showroom subject kind, once the product models have loaded. */
  onShopPrints?: (prints: Partial<Record<ShowroomModel, string>>) => void;
  onReady?: () => void;
  onAssetError?: () => void;
}

class Boundary extends Component<{ fallback: ReactNode; onError?: () => void; children: ReactNode }, { failed: boolean }> {
  override state = { failed: false };
  static getDerivedStateFromError() { return { failed: true }; }
  override componentDidCatch(err: unknown) {
    console.warn('[stage] asset load failed, showing the fallback set:', err);
    this.props.onError?.();
  }
  override render() { return this.state.failed ? this.props.fallback : this.props.children; }
}

/** Studio strips baked into the reflection environment: clean highlight lines for chrome and paint. */
function Strips() {
  return (
    <>
      <Lightformer form="rect" intensity={3} position={[0, 4, -6]} scale={[10, 0.35, 1]} />
      <Lightformer form="rect" intensity={2.2} position={[-6, 1.2, 1]} rotation={[0, Math.PI / 2, 0]} scale={[9, 0.22, 1]} />
      <Lightformer form="rect" intensity={1.8} position={[6, 1.6, -1]} rotation={[0, -Math.PI / 2, 0]} scale={[9, 0.18, 1]} />
      <Lightformer form="rect" intensity={1.4} position={[0, 6, 1]} rotation={[Math.PI / 2, 0, 0]} scale={[7, 2.2, 1]} />
    </>
  );
}

function FlashKey({ charge }: { charge: MutableRefObject<number> }) {
  const light = useRef<THREE.SpotLight>(null);
  const camera = useThree((s) => s.camera);
  const scene = useThree((s) => s.scene);
  useEffect(() => {
    flash.light = light.current;
    flash.scene = scene;
    return () => { flash.light = null; flash.scene = null; };
  }, [scene]);
  useFrame(() => {
    if (light.current) placeFlash(light.current, camera, charge.current);
  });
  return (
    // A wide, fully feathered cone: a bare speedlight, not a theatre spot. The falloff into black
    // comes from distance (inverse square), not from the cone's edge.
    <spotLight
      ref={light}
      angle={0.85}
      penumbra={1}
      decay={2}
      distance={0}
      intensity={100}
      color="#fffaf2"
      castShadow
      shadow-mapSize-width={2048}
      shadow-mapSize-height={2048}
      shadow-bias={-0.00015}
      shadow-normalBias={0.015}
      shadow-camera-near={0.5}
      shadow-camera-far={30}
    />
  );
}

/** GSAP dolly between shot keys; a cut between two play framings; light cursor parallax on top. */
function CameraRig({ shot, playFrame, reducedMotion }: { shot: Shot; playFrame: number; reducedMotion: boolean }) {
  const { camera, invalidate } = useThree();
  const cam = camera as THREE.PerspectiveCamera;
  const first = keyFor(shot, playFrame);
  const pos = useRef(new THREE.Vector3(...first.pos));
  const target = useRef(new THREE.Vector3(...first.target));
  const pointer = useRef({ x: 0, y: 0 });
  const smooth = useRef({ x: 0, y: 0 });
  const lastShot = useRef(shot);

  useEffect(() => {
    const onMove = (e: PointerEvent) => {
      pointer.current.x = (e.clientX / window.innerWidth) * 2 - 1;
      pointer.current.y = (e.clientY / window.innerHeight) * 2 - 1;
    };
    window.addEventListener('pointermove', onMove);
    return () => window.removeEventListener('pointermove', onMove);
  }, []);

  useEffect(() => {
    const k = keyFor(shot, playFrame);
    frameAim.set(...(k.frame ?? k.aim));
    const from = { px: pos.current.x, py: pos.current.y, pz: pos.current.z, tx: target.current.x, ty: target.current.y, tz: target.current.z, ax: flashAim.x, ay: flashAim.y, az: flashAim.z, fov: cam.fov };
    const to = { px: k.pos[0], py: k.pos[1], pz: k.pos[2], tx: k.target[0], ty: k.target[1], tz: k.target[2], ax: k.aim[0], ay: k.aim[1], az: k.aim[2], fov: k.fov };
    // A new frame on the same screen is a cut (two prints on a contact sheet), and so is
    // crossing into or out of the showroom; between screens the camera dollies.
    const sameScreen = lastShot.current === shot;
    lastShot.current = shot;
    const cut = reducedMotion || sameScreen || (shot === 'shop') !== (Math.abs(from.px - SX) < 20);
    const apply = (v: typeof from) => {
      pos.current.set(v.px, v.py, v.pz);
      target.current.set(v.tx, v.ty, v.tz);
      flashAim.set(v.ax, v.ay, v.az);
      cam.fov = v.fov;
      cam.updateProjectionMatrix();
      invalidate();
    };
    if (cut) { apply(to); return; }
    const tween = gsap.to(from, { ...to, duration: 1.5, ease: 'power3.inOut', onUpdate: () => apply(from) });
    return () => { tween.kill(); };
  }, [shot, playFrame, reducedMotion, cam, invalidate]);

  useFrame((_, dt) => {
    const k = 1 - Math.exp(-dt * 2.4);
    const amp = reducedMotion ? 0 : 1;
    smooth.current.x += (pointer.current.x * amp - smooth.current.x) * k;
    smooth.current.y += (pointer.current.y * amp - smooth.current.y) * k;
    cam.position.set(pos.current.x + smooth.current.x * 0.08, pos.current.y - smooth.current.y * 0.04, pos.current.z);
    cam.lookAt(target.current);
  });
  return null;
}

/** Frame-time readout for the perf budget: window.__gagePerf = { avgMs, p95Ms, frames }. */
function PerfProbe() {
  const samples = useRef<number[]>([]);
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

/**
 * Registers the snapshot function: renders the composer once and copies a crop of the canvas
 * in the same task (the drawing buffer is only valid until the browser composites).
 */
function Snapshotter({ composer }: { composer: MutableRefObject<PPComposer | null> }) {
  const gl = useThree((s) => s.gl);
  const camera = useThree((s) => s.camera);
  useEffect(() => {
    const out = document.createElement('canvas');
    registerSnapshot((crop: Crop, outW: number, outH: number) => {
      const c = composer.current;
      if (!c) return null;
      c.render(0);
      const src = gl.domElement;
      const [u, v] = onCanvas(camera as THREE.PerspectiveCamera, frameAim);
      const r = cropRect(src.width, src.height, crop, outW, outH, u, v);
      out.width = outW;
      out.height = outH;
      const ctx = out.getContext('2d');
      if (!ctx) return null;
      ctx.drawImage(src, r.sx, r.sy, r.sw, r.sh, 0, 0, outW, outH);
      return out.toDataURL('image/webp', 0.82);
    });
    return () => registerSnapshot(null);
  }, [gl, camera, composer]);
  return null;
}

/**
 * Photograph each key in one synchronous pass (no framing ever reaches the screen: the browser
 * only composites after the task) and restore the camera and flash. 2:1 WebP crops centred on
 * each key's subject (`frame`, else `aim`).
 */
function printKeys(composer: PPComposer | null, gl: THREE.WebGLRenderer, cam: THREE.PerspectiveCamera, keys: readonly Key[], charge: number, crop: Crop = SUBJECT_CROP): (string | null)[] {
  const out = document.createElement('canvas');
  out.width = 440;
  out.height = 220;
  const ctx = out.getContext('2d');
  if (!composer || !ctx || !flash.light) return keys.map(() => null);
  const light = flash.light;
  const saved = { pos: cam.position.clone(), quat: cam.quaternion.clone(), fov: cam.fov, aim: flashAim.clone() };
  const src = gl.domElement;
  const urls = keys.map((k) => {
    cam.position.set(...k.pos);
    cam.lookAt(...k.target);
    cam.fov = k.fov;
    cam.updateProjectionMatrix();
    flashAim.set(...k.aim);
    placeFlash(light, cam, 0);
    composer.render(0);
    const [u, v] = onCanvas(cam, _centre.set(...(k.frame ?? k.aim)));
    const r = cropRect(src.width, src.height, crop, 440, 220, u, v);
    ctx.drawImage(src, r.sx, r.sy, r.sw, r.sh, 0, 0, 440, 220);
    return out.toDataURL('image/webp', 0.8);
  });
  cam.position.copy(saved.pos);
  cam.quaternion.copy(saved.quat);
  cam.fov = saved.fov;
  cam.updateProjectionMatrix();
  flashAim.copy(saved.aim);
  placeFlash(light, cam, charge);
  composer.render(0);
  return urls;
}

/** Wait for the reflection environment (at most `ms`) so chrome prints with its highlights. */
function whenEnvReady(ms: number, run: () => void): () => void {
  const started = performance.now();
  let raf = 0;
  const tick = () => {
    if (!envScene.environment && performance.now() - started < ms) { raf = requestAnimationFrame(tick); return; }
    syncEnv();
    run();
  };
  raf = requestAnimationFrame(tick);
  return () => cancelAnimationFrame(raf);
}

/** After the street has loaded: print every play framing for the contact strip, then report ready. */
function PrintPass({ composer, charge, onPrints, onReady }: { composer: MutableRefObject<PPComposer | null>; charge: MutableRefObject<number>; onPrints?: ((urls: (string | null)[]) => void) | undefined; onReady?: (() => void) | undefined }) {
  const camera = useThree((s) => s.camera);
  const gl = useThree((s) => s.gl);
  const invalidate = useThree((s) => s.invalidate);
  const done = useRef(false);
  useEffect(() => {
    if (done.current) return undefined;
    done.current = true;
    return whenEnvReady(1200, () => {
      const urls = printKeys(composer.current, gl, camera as THREE.PerspectiveCamera, PLAY_FRAMES, charge.current);
      invalidate();
      onPrints?.(urls);
      onReady?.();
      preloadShowroom();
    });
  }, [camera, gl, composer, charge, invalidate, onPrints, onReady]);
  return null;
}

/** Once the product models have loaded: photograph each subject kind on the print rack. */
function ShopPrintPass({ composer, charge, rack, onShopPrints }: { composer: MutableRefObject<PPComposer | null>; charge: MutableRefObject<number>; rack: MutableRefObject<THREE.Group | null>; onShopPrints?: ((p: Partial<Record<ShowroomModel, string>>) => void) | undefined }) {
  const camera = useThree((s) => s.camera);
  const gl = useThree((s) => s.gl);
  const invalidate = useThree((s) => s.invalidate);
  const done = useRef(false);
  useEffect(() => {
    if (done.current || !onShopPrints) return undefined;
    done.current = true;
    return whenEnvReady(2500, () => {
      const r = rack.current;
      if (!r) return;
      const shop = SHOTS.shop;
      // Each subject photographed from the showroom angle, dollied in along the ray through
      // its aim point (so it keeps its place in the frame) until it fills the print.
      const keys = PRINT_KINDS.map((kind, i): Key => {
        const dx = RACK_X0 + i * RACK_STEP - SX;
        const k = PRINT_DOLLY[kind];
        const aim: V3 = [shop.aim[0] + dx, shop.aim[1], shop.aim[2]];
        const toward = (p: V3): V3 => [aim[0] + (p[0] + dx - aim[0]) * k, aim[1] + (p[1] - aim[1]) * k, aim[2] + (p[2] - aim[2]) * k];
        return { pos: toward(shop.pos), target: toward(shop.target), aim, fov: shop.fov, frame: [aim[0], PRINT_CENTRE_Y[kind], aim[2]] };
      });
      r.visible = true;
      const urls = printKeys(composer.current, gl, camera as THREE.PerspectiveCamera, keys, charge.current);
      r.visible = false;
      composer.current?.render(0);
      invalidate();
      const prints: Partial<Record<ShowroomModel, string>> = {};
      PRINT_KINDS.forEach((kind, i) => { const u = urls[i]; if (u) prints[kind] = u; });
      onShopPrints(prints);
    });
  }, [camera, gl, composer, charge, rack, invalidate, onShopPrints]);
  return null;
}

/** A pointer ref shared with the emblem (it leans toward the cursor). */
const sharedPointer = { current: { x: 0, y: 0 } };
if (typeof window !== 'undefined') {
  window.addEventListener('pointermove', (e) => {
    sharedPointer.current.x = (e.clientX / window.innerWidth) * 2 - 1;
    sharedPointer.current.y = (e.clientY / window.innerHeight) * 2 - 1;
  });
}

export function Stage({ shot, playFrame, active, reducedMotion, charge, showroomModel, tableAngle, onPrints, onShopPrints, onReady, onAssetError }: StageProps) {
  const visitedShop = useRef(false);
  const composer = useRef<PPComposer | null>(null);
  const rack = useRef<THREE.Group | null>(null);
  if (shot === 'shop') visitedShop.current = true;
  const inShop = shot === 'shop';
  const k = keyFor(shot, playFrame);
  return (
    <Canvas
      frameloop={reducedMotion ? 'demand' : active ? 'always' : 'never'}
      dpr={[1, 1.5]}
      shadows="percentage"
      gl={{ antialias: false, alpha: false, stencil: false, powerPreference: 'high-performance' }}
      camera={{ fov: k.fov, near: 0.1, far: 80, position: k.pos }}
      onCreated={({ gl, scene, camera }) => {
        gl.setClearColor('#050506');
        gl.toneMapping = THREE.NoToneMapping;
        // Dev server only: the scene graph for layout debugging from the console / Playwright.
        if (import.meta.env.DEV) (window as unknown as { __gageThree?: unknown }).__gageThree = { scene, camera, THREE };
      }}
      aria-hidden
    >
      <color attach="background" args={['#050506']} />
      {/* near/far follow the subject every frame (placeFlash). */}
      <fog attach="fog" args={['#050506', 4.5, 4.5 + FOG_DEPTH]} />
      <CameraRig shot={shot} playFrame={playFrame} reducedMotion={reducedMotion} />
      <FlashKey charge={charge} />
      <PerfProbe />
      <Snapshotter composer={composer} />

      <Boundary fallback={<Environment scene={envScene} resolution={256}><Strips /></Environment>}>
        <Suspense fallback={null}>
          <Environment scene={envScene} files={ASSET.hdri} resolution={512}>
            <Strips />
          </Environment>
        </Suspense>
      </Boundary>

      <Boundary
        onError={onAssetError}
        fallback={<Emblem size={1.2} position={[0, 1.15, 0]} charge={charge} pointer={sharedPointer} />}
      >
        <Suspense fallback={null}>
          <group visible={!inShop}>
            <Street />
            {/* Sign-mounted on the wall over his shoulder: the flash throws its hard shadow. It does not
                lean toward the cursor: a sign is fixed, and any lean toward the lens mirrors the flash
                and blows its face to white (seen in the r5 sign-in captures). */}
            <Emblem size={0.62} position={EMBLEM_POS} rotation={[0, EMBLEM_YAW, 0]} charge={charge} pointer={null} />
          </group>
          <PrintPass composer={composer} charge={charge} onPrints={onPrints} onReady={onReady} />
        </Suspense>
        {visitedShop.current && (
          <Suspense fallback={null}>
            <group visible={inShop}>
              <Showroom model={showroomModel} angle={tableAngle} />
            </group>
          </Suspense>
        )}
      </Boundary>

      {/* Its own boundary: a product model that fails to load costs the shop its prints, never the street. */}
      <Boundary fallback={null}>
        <Suspense fallback={null}>
          <PrintRack rack={rack} />
          <ShopPrintPass composer={composer} charge={charge} rack={rack} onShopPrints={onShopPrints} />
        </Suspense>
      </Boundary>

      <EffectComposer ref={composer} multisampling={4} enableNormalPass={false}>
        <Bloom intensity={0.34} luminanceThreshold={0.86} luminanceSmoothing={0.1} mipmapBlur radius={0.55} />
        <Vignette offset={0.26} darkness={0.82} />
        <ToneMapping mode={ToneMappingMode.ACES_FILMIC} />
        {/* Grain last, after the tone curve, so it never turns into speckle on a blown face. */}
        <Noise premultiply opacity={0.16} blendFunction={BlendFunction.SOFT_LIGHT} />
      </EffectComposer>
    </Canvas>
  );
}
