import { useFrame } from '@react-three/fiber';
import { useEffect, useMemo, useRef, type MutableRefObject } from 'react';
import * as THREE from 'three';
import { MARK_G, MARK_PLATE, skewPoint, type Poly } from '../brand/mark';
import { dropEnv, withEnv } from './env';

// The GAGE mark as a real object: the oblique plate extruded from the brand polygons with a
// one-segment (45-degree, the mark's own angle) chamfer, polished chrome, with the G cut clean
// through and a gold enamel back plate showing in the cut. `charge` (0..1) drives the enamel's
// emission and an inner light: it rises when PLAY is pressed.

const UNIT = 1 / 64; // one design-grid unit in local space; the emblem is 1 x 1 before `size`

function polyToPoints(poly: Poly): THREE.Vector2[] {
  return poly.map((p) => {
    const [x, y] = skewPoint(p);
    return new THREE.Vector2((x - 32) * UNIT, (32 - y) * UNIT);
  });
}

/** The chrome plate with the G as a hole, chamfered front and back. */
export function makePlateGeometry(depth = 7 * UNIT, chamfer = 1.4 * UNIT): THREE.ExtrudeGeometry {
  const shape = new THREE.Shape(polyToPoints(MARK_PLATE));
  shape.holes.push(new THREE.Path(polyToPoints(MARK_G)));
  const g = new THREE.ExtrudeGeometry(shape, {
    depth,
    bevelEnabled: true,
    bevelThickness: chamfer,
    bevelSize: chamfer,
    bevelSegments: 1,
    curveSegments: 1,
  });
  g.translate(0, 0, -depth / 2);
  g.computeVertexNormals();
  return g;
}

/** The enamel back plate: the plate outline, thinner, set behind the chrome. */
export function makeBackGeometry(depth = 2 * UNIT): THREE.ExtrudeGeometry {
  const shape = new THREE.Shape(polyToPoints(MARK_PLATE));
  const g = new THREE.ExtrudeGeometry(shape, { depth, bevelEnabled: false, curveSegments: 1 });
  g.translate(0, 0, -depth / 2);
  g.scale(0.96, 0.96, 1); // tucked inside the chrome's silhouette
  return g;
}

/** A chamfered coin with the mark in relief, for currency products. */
export function makeCoinGeometry(): { disc: THREE.ExtrudeGeometry; relief: THREE.ExtrudeGeometry } {
  const disc = new THREE.Shape();
  disc.absarc(0, 0, 0.62, 0, Math.PI * 2, false);
  const d = new THREE.ExtrudeGeometry(disc, { depth: 0.07, bevelEnabled: true, bevelThickness: 0.03, bevelSize: 0.03, bevelSegments: 1, curveSegments: 96 });
  d.translate(0, 0, -0.035);
  const relief = makePlateGeometry(3 * UNIT, 0.8 * UNIT);
  relief.scale(0.82, 0.82, 1);
  relief.translate(0, 0, 0.07);
  return { disc: d, relief };
}

export interface EmblemProps {
  size?: number;
  position?: [number, number, number];
  rotation?: [number, number, number];
  /** 0..1, read every frame (a ref so GSAP can drive it without re-rendering). */
  charge?: MutableRefObject<number>;
  /** Normalised pointer (-1..1) the emblem leans toward; null = idle sway only. */
  pointer?: MutableRefObject<{ x: number; y: number }> | null;
  /** Slow idle turn (radians per second); 0 = none. */
  spin?: number;
  castShadow?: boolean;
  /** Chrome reflection strength: low under a flash key, high when the environment is the only light. */
  reflect?: number;
  /** Idle sway and bob; false holds the emblem still (reduced motion). */
  sway?: boolean;
}

/** Shared chrome: near-white base, fully metallic, polished. Reflections come from the HDRI, at `env`. */
export function useChrome(env = 0.9): THREE.MeshPhysicalMaterial {
  return useMemo(() => withEnv(new THREE.MeshPhysicalMaterial({ color: '#f3f3f3', metalness: 1, roughness: 0.09 }), env), [env]);
}

