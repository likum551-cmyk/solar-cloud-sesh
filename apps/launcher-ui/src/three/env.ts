import * as THREE from 'three';

// Reflections without fill. A scene-wide `scene.environment` lights every material evenly and
// overrides each material's envMapIntensity (three r173 WebGLRenderer), which flattens the flash
// look into mid-grey. So the HDRI + strips render into this side scene only, and each material
// that should reflect (chrome, car paint, the polished floor, wet asphalt) opts in at its own
// strength. Everything else is lit by the flash alone.

/** The scene drei's <Environment scene={envScene}> writes its texture into. Never rendered. */
export const envScene = new THREE.Scene();

type EnvMaterial = THREE.MeshStandardMaterial;
const registered = new Set<EnvMaterial>();
let bound: THREE.Texture | null = null;

/** Give `mat` the studio reflections at `intensity`. Safe to call before the HDRI has loaded. */
export function withEnv<T extends EnvMaterial>(mat: T, intensity: number): T {
  mat.envMapIntensity = intensity;
  registered.add(mat);
  if (bound && mat.envMap !== bound) {
    mat.envMap = bound;
    mat.needsUpdate = true;
  }
  return mat;
}

/** Called every frame by the stage: binds a newly loaded environment to every opted-in material. */
export function syncEnv(): void {
  const tex = envScene.environment;
  if (!tex || tex === bound) return;
  bound = tex;
  for (const m of registered) {
    m.envMap = tex;
    m.needsUpdate = true;
  }
}

/** Forget a disposed material. */
export function dropEnv(mat: EnvMaterial): void {
  registered.delete(mat);
}
