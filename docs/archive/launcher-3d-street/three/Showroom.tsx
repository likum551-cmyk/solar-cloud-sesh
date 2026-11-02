import { RoundedBox, useTexture } from '@react-three/drei';
import { useFrame } from '@react-three/fiber';
import { Suspense, useLayoutEffect, useMemo, useRef, type MutableRefObject } from 'react';
import * as THREE from 'three';
import type { ShowroomModel } from '../state/shop';
import { ASSET, useModel } from './assets';
import { Coin, PassCard, useBrushed, useChrome } from './Emblem';
import { withEnv } from './env';
import { eachMaterial, tuneCar } from './paint';

// The shop's showroom, shot like a product photograph: a matte black seamless floor, a matte
// turntable, the same single flash as the street. Large subjects (car, figure) stand on the
// table; small ones sit on cast-concrete plinths on their own mounts (a sloped watch display, a
// coin stand, an easel for the pass). A contact shadow grounds every subject. The visitor can
// drag to turn the table.

export const SHOWROOM_ORIGIN: [number, number, number] = [60, 0, 0];
/**
 * The box each subject is scaled into: `radius` = largest half-diagonal on the table (x/z),
 * `height` = tallest it may stand. The car is the hero and fills the table; the shop camera key
 * (Stage.tsx) is framed for a subject of about this size.
 */
interface Fit { radius: number; height: number }
const FIT_CAR: Fit = { radius: 1.55, height: 1.2 };
const FIT_OUTFIT: Fit = { radius: 1.0, height: 1.5 };
const FIT_WATCH: Fit = { radius: 0.34, height: 0.62 };
const TABLE_RADIUS = 1.75;
/** Table top above the floor (extrude depth + bevel), and the plinth on it. */
const TABLE_TOP = 0.075;
export const PLINTH_H = 0.42;
/** The watch display's slope: 0.29 m of rise over 0.66 m of depth (about 24 degrees). */
const WEDGE_D = 0.66;
const WEDGE_LOW = 0.05;
const WEDGE_HIGH = 0.34;
const WEDGE_ANGLE = Math.atan2(WEDGE_HIGH - WEDGE_LOW, WEDGE_D);
/** The pass leans back on its easel by this much. */
const EASEL_LEAN = 0.22;
const PASS_SIZE = 0.95;

/** Centre a model on the table and scale it into `fit`. */
function useFitted(url: string, fit: Fit, tune?: (o: THREE.Object3D) => void) {
  const { scene } = useModel(url);
  return useMemo(() => {
    const obj = scene.clone(true);
    obj.traverse((o) => {
      const m = o as THREE.Mesh;
      if (m.isMesh) { m.castShadow = true; m.receiveShadow = true; }
    });
    tune?.(obj);
    const box = new THREE.Box3().setFromObject(obj);
    const size = box.getSize(new THREE.Vector3());
    const centre = box.getCenter(new THREE.Vector3());
    const s = Math.min(fit.radius / Math.hypot(size.x / 2, size.z / 2), fit.height / size.y);
    const wrap = new THREE.Group();
    obj.position.sub(new THREE.Vector3(centre.x, box.min.y, centre.z));
    wrap.add(obj);
    wrap.scale.setScalar(s);
    return wrap;
  }, [scene, tune, fit]);
}

const tuneShowroomCar = (root: THREE.Object3D) => tuneCar(root, 0.8);
const tuneWatch = (root: THREE.Object3D) => eachMaterial(root, (m) => withEnv(m, 0.9));
const tuneOutfit = (root: THREE.Object3D) => eachMaterial(root, (m) => withEnv(m, 0.08));

function FittedModel({ url, fit, tune }: { url: string; fit: Fit; tune?: (o: THREE.Object3D) => void }) {
  const obj = useFitted(url, fit, tune);
  return <primitive object={obj} />;
}

/**
 * Cast concrete for the plinths and mounts: the wall's board-formed concrete maps (cloned, so the
 * wall's tiling is untouched), darkened so the flash hotspot stays below white.
 */
