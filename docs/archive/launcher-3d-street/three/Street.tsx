import { useTexture } from '@react-three/drei';
import { useLayoutEffect, useMemo, useRef } from 'react';
import * as THREE from 'three';
import { ASSET, useModel } from './assets';
import { withEnv } from './env';
import { tuneCar } from './paint';

// The set: a player shot with an on-camera flash against a board-formed concrete wall at night.
// Wet asphalt, a lamp post, barriers, a hydrant, a manhole and a bag, all CC0 scans/models
// (ASSETS.md). The flash (Stage.tsx) is the light: the player stands close to the wall so it
// catches the hotspot and his hard shadow, and everything past a few metres falls to black.

/** Where the wall's face stands (world z). */
export const WALL_Z = -0.55;
/** Where the player stands (feet), 0.65 m off the wall. */
export const PLAYER: [number, number, number] = [-0.55, 0, 0.1];
/** The set pieces each server frame is about (Stage.tsx PLAY_FRAMES). */
export const CAR_POS: [number, number, number] = [5.2, 0, 1.2];
export const COVERED_POS: [number, number, number] = [13, 0, 0.8];
/** The street lamp; the hydrant, barrier and bag stand around it. */
export const PROPS_POS: [number, number, number] = [-5.05, 0, WALL_Z + 0.35];

/** Mark every mesh of a loaded scene as shadow caster/receiver and opt its materials into reflections. */
function prepare(root: THREE.Object3D, opts: { cast?: boolean; receive?: boolean; env?: number; roughnessMul?: number } = {}) {
  root.traverse((o) => {
    const m = o as THREE.Mesh;
    if (!m.isMesh) return;
    m.castShadow = opts.cast ?? true;
    m.receiveShadow = opts.receive ?? true;
    const mats = Array.isArray(m.material) ? m.material : [m.material];
    for (const mat of mats) {
      const s = mat as THREE.MeshStandardMaterial;
      if (!s.isMeshStandardMaterial) continue;
      if (opts.env !== undefined) withEnv(s, opts.env);
      if (opts.roughnessMul !== undefined) s.roughness = Math.min(1, s.roughness * opts.roughnessMul);
    }
  });
}

function Prop({ url, position, rotation = [0, 0, 0], scale = 1, roughnessMul, hide }: { url: string; position: [number, number, number]; rotation?: [number, number, number]; scale?: number; roughnessMul?: number; /** Node names to leave out (a source that ships variants side by side). */ hide?: readonly string[] }) {
  const { scene } = useModel(url);
  const obj = useMemo(() => scene.clone(true), [scene]);
  useLayoutEffect(() => {
    prepare(obj, { env: 0.12, ...(roughnessMul !== undefined ? { roughnessMul } : {}) });
    if (hide) obj.traverse((o) => { if (hide.includes(o.name)) o.visible = false; });
  }, [obj, roughnessMul, hide]);
  return <primitive object={obj} position={position} rotation={rotation} scale={scale} />;
}

/** The Poly Haven hydrant ships a clean and an aged variant side by side: keep the aged one. */
const HYDRANT_HIDE = ['fire_hydrant'] as const;

/**
 * The night-racing car, parked along the wall with its nose turned toward the street, well right
 * of the player so the hero framings never reach it. Real scale (4.4 m), wheels on the asphalt.
 */
function StreetCar() {
  const { scene } = useModel(ASSET.car);
  const obj = useMemo(() => {
    const o = scene.clone(true);
    tuneCar(o, 0.7);
    o.traverse((c) => {
      const m = c as THREE.Mesh;
      if (m.isMesh) { m.castShadow = true; m.receiveShadow = true; }
    });
    return o;
  }, [scene]);
  return <primitive object={obj} position={CAR_POS} rotation={[0, -Math.PI / 2 + 0.3, 0]} />;
}

type TexSet = { map: string; normal: string; roughness: string; ao: string };

/** A tiled PBR texture set (colour in sRGB, the rest linear). */
function useTiled(set: TexSet, repeat: [number, number]) {
  const tex = useTexture({ map: set.map, normalMap: set.normal, roughnessMap: set.roughness, aoMap: set.ao });
  useLayoutEffect(() => {
    for (const t of Object.values(tex)) {
      t.wrapS = t.wrapT = THREE.RepeatWrapping;
      t.repeat.set(repeat[0], repeat[1]);
      t.anisotropy = 8;
      t.needsUpdate = true;
    }
    tex.map.colorSpace = THREE.SRGBColorSpace;
  }, [tex, repeat]);
  return tex;
}

const ASPHALT_REPEAT: [number, number] = [10, 9];
/** The wall runs 36 m, from the props on the left past the covered car on the right. */
const WALL_W = 36;
const WALL_REPEAT: [number, number] = [WALL_W / 3.2, 1.6];

/** Opt a JSX material into reflections once it exists. */
const envRef = (k: number) => (m: THREE.MeshStandardMaterial | null) => { if (m) withEnv(m, k); };

