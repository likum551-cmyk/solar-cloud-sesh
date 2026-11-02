import * as THREE from 'three';
import { withEnv } from './env';

// Material edits shared by the street and the showroom.

/** Clone every standard material under `root` once (the cached GLTF stays untouched) and edit it. */
export function eachMaterial(root: THREE.Object3D, edit: (m: THREE.MeshStandardMaterial) => void): void {
  const done = new Map<THREE.Material, THREE.Material>();
  root.traverse((o) => {
    const mesh = o as THREE.Mesh;
    if (!mesh.isMesh) return;
    const swap = (mat: THREE.Material): THREE.Material => {
      const hit = done.get(mat);
      if (hit) return hit;
      const src = mat as THREE.MeshStandardMaterial;
      if (!src.isMeshStandardMaterial) return mat;
      const p = src.clone();
      edit(p);
      done.set(mat, p);
      return p;
    };
    mesh.material = Array.isArray(mesh.material) ? mesh.material.map(swap) : swap(mesh.material);
  });
}

/**
 * The concept car in gloss black: a deep base under a slightly soft clearcoat, so the flash leaves
 * one highlight per panel and the studio strips a line, not a mirror of the whole environment.
 * The source ships Carmine as the bound variant: body panels (Paint 1/2) go black, the red
 * interior goes graphite, and the glass keeps only a faint reflection.
 */
export function tuneCar(root: THREE.Object3D, env = 0.8): void {
  eachMaterial(root, (m) => {
    const p = m as THREE.MeshPhysicalMaterial;
    if (/^Paint [12]/.test(p.name)) {
      p.color.set('#0b0b0d');
      p.metalness = 0.3;
      p.roughness = 0.24;
      if (p.isMeshPhysicalMaterial) { p.clearcoat = 1; p.clearcoatRoughness = 0.08; }
    }
    if (/^Interior 3/.test(p.name)) p.color.set('#16171a');
    withEnv(p, /glass|window/i.test(p.name) ? env * 0.45 : env);
  });
}