function useConcrete(repeat: number, color: string): THREE.MeshStandardMaterial {
  const tex = useTexture({ map: ASSET.wall.map, normalMap: ASSET.wall.normal, roughnessMap: ASSET.wall.roughness });
  const mat = useMemo(() => {
    const own = (t: THREE.Texture, srgb: boolean) => {
      const c = t.clone();
      c.wrapS = c.wrapT = THREE.RepeatWrapping;
      c.repeat.set(repeat, repeat);
      c.anisotropy = 8;
      if (srgb) c.colorSpace = THREE.SRGBColorSpace;
      c.needsUpdate = true;
      return c;
    };
    return withEnv(new THREE.MeshStandardMaterial({
      map: own(tex.map, true),
      normalMap: own(tex.normalMap, false),
      roughnessMap: own(tex.roughnessMap, false),
      normalScale: new THREE.Vector2(0.9, 0.9),
      color,
      roughness: 1,
      metalness: 0,
    }), 0.12);
  }, [tex, repeat, color]);
  useLayoutEffect(() => () => {
    mat.map?.dispose(); mat.normalMap?.dispose(); mat.roughnessMap?.dispose(); mat.dispose();
  }, [mat]);
  return mat;
}

/** A chamfered cast-concrete plinth for small products, the edges rounded over three segments. */
function Plinth() {
  const geo = useMemo(() => {
    const r = 0.34, c = 0.09, b = 0.02;
    const s = new THREE.Shape();
    s.moveTo(-r + c, -r); s.lineTo(r - c, -r); s.lineTo(r, -r + c); s.lineTo(r, r - c);
    s.lineTo(r - c, r); s.lineTo(-r + c, r); s.lineTo(-r, r - c); s.lineTo(-r, -r + c); s.closePath();
    const g = new THREE.ExtrudeGeometry(s, { depth: PLINTH_H - 2 * b, bevelEnabled: true, bevelThickness: b, bevelSize: b, bevelSegments: 3, curveSegments: 1 });
    g.rotateX(-Math.PI / 2);
    g.translate(0, b, 0);
    return g;
  }, []);
  const mat = useConcrete(0.36, '#5c5c5e');
  useLayoutEffect(() => () => geo.dispose(), [geo]);
  return (
    <>
      <Blob w={1.0} d={1.0} />
      <mesh geometry={geo} material={mat} castShadow receiveShadow />
    </>
  );
}

let blobTexture: THREE.CanvasTexture | null = null;
/** A radial falloff, black with alpha 1 at the centre to 0 at the rim (one texture for every blob). */
function getBlobTexture(): THREE.CanvasTexture {
  if (blobTexture) return blobTexture;
  const c = document.createElement('canvas');
  c.width = c.height = 128;
  const ctx = c.getContext('2d');
  if (ctx) {
    const g = ctx.createRadialGradient(64, 64, 0, 64, 64, 64);
    g.addColorStop(0, 'rgba(0,0,0,1)');
    g.addColorStop(0.45, 'rgba(0,0,0,0.7)');
    g.addColorStop(1, 'rgba(0,0,0,0)');
    ctx.fillStyle = g;
    ctx.fillRect(0, 0, 128, 128);
  }
  blobTexture = new THREE.CanvasTexture(c);
  return blobTexture;
}

/**
 * The contact shadow under a subject, baked as a soft ellipse `w` (x) by `d` (z) that turns with
 * it: product-photo grounding with no extra render pass (drei's ContactShadows re-rendered the
 * whole table every frame and left a hard-edged patch on it).
 */
function Blob({ w, d, opacity = 0.85 }: { w: number; d: number; opacity?: number }) {
  const map = useMemo(getBlobTexture, []);
  return (
    <mesh rotation={[-Math.PI / 2, 0, 0]} position={[0, 0.002, 0]} scale={[w, d, 1]} renderOrder={1}>
      <planeGeometry args={[1, 1]} />
      <meshBasicMaterial map={map} color="#000000" transparent opacity={opacity} depthWrite={false} />
    </mesh>
  );
}

/** The watch display: a concrete wedge, low at the front, so the dial faces the lens. */
function WatchWedge() {
  const geo = useMemo(() => {
    const b = 0.008, w = 0.3;
    const s = new THREE.Shape();
    s.moveTo(-WEDGE_D / 2, 0);
    s.lineTo(WEDGE_D / 2, 0);
    s.lineTo(WEDGE_D / 2, WEDGE_HIGH);
    s.lineTo(-WEDGE_D / 2, WEDGE_LOW);
    s.closePath();
    const g = new THREE.ExtrudeGeometry(s, { depth: w, bevelEnabled: true, bevelThickness: b, bevelSize: b, bevelSegments: 3, curveSegments: 1 });
    // Shape x runs front (-) to back (+); extrusion runs across. Turn it so the low edge faces +z.
    g.rotateY(Math.PI / 2);
    g.translate(-w / 2, b, 0);
    return g;
  }, []);
  const mat = useConcrete(0.5, '#3e3e40');
  useLayoutEffect(() => () => geo.dispose(), [geo]);
  return <mesh geometry={geo} material={mat} castShadow receiveShadow />;
}

