import * as THREE from 'three';
import gsap from 'gsap';
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js';
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js';
import { UnrealBloomPass } from 'three/addons/postprocessing/UnrealBloomPass.js';
import { OutputPass } from 'three/addons/postprocessing/OutputPass.js';
import {
  buildingShader,
  glowShader,
  ridgeShader,
  roadShader,
  signShader,
  skyShader,
  trailShader,
} from './shaders.js';
import {
  GANTRY,
  buildBarrier,
  buildGantryTruss,
  buildGuardrail,
  buildLampLens,
  buildLampPole,
  buildPalm,
  buildRidge,
  buildSignBack,
  buildTabBack,
} from './geometry.js';
import {
  PLATES,
  SIGN_CANVAS_H,
  SIGN_CANVAS_W,
  SIGN_H,
  SIGN_W,
  THRU_CANVAS_H,
  THRU_CANVAS_W,
  THRU_H,
  drawSign,
  drawThruSign,
} from './signs.js';
import { loadFreewayAssets } from './assets.js';

// The second panel on each gantry: Los Santos' own freeways, straight on,
// a different one per gantry so consecutive gantries never match.
const THRU_SIGNS = [
  { title: 'Del Perro Fwy', sub: 'Vespucci Beach' },
  { title: 'Olympic Fwy', sub: 'Downtown' },
  { title: 'Los Santos Fwy', sub: 'LS Int’l Airport' },
  { title: 'Great Ocean Hwy', sub: 'Paleto Bay' },
  { title: 'Palomino Fwy', sub: 'Sandy Shores' },
];

// World period: lamps every 40 m, lane dashes every 10 m, a gantry every P.
// The camera drives continuously; each time it has covered P the world
// snaps back by P, which is invisible because everything near repeats in P.
const P = 160;
const LAMP_SPACING = 40;
// The pole 20 m behind each gantry is left out: at every reading distance its
// heads sat on the plate's top corners, the brightest thing in the band. Same
// rule in shaders.js (roadShader's pools and streaks).
// which destination a sign shows: a server by address, a prompt by its title
const signKey = (c) => (c.kind === 'server' ? `server:${c.host}:${c.port}` : `prompt:${c.title}`);
const lampSkipped = (z) => {
  const d = (((z - (GANTRY_Z - 20)) % P) + P) % P;
  return d < 1 || d > P - 1;
};
const GANTRY_Z = -20;
const SIGN_X = 6.2;
const SIGN_Y = 8.0; // plate bottom 5.85 m, a real overhead clearance
// the through-route panel over lane 4 and the shoulder
// narrower than the exit sign, so it never competes with it
const THRU_X = 15.5;
const THRU_W = 4.6;
// sign luminaires on the catwalk, under the plate
const FIXTURE_X = [3.0, 6.4, 9.8];
const THRU_FIXTURE_X = [14.0, 16.0, 17.8]; // geometry.js luminaires
const FIXTURE_Y = GANTRY.cwY; // fixtures sit on the catwalk deck

// how far east of the freeway the skycam's aerial hold sits (m)
const AERIAL_SWING = 250;

// The drive (SHOTS.md): lane 3, a raised driver's eye on a long lens, so the
// plate reads for about 4 s of each approach.
const CAM = { x: 10.65, y: 3.9, z: 0.9, fov: 17 };
// The car eases off as each sign approaches (the plate reads for ~5 s),
// then picks up again once it has passed under.
const SPEED_FAR = 17; // m/s
const SPEED_NEAR = 8;
// a selection carries the car to this far short of the sign that turns
const HOLD_AHEAD = 62;
// the reading hold after a selection: how long, how slow, and how near a
// sign may already be and still take the new destination
const HOLD_S = 8;
const HOLD_CREEP = 0.8;
const HOLD_MIN = 48;
// metres from the camera to the next gantry at boot and in reduced motion
const BOOT_AHEAD = 112;
const STILL_AHEAD = 42;
const FLIP_S = 0.5;
// a selection flips the next gantry unless it is this close (already overhead)
const FLIP_MIN_AHEAD = 16;

// offset (metres travelled) that puts gantry 1 `ahead` metres in front
const offsetFor = (ahead) => P + CAM.z - GANTRY_Z - ahead;

// a cool blue-grey haze: the sodium stays the only warm light, so the
// cool-sky / sodium split survives distance
const FOG_COLOR = new THREE.Color(0.013, 0.016, 0.024);
const SODIUM = new THREE.Color(1.0, 0.56, 0.2);

// World-space triplanar colour and roughness for a MeshStandardMaterial: the
// truss, rails and barrier are long thin boxes whose own UVs would stretch a
// photo texture forty times along a beam. `uni` holds {value} slots filled
// once the textures decode (ASSETS.md). `extra` is GLSL run just before the
// opaque output, with `outgoingLight` in scope.
function triplanar(material, key, uni, metres, extra = '') {
  material.customProgramCacheKey = () => key;
  material.onBeforeCompile = (shader) => {
    shader.uniforms.uTriMap = uni.map;
    shader.uniforms.uTriRough = uni.rough;
    shader.uniforms.uTriOn = uni.on;
    shader.vertexShader = shader.vertexShader
      .replace('#include <common>', '#include <common>\nvarying vec3 vTriPos;\nvarying vec3 vTriN;')
      .replace(
        '#include <worldpos_vertex>',
        `#include <worldpos_vertex>
  mat4 triM = modelMatrix;
  #ifdef USE_INSTANCING
    triM = modelMatrix * instanceMatrix;
  #endif
  vTriPos = (triM * vec4(transformed, 1.0)).xyz;
  vTriN = normalize(mat3(triM) * objectNormal);`,
      );
    shader.fragmentShader = shader.fragmentShader
      .replace(
        '#include <common>',
        `#include <common>
varying vec3 vTriPos;
varying vec3 vTriN;
uniform sampler2D uTriMap;
uniform sampler2D uTriRough;
uniform float uTriOn;
vec4 tri(sampler2D s) {
  vec3 w = pow(abs(vTriN), vec3(4.0));
  w /= w.x + w.y + w.z;
  vec3 p = vTriPos / ${metres.toFixed(2)};
  return texture2D(s, p.yz) * w.x + texture2D(s, p.xz) * w.y + texture2D(s, p.xy) * w.z;
}`,
      )
      // the photo modulates the material colour around 1 (gain = 1 / its mean)
      .replace(
        '#include <map_fragment>',
        `#include <map_fragment>\n  diffuseColor.rgb *= mix(vec3(1.0), tri(uTriMap).rgb * ${uni.gain.toFixed(3)}, uTriOn);`,
      )
      .replace(
        '#include <roughnessmap_fragment>',
        '#include <roughnessmap_fragment>\n  roughnessFactor *= mix(1.0, 0.4 + 1.2 * tri(uTriRough).g, uTriOn);',
      )
      .replace('#include <opaque_fragment>', `${extra}\n#include <opaque_fragment>`);
  };
  return material;
}

function seeded(seed) {
  let s = seed;
  return () => {
    s = (s * 16807) % 2147483647;
    return s / 2147483647;
  };
}