function Asphalt() {
  const tex = useTiled(ASSET.asphalt, ASPHALT_REPEAT);
  return (
    <mesh rotation={[-Math.PI / 2, 0, 0]} position={[0, 0, WALL_Z + 18]} receiveShadow>
      <planeGeometry args={[40, 36]} />
      {/* Wet: low roughness so the flash leaves a specular pool and the strips a sheen. */}
      <meshStandardMaterial ref={envRef(0.35)} {...tex} roughness={0.42} metalness={0} color="#9a9894" normalScale={new THREE.Vector2(1.1, 1.1)} />
    </mesh>
  );
}

/** Board-formed concrete, 36 m by 5 m: the backdrop that catches the flash and the shadow. No reflections. */
function Wall() {
  const tex = useTiled(ASSET.wall, WALL_REPEAT);
  return (
    <group position={[0, 0, WALL_Z]}>
      <mesh position={[0, 2.5, -0.15]} receiveShadow castShadow>
        <boxGeometry args={[WALL_W, 5, 0.3]} />
        <meshStandardMaterial {...tex} color="#d8d8d6" roughness={0.92} metalness={0} normalScale={new THREE.Vector2(1.4, 1.4)} />
      </mesh>
      {/* A grime kick at the foot of the wall where the street water sits. */}
      <mesh position={[0, 0.16, 0.012]} receiveShadow>
        <planeGeometry args={[WALL_W, 0.32]} />
        <meshStandardMaterial ref={envRef(0.3)} color="#141416" roughness={0.35} metalness={0} transparent opacity={0.72} />
      </mesh>
    </group>
  );
}

/** The player's character (CC0, MakeHuman-based; see ASSETS.md), feet on the origin. */
export function Character({ position = [0, 0, 0], rotationY = 0 }: { position?: [number, number, number]; rotationY?: number }) {
  const { scene } = useModel(ASSET.character);
  const obj = useMemo(() => scene.clone(true), [scene]);
  useLayoutEffect(() => {
    prepare(obj, { env: 0.06 });
    obj.traverse((o) => {
      const m = o as THREE.Mesh;
      if (!m.isMesh) return;
      const mat = m.material as THREE.MeshStandardMaterial;
      // The source eyes are a near-mirror with a pale texture: under an on-axis flash they read
      // as blank slits. A dark, glossy eye takes a single catchlight from the flash instead.
      if (/eyes/i.test(mat.name)) {
        const eye = mat.clone();
        eye.color.set('#2a2420');
        eye.roughness = 0.12;
        withEnv(eye, 0.5);
        m.material = eye;
      }
    });
    // Feet on the ground, whatever the source pivot.
    const box = new THREE.Box3().setFromObject(obj);
    obj.position.y -= box.min.y;
  }, [obj]);
  return (
    <group position={position} rotation={[0, rotationY, 0]}>
      <primitive object={obj} />
    </group>
  );
}

/**
 * The lamp's spill from behind-left, at a tenth of a key: just enough rim to keep the player's
 * dark side off the dark wall. No shadow map (the flash owns the only one).
 */
function RimLight() {
  const light = useRef<THREE.SpotLight>(null);
  const target = useMemo(() => {
    const t = new THREE.Object3D();
    t.position.set(PLAYER[0], 1.45, PLAYER[2]);
    return t;
  }, []);
  useLayoutEffect(() => {
    if (light.current) light.current.target = target;
  }, [target]);
  return (
    <>
      <primitive object={target} />
      <spotLight ref={light} position={[-2.6, 3.4, WALL_Z + 0.3]} color="#dfe8f5" intensity={4} angle={0.4} penumbra={0.8} decay={2} distance={0} />
    </>
  );
}

export function Street() {
  return (
    <group>
      <Asphalt />
      <Wall />
      {/* Turned three-quarters toward the copy: the far arm falls behind the torso. */}
      <Character position={PLAYER} rotationY={1.05} />
      <RimLight />
      <Prop url={ASSET.manhole} position={[0.6, 0.005, 1.35]} rotation={[0, 0.4, 0]} />
      {/* The kerb group (Harbor's frame): hydrant, lamp, barrier and a bag around PROPS_POS. */}
      <Prop url={ASSET.hydrant} position={[PROPS_POS[0] + 1.05, 0, WALL_Z + 0.55]} rotation={[0, 0.7, 0]} hide={HYDRANT_HIDE} />
      <Prop url={ASSET.streetLamp} position={PROPS_POS} rotation={[0, 0.9, 0]} />
      <Prop url={ASSET.barrier} position={[PROPS_POS[0] - 0.85, 0, WALL_Z + 0.5]} rotation={[0, 0.05, 0]} />
      <Prop url={ASSET.trashbag} position={[PROPS_POS[0] - 0.55, 0, WALL_Z + 0.55]} rotation={[0, 2.1, 0]} />
      <StreetCar />
      {/* Freeroam's frame: the covered car on the far right, a second manhole in front of it. */}
      <Prop url={ASSET.coveredCar} position={COVERED_POS} rotation={[0, Math.PI / 2 + 0.12, 0]} />
      <Prop url={ASSET.manhole} position={[COVERED_POS[0] - 2.34, 0.005, COVERED_POS[2] + 2.6]} rotation={[0, 1.1, 0]} />
    </group>
  );
}