export function Emblem({ size = 1, position = [0, 0, 0], rotation = [0, 0, 0], charge, pointer, spin = 0, castShadow = true, reflect = 0.55, sway = true }: EmblemProps) {
  const group = useRef<THREE.Group>(null);
  const plate = useMemo(() => makePlateGeometry(), []);
  const back = useMemo(() => makeBackGeometry(), []);
  const chrome = useChrome(reflect);
  const enamel = useMemo(
    () => withEnv(new THREE.MeshPhysicalMaterial({ color: '#F2B134', metalness: 0.35, roughness: 0.32, clearcoat: 1, clearcoatRoughness: 0.08, emissive: new THREE.Color('#F2B134'), emissiveIntensity: 0 }), 0.9),
    [],
  );
  const glow = useRef<THREE.PointLight>(null);
  // Registered again on every mount (StrictMode mounts twice in dev), released on unmount.
  useEffect(() => {
    withEnv(chrome, reflect);
    withEnv(enamel, 0.9);
    return () => {
      plate.dispose();
      back.dispose();
      dropEnv(chrome);
      dropEnv(enamel);
      chrome.dispose();
      enamel.dispose();
    };
  }, [plate, back, chrome, enamel, reflect]);

  useFrame((state, dt) => {
    const g = group.current;
    if (!g) return;
    const c = charge?.current ?? 0;
    enamel.emissiveIntensity = 0.05 + c * c * 9;
    enamel.emissive.setRGB(0.176 + c * 0.8, 0.43 + c * 0.55, 0.725 + c * 0.27);
    if (glow.current) glow.current.intensity = c * c * 14;
    const t = sway ? state.clock.elapsedTime : 0;
    const px = pointer?.current.x ?? 0;
    const py = pointer?.current.y ?? 0;
    const k = sway ? 1 - Math.exp(-dt * 3.2) : 1;
    const targetY = rotation[1] + px * 0.14 + Math.sin(t * 0.35) * 0.05 + (spin ? t * spin : 0);
    const targetX = rotation[0] - py * 0.07 + Math.sin(t * 0.5) * 0.02;
    g.rotation.y += (targetY - g.rotation.y) * (spin ? 1 : k);
    g.rotation.x += (targetX - g.rotation.x) * k;
    g.position.y = position[1] + Math.sin(t * 0.8) * 0.012 * size;
  });

  return (
    <group ref={group} position={position} rotation={rotation} scale={size}>
      <mesh geometry={plate} material={chrome} castShadow={castShadow} receiveShadow />
      <mesh geometry={back} material={enamel} position={[0, 0, -6 * UNIT]} castShadow={castShadow} />
      <pointLight ref={glow} position={[0, 0, 0.12]} color="#ffe2b0" intensity={0} distance={2.2} decay={2} />
    </group>
  );
}

/** Brushed dark metal: the coin's field and the pass's rim, so the polished mark reads on it. */
export function useBrushed(): THREE.MeshPhysicalMaterial {
  return useMemo(() => withEnv(new THREE.MeshPhysicalMaterial({ color: '#6b6c70', metalness: 1, roughness: 0.42 }), 0.7), []);
}

/** The currency coin: a brushed field with the mark in polished relief on both faces. */
export function Coin({ size = 1, spin = 0.5 }: { size?: number; spin?: number }) {
  const group = useRef<THREE.Group>(null);
  const geo = useMemo(() => makeCoinGeometry(), []);
  const chrome = useChrome();
  const field = useBrushed();
  useFrame((state) => {
    if (group.current) group.current.rotation.y = state.clock.elapsedTime * spin;
  });
  return (
    <group ref={group} scale={size}>
      <mesh geometry={geo.disc} material={field} castShadow receiveShadow />
      <mesh geometry={geo.relief} material={chrome} castShadow />
      <mesh geometry={geo.relief} material={chrome} rotation={[0, Math.PI, 0]} castShadow />
    </group>
  );
}

/** ID-1 proportions (85.6 x 54 mm) at 1 unit wide, and the card's own thickness. */
const PASS_W = 1.0;
const PASS_H = 0.63;
const PASS_CORE = 0.008;
const PASS_EDGE = 0.002;
/** The face sits this far in front of the card's centre plane (core / 2 + edge + a hair). */
const PASS_FACE_Z = PASS_CORE / 2 + PASS_EDGE + 0.0006;

/**
 * The pass outline: two 45-degree corners, the brand's chamfer. `mirror` flips it in x, for the
 * back face (turned half a revolution, it lines up with the card and its print reads right).
 */
function passShape(mirror = false): THREE.Shape {
  const w = PASS_W, h = PASS_H, c = 0.07, m = mirror ? -1 : 1;
  const pts: [number, number][] = [[-w / 2 + c, -h / 2], [w / 2, -h / 2], [w / 2, h / 2 - c], [w / 2 - c, h / 2], [-w / 2, h / 2], [-w / 2, -h / 2 + c]];
  // ShapeGeometry fixes the winding itself, so a mirrored outline still faces +z.
  return new THREE.Shape(pts.map(([x, y]) => new THREE.Vector2(x * m, y)));
}

const PRINT_PX = 1024;
const CODE_FONT = '"Sofia Sans Extra Condensed", "Arial Narrow", sans-serif';

/** A fixed bar pattern (not a real symbology: it encodes nothing). */
const BARS = [3, 1, 2, 1, 1, 3, 2, 1, 1, 2, 3, 1, 2, 2, 1, 1, 3, 1, 1, 2, 2, 1, 3, 1, 1, 1, 2, 3, 1, 2, 1, 1, 2, 1, 3, 2, 1, 1, 2, 1];

/**
 * The printed faces of the pass, drawn on a canvas in the client's own type: the front carries
 * SOLAR RP, VIP, the 30-day term and a bar strip over a printed code; the back a stripe.
 * (Retired with the 3D showroom: nothing renders the pass since the footage-led client.)
 * Redrawn once the display face has loaded (the first draw may fall back to Arial Narrow).
 */
