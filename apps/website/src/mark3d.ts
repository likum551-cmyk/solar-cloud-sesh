// The Solar RP S in 3D: the brand mark's two hook polygons (50x100 grid, brand/solar/solar-rp-noon)
// extruded with a 45-degree chamfer cut INTO the outline (bevelOffset), so the gap between the hooks
// stays the mark's own. Upper hook: cream enamel with a clear coat. Lower hook: gold metal.
//
// Each stage has a Look: the S reflects that stage's own shot (its poster on a sphere, through PMREM)
// plus a light line above the horizon and one strip light, takes that shot's haze as fog and its key
// and rim light. A flat face reflects only a narrow cone of the world, so without the line the faces
// read as one printed tone; without the shot the S does not belong to the footage it stands in.
//
// One renderer; its canvas moves between the stages (hero, close) that are on screen. It renders only
// when the pose or the pointer changes.
import {
  ACESFilmicToneMapping,
  BackSide,
  CanvasTexture,
  Color,
  CylinderGeometry,
  DirectionalLight,
  DoubleSide,
  ExtrudeGeometry,
  Fog,
  Group,
  HemisphereLight,
  MathUtils,
  Mesh,
  MeshBasicMaterial,
  MeshPhysicalMaterial,
  NeutralToneMapping,
  PerspectiveCamera,
  PlaneGeometry,
  PMREMGenerator,
  PointLight,
  RepeatWrapping,
  Scene,
  Shape,
  SphereGeometry,
  SRGBColorSpace,
  type Texture,
  TextureLoader,
  Vector2,
  WebGLRenderer,
} from 'three';

const UPPER: Array<[number, number]> = [[0, 0], [41, 0], [50, 9], [50, 31], [32, 31], [32, 18], [18, 18], [18, 41], [30, 41], [12, 59], [0, 59]];
const LOWER: Array<[number, number]> = [[50, 100], [9, 100], [0, 91], [0, 69], [18, 69], [18, 82], [32, 82], [32, 59], [20, 59], [38, 41], [50, 41]];

/** Where the mark stands in its stage: x/y in -1..1 of the stage (y up), height as a share of the
 *  stage height, yaw/pitch in degrees, opacity 0..1. */
export interface Pose {
  x: number;
  y: number;
  height: number;
  yaw: number;
  pitch: number;
  opacity: number;
  /** environment rotation in degrees: sweeps the studio highlight across the faces (default 0) */
  sweep?: number;
}

/** How one stage lights the S. */
export interface Look {
  /** the stage's poster: reflected by the S */
  env: string;
  /** the shot's haze colour; about 18% of it lies over the S */
  fog: number;
  key: { color: number; intensity: number; position: [number, number, number] };
  rim: { color: number; intensity: number; position: [number, number, number] };
}

export interface Mark3D {
  /** Moves the canvas into `el`, sizes it to it and lights the S with `look`. */
  attach(el: HTMLElement, look: string): void;
  /** The pose to settle towards (smoothed per frame). `snap` jumps there. */
  setPose(p: Pose, snap?: boolean): void;
  setActive(on: boolean): void;
  dispose(): void;
}

const FOV = 26;
const DIST = 10;
const UNIT = 0.01; // one brand-grid unit in world units: the mark is 1.0 tall
const CHAMFER = 2.8; // grid units, at 45 degrees
const MAX_TURN = 12; // degrees either way, the pointer included: a three-quarter S reads as a "5"

function shapeOf(points: Array<[number, number]>): Shape {
  const s = new Shape();
  points.forEach(([x, y], i) => {
    const px = (x - 25) * UNIT;
    const py = (50 - y) * UNIT;
    if (i === 0) s.moveTo(px, py);
    else s.lineTo(px, py);
  });
  s.closePath();
  return s;
}

/** The reflection world for one look: the poster on a sphere, a light line 2-14 degrees above the
 *  horizon and a tall strip light to the right. */