export class FreewayScene {
  constructor(canvas, { reducedMotion = false, onFrame } = {}) {
    this.canvas = canvas;
    this.reducedMotion = reducedMotion;
    this.onFrame = onFrame;
    this.running = false;
    this.time = reducedMotion ? 41.0 : 0;
    this.boot = reducedMotion ? 99 : 0;
    // metres travelled since the last snap; wraps every P
    this.offset = offsetFor(reducedMotion ? STILL_AHEAD : BOOT_AHEAD);
    this.surge = 0; // 0..1 selection speed-up (lens breath)
    this.speed = SPEED_FAR;
    // metres a selection's surge has added so far, and as of the last frame
    this.extra = 0;
    this.lastExtra = 0;
    this.hold = null; // {slot, until}: the reading hold after a selection
    this.pendingTurn = null; // the slot a selection's surge will turn
    // frame()'s camera maths, reused every frame
    this.scratch = {
      pos: new THREE.Vector3(),
      dir: new THREE.Vector3(),
      toSign: new THREE.Vector3(),
      look: new THREE.Vector3(),
      aerial: new THREE.Vector3(),
    };
    this.exit = 0; // 0..1 skycam rise
    this.content = null;
    this.disposables = [];
    this.lastFrame = 0;
    // per-instance culling (beginCull); 0 = not yet placed, keep everything
    this.cullFrustum = new THREE.Frustum();
    this.cullMatrix = new THREE.Matrix4();
    this.cullSphere = new THREE.Sphere();
    this.cullFar = 0;

    // MSAA lives on the composer's target; the canvas itself needs none
    const renderer = new THREE.WebGLRenderer({
      canvas,
      antialias: false,
      powerPreference: 'high-performance',
      stencil: false,
    });
    // Neutral leaves everything under ~0.76 untouched, so the sign's green
    // and white come through the shared tone map as painted
    renderer.toneMapping = THREE.NeutralToneMapping;
    renderer.toneMappingExposure = 1.0;
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 1.5));
    this.renderer = renderer;

    this.scene = new THREE.Scene();
    this.scene.background = new THREE.Color(0, 0, 0);
    this.fogColor = FOG_COLOR.clone();
    this.scene.fog = new THREE.FogExp2(this.fogColor, 0.0034);
    this.camera = new THREE.PerspectiveCamera(CAM.fov, 2, 0.3, 5000);

    this.shared = {
      uTime: { value: 0 },
      uBoot: { value: this.boot },
      uCam: { value: new THREE.Vector3() },
      uFogColor: { value: this.fogColor },
      uFogDensity: { value: 0.0034 },
      uSkyFog: { value: 0.00085 },
      uSodium: { value: SODIUM },
      // 0 on the freeway, 1 in the skycam's aerial hold
      uAerial: { value: 0 },
      // 1 once the photo textures have decoded
      uTexOn: { value: 0 },
    };
    // texture slots, filled when loadAssets() decodes them
    this.texU = Object.fromEntries(
      ['asphalt', 'asphaltN', 'zinc', 'zincR', 'concrete', 'office', 'officeE', 'glass', 'glassE'].map((k) => [k, { value: null }]),
    );
    this.triOn = { value: 0 };

    this.buildEnvironment();
    this.buildSky();
    this.buildRoad();
    this.buildTown();
    this.buildFurniture();
    this.buildLamps();
    this.buildGantries();
    this.buildTraffic();
    this.buildLights();
    this.buildPost();
    // palms and photo textures arrive with the decoded assets
    this.ready = this.loadAssets();
    // Preview builds only (npm run build:preview): the capture and probe
    // scripts read the scene's state through this. Absent from `npm run build`.
    if (import.meta.env.MODE === 'preview') window.__fwDebug = this;

    this.onLost = (e) => {
      e.preventDefault();
      this.stop();
      this.lost = true;
      canvas.dispatchEvent(new CustomEvent('freeway-lost'));
    };
    canvas.addEventListener('webglcontextlost', this.onLost);
  }

  // Every sourced asset (ASSETS.md) in one pass. If any fails to decode the
  // band still runs: drawn palms, untextured steel and procedural asphalt.
  async loadAssets() {
    let a;
    try {
      a = await loadFreewayAssets(this.renderer);
    } catch (err) {
      console.error('freeway assets failed; using the drawn fallbacks', err);
      if (this.disposed) return;
      this.buildPalms(this.track(buildPalm()), this.palmFallbackMaterial());
      return;
    }
    if (this.disposed) return;
    for (const [k, t] of Object.entries(a.tex)) {
      this.texU[k].value = this.track(t);
    }
    this.triOn.value = 1;
    this.shared.uTexOn.value = 1;
    const pm = new THREE.PMREMGenerator(this.renderer);
    const rt = pm.fromEquirectangular(a.env);
    pm.dispose();
    a.env.dispose();
    this.scene.environment = rt.texture;
    // a dusk rooftop is far brighter than a freeway at night
    this.scene.environmentIntensity = 0.45;
    this.disposables.push(rt);
    this.track(a.palm.geometry);
    this.buildPalms(a.palm.geometry, this.track(a.palm.material));
    for (const g of a.cars) this.track(g);
    this.buildCars(a.cars);
    if (!this.running) this.renderOnce();
  }

  palmFallbackMaterial() {
    return this.track(
      new THREE.MeshStandardMaterial({
        color: new THREE.Color(0.06, 0.058, 0.04),
        roughness: 0.82,
        metalness: 0,
        side: THREE.DoubleSide,
      }),
    );
  }

  material(def, extra = {}, opts = {}) {
    const m = new THREE.ShaderMaterial({
      vertexShader: def.vertexShader,
      fragmentShader: def.fragmentShader,
      uniforms: { ...this.shared, ...extra },
      ...opts,
    });
    this.disposables.push(m);
    return m;
  }

  track(o) {
    this.disposables.push(o);
    return o;
  }

  buildEnvironment() {
    // Night reflection environment for the steel: dark, with sodium strips
    // overhead, a cool sky dome and a warm band of city glow at the horizon,
    // so galvanised tops read grey-blue and undersides sodium. Prefiltered once.
    const env = new THREE.Scene();
    env.background = new THREE.Color(0.012, 0.011, 0.012);
    const glow = new THREE.MeshBasicMaterial({ color: new THREE.Color(2.4, 1.3, 0.45) });
    const sky = new THREE.MeshBasicMaterial({ color: new THREE.Color(0.05, 0.075, 0.12), side: THREE.BackSide });
    const dome = new THREE.Mesh(new THREE.SphereGeometry(40, 16, 8, 0, Math.PI * 2, 0, Math.PI / 2.6), sky);
    env.add(dome);
    const haze = new THREE.MeshBasicMaterial({ color: new THREE.Color(0.16, 0.085, 0.04), side: THREE.BackSide });
    env.add(new THREE.Mesh(new THREE.CylinderGeometry(39, 39, 9, 24, 1, true).translate(0, 3, 0), haze));
    for (let i = -3; i <= 3; i++) {
      const strip = new THREE.Mesh(new THREE.BoxGeometry(1.2, 0.2, 0.6), glow);
      strip.position.set(i % 2 ? 7 : -7, 12, i * 9);
      env.add(strip);
    }
    const pm = new THREE.PMREMGenerator(this.renderer);
    const rt = pm.fromScene(env, 0.04);
    this.scene.environment = rt.texture;
    this.disposables.push(rt);
    pm.dispose();
    env.traverse((o) => {
      if (o.geometry) o.geometry.dispose();
    });
    glow.dispose();
    sky.dispose();
    haze.dispose();
  }

  buildSky() {
    // Sky, skyline and ridge ride with the camera so a P-snap never shows.
    this.far = new THREE.Group();
    this.scene.add(this.far);

    const skyGeo = this.track(new THREE.SphereGeometry(4000, 48, 24));
    const sky = new THREE.Mesh(skyGeo, this.material(skyShader, {}, { side: THREE.BackSide, depthWrite: false, fog: false }));
    sky.renderOrder = -10;
    sky.frustumCulled = false;
    this.far.add(sky);

    // the skyline and ridge take the thin sky haze, lifted toward the horizon
    // glow; the lift eases off in the skycam's aerial, where that haze would
    // otherwise turn the far city and the ridge into flat grey walls
    this.cityTint = { value: 1.8 };
    const ridge = new THREE.Mesh(
      this.track(buildRidge()),
      this.material(ridgeShader, { uFogTint: this.cityTint }, { fog: false }),
    );
    ridge.position.set(120, 0, -2200);
    this.far.add(ridge);

    const rnd = seeded(90210);
    const count = 340;
    // Downtown's high-rises: a cluster of stepped towers of varied height so
    // the skyline has a profile, not one box. [x, z, base w, d, height, tiers]
    const TOWERS = [
      [318, -1238, 46, 40, 300, 3],
      [372, -1206, 38, 38, 236, 2],
      [268, -1282, 36, 44, 212, 3],
      [416, -1270, 34, 30, 178, 2],
      [232, -1214, 30, 30, 156, 1],
      [348, -1300, 40, 34, 262, 2],
    ];
    const tierParts = TOWERS.reduce((n, t) => n + t[5], 0);
    const geo = this.track(new THREE.BoxGeometry(1, 1, 1).translate(0, 0.5, 0));
    const cityMat = this.material(
      buildingShader,
      {
        uBFog: this.shared.uSkyFog,
        uFogTint: this.cityTint,
        uLitShare: { value: 0.34 },
        uWinGain: { value: 1.4 },
        uStreetLit: { value: 0.0 },
        uFacadeOn: { value: 1 },
        uOffice: this.texU.office,
        uOfficeE: this.texU.officeE,
        uGlass: this.texU.glass,
        uGlassE: this.texU.glassE,
      },
      { fog: false },
    );
    const city = new THREE.InstancedMesh(geo, cityMat, count + tierParts);
    const m = new THREE.Matrix4();
    const beacons = [];
    for (let i = 0; i < count; i++) {
      const downtown = i < 150;
      let x;
      let z;
      let h;
      if (downtown) {
        // the mid-rise mass around the towers; none rivals them
        const a = rnd() * Math.PI * 2;
        const r = Math.sqrt(rnd()) * 230;
        x = 330 + Math.cos(a) * r * 1.3;
        z = -1250 + Math.sin(a) * r;
        h = 30 + (1 - r / 230) ** 1.6 * 120 * (0.45 + rnd() * 0.75);
      } else {
        // beyond the town grid (buildTown), which ends near z = -1120
        x = (rnd() - 0.45) * 2400;
        z = -1180 - rnd() * 700;
        h = 10 + rnd() ** 2 * 45;
      }
      const w = 18 + rnd() * 34;
      const d = 18 + rnd() * 34;
      m.makeScale(w, h, d);
      m.setPosition(x, 0, z);
      city.setMatrixAt(i, m);
      if (h > 150) beacons.push([x, h + 2, z]);
    }
    // each tower steps back as it rises; every top carries a red beacon
    const SPLIT = { 1: [1], 2: [0.7, 0.3], 3: [0.62, 0.26, 0.12] }; // tier heights
    let n = count;
    for (const [x, z, w, d, h, tiers] of TOWERS) {
      let y = 0;
      for (let k = 0; k < tiers; k++) {
        const th = h * SPLIT[tiers][k];
        const s = 1 - 0.2 * k;
        m.makeScale(w * s, th, d * s);
        m.setPosition(x, y, z);
        city.setMatrixAt(n++, m);
        y += th;
      }
      beacons.push([x, h + 3, z]);
    }
    city.instanceMatrix.needsUpdate = true;
    city.frustumCulled = false;
    this.far.add(city);

    if (beacons.length) {
      const g = this.track(new THREE.BufferGeometry());
      const pos = new Float32Array(beacons.length * 3);
      const col = new Float32Array(beacons.length * 3);
      const size = new Float32Array(beacons.length);
      const kind = new Float32Array(beacons.length);
      beacons.forEach((b, i) => {
        pos.set(b, i * 3);
        col.set([1.8, 0.1, 0.05], i * 3);
        size[i] = 15;
        kind[i] = 2;
      });
      g.setAttribute('position', new THREE.BufferAttribute(pos, 3));
      g.setAttribute('aColor', new THREE.BufferAttribute(col, 3));
      g.setAttribute('aSize', new THREE.BufferAttribute(size, 1));
      g.setAttribute('aKind', new THREE.BufferAttribute(kind, 1));
      const pts = new THREE.Points(g, this.glowMaterial());
      pts.frustumCulled = false;
      this.far.add(pts);
    }
  }

  glowMaterial() {
    if (!this.glowMat) {
      this.uPx = { value: 400 };
      this.glowMat = this.material(
        glowShader,
        { uPx: this.uPx },
        { transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, fog: false },
      );
    }
    return this.glowMat;
  }

  buildRoad() {
    // wide enough that the skycam's yawed aerial never reaches an edge
    const geo = this.track(new THREE.PlaneGeometry(10000, 4200, 1, 1));
    geo.rotateX(-Math.PI / 2);
    geo.translate(0, 0, -1500);
    const road = new THREE.Mesh(
      geo,
      this.material(roadShader, { uAsphalt: this.texU.asphalt, uAsphaltN: this.texU.asphaltN }, { fog: false }),
    );
    road.frustumCulled = false;
    this.scene.add(road);
  }

  // Low-rise blocks filling the 80 m street grid the road shader paints beyond
  // the freeway: three lots a side inside each block, clear of the streets and
  // their lamps. Lots are keyed by block row modulo two, so the town repeats
  // every P along z and the fly-and-snap stays seamless.
  buildTown() {
    const BLOCK = 80;
    const EDGE = 8.5; // street half-width plus kerb and lamp line
    const lot = (BLOCK - 2 * EDGE) / 3;
    const hash = (a, b, c) => {
      const s = Math.sin(a * 127.1 + b * 311.7 + c * 74.7) * 43758.5453;
      return s - Math.floor(s);
    };
    const m = new THREE.Matrix4();
    const mats = [];
    const tops = []; // roof parapets and plant: [x, y, z, w, h, d]
    // Arterials (they match the road shader's sodium-lit streets): every
    // second x-running street (z = 0 mod 160) and every third z-running one
    // (x = 0 mod 240). Their frontage builds taller, so the grid reads.
    const mod = (a, n) => ((a % n) + n) % n;
    // runs further east than west: the skycam's aerial swings out over that side
    for (let bx = -12; bx < 16; bx++) {
      for (let bz = 3; bz > -15; bz--) {
        const row = ((bz % 2) + 2) % 2;
        for (let i = 0; i < 3; i++) {
          for (let j = 0; j < 3; j++) {
            const onArtX = (row === 0 && j === 0) || (row === 1 && j === 2);
            const onArtZ = (mod(bx, 3) === 0 && i === 0) || (mod(bx + 1, 3) === 0 && i === 2);
            const cx = bx * BLOCK + EDGE + lot * (i + 0.5);
            const cz = bz * BLOCK + EDGE + lot * (j + 0.5);
            const h1 = hash(bx, row, i * 3 + j);
            const h2 = hash(bx + 0.37, row, i * 3 + j + 11);
            const h3 = hash(bx + 0.71, row, i * 3 + j + 23);
            if (h1 < 0.16) continue; // parking lots and yards
            const setX = 2 + 4 * h2;
            const setZ = 2 + 4 * h3;
            let x0 = cx - lot / 2 + setX;
            let x1 = cx + lot / 2 - setX;
            // keep an open strip along the freeway (fence ~21 m out, then the
            // frontage land) so the near field stays palms and sky, not walls
            const CLEAR = 58;
            if (x1 > -CLEAR && x0 < CLEAR) {
              if (cx > 0) x0 = Math.max(x0, CLEAR);
              else x1 = Math.min(x1, -CLEAR);
              if (x1 - x0 < 6) continue;
            }
            const w = x1 - x0;
            const d = lot - 2 * setZ;
            const far = Math.abs(cx) > 120;
            const tall = h2 > 0.93 && far;
            let h = tall ? 18 + 22 * h3 : 4 + 9 * h1 * h1;
            if ((onArtX || onArtZ) && far) {
              // main-street mid-rises, tallest where two arterials cross
              h = Math.max(h, (onArtX && onArtZ ? 24 : 9) + (onArtX && onArtZ ? 30 : 17) * h3 * h3);
            }
            const x = (x0 + x1) / 2;
            m.makeScale(w, h, d);
            m.setPosition(x, 0, cz);
            mats.push(m.clone());
            // a coping lip on every roof; plant rooms and tanks on most
            tops.push([x, h, cz, w + 0.5, 0.55, d + 0.5]);
            if (h2 > 0.3) {
              const pw = Math.min(w * 0.4, 3 + 3 * h1);
              const pd = Math.min(d * 0.4, 2.5 + 3 * h3);
              tops.push([x + (h3 - 0.5) * (w - pw) * 0.6, h, cz + (h1 - 0.5) * (d - pd) * 0.6, pw, 1.4 + 2.2 * h2, pd]);
            }
          }
        }
      }
    }
    const geo = this.track(new THREE.BoxGeometry(1, 1, 1).translate(0, 0.5, 0));
    const mat = this.material(
      buildingShader,
      {
        uBFog: this.shared.uFogDensity,
        uFogTint: { value: 1.0 },
        uLitShare: { value: 0.24 },
        uWinGain: { value: 0.7 },
        uStreetLit: { value: 1.0 },
        // the low-rise town keeps its drawn windows: the photo facades are
        // office towers, wrong at four storeys
        uFacadeOn: { value: 0 },
      },
      { fog: false },
    );
    const town = new THREE.InstancedMesh(geo, mat, mats.length);
    mats.forEach((mm, i) => town.setMatrixAt(i, mm));
    town.instanceMatrix.needsUpdate = true;
    town.frustumCulled = false;
    this.scene.add(town);

    // Roof furniture in plain dark concrete: it breaks every box's top edge
    // against the sky and gives the aerial roofs a second level of detail.
    const roofMat = this.track(
      new THREE.MeshStandardMaterial({
        color: new THREE.Color(0.11, 0.105, 0.1),
        roughness: 0.9,
        metalness: 0,
        emissive: new THREE.Color(0.011, 0.01, 0.009),
      }),
    );
    const roof = new THREE.InstancedMesh(geo, roofMat, tops.length);
    tops.forEach(([x, y, z, w, h, d], i) => {
      m.makeScale(w, h, d);
      m.setPosition(x, y - 0.05, z);
      roof.setMatrixAt(i, m);
    });
    roof.instanceMatrix.needsUpdate = true;
    roof.frustumCulled = false;
    this.scene.add(roof);
  }

  // Palms: a row along each verge between the lamps, and loose stands along
  // the town streets. Periodic in P like everything near. `geo` is the
  // sourced palm (ASSETS.md, 440 units tall) or the drawn fallback.
  buildPalms(geo, mat) {
    geo.computeBoundingBox();
    // the sourced model to about 18 m, a tall Los Angeles palm
    const unit = 18 / Math.max(1e-3, geo.boundingBox.max.y);
    if (mat.map) {
      // soft frond edges under the composer's MSAA instead of alpha-test stair-steps
      mat.alphaToCoverage = true;
      // a night silhouette: the daylight albedo would read as lit green
      mat.color.setScalar(0.55);
      mat.envMapIntensity = 0.6;
    }
    const hash = (a, b) => {
      const s = Math.sin(a * 91.3 + b * 47.9) * 24634.6345;
      return s - Math.floor(s);
    };
    const place = [];
    // verge rows: 4 palms per period per side, between the lamp poles
    for (let z = 120; z > -1500; z -= 40) {
      const k = (((z / 40) % 4) + 4) % 4;
      for (const side of [1, -1]) {
        // k 0 lands on a gantry's right-hand tower: keep that one clear
        if (hash(k, side) < 0.2 || (k === 0 && side === 1)) continue;
        place.push([side * (23.5 + 3.5 * hash(k + 9, side)), z - 20 + 8 * (hash(k + 3, side) - 0.5), 0.85 + 0.35 * hash(k + 5, side), k]);
      }
    }
    // along the town's z-running streets, every 20 m on both kerbs
    for (let sx = -640; sx <= 640; sx += 80) {
      if (sx === 0) continue;
      const streetHas = hash(sx, 3) < 0.45;
      if (!streetHas) continue;
      for (let z = 120; z > -1100; z -= 20) {
        const k = (((z / 20) % 8) + 8) % 8;
        for (const kerb of [-7.5, 7.5]) {
          if (hash(sx + kerb, k) < 0.3) continue;
          place.push([sx + kerb, z + 6 * (hash(k, sx) - 0.5), 0.75 + 0.4 * hash(sx, k + kerb), k]);
        }
      }
    }
    const palms = new THREE.InstancedMesh(geo, mat, place.length);
    const m = new THREE.Matrix4();
    const q = new THREE.Quaternion();
    const s = new THREE.Vector3();
    geo.computeBoundingSphere();
    const bs = geo.boundingSphere;
    // every placement's matrix and bounding sphere; updatePalms draws the
    // visible ones (cullInstance)
    const mats = new Float32Array(place.length * 16);
    const spheres = new Float32Array(place.length * 4);
    place.forEach(([x, z, sc, k], i) => {
      q.setFromAxisAngle(new THREE.Vector3(0, 1, 0), hash(x, k) * Math.PI * 2);
      const scale = sc * (mat.map ? unit : 1);
      s.setScalar(scale);
      m.compose(new THREE.Vector3(x, 0, z), q, s);
      m.toArray(mats, i * 16);
      // the yaw can swing the sphere's centre around the trunk: widen by its offset
      spheres.set([x, bs.center.y * scale, z, (bs.radius + Math.hypot(bs.center.x, bs.center.z)) * scale], i * 4);
      palms.setMatrixAt(i, m);
    });
    palms.instanceMatrix.needsUpdate = true;
    palms.frustumCulled = false;
    palms.userData = { mats, spheres };
    this.scene.add(palms);
    this.palms = palms;
    this.updatePalms();
  }

  // three culls an InstancedMesh only as a whole, so without this every palm
  // in the town and every car on the 1.6 km loop is drawn each frame. Call
  // once per frame after the camera is placed.
  beginCull(fogDensity) {
    const cam = this.camera;
    cam.updateMatrixWorld();
    this.cullMatrix.multiplyMatrices(cam.projectionMatrix, cam.matrixWorldInverse);
    this.cullFrustum.setFromProjectionMatrix(this.cullMatrix);
    // exp2 fog leaves under 2% of a surface past this distance
    this.cullFar = 1.98 / fogDensity;
  }

  cullInstance(x, y, z, r) {
    if (!this.cullFar) return true;
    const sp = this.cullSphere;
    sp.center.set(x, y, z);
    sp.radius = r;
    return sp.center.distanceTo(this.camera.position) - r < this.cullFar && this.cullFrustum.intersectsSphere(sp);
  }

  // pack the visible palms to the front of the instance buffer
  updatePalms() {
    if (!this.palms) return;
    const { mats, spheres } = this.palms.userData;
    const dst = this.palms.instanceMatrix.array;
    let n = 0;
    for (let i = 0; i < spheres.length / 4; i++) {
      if (!this.cullInstance(spheres[i * 4], spheres[i * 4 + 1], spheres[i * 4 + 2], spheres[i * 4 + 3])) continue;
      dst.set(mats.subarray(i * 16, i * 16 + 16), n * 16);
      n++;
    }
    this.palms.count = n;
    this.palms.instanceMatrix.needsUpdate = true;
  }

  buildFurniture() {
    // ASSETS.md: Concrete034 on the barrier, Metal038 on every steel part;
    // gains are 1 / each photo's linear mean (0.48, 0.12)
    const concrete = this.track(
      triplanar(
        // weathered, not new: under the sodium a pale K-rail reads as a white strip
        new THREE.MeshStandardMaterial({ color: new THREE.Color(0.15, 0.145, 0.135), roughness: 0.92, metalness: 0 }),
        'tri-concrete',
        { map: this.texU.concrete, rough: this.texU.concrete, on: this.triOn, gain: 2.07 },
        2.5,
      ),
    );
    this.concrete = concrete;
    const steel = this.track(
      triplanar(
        new THREE.MeshStandardMaterial({ color: new THREE.Color(0.62, 0.64, 0.66), roughness: 0.38, metalness: 0.85 }),
        'tri-steel',
        { map: this.texU.zinc, rough: this.texU.zincR, on: this.triOn, gain: 8.3 },
        1.5,
      ),
    );
    this.steel = steel;
    const barrier = new THREE.Mesh(this.track(buildBarrier(-1500, 1700)), concrete);
    this.scene.add(barrier);
    for (const [x, f] of [
      [19.4, 1],
      [-19.4, -1],
    ]) {
      this.scene.add(new THREE.Mesh(this.track(buildGuardrail(x, -900, 1000, f)), steel));
    }
    // guardrail posts every 2 m
    const postGeo = this.track(new THREE.BoxGeometry(0.15, 0.8, 0.12).translate(0, 0.4, 0));
    const n = 500;
    const posts = new THREE.InstancedMesh(postGeo, steel, n * 2);
    const m = new THREE.Matrix4();
    for (let i = 0; i < n; i++) {
      const z = 100 - i * 2;
      m.makeTranslation(19.5, 0, z);
      posts.setMatrixAt(i * 2, m);
      m.makeTranslation(-19.5, 0, z);
      posts.setMatrixAt(i * 2 + 1, m);
    }
    posts.instanceMatrix.needsUpdate = true;
    posts.frustumCulled = false;
    this.scene.add(posts);
  }

  buildLamps() {
    const first = 120;
    const zs = [];
    for (let i = 0; i < 34; i++) {
      const z = first - i * LAMP_SPACING;
      if (!lampSkipped(z)) zs.push(z);
    }
    const pole = new THREE.InstancedMesh(this.track(buildLampPole()), this.steel, zs.length);
    const lensMat = this.track(new THREE.MeshBasicMaterial({ color: new THREE.Color(3.2, 1.7, 0.55) }));
    const lens = new THREE.InstancedMesh(this.track(buildLampLens()), lensMat, zs.length);
    const m = new THREE.Matrix4();
    const glowPos = [];
    zs.forEach((z, i) => {
      m.makeTranslation(0, 0, z);
      pole.setMatrixAt(i, m);
      lens.setMatrixAt(i, m);
      glowPos.push([7.0, 12.12, z], [-7.0, 12.12, z]);
    });
    pole.instanceMatrix.needsUpdate = true;
    lens.instanceMatrix.needsUpdate = true;
    pole.frustumCulled = false;
    lens.frustumCulled = false;
    this.scene.add(pole, lens);
    this.lensMat = lensMat;

    // (the sign luminaires get no glow: they aim up at the plate, so from the
    // road only the lit plate shows; a sprite there clipped to a white burst)
    const g = this.track(new THREE.BufferGeometry());
    const pos = new Float32Array(glowPos.length * 3);
    const col = new Float32Array(glowPos.length * 3);
    const size = new Float32Array(glowPos.length);
    const kind = new Float32Array(glowPos.length);
    glowPos.forEach((p, i) => {
      pos.set(p, i * 3);
      col.set([1, 1, 1], i * 3);
      size[i] = 5.6;
      kind[i] = 0;
    });
    g.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    g.setAttribute('aColor', new THREE.BufferAttribute(col, 3));
    g.setAttribute('aSize', new THREE.BufferAttribute(size, 1));
    g.setAttribute('aKind', new THREE.BufferAttribute(kind, 1));
    const pts = new THREE.Points(g, this.glowMaterial());
    pts.frustumCulled = false;
    this.scene.add(pts);
  }

  buildGantries() {
    const truss = this.track(buildGantryTruss());
    // galvanised zinc: grey with a faint cool cast, satin rather than mirror,
    // so the sodium lamps tint it instead of turning it to bronze; the
    // truss's vertex colours carry the grime at joints and footings
    const zinc = this.track(
      new THREE.MeshStandardMaterial({
        color: new THREE.Color(0.5, 0.54, 0.6),
        roughness: 0.55,
        metalness: 0.4,
        envMapIntensity: 0.7,
        vertexColors: true,
      }),
    );
    // Zinc under sodium still reads as grey metal to the eye: keep 40% of the
    // lamps' orange and let the rest go neutral, a touch cool.
    triplanar(
      zinc,
      'tri-zinc',
      { map: this.texU.zinc, rough: this.texU.zincR, on: this.triOn, gain: 8.3 },
      1.5,
      'outgoingLight = mix(outgoingLight, vec3(dot(outgoingLight, vec3(0.2126, 0.7152, 0.0722))) * vec3(0.93, 0.97, 1.05), 0.6);',
    );
    const signFace = { w: SIGN_W, h: SIGN_H, cw: SIGN_CANVAS_W, ch: SIGN_CANVAS_H };
    const back = this.track(buildSignBack(signFace, PLATES.main));
    const tabBack = this.track(buildTabBack(signFace, PLATES.tab));
    const face = this.track(new THREE.PlaneGeometry(SIGN_W, SIGN_H));
    const footing = this.track(new THREE.BoxGeometry(1.6, 0.9, 1.6).translate(GANTRY.x1, 0.0, 0));
    const concrete = this.concrete;
    const aniso = this.renderer.capabilities.getMaxAnisotropy();
    const left = SIGN_X - SIGN_W / 2;
    const fixtures = new THREE.Vector3(...FIXTURE_X.map((x) => (x - left) / SIGN_W));
    const drop = (SIGN_Y - SIGN_H / 2 - FIXTURE_Y) / SIGN_H;
    const canvasTex = (w, h) => {
      const canvas = document.createElement('canvas');
      canvas.width = w;
      canvas.height = h;
      const tex = this.track(new THREE.CanvasTexture(canvas));
      tex.colorSpace = THREE.SRGBColorSpace;
      tex.anisotropy = aniso;
      return { canvas, tex };
    };

    const thruFace = this.track(new THREE.PlaneGeometry(THRU_W, THRU_H));
    const thruBack = this.track(
      buildSignBack({ w: THRU_W, h: THRU_H, cw: THRU_CANVAS_W, ch: THRU_CANVAS_H }, PLATES.thru),
    );
    const thruLeft = THRU_X - THRU_W / 2;
    const thruFixtures = new THREE.Vector3(...THRU_FIXTURE_X.map((x) => (x - thruLeft) / THRU_W));
    this.thruCount = 0;

    this.gantries = [];
    for (let k = 0; k < 3; k++) {
      const group = new THREE.Group();
      group.position.set(0, 0, GANTRY_Z - k * P);
      group.add(new THREE.Mesh(truss, zinc));
      group.add(new THREE.Mesh(footing, concrete));

      // two canvases per sign: the face it shows and the one it is turning from
      const cur = canvasTex(SIGN_CANVAS_W, SIGN_CANVAS_H);
      const prev = canvasTex(SIGN_CANVAS_W, SIGN_CANVAS_H);
      const mat = this.material(signShader, {
        map: { value: cur.tex },
        uPrev: { value: prev.tex },
        uFlip: { value: 1 },
        uFixtures: { value: fixtures },
        uDrop: { value: drop },
      });
      const sign = new THREE.Group();
      sign.position.set(SIGN_X, SIGN_Y, GANTRY.zf + 0.2);
      const faceMesh = new THREE.Mesh(face, mat);
      faceMesh.position.z = 0.002;
      sign.add(faceMesh);
      sign.add(new THREE.Mesh(back, this.steel));
      const tab = new THREE.Mesh(tabBack, this.steel);
      tab.visible = false;
      sign.add(tab);
      group.add(sign);

      // the through panel: a different Los Santos freeway on each gantry
      const thru = canvasTex(THRU_CANVAS_W, THRU_CANVAS_H);
      drawThruSign(thru.canvas, this.nextThru());
      const thruMat = this.material(signShader, {
        map: { value: thru.tex },
        uPrev: { value: thru.tex },
        uFlip: { value: 1 },
        uFixtures: { value: thruFixtures },
        uDrop: { value: (SIGN_Y - SIGN_H / 2 - FIXTURE_Y) / THRU_H },
      });
      const panel = new THREE.Group();
      // plate bottoms level with the exit sign's plate
      panel.position.set(THRU_X, SIGN_Y - SIGN_H / 2 + THRU_H / 2, GANTRY.zf + 0.2);
      const thruMesh = new THREE.Mesh(thruFace, thruMat);
      thruMesh.position.z = 0.002;
      panel.add(thruMesh, new THREE.Mesh(thruBack, this.steel));
      group.add(panel);

      this.scene.add(group);
      this.gantries.push({ group, mat, thruMat, tab, slot: { cur, prev, thru, kind: 'prompt', flip: 1 } });
    }
  }

  // Car bodies (Kenney car kit, ASSETS.md) under the traffic lamps: dark
  // night paint, one instanced mesh per model, placed each frame by the same
  // formula the lamp shader uses (trailShader), so lamps and bodies agree.
  buildCars(geos) {
    // [width, height, length] in metres for the sedan, the SUV and the box van
    const SIZE = [
      [1.85, 1.45, 4.7],
      [1.95, 1.75, 4.9],
      [2.15, 2.7, 6.4],
    ];
    const PAINT = [0x0b0c0e, 0x16181b, 0x2a0d0e, 0x0d1626, 0x3a3d42, 0x1d1f16];
    // clear-coated paint: the roofs catch the sodium as cars pass under lamps
    const paint = this.track(
      new THREE.MeshPhysicalMaterial({
        color: 0xffffff,
        metalness: 0.5,
        roughness: 0.45,
        clearcoat: 1,
        clearcoatRoughness: 0.12,
        envMapIntensity: 1.2,
      }),
    );
    // contact shadows: a soft dark ellipse under each visible body, fading
    // with the same fog as the road so far ones don't punch holes in the haze
    this.carShadow = new THREE.InstancedMesh(
      this.track(new THREE.PlaneGeometry(1, 1).rotateX(-Math.PI / 2)),
      this.track(
        new THREE.ShaderMaterial({
          uniforms: { uFogDensity: this.shared.uFogDensity, uCam: this.shared.uCam },
          vertexShader: /* glsl */ `
uniform vec3 uCam;
varying vec2 vUv;
varying float vDist;
void main() {
  vUv = uv * 2.0 - 1.0;
  vec4 w = modelMatrix * instanceMatrix * vec4(position, 1.0);
  vDist = length(w.xyz - uCam);
  gl_Position = projectionMatrix * viewMatrix * w;
}`,
          fragmentShader: /* glsl */ `
uniform float uFogDensity;
varying vec2 vUv;
varying float vDist;
void main() {
  float f = vDist * uFogDensity;
  float a = 0.62 * (1.0 - smoothstep(0.35, 1.0, length(vUv))) * exp(-f * f);
  gl_FragColor = vec4(0.0, 0.0, 0.0, a);
}`,
          transparent: true,
          depthWrite: false,
          polygonOffset: true,
          polygonOffsetFactor: -1,
          polygonOffsetUnits: -1,
        }),
      ),
      this.cars.length,
    );
    this.carShadow.frustumCulled = false;
    this.carShadow.count = 0;
    this.scene.add(this.carShadow);
    this.carMeshes = geos.map((src, m) => {
      const geo = this.track(src.clone());
      geo.computeBoundingBox();
      const b = geo.boundingBox;
      // centred on x/z, wheels on y 0, stretched to real proportions; the
      // kit's cars face +z: turn them so the front leads along -z
      geo.translate(-(b.min.x + b.max.x) / 2, -b.min.y, -(b.min.z + b.max.z) / 2);
      geo.scale(SIZE[m][0] / (b.max.x - b.min.x), SIZE[m][1] / (b.max.y - b.min.y), SIZE[m][2] / (b.max.z - b.min.z));
      geo.rotateY(Math.PI);
      const list = this.cars.filter((c) => c.model === m);
      const mesh = new THREE.InstancedMesh(geo, paint, list.length);
      // culling packs the visible cars to the front, so each keeps its own paint
      for (const c of list) c.paint = new THREE.Color(PAINT[Math.floor(c.tone * PAINT.length)]);
      list.forEach((c, i) => mesh.setColorAt(i, c.paint));
      mesh.frustumCulled = false;
      mesh.userData = { list, half: SIZE[m][2] / 2, width: SIZE[m][0] };
      this.scene.add(mesh);
      return mesh;
    });
    this.updateCars();
  }

  updateCars() {
    if (!this.carMeshes) return;
    const t = this.shared.uTime.value;
    const shift = this.trailU.uShift.value;
    const zmax = this.trailU.uZmax.value;
    const loop = this.trailU.uLoop.value;
    const mod = (a, n) => a - n * Math.floor(a / n);
    const m = this.carMatrix || (this.carMatrix = new THREE.Matrix4());
    const shadow = this.carShadow;
    let ns = 0;
    for (const mesh of this.carMeshes) {
      const { list, half, width } = mesh.userData;
      let n = 0;
      for (const c of list) {
        const s = c.phase + c.speed * t;
        // the lamp pair's z (trailShader); the body sits just toward -z of
        // it, so the pair is on the tail of a car driving away and on the
        // nose of an oncoming one (the kit's cars are turned to face -z)
        const lz = c.d > 0 ? zmax - mod(s - shift, loop) : zmax - loop + mod(s + shift, loop);
        const z = lz - half - 0.05;
        if (!this.cullInstance(c.x, 1.3, z, half + 0.6)) continue;
        m.makeRotationY(c.d > 0 ? 0 : Math.PI);
        m.setPosition(c.x, 0, z);
        mesh.setMatrixAt(n, m);
        mesh.setColorAt(n, c.paint);
        n++;
        // a soft contact shadow, a little larger than the footprint
        m.makeScale(width * 1.3, 1, half * 2.3);
        m.setPosition(c.x, 0.03, z);
        shadow.setMatrixAt(ns++, m);
      }
      mesh.count = n;
      mesh.instanceMatrix.needsUpdate = true;
      mesh.instanceColor.needsUpdate = true;
    }
    shadow.count = ns;
    shadow.instanceMatrix.needsUpdate = true;
  }

  nextThru() {
    return THRU_SIGNS[this.thruCount++ % THRU_SIGNS.length];
  }

  // Traffic: each car is a body (sourced, see buildCars) carrying a pair of
  // lamps. Cars in one lane share one speed and an even spacing, so no car
  // ever drives through another; the camera's own lane stays clear.
  buildTraffic() {
    const rnd = seeded(5150);
    const LOOP = 1600;
    const lanes = [3.25, 6.95, 10.65, 14.35];
    const cars = [];
    for (const d of [1, -1]) {
      lanes.forEach((lx, lane) => {
        // the camera drives lane 3 (x 10.65) forward: nothing in it
        if (d > 0 && lane === 2) return;
        const n = d > 0 ? 24 : 18;
        const gap = LOOP / n;
        const speed = (d > 0 ? 24 : 25) + (3 - lane) * 3.4;
        const base = rnd() * gap;
        for (let j = 0; j < n; j++) {
          // gaps vary but never close below ~25 m
          const phase = base + j * gap + (rnd() - 0.5) * (gap - 25);
          const r = rnd();
          cars.push({ x: lx * (d > 0 ? 1 : -1), speed, phase, d, model: r < 0.55 ? 0 : r < 0.85 ? 1 : 2, tone: rnd() });
        }
      });
    }
    this.cars = cars;
    // [x offset, height, size, rgb] per lamp; hot enough to cross the bloom
    // threshold, the sign stays under it
    const TAIL = [4.6, 0.16, 0.08];
    const AMBER = [3.6, 1.5, 0.2];
    const lampsOf = (car) => {
      const away = car.d > 0;
      const head = car.tone > 0.35 ? [3.4, 3.1, 2.7] : [3.6, 2.8, 1.8];
      const out = [-0.72, 0.72].map((x) => [x, away ? 0.82 : 0.68, 1, away ? TAIL : head]);
      // a box van also carries its upper row: three ID lamps centred and a
      // clearance lamp at each top corner (red behind, amber in front). On
      // the nearest van the lower pair drops below the band; these keep it a
      // lit truck instead of a dark slab.
      if (car.model === 2) {
        for (const x of [-0.3, 0, 0.3, -0.98, 0.98]) out.push([x, 2.55, 0.5, away ? TAIL : AMBER]);
      }
      return out;
    };
    const lamps = cars.flatMap((car) => lampsOf(car).map((l) => [car, ...l]));
    const n = lamps.length;
    const info = new Float32Array(n * 3);
    const color = new Float32Array(n * 3);
    const dir = new Float32Array(n);
    const lamp = new Float32Array(n * 2);
    lamps.forEach(([car, x, h, size, rgb], i) => {
      info.set([car.x + x, car.speed, car.phase], i * 3);
      color.set(rgb, i * 3);
      dir[i] = car.d;
      lamp.set([h, size], i * 2);
    });
    const quad = new THREE.BufferGeometry();
    quad.setAttribute(
      'position',
      new THREE.BufferAttribute(new Float32Array([0, -0.5, 0, 1, -0.5, 0, 1, 0.5, 0, 0, 0.5, 0]), 3),
    );
    quad.setIndex([0, 1, 2, 0, 2, 3]);
    const geo = this.track(new THREE.InstancedBufferGeometry());
    geo.setIndex(quad.index);
    geo.setAttribute('position', quad.attributes.position);
    geo.setAttribute('aInfo', new THREE.InstancedBufferAttribute(info, 3));
    geo.setAttribute('aColor', new THREE.InstancedBufferAttribute(color, 3));
    geo.setAttribute('aDir', new THREE.InstancedBufferAttribute(dir, 1));
    geo.setAttribute('aLamp', new THREE.InstancedBufferAttribute(lamp, 2));
    geo.instanceCount = n;

    this.trailU = {
      uShift: { value: 0 },
      uZmax: { value: 90 },
      uLoop: { value: 1600 },
      uLen: { value: 7.5 },
      uWidth: { value: 0.16 },
      uPx: this.uPx,
      uLampR: { value: 0.07 },
    };
    const opts = { transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, fog: false };
    // Seen in real time, not long exposure: each lamp is a glow on the car's
    // tail or nose, while its reflection smears down the wet road toward the
    // camera.
    const direct = new THREE.Mesh(
      geo,
      this.material(trailShader, { ...this.trailU, uLen: { value: 0.6 }, uReflect: { value: 0 } }, opts),
    );
    const mirror = new THREE.Mesh(
      geo,
      this.material(trailShader, { ...this.trailU, uLen: { value: 3.0 }, uReflect: { value: 1 } }, opts),
    );
    direct.frustumCulled = false;
    mirror.frustumCulled = false;
    mirror.renderOrder = 1;
    direct.renderOrder = 2;
    this.scene.add(mirror, direct);
  }

  buildLights() {
    // a cool night sky above, warm sodium bounce off the asphalt below: the
    // split that keeps grey steel grey instead of all-over bronze
    this.scene.add(new THREE.HemisphereLight(0x2a3444, 0x1a1008, 0.45));
    // Sodium point lights on the three lamp rows around the camera, re-seated
    // every 40 m as it drives; each fades in far ahead and out behind, so the
    // re-seat never pops (see placeLamps).
    this.lamps = [];
    for (let j = 0; j < 3; j++) {
      for (const x of [7, -7]) {
        const l = new THREE.PointLight(SODIUM, 0, 70, 2);
        l.position.set(x, 11.9, 0);
        l.userData.row = j;
        this.scene.add(l);
        this.lamps.push(l);
      }
    }
    // No catwalk spotlight: one a few decimetres off the deck blew a white
    // star under every sign. The plate's luminaire light is in signShader.
  }

  buildPost() {
    // HDR target with MSAA; bloom takes only what is hotter than the sign
    // legend (threshold 1.8, below), so lamps, glows and tail lights flare and
    // the plate stays crisp
    const target = new THREE.WebGLRenderTarget(1, 1, { type: THREE.HalfFloatType, samples: 4 });
    this.composer = new EffectComposer(this.renderer, target);
    this.composer.addPass(new RenderPass(this.scene, this.camera));
    // A tight bloom: almost nothing from the widest mips, which carried each
    // lamp head passing just above the frame into the whole sky as a sepia
    // veil and pumped the exposure with every pole. Radius 0 so the pass
    // uses these per-mip factors (finest first) as given; it lerps each one
    // toward 1.2 - f by the radius.
    this.bloom = new UnrealBloomPass(new THREE.Vector2(256, 256), 0.55, 0, 1.8);
    this.bloom.compositeMaterial.uniforms.bloomFactors.value = [1.0, 0.55, 0.22, 0.06, 0.015];
    this.composer.addPass(this.bloom);
    this.composer.addPass(new OutputPass());
  }

  placeLamps(camZ, warm) {
    const base = 40 * Math.floor((camZ + 40) / 40);
    for (const l of this.lamps) {
      const z = base - 40 * l.userData.row;
      const ahead = camZ - z;
      const w = THREE.MathUtils.smoothstep(ahead, -40, -12) * (1 - THREE.MathUtils.smoothstep(ahead, 56, 80));
      l.position.z = z;
      l.intensity = lampSkipped(z) ? 0 : 170 * warm * w;
    }
  }

  // ---------------------------------------------------------------- content
  // Each gantry owns a slot: the face it shows and the face it is turning
  // from. Slots rotate with the gantries on every snap, so a sign mid-turn
  // keeps turning while its gantry becomes gantry 0.
  paintSlot(gantry, content) {
    const s = gantry.slot;
    drawSign(s.cur.canvas, content);
    s.cur.tex.needsUpdate = true;
    s.kind = content.kind;
    s.key = signKey(content);
    gantry.tab.visible = content.kind === 'server';
  }

  // the gantry (0..2) the camera reaches next that is more than `min` m ahead
  nextGantry(min = 0) {
    const camZ = CAM.z - this.offset;
    for (let k = 0; k < 3; k++) {
      if (camZ - (GANTRY_Z - k * P) > min) return k;
    }
    return 2;
  }

  settle(gantry, content) {
    gsap.killTweensOf(gantry.slot);
    gantry.slot.flip = 1;
    this.paintSlot(gantry, content);
  }

  // A new destination: the next gantry ahead turns its slats to it while the
  // car surges toward it; gantries further on just carry it. One already
  // overhead keeps what the driver read.
  setSign(content) {
    const had = this.content;
    this.content = content;
    this.pendingTurn = null;
    const animate = had && !this.reducedMotion && this.running && !this.lost && this.exit === 0;
    if (!animate) {
      for (const g of this.gantries) this.settle(g, content);
      if (!this.running) this.renderOnce();
      return;
    }
    // The payoff must happen at reading distance: surge up to HOLD_AHEAD
    // short of the target sign, turning its slats on the way in, then hold
    // there (frame) so the new plate can be read. A sign already within reach
    // turns where it is; one closer than HOLD_MIN would rise out of the band
    // while held, so the next gantry takes it.
    const camZ = CAM.z - this.offset;
    let k = this.nextGantry(FLIP_MIN_AHEAD);
    let ahead = camZ - (GANTRY_Z - k * P);
    if (ahead < HOLD_MIN && k < 2) {
      k += 1;
      ahead += P;
    }
    // the base drive keeps rolling under the surge, easing toward SPEED_NEAR
    // (time constant 1.1 s, frame): leave it its share or the car overshoots
    // the hold and the plate clips the band's top
    const gap = Math.max(0, ahead - HOLD_AHEAD);
    const dur = Math.min(2.2, 0.7 + gap / 95);
    const base = SPEED_NEAR * dur + (this.speed - SPEED_NEAR) * 1.1 * (1 - Math.exp(-dur / 1.1));
    const travel = Math.max(0, gap - base);
    const target = this.gantries[k].slot;
    this.pendingTurn = target;
    this.hold = { slot: target, until: this.time + HOLD_S };
    this.gantries.forEach((g, i) => {
      if (i > k) this.settle(g, content);
    });
    this.surgeTl?.kill();
    this.extra = 0;
    this.lastExtra = 0;
    if (travel < 8) {
      this.turnSlot(target);
      return;
    }
    this.surgeTl = gsap
      .timeline()
      .to(this, { extra: travel, duration: dur, ease: 'power2.inOut' }, 0)
      .to(this, { surge: 1, duration: dur * 0.45, ease: 'power2.out' }, 0)
      .to(this, { surge: 0, duration: dur * 0.55, ease: 'power2.in' }, dur * 0.45)
      // slats start mid-approach and land as the car settles: feedback 0.3 to 0.9 s after the click
      .call(() => this.turnSlot(target), null, Math.max(0, Math.min(dur * 0.4, dur - FLIP_S)));
  }

  // Turn whichever gantry carries `slot` now (snaps rotate slots between
  // gantries) to the current content, so a ping that refreshed the facts
  // mid-surge lands on the new face. A later setSign replaces pendingTurn
  // and kills the timeline that would call this.
  turnSlot(slot) {
    const g = this.gantries.find((x) => x.slot === slot);
    if (g && this.pendingTurn === slot) this.turn(g, this.content);
  }

  turn(gantry, content) {
    const s = gantry.slot;
    if (this.pendingTurn === s) this.pendingTurn = null;
    gsap.killTweensOf(s);
    // turn from whatever the face last settled on
    const g = s.prev.canvas.getContext('2d');
    g.clearRect(0, 0, s.prev.canvas.width, s.prev.canvas.height);
    g.drawImage(s.cur.canvas, 0, 0);
    s.prev.tex.needsUpdate = true;
    const hadTab = s.kind === 'server';
    this.paintSlot(gantry, content);
    // keep the tab's steel backing while either face has a tab
    gantry.tab.visible = hadTab || s.kind === 'server';
    s.flip = 0;
    gsap.to(s, {
      flip: 1,
      duration: FLIP_S,
      ease: 'power1.inOut',
      onComplete: () => {
        gantry.tab.visible = s.kind === 'server';
      },
    });
  }

  // Same destination, fresher facts (a ping landed), or a new list prompt:
  // repaint in place, no turn. Only signs showing this destination (or the
  // prompt it replaces) repaint: the one still queued to turn paints the
  // new content when it turns, one mid-turn keeps turning onto the fresh
  // face, and one passed while showing an earlier destination keeps it.
  redrawSign(content) {
    const was = this.content ? signKey(this.content) : null;
    const key = signKey(content);
    this.content = content;
    for (const g of this.gantries) {
      const s = g.slot;
      if (s === this.pendingTurn) continue;
      if (was !== null && s.key !== was && s.key !== key) continue;
      if (gsap.isTweening(s)) this.paintSlot(g, content);
      else this.settle(g, content);
    }
    if (!this.running) this.renderOnce();
  }

  snap() {
    const g = this.gantries;
    const slots = g.map((x) => x.slot);
    for (let k = 0; k < 3; k++) {
      g[k].slot = slots[(k + 1) % 3];
      g[k].tab.visible = g[k].slot.kind === 'server';
    }
    this.offset -= P;
    this.trailU.uShift.value += P;
    // the far gantry is new road ahead: it carries the current sign and the
    // next freeway name
    if (this.content) this.settle(g[2], this.content);
    drawThruSign(g[2].slot.thru.canvas, this.nextThru());
    g[2].slot.thru.tex.needsUpdate = true;
  }

  // The character-switch skycam: the drive eases off as the camera rises
  // straight up off the freeway, then holds over the town while the game boots.
  takeExit() {
    if (this.reducedMotion || this.lost) return Promise.resolve();
    gsap.killTweensOf(this, 'exit');
    return new Promise((resolve) => {
      gsap.to(this, { exit: 1, duration: 1.9, ease: 'power3.inOut', onComplete: resolve });
    });
  }

  returnFromExit() {
    gsap.killTweensOf(this, 'exit');
    if (this.reducedMotion || !this.running) {
      this.exit = 0;
      this.renderOnce();
      return;
    }
    gsap.to(this, { exit: 0, duration: 1.6, ease: 'power3.inOut' });
  }

  // ---------------------------------------------------------------- loop
  setSize(w, h) {
    if (!w || !h) return;
    this.renderer.setSize(w, h, false);
    this.composer.setPixelRatio(this.renderer.getPixelRatio());
    this.composer.setSize(w, h);
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
    this.updatePx();
    this.renderOnce();
  }

  updatePx() {
    const px = this.renderer.domElement.height;
    if (this.uPx) this.uPx.value = px / (2 * Math.tan(THREE.MathUtils.degToRad(this.camera.fov) / 2));
  }

  start() {
    if (this.running || this.lost) return;
    this.running = true;
    this.lastFrame = performance.now();
    const tick = (now) => {
      if (!this.running) return;
      this.raf = requestAnimationFrame(tick);
      const elapsed = now - this.lastFrame;
      // Cap at 60 fps: a launcher has no business pinning a 144 Hz GPU.
      // lastFrame runs on a fixed 60 Hz schedule rather than resetting to
      // `now` (which rendered every third vsync on 144 Hz, 48 fps), and the
      // 2 ms slack keeps 60 Hz vsync jitter from dropping frames. After a
      // long gap (a stall, a resume) the schedule restarts.
      const iv = 1000 / 60;
      if (elapsed < iv - 2) return;
      const steps = Math.max(1, Math.floor(elapsed / iv));
      this.lastFrame = elapsed > 4 * iv ? now : this.lastFrame + steps * iv;
      const dt = Math.min(elapsed > 4 * iv ? elapsed : steps * iv, 50) / 1000;
      if (!this.reducedMotion) {
        this.time += dt;
        this.boot += dt;
      }
      this.frame(this.reducedMotion ? 0 : dt);
    };
    this.raf = requestAnimationFrame(tick);
  }

  stop() {
    this.running = false;
    cancelAnimationFrame(this.raf);
  }

  renderOnce() {
    if (this.lost) return;
    this.frame(0);
  }

  frame(dt) {
    const t = this.time;
    const cam = this.camera;
    const drift = this.reducedMotion ? 0 : 1;
    // the skycam: up off the freeway and over the town onto a held aerial,
    // the freeway's light river running into downtown
    const climb = THREE.MathUtils.smootherstep(this.exit, 0.08, 1);

    // the drive: easing off toward each sign and away again after it, plus
    // the metres a selection's surge adds, all fading as the camera lifts
    const ahead0 = CAM.z - this.offset - (GANTRY_Z - this.nextGantry(0) * P);
    let want = SPEED_NEAR + (SPEED_FAR - SPEED_NEAR) * THREE.MathUtils.smoothstep(ahead0, 40, 125);
    // the reading hold after a selection: close to HOLD_AHEAD, then creep
    const hk = this.hold && this.time < this.hold.until ? this.gantries.findIndex((g) => g.slot === this.hold.slot) : -1;
    if (hk >= 0) {
      const aheadH = CAM.z - this.offset - (GANTRY_Z - hk * P);
      // settles on ~HOLD_AHEAD - 6 m, creeping; it never closes past HOLD_MIN in HOLD_S
      want = Math.min(want, THREE.MathUtils.clamp((aheadH - HOLD_AHEAD + 6) * 0.4, HOLD_CREEP, SPEED_NEAR));
    } else this.hold = null;
    // the target jumps when a gantry passes overhead: follow it, never step
    this.speed += (want - this.speed) * (1 - Math.exp(-dt / 1.1));
    this.offset += this.speed * dt * (1 - climb) + (this.extra - this.lastExtra);
    this.lastExtra = this.extra;
    while (this.offset >= P) this.snap();
    const camZ = CAM.z - this.offset;

    const { pos, dir, toSign, look, aerial } = this.scratch;
    pos.set(CAM.x, CAM.y, camZ);
    // a slow drift inside the lane and the faintest float of the suspension;
    // never shake
    pos.x += drift * (Math.sin(t * 0.13) * 0.26 + Math.sin(t * 0.051 + 2.0) * 0.1);
    pos.y += drift * Math.sin(t * 0.37 + 1.2) * 0.025;
    pos.y += climb * 165;
    pos.x += climb * AERIAL_SWING;

    // Eyes on the road, glancing up at the next sign the way a driver does:
    // the glance builds from 150 m out and lets go over the last 30 m, so the
    // plate grows, holds, then rises out through the top of the frame.
    const k = this.nextGantry(0);
    const gz = GANTRY_Z - k * P;
    const ahead = camZ - gz;
    const glance =
      0.5 * (1 - THREE.MathUtils.smoothstep(ahead, 60, 150)) * THREE.MathUtils.smoothstep(ahead, 18, 40) * drift +
      (this.reducedMotion ? 0.35 : 0);
    dir.set(pos.x - 1.4, CAM.y + 0.9, camZ - 160).sub(pos).normalize();
    toSign.set(SIGN_X, SIGN_Y, gz).sub(pos).normalize();
    dir.lerp(toSign, glance).normalize();
    look.copy(pos).addScaledVector(dir, 120);
    // aerial target: ~8.5 degrees down, yawed back left, so the light river
    // enters low left and runs on a diagonal into downtown on the right third
    if (climb > 0) look.lerp(aerial.set(pos.x - 239, 0, pos.z - 1125), climb);

    cam.position.copy(pos);
    cam.lookAt(look);
    // a breath of extra width in the surge sells the speed
    const fov = CAM.fov + this.surge * 2.2 + climb * climb * 27;
    if (cam.fov !== fov) {
      cam.fov = fov;
      cam.updateProjectionMatrix();
      this.updatePx();
    }

    this.far.position.set(pos.x - CAM.x, 0, pos.z - CAM.z);
    this.cityTint.value = 1.8 - 0.9 * climb;
    // from altitude the haze reads darker against the lit grid
    this.fogColor.copy(FOG_COLOR).multiplyScalar(1 - 0.4 * climb);
    this.scene.fog.color.copy(this.fogColor);
    this.placeLamps(camZ, Math.min(1, Math.max(0, (this.boot - 0.6) / 1.6)));
    for (const g of this.gantries) {
      const u = g.mat.uniforms;
      u.map.value = g.slot.cur.tex;
      u.uPrev.value = g.slot.prev.tex;
      u.uFlip.value = g.slot.flip;
      g.thruMat.uniforms.map.value = g.slot.thru.tex;
      g.thruMat.uniforms.uPrev.value = g.slot.thru.tex;
    }

    this.shared.uTime.value = t;
    // the haze thins as the skycam climbs above it, so the skyline keeps its
    // lit windows
    this.shared.uSkyFog.value = 0.00085 * (1 - 0.45 * climb);
    const density = 0.0034 * (1 - 0.55 * climb);
    this.beginCull(density);
    this.updatePalms();
    this.updateCars();
    this.shared.uBoot.value = this.boot;
    this.shared.uCam.value.copy(pos);
    this.shared.uAerial.value = climb;
    this.shared.uFogDensity.value = density;
    this.scene.fog.density = density;
    this.lensMat.color.setRGB(3.2, 1.7, 0.55).multiplyScalar(Math.min(1, 0.15 + this.boot / 2.2));

    this.composer.render(dt);
    if (this.onFrame) this.onFrame();
  }

  dispose() {
    this.disposed = true;
    this.stop();
    gsap.killTweensOf(this);
    this.surgeTl?.kill();
    for (const g of this.gantries) gsap.killTweensOf(g.slot);
    this.canvas.removeEventListener('webglcontextlost', this.onLost);
    for (const d of this.disposables) d.dispose?.();
    this.bloom.dispose();
    this.composer.dispose();
    this.renderer.dispose();
  }
}
