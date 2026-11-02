import { useGLTF } from '@react-three/drei';

// Every heavy asset the 3D layer loads. They are never inlined into index.html: Vite serves them
// from public/assets in dev; in GAGE-MP.exe the host answers https://app.gage/assets/<path> from
// {app}\launcher-assets\<path> (NATIVE-INTEGRATION.md). Relative URLs resolve against the page,
// so the same build works in both. Licences and sources: ASSETS.md.

const base = `${import.meta.env.BASE_URL}assets/`;

export const ASSET = {
  character: `${base}models/character.glb`,
  car: `${base}models/car.glb`,
  watch: `${base}models/watch.glb`,
  streetLamp: `${base}models/street_lamp.glb`,
  hydrant: `${base}models/fire_hydrant.glb`,
  barrier: `${base}models/road_barrier.glb`,
  manhole: `${base}models/manhole.glb`,
  trashbag: `${base}models/trashbag.glb`,
  coveredCar: `${base}models/covered_car.glb`,
  /** Poly Haven "Hansaplatz", desaturated to 12% so the flash stays the only colour of light. */
  hdri: `${base}hdri/hansaplatz_neutral_1k.hdr`,
  asphalt: {
    map: `${base}textures/asphalt_02_diff_1k.webp`,
    normal: `${base}textures/asphalt_02_nor_gl_1k.webp`,
    roughness: `${base}textures/asphalt_02_rough_1k.webp`,
    ao: `${base}textures/asphalt_02_ao_1k.webp`,
  },
  wall: {
    map: `${base}textures/concrete_wall_007_diff_1k.webp`,
    normal: `${base}textures/concrete_wall_007_nor_gl_1k.webp`,
    roughness: `${base}textures/concrete_wall_007_rough_1k.webp`,
    ao: `${base}textures/concrete_wall_007_ao_1k.webp`,
  },
} as const;

/** Load a meshopt-compressed GLB. Draco stays off: drei's default decoder path is a CDN. */
export function useModel(url: string) {
  return useGLTF(url, false, true);
}

/** Start fetching the street set in parallel, before the first Suspense boundary asks for it. */
export function preloadStreet(): void {
  for (const u of [ASSET.character, ASSET.car, ASSET.streetLamp, ASSET.hydrant, ASSET.barrier, ASSET.manhole, ASSET.trashbag, ASSET.coveredCar]) {
    useGLTF.preload(u, false, true);
  }
}

/** The shop's models, fetched once the street is up so the first shop visit does not wait. */
export function preloadShowroom(): void {
  for (const u of [ASSET.car, ASSET.watch]) useGLTF.preload(u, false, true);
}