function usePassPrint(side: 'front' | 'back'): THREE.CanvasTexture {
  const tex = useMemo(() => {
    const canvas = document.createElement('canvas');
    canvas.width = PRINT_PX;
    canvas.height = Math.round(PRINT_PX * (PASS_H / PASS_W));
    const t = new THREE.CanvasTexture(canvas);
    t.colorSpace = THREE.SRGBColorSpace;
    t.anisotropy = 8;
    // ShapeGeometry UVs are the shape's own coordinates: map [-w/2, w/2] x [-h/2, h/2] to [0, 1].
    t.repeat.set(1 / PASS_W, 1 / PASS_H);
    t.offset.set(0.5, 0.5);
    return t;
  }, []);
  useEffect(() => {
    const canvas = tex.image as HTMLCanvasElement;
    const draw = () => {
      const ctx = canvas.getContext('2d');
      if (!ctx) return;
      const W = canvas.width, H = canvas.height, ink = '#f7f2e8', sun = '#ffc75a';
      ctx.fillStyle = '#0c0c0e';
      ctx.fillRect(0, 0, W, H);
      if (side === 'back') {
        ctx.fillStyle = '#1d1d20';
        ctx.fillRect(0, H * 0.14, W, H * 0.17);
        ctx.fillStyle = '#a7abb2';
        ctx.font = `700 34px ${CODE_FONT}`;
        ctx.fillText('SOLAR RP  VIP PASS', W * 0.07, H * 0.88);
      } else {
        ctx.fillStyle = ink;
        ctx.font = `800 58px ${CODE_FONT}`;
        ctx.fillText('SOLAR RP', W * 0.06, H * 0.3);
        ctx.font = `800 330px ${CODE_FONT}`;
        ctx.fillText('VIP', W * 0.05, H * 0.78);
        ctx.fillStyle = sun;
        ctx.font = `700 56px ${CODE_FONT}`;
        ctx.fillText('30 DAYS', W * 0.06, H * 0.91);
        const x0 = W * 0.6, y0 = H * 0.66, bh = H * 0.15, unit = 6;
        let x = x0;
        ctx.fillStyle = ink;
        BARS.forEach((wd, i) => { if (i % 2 === 0) ctx.fillRect(x, y0, wd * unit, bh); x += wd * unit; });
        ctx.fillStyle = '#a7abb2';
        ctx.font = `700 36px ${CODE_FONT}`;
        ctx.fillText('SRP · VIP · 030', x0, y0 + bh + 44);
      }
      tex.needsUpdate = true;
    };
    draw();
    let live = true;
    document.fonts.load(`800 100px "Sofia Sans Extra Condensed"`).then(() => { if (live) draw(); }, () => undefined);
    return () => { live = false; };
  }, [tex, side]);
  useEffect(() => () => tex.dispose(), [tex]);
  return tex;
}

/** The VIP pass: a thin gloss-black card, printed both sides, the mark inset in chrome on the front. */
export function PassCard({ size = 1 }: { size?: number }) {
  const { core, face, rear } = useMemo(() => {
    const s = passShape();
    const c = new THREE.ExtrudeGeometry(s, { depth: PASS_CORE, bevelEnabled: true, bevelThickness: PASS_EDGE, bevelSize: PASS_EDGE, bevelSegments: 2, curveSegments: 1 });
    c.translate(0, 0, -PASS_CORE / 2);
    return { core: c, face: new THREE.ShapeGeometry(s), rear: new THREE.ShapeGeometry(passShape(true)) };
  }, []);
  const mark = useMemo(() => {
    const g = makePlateGeometry(1.6 * UNIT, 0.4 * UNIT);
    g.scale(0.3, 0.3, 1);
    return g;
  }, []);
  const chrome = useChrome();
  const front = usePassPrint('front');
  const back = usePassPrint('back');
  const edge = useMemo(() => withEnv(new THREE.MeshPhysicalMaterial({ color: '#0b0b0d', metalness: 0.2, roughness: 0.3, clearcoat: 1, clearcoatRoughness: 0.1 }), 0.6), []);
  const frontMat = useMemo(() => withEnv(new THREE.MeshPhysicalMaterial({ map: front, roughness: 0.5, metalness: 0, clearcoat: 0.35, clearcoatRoughness: 0.2 }), 0.22), [front]);
  const backMat = useMemo(() => withEnv(new THREE.MeshPhysicalMaterial({ map: back, roughness: 0.5, metalness: 0 }), 0.3), [back]);
  useEffect(() => () => { core.dispose(); face.dispose(); rear.dispose(); mark.dispose(); edge.dispose(); frontMat.dispose(); backMat.dispose(); }, [core, face, rear, mark, edge, frontMat, backMat]);
  return (
    <group scale={size}>
      <mesh geometry={core} material={edge} castShadow receiveShadow />
      <mesh geometry={face} material={frontMat} position={[0, 0, PASS_FACE_Z]} receiveShadow />
      <mesh geometry={rear} material={backMat} position={[0, 0, -PASS_FACE_Z]} rotation={[0, Math.PI, 0]} receiveShadow />
      <mesh geometry={mark} material={chrome} position={[0.3, 0.02, PASS_FACE_Z + 0.004]} castShadow />
    </group>
  );
}