/** A brushed-metal easel: a ledge with a lip in front and one raked leg behind. */
function Easel() {
  const metal = useBrushed();
  // [height, depth] of the leg's ends: the top meets the pass's back, the foot stands behind.
  const legTop: [number, number] = [0.5, -0.1];
  const legFoot: [number, number] = [0, -0.34];
  const len = Math.hypot(legTop[0] - legFoot[0], legTop[1] - legFoot[1]);
  /** Lean of the leg from vertical, top toward the front (+z). */
  const rake = Math.atan2(legTop[1] - legFoot[1], legTop[0] - legFoot[0]);
  return (
    <group>
      <RoundedBox args={[0.82, 0.035, 0.1]} radius={0.008} smoothness={3} position={[0, 0.0175, 0]} material={metal} castShadow receiveShadow />
      <RoundedBox args={[0.82, 0.03, 0.012]} radius={0.004} smoothness={2} position={[0, 0.05, 0.044]} material={metal} castShadow receiveShadow />
      <RoundedBox args={[0.05, len, 0.025]} radius={0.006} smoothness={2} position={[0, (legTop[0] + legFoot[0]) / 2, (legTop[1] + legFoot[1]) / 2]} rotation={[rake, 0, 0]} material={metal} castShadow receiveShadow />
    </group>
  );
}

/** A low concrete block with the coin standing in its slot. */
function CoinStand() {
  const mat = useConcrete(0.6, '#3e3e40');
  return <RoundedBox args={[0.36, 0.07, 0.2]} radius={0.012} smoothness={3} position={[0, 0.035, 0]} material={mat} castShadow receiveShadow />;
}

function Subject({ model }: { model: ShowroomModel }) {
  switch (model) {
    case 'car': return <><Blob w={1.9} d={3.2} /><FittedModel url={ASSET.car} fit={FIT_CAR} tune={tuneShowroomCar} /></>;
    case 'outfit': return <><Blob w={0.8} d={0.55} /><FittedModel url={ASSET.character} fit={FIT_OUTFIT} tune={tuneOutfit} /></>;
    case 'watch':
      return (
        <>
          <Plinth />
          <group position={[0, PLINTH_H, 0]}>
            <WatchWedge />
            {/* The flat-laid watch lies on the slope, strap running down it, dial to the lens. */}
            <group position={[0, WEDGE_LOW + (WEDGE_HIGH - WEDGE_LOW) / 2 + 0.014, 0]} rotation={[WEDGE_ANGLE, 0, 0]}>
              <FittedModel url={ASSET.watch} fit={FIT_WATCH} tune={tuneWatch} />
            </group>
          </group>
        </>
      );
    case 'emblem':
      return (
        <>
          <Plinth />
          <group position={[0, PLINTH_H, 0]}>
            <Easel />
            {/* The pass stands on the ledge behind the lip, leaning back on the leg. */}
            <group position={[0, 0.036, 0.02]} rotation={[-EASEL_LEAN, 0, 0]}>
              <group position={[0, (0.63 * PASS_SIZE) / 2, 0]}><PassCard size={PASS_SIZE} /></group>
            </group>
          </group>
        </>
      );
    case 'coin':
      return (
        <>
          <Plinth />
          {/* Turned a little off the lens so the rim and the relief catch the flash. */}
          <group position={[0, PLINTH_H, 0]} rotation={[0, 0.55, 0]}>
            <CoinStand />
            <group position={[0, 0.07 - 0.03 + 0.62 * 0.65, 0]} rotation={[-0.05, 0, 0]}><Coin size={0.62} spin={0} /></group>
          </group>
        </>
      );
    case 'none': return null;
  }
}