function envScene(poster: Texture): { scene: Scene; dispose(): void } {
  const scene = new Scene();
  const sphereGeo = new SphereGeometry(10, 64, 32);
  const sphereMat = new MeshBasicMaterial({ map: poster, side: BackSide });
  scene.add(new Mesh(sphereGeo, sphereMat));
  const r = 9;
  const y0 = r * Math.tan(MathUtils.degToRad(2));
  const y1 = r * Math.tan(MathUtils.degToRad(14));
  const bandGeo = new CylinderGeometry(r, r, y1 - y0, 96, 1, true);
  const bandMat = new MeshBasicMaterial({ color: new Color(2.1, 1.85, 1.5), side: DoubleSide });
  const band = new Mesh(bandGeo, bandMat);
  band.position.y = (y0 + y1) / 2;
  scene.add(band);
  const stripGeo = new PlaneGeometry(1.1, 7);
  const stripMat = new MeshBasicMaterial({ color: new Color(2.6, 2.4, 2.1), side: DoubleSide });
  const strip = new Mesh(stripGeo, stripMat);
  const az = MathUtils.degToRad(55);
  strip.position.set(Math.sin(az) * 8.5, 1.5, Math.cos(az) * 8.5);
  strip.lookAt(0, 1.5, 0);
  scene.add(strip);
  // Studio rig behind the camera, where the front faces look: a tall graded softbox with a hard edge
  // and a black flag beside it. The faces see a few degrees of this, so the edge crosses them as one
  // clean highlight line, and the env rotation (Pose.sweep) moves it across during the fly-past.
  const grad = softboxTexture();
  const boxGeo = new PlaneGeometry(5.2, 7.5);
  const boxMat = new MeshBasicMaterial({ map: grad, color: new Color(3.2, 3.0, 2.7), side: DoubleSide });
  const box = new Mesh(boxGeo, boxMat);
  const flagGeo = new PlaneGeometry(5.6, 9);
  const flagMat = new MeshBasicMaterial({ color: 0x050403, side: DoubleSide });
  const flag = new Mesh(flagGeo, flagMat);
  const place = (m: Mesh, azDeg: number, y: number, r: number) => {
    const a = MathUtils.degToRad(azDeg);
    m.position.set(Math.sin(a) * r, y, Math.cos(a) * r);
    m.lookAt(0, y, 0);
    scene.add(m);
  };
  place(box, 22, 1.2, 7.5);
  place(flag, -18, 0.6, 7.8);
  return {
    scene,
    dispose() {
      sphereGeo.dispose(); sphereMat.dispose(); bandGeo.dispose(); bandMat.dispose(); stripGeo.dispose(); stripMat.dispose();
      grad.dispose(); boxGeo.dispose(); boxMat.dispose(); flagGeo.dispose(); flagMat.dispose();
    },
  };
}

/** A softbox face: bright at the top, falling off to a third at the bottom, so a flat face shows a gradient. */
function softboxTexture(): CanvasTexture {
  const c = document.createElement('canvas');
  c.width = 4;
  c.height = 128;
  const g = c.getContext('2d');
  if (!g) throw new Error('2d canvas unavailable');
  const lin = g.createLinearGradient(0, 0, 0, 128);
  lin.addColorStop(0, '#ffffff');
  lin.addColorStop(0.55, '#b8b8b8');
  lin.addColorStop(1, '#555555');
  g.fillStyle = lin;
  g.fillRect(0, 0, 4, 128);
  const t = new CanvasTexture(c);
  t.colorSpace = SRGBColorSpace;
  return t;
}

/** Brushed metal: fine horizontal streaks in roughness (green channel), seeded so every load matches. */
function brushedRoughness(): CanvasTexture {
  const c = document.createElement('canvas');
  c.width = 256;
  c.height = 256;
  const g = c.getContext('2d');
  if (!g) throw new Error('2d canvas unavailable');
  let seed = 7;
  const rnd = () => ((seed = (seed * 16807) % 2147483647) - 1) / 2147483646;
  g.fillStyle = 'rgb(0,74,0)';
  g.fillRect(0, 0, 256, 256);
  for (let i = 0; i < 1400; i++) {
    const v = Math.round(62 + rnd() * 26);
    g.fillStyle = `rgb(0,${v},0)`;
    g.fillRect(rnd() * 256 - 64, rnd() * 256, 40 + rnd() * 200, 1);
  }
  const t = new CanvasTexture(c);
  t.wrapS = RepeatWrapping;
  t.wrapT = RepeatWrapping;
  t.repeat.set(7, 7); // fine enough to stay brushed metal, not wood grain, when the S fills the screen
  return t;
}

