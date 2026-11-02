import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { HDRLoader } from 'three/addons/loaders/HDRLoader.js';
// CC0 sources and conversions: ASSETS.md
import palmUrl from '../assets/freeway/palm.glb?url';
import carsUrl from '../assets/freeway/cars.glb?url';
import envUrl from '../assets/freeway/night-env.hdr?url';
import asphaltColor from '../assets/freeway/asphalt-color.webp';
import asphaltNormal from '../assets/freeway/asphalt-normal.webp';
import zincColor from '../assets/freeway/zinc-color.webp';
import zincRough from '../assets/freeway/zinc-rough.webp';
import concreteColor from '../assets/freeway/concrete-color.webp';
import officeColor from '../assets/freeway/facade-office-color.webp';
import officeEmit from '../assets/freeway/facade-office-emit.webp';
import glassColor from '../assets/freeway/facade-glass-color.webp';
import glassEmit from '../assets/freeway/facade-glass-emit.webp';

const IMAGES = {
  asphalt: [asphaltColor, true],
  asphaltN: [asphaltNormal, false],
  zinc: [zincColor, true],
  zincR: [zincRough, false],
  concrete: [concreteColor, true],
  office: [officeColor, true],
  officeE: [officeEmit, true],
  glass: [glassColor, true],
  glassE: [glassEmit, true],
};

/**
 * Decodes every band asset. Resolves to
 * { tex: {name: Texture}, env: equirect DataTexture, palm: {geometry, material},
 *   cars: [sedan, suv, delivery] BufferGeometry }
 * and rejects if any one fails, so the caller can fall back as a whole.
 */
export async function loadFreewayAssets(renderer) {
  const aniso = renderer.capabilities.getMaxAnisotropy();
  const texLoader = new THREE.TextureLoader();
  const tex = {};
  const images = Object.entries(IMAGES).map(async ([name, [url, srgb]]) => {
    const t = await texLoader.loadAsync(url);
    t.colorSpace = srgb ? THREE.SRGBColorSpace : THREE.NoColorSpace;
    t.wrapS = THREE.RepeatWrapping;
    t.wrapT = THREE.RepeatWrapping;
    t.anisotropy = aniso;
    tex[name] = t;
  });
  const envP = new HDRLoader().loadAsync(envUrl).then((t) => {
    t.mapping = THREE.EquirectangularReflectionMapping;
    return t;
  });
  const palmP = new GLTFLoader().loadAsync(palmUrl).then((gltf) => {
    let mesh = null;
    gltf.scene.traverse((o) => {
      if (o.isMesh && !mesh) mesh = o;
    });
    if (!mesh) throw new Error('palm.glb has no mesh');
    return { geometry: mesh.geometry, material: mesh.material };
  });
  // geometry only, one mesh per model (ASSETS.md): sedan, suv, delivery
  const carsP = new GLTFLoader().loadAsync(carsUrl).then((gltf) => {
    const cars = [];
    gltf.scene.traverse((o) => {
      if (o.isMesh) cars.push(o.geometry);
    });
    if (cars.length !== 3) throw new Error(`cars.glb: expected 3 meshes, got ${cars.length}`);
    return cars;
  });
  const [env, palm, cars] = await Promise.all([envP, palmP, carsP, ...images]);
  return { tex, env, palm, cars };
}