/** The table itself: a matte black disc with a softened edge and a thin chrome ring. */
function TableTop() {
  const chrome = useChrome();
  const geo = useMemo(() => {
    const s = new THREE.Shape();
    s.absarc(0, 0, TABLE_RADIUS, 0, Math.PI * 2, false);
    const g = new THREE.ExtrudeGeometry(s, { depth: 0.05, bevelEnabled: true, bevelThickness: 0.025, bevelSize: 0.025, bevelSegments: 3, curveSegments: 128 });
    g.rotateX(-Math.PI / 2);
    return g;
  }, []);
  const top = useMemo(() => withEnv(new THREE.MeshStandardMaterial({ color: '#0e0e10', roughness: 0.68, metalness: 0.05 }), 0.2), []);
  useLayoutEffect(() => () => { geo.dispose(); top.dispose(); }, [geo, top]);
  return (
    <>
      <mesh geometry={geo} material={top} receiveShadow castShadow />
      <mesh rotation={[-Math.PI / 2, 0, 0]} material={chrome}>
        <ringGeometry args={[TABLE_RADIUS + 0.005, TABLE_RADIUS + 0.04, 128]} />
      </mesh>
    </>
  );
}

/** Every showroom subject kind, in print order. */
export const PRINT_KINDS: readonly Exclude<ShowroomModel, 'none'>[] = ['car', 'outfit', 'watch', 'emblem', 'coin'];
/** Where each kind's print is centred (height above the floor): the middle of the subject. */
export const PRINT_CENTRE_Y: Record<(typeof PRINT_KINDS)[number], number> = {
  car: 0.62,
  outfit: TABLE_TOP + 1.0,
  watch: TABLE_TOP + PLINTH_H + 0.17,
  emblem: TABLE_TOP + PLINTH_H + 0.3,
  coin: TABLE_TOP + PLINTH_H + 0.443,
};
/** The print rack stands far past the showroom, out of the fog's reach: x = RACK_X0 + i * RACK_STEP. */
export const RACK_X0 = SHOWROOM_ORIGIN[0] + 24;
export const RACK_STEP = 14;

/**
 * One static table per subject kind, hidden except during the shop print pass (Stage.tsx),
 * which photographs each for the shop's contact strip.
 */
export function PrintRack({ rack }: { rack: MutableRefObject<THREE.Group | null> }) {
  return (
    <group ref={rack} visible={false}>
      {PRINT_KINDS.map((kind, i) => (
        <group key={kind} position={[RACK_X0 + i * RACK_STEP, 0, 0]} rotation={[0, kind === 'car' ? 0.5 : 0.35, 0]}>
          <TableTop />
          <group position={[0, TABLE_TOP, 0]}><Subject model={kind} /></group>
        </group>
      ))}
    </group>
  );
}

function Turntable({ children, angle }: { children: React.ReactNode; angle: MutableRefObject<number> }) {
  const table = useRef<THREE.Group>(null);
  useFrame((_, dt) => {
    angle.current += dt * 0.22;
    if (table.current) table.current.rotation.y = angle.current;
  });
  return (
    <group>
      <TableTop />
      <group ref={table} position={[0, TABLE_TOP, 0]}>{children}</group>
    </group>
  );
}

/** A cool back rim so the black car separates from the black floor. */
function BackRim() {
  const light = useRef<THREE.SpotLight>(null);
  const target = useMemo(() => new THREE.Object3D(), []);
  useLayoutEffect(() => {
    if (light.current) light.current.target = target;
  }, [target]);
  return (
    <>
      <primitive object={target} position={[0, 0.5, 0]} />
      <spotLight ref={light} position={[-1.8, 2.6, -3.4]} color="#dfe6f0" intensity={16} angle={0.62} penumbra={0.9} decay={2} distance={0} />
    </>
  );
}

export function Showroom({ model, angle }: { model: ShowroomModel; angle: MutableRefObject<number> }) {
  // Matte seamless: no mirror of the table, only the flash falling off into black.
  const floor = useMemo(() => withEnv(new THREE.MeshStandardMaterial({ color: '#0b0b0c', roughness: 0.78, metalness: 0 }), 0.1), []);
  useLayoutEffect(() => () => floor.dispose(), [floor]);
  return (
    <group position={SHOWROOM_ORIGIN}>
      <mesh rotation={[-Math.PI / 2, 0, 0]} material={floor} receiveShadow>
        <planeGeometry args={[40, 40]} />
      </mesh>
      <BackRim />
      <Turntable angle={angle}>
        <Suspense fallback={null}>
          <Subject model={model} />
        </Suspense>
      </Turntable>
    </group>
  );
}