export function createMark3D(reducedMotion: boolean, looks: Record<string, Look>): Mark3D {
  const renderer = new WebGLRenderer({ antialias: true, alpha: true, powerPreference: 'high-performance' });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.outputColorSpace = SRGBColorSpace;
  renderer.toneMapping = typeof NeutralToneMapping === 'number' ? NeutralToneMapping : ACESFilmicToneMapping;
  renderer.toneMappingExposure = 1;
  renderer.setClearColor(0x000000, 0);

  const scene = new Scene();
  const fog = new Fog(0x000000, 0, 55); // at the mark's distance (10) this is about 18% of the haze colour
  scene.fog = fog;
  const camera = new PerspectiveCamera(FOV, 1, 0.1, 100);
  camera.position.set(0, 0, DIST);

  const depth = 9 * UNIT;
  const opts = {
    depth,
    bevelEnabled: true,
    bevelThickness: CHAMFER * UNIT,
    bevelSize: CHAMFER * UNIT,
    bevelOffset: -CHAMFER * UNIT, // cut inward: the silhouette stays the mark's
    bevelSegments: 1, // one flat 45-degree facet: the mark's own cut, and a clean highlight line
    curveSegments: 1,
  };
  const upperGeo = new ExtrudeGeometry(shapeOf(UPPER), opts);
  const lowerGeo = new ExtrudeGeometry(shapeOf(LOWER), opts);
  upperGeo.translate(0, 0, -depth / 2);
  lowerGeo.translate(0, 0, -depth / 2);

  // brand albedo (ink #f3ecdf enamel, gold #f2b134 brushed metal): the 3D S is the logo, not a darker copy
  const ink = new MeshPhysicalMaterial({ color: 0xf3ecdf, roughness: 0.3, metalness: 0, clearcoat: 1, clearcoatRoughness: 0.06, envMapIntensity: 1 });
  const brushed = brushedRoughness();
  const gold = new MeshPhysicalMaterial({ color: 0xf2b134, roughness: 1, roughnessMap: brushed, metalness: 0.85, envMapIntensity: 1.2 });
  const mark = new Group();
  mark.add(new Mesh(upperGeo, ink));
  mark.add(new Mesh(lowerGeo, gold));
  scene.add(mark);

  const key = new DirectionalLight(0xffffff, 1);
  const rim = new DirectionalLight(0xffffff, 1);
  const fill = new HemisphereLight(0xffe9c8, 0x140e08, 0.15);
  scene.add(key, rim, fill);
  // a lamp that rides with the mark, above-left and in front: a falloff across the flat faces
  const glint = new PointLight(0xfff1dc, 0, 0, 2);
  glint.position.set(-0.55, 0.75, 0.85);
  mark.add(glint);

  // ---- looks: one environment per stage, built on first use
  const pmrem = new PMREMGenerator(renderer);
  const loader = new TextureLoader();
  const envs = new Map<string, Texture>();
  const pending = new Set<string>();
  let lookName = '';

  function applyLook(name: string) {
    const look = looks[name];
    if (!look) throw new Error(`unknown look ${name}`);
    lookName = name;
    fog.color.setHex(look.fog);
    key.color.setHex(look.key.color);
    key.intensity = look.key.intensity;
    key.position.set(...look.key.position);
    rim.color.setHex(look.rim.color);
    rim.intensity = look.rim.intensity;
    rim.position.set(...look.rim.position);
    const env = envs.get(name);
    scene.environment = env ?? null;
    if (!env && !pending.has(name)) {
      pending.add(name);
      loader.load(
        look.env,
        (tex) => {
          tex.colorSpace = SRGBColorSpace;
          const world = envScene(tex);
          const built = pmrem.fromScene(world.scene, 0.02).texture;
          world.dispose();
          tex.dispose();
          envs.set(name, built);
          pending.delete(name);
          if (lookName === name) scene.environment = built;
          dirty = true;
          kick();
        },
        undefined,
        () => {
          // the S still renders with its lights; say why it looks flatter
          console.warn(`[mark3d] environment ${look.env} did not load`);
          pending.delete(name);
        },
      );
    }
    dirty = true;
  }

  const target: Required<Pose> = { x: 0, y: 0, height: 0.5, yaw: 0, pitch: 0, opacity: 1, sweep: 0 };
  const cur: Required<Pose> = { ...target };
  const pointer = new Vector2();
  const pointerCur = new Vector2();
  let host: HTMLElement | null = null;
  let active = false;
  let raf = 0;
  let dirty = true;
  let w = 1;
  let h = 1;

  const onPointer = (e: PointerEvent) => {
    pointer.set((e.clientX / window.innerWidth) * 2 - 1, (e.clientY / window.innerHeight) * 2 - 1);
    kick();
  };
  if (!reducedMotion) window.addEventListener('pointermove', onPointer, { passive: true });

  const ro = new ResizeObserver(() => size());
  function size() {
    if (!host) return;
    const r = host.getBoundingClientRect();
    w = Math.max(1, Math.round(r.width));
    h = Math.max(1, Math.round(r.height));
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
    dirty = true;
    kick();
  }

  function apply() {
    const visH = 2 * Math.tan(MathUtils.degToRad(FOV / 2)) * DIST;
    const visW = visH * camera.aspect;
    mark.position.set((cur.x * visW) / 2, (cur.y * visH) / 2, 0);
    const s = Math.max(0.0001, cur.height * visH);
    mark.scale.setScalar(s);
    glint.intensity = 2.2 * s * s; // the lamp scales with the mark, so its light must too (inverse square)
    const yaw = MathUtils.clamp(cur.yaw + pointerCur.x * 2, -MAX_TURN, MAX_TURN);
    const pitch = MathUtils.clamp(cur.pitch + pointerCur.y * 2, -MAX_TURN, MAX_TURN);
    mark.rotation.set(MathUtils.degToRad(pitch), MathUtils.degToRad(yaw), 0);
    scene.environmentRotation.y = MathUtils.degToRad(cur.sweep);
    const o = MathUtils.clamp(cur.opacity, 0, 1);
    for (const m of [ink, gold]) {
      m.transparent = o < 0.999;
      m.opacity = o;
    }
    mark.visible = o > 0.004;
  }

  function frame() {
    raf = 0;
    if (!active) return;
    const k = reducedMotion ? 1 : 0.14;
    let moving = false;
    for (const key of Object.keys(target) as Array<keyof Required<Pose>>) {
      const d = target[key] - cur[key];
      if (Math.abs(d) > 1e-4) moving = true;
      cur[key] += d * k;
    }
    const pd = pointer.clone().sub(pointerCur);
    if (pd.lengthSq() > 1e-6) moving = true;
    pointerCur.addScaledVector(pd, 0.06);
    if (moving || dirty) {
      apply();
      renderer.render(scene, camera);
      dirty = false;
    }
    // keep the loop only while something is still settling
    if (moving) raf = requestAnimationFrame(frame);
  }
  function kick() {
    if (active && !raf) raf = requestAnimationFrame(frame);
  }

  return {
    attach(el, look) {
      if (look !== lookName) applyLook(look);
      if (host !== el) {
        if (host) ro.unobserve(host);
        host = el;
        el.appendChild(renderer.domElement);
        ro.observe(el);
        size();
      }
      kick();
    },
    setPose(p, snap = false) {
      Object.assign(target, p);
      if (snap) Object.assign(cur, p);
      dirty = true;
      kick();
    },
    setActive(on) {
      active = on;
      if (on) kick();
      if (!on && raf) { cancelAnimationFrame(raf); raf = 0; }
    },
    dispose() {
      this.setActive(false);
      window.removeEventListener('pointermove', onPointer);
      ro.disconnect();
      upperGeo.dispose();
      lowerGeo.dispose();
      ink.dispose();
      gold.dispose();
      brushed.dispose();
      for (const t of envs.values()) t.dispose();
      pmrem.dispose();
      renderer.dispose();
      renderer.domElement.remove();
    },
  };
}

/** True when a WebGL context can be made at all. */
export function webglAvailable(): boolean {
  try {
    const c = document.createElement('canvas');
    return !!(c.getContext('webgl2') || c.getContext('webgl'));
  } catch {
    return false;
  }
}
