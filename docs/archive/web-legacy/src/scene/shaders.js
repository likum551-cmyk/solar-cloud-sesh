// GLSL for the night-freeway scene. Light, markings and the town grid are
// procedural; the asphalt and the skyline's facades sample the CC0 photo
// textures listed in ASSETS.md (inlined into the single-file build).

const NOISE = /* glsl */ `
float hash12(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float vnoise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x),
             mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}
float fbm(vec2 p) {
  float a = 0.5;
  float s = 0.0;
  for (int i = 0; i < 4; i++) {
    s += a * vnoise(p);
    p = p * 2.03 + 17.1;
    a *= 0.5;
  }
  return s;
}
// Noise that repeats every per lattice cells along y (world z). Ground
// patterns must repeat with the gantry period so the fly-and-snap is seamless.
float pnoise(vec2 p, float per) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  float y0 = mod(i.y, per);
  float y1 = mod(i.y + 1.0, per);
  return mix(mix(hash12(vec2(i.x, y0)), hash12(vec2(i.x + 1.0, y0)), u.x),
             mix(hash12(vec2(i.x, y1)), hash12(vec2(i.x + 1.0, y1)), u.x), u.y);
}
float pfbm(vec2 p, float per) {
  float a = 0.5;
  float s = 0.0;
  for (int i = 0; i < 4; i++) {
    s += a * pnoise(p, per);
    p = p * 2.0 + vec2(17.1, 0.0);
    per *= 2.0;
    a *= 0.5;
  }
  return s;
}
`;

const FOG = /* glsl */ `
uniform vec3 uFogColor;
uniform float uFogDensity;
float fogAmount(float dist, float density) {
  float d = density * dist;
  return clamp(1.0 - exp(-d * d), 0.0, 1.0);
}
`;

// Sodium lamps warm up from a dim red glow to full orange, each on its own
// delay, the way a real high-pressure sodium run lights up after dusk.
const LAMP = /* glsl */ `
uniform float uBoot;
uniform vec3 uSodium;
float lampWarm(float lz, float lx) {
  float delay = 0.25 + hash12(vec2(lz * 0.0131, lx * 0.71)) * 1.6;
  return smoothstep(delay, delay + 1.4, uBoot);
}
vec3 lampColor(float w) {
  vec3 strike = vec3(0.55, 0.08, 0.03) * 0.25;
  return mix(strike, uSodium, w) * (0.08 + 0.92 * w);
}
`;

const WORLD_VERT = /* glsl */ `
varying vec3 vWorld;
void main() {
  vec4 w = modelMatrix * vec4(position, 1.0);
  vWorld = w.xyz;
  gl_Position = projectionMatrix * viewMatrix * w;
}
`;

export const roadShader = {
  vertexShader: WORLD_VERT,
  fragmentShader: /* glsl */ `
uniform vec3 uCam;
uniform float uTime;
// 0 on the freeway, 1 in the skycam's aerial hold
uniform float uAerial;
// Poly Haven asphalt_track (ASSETS.md), tiled every ASPHALT_M metres
uniform sampler2D uAsphalt;
uniform sampler2D uAsphaltN;
uniform float uTexOn;
varying vec3 vWorld;
${NOISE}
${FOG}
${LAMP}
// divides 160, so the tiling repeats with the gantry period
const float ASPHALT_M = 4.0;

float band(float x, float c, float w) {
  float aa = fwidth(x) * 0.8 + 0.002;
  return 1.0 - smoothstep(w * 0.5 - aa, w * 0.5 + aa, abs(x - c));
}

void main() {
  float x = vWorld.x;
  float z = vWorld.z;
  float ax = abs(x);
  vec2 p = vWorld.xz;

  // --- surface -----------------------------------------------------------
  // every pattern repeats exactly every 160 m (the gantry period)
  vec2 auv = p / ASPHALT_M;
  // the photo is warm brown under daylight; the night road reads a cool grey,
  // so only its luminance is used (it averages 0.0138; this keeps the mean
  // near 1). Before the textures decode, value noise stands in.
  float aggregate = uTexOn > 0.5
    ? 0.3 + 50.0 * dot(texture2D(uAsphalt, auv).rgb, vec3(0.2126, 0.7152, 0.0722))
    : 0.72 + 0.55 * pfbm(p * 1.9, 304.0);
  // tangent-space normal of the aggregate; only its tilt is used, to break up
  // the wet reflections the way real chip seal does
  vec2 tilt = uTexOn > 0.5 ? texture2D(uAsphaltN, auv).xy * 2.0 - 1.0 : vec2(0.0);
  float patches = pfbm(p * 0.05, 8.0);
  vec3 asphalt = vec3(0.042, 0.043, 0.047) * aggregate;
  asphalt *= 0.85 + 0.3 * smoothstep(0.35, 0.7, patches);
  // tyre polish along the wheel paths of each lane
  float lanePos = mod(ax - 1.4, 3.7);
  float wheel = band(lanePos, 0.95, 0.7) + band(lanePos, 2.75, 0.7);
  asphalt *= 1.0 - 0.18 * wheel * step(1.4, ax) * step(ax, 16.2);

  float shoulder = smoothstep(16.3, 16.5, ax);
  asphalt = mix(asphalt, asphalt * 1.25 + 0.005, shoulder);
  float verge = smoothstep(20.3, 21.2, ax);
  vec3 dirt = vec3(0.016, 0.018, 0.015) * (0.6 + 0.8 * pfbm(p * 0.6, 96.0));
  vec3 albedo = mix(asphalt, dirt, verge);

  float wet = smoothstep(0.42, 0.72, pfbm(p * 0.075 + vec2(3.1, 0.0), 12.0)) * (1.0 - verge);
  albedo *= mix(1.0, 0.55, wet);

  // --- markings ------------------------------------------------------------
  float dz = mod(z, 10.0);
  float aaz = fwidth(z) + 0.001;
  float dash = smoothstep(0.0, aaz, dz) * (1.0 - smoothstep(3.0 - aaz, 3.0, dz));
  float wear = 0.55 + 0.45 * pnoise(p * vec2(3.0, 0.4), 64.0);
  float yellow = band(ax, 1.4, 0.15);
  float white = band(ax, 16.2, 0.18)
    + (band(ax, 5.1, 0.13) + band(ax, 8.8, 0.13) + band(ax, 12.5, 0.13)) * dash;
  albedo = mix(albedo, vec3(0.72, 0.52, 0.12) * wear, yellow * (1.0 - verge));
  albedo = mix(albedo, vec3(0.70, 0.70, 0.66) * wear, clamp(white, 0.0, 1.0));

  // --- sodium pools ----------------------------------------------------------
  vec3 light = vec3(0.010, 0.011, 0.014);
  float lzBase = floor(z / 40.0 + 0.5) * 40.0;
  for (int i = -1; i <= 1; i++) {
    float lz = lzBase + float(i) * 40.0;
    // no pole 20 m behind a gantry (freeway.js lampSkipped): z = -40 mod 160
    if (abs(mod(lz + 40.0, 160.0) - 80.0) > 79.0) continue;
    for (int s = 0; s < 2; s++) {
      float lx = s == 0 ? 7.0 : -7.0;
      vec3 L = vec3(lx, 12.1, lz) - vec3(x, 0.0, z);
      float d2 = dot(L, L);
      float ndl = L.y * inversesqrt(d2);
      light += lampColor(lampWarm(lz, lx)) * (ndl * 105.0 / d2);
    }
  }
  vec3 col = albedo * light;

  // --- wet-road streak reflections of the lamps ahead -----------------------
  vec3 spec = vec3(0.0);
  float lzCam = floor(uCam.z / 40.0) * 40.0;
  float t = uCam.y / (uCam.y + 12.1);
  for (int i = 0; i < 12; i++) {
    float lz = lzCam - float(i) * 40.0;
    if (abs(mod(lz + 40.0, 160.0) - 80.0) > 79.0) continue;
    for (int s = 0; s < 2; s++) {
      float lx = s == 0 ? 7.0 : -7.0;
      vec3 R = uCam + (vec3(lx, -12.1, lz) - uCam) * t;
      float dR = length(R.xz - uCam.xz);
      // wet asphalt stretches each reflection toward the eye: a narrow
      // streak, many times longer than wide, never a round pool
      float wx = 0.16 + dR * 0.005;
      float wz = 3.0 + dR * 0.55;
      float ex = (x - R.x) / wx + tilt.x * 0.55;
      float ez = (z - R.z) / wz + tilt.y * 0.35;
      spec += lampColor(lampWarm(lz, lx)) * exp(-ex * ex - ez * ez);
    }
  }
  float gloss = 0.18 + 0.82 * wet;
  col += spec * gloss * (0.55 - 0.4 * verge);

  // --- the city grid beyond the freeway -------------------------------------
  // 80 m blocks, street lamps every 20 m, scattered windows; periodic in 160 m.
  float town = smoothstep(26.0, 34.0, ax);
  if (town > 0.0) {
    // point lights stay about a pixel wide at any distance; at grazing angles
    // they compress along z the way distant lights do
    vec2 rr = max(vec2(0.9), fwidth(p) * 1.6);
    float energy = 0.81 / (rr.x * rr.y);
    vec2 blockC = (floor(p / 80.0) + 0.5) * 80.0;
    vec2 toEdge = 40.0 - abs(p - blockC);
    float street = 1.0 - smoothstep(5.0, 6.5, min(toEdge.x, toEdge.y));
    // nearest z-running (x = sx) and x-running (z = sz) streets; arterials are
    // every third z-running and every second x-running one (buildTown builds
    // their frontage taller); side streets get dimmer, cooler LED heads
    float sx = round(x / 80.0) * 80.0;
    float sz = round(z / 80.0) * 80.0;
    float artZ = step(mod(round(x / 80.0), 3.0), 0.5);
    float artX = step(mod(round(z / 80.0), 2.0), 0.5);
    vec3 ground = mix(vec3(0.006, 0.007, 0.008), vec3(0.011, 0.011, 0.012), street);
    vec3 sodium = vec3(1.0, 0.54, 0.18);
    vec3 led = vec3(0.6, 0.74, 1.0);
    // the frontage land beside the freeway stays dark: at that grazing angle
    // its ground lights only smear into dashes
    float front = smoothstep(52.0, 76.0, ax);
    // the arterials' carriageway glows under its lamp run
    float gz = (x - sx) / 9.0;
    float gx = (z - sz) / 9.0;
    // from the air the arterials read as a sodium grid over cooler side streets
    // (narrower and dimmer than the freeway, which stays the brightest line)
    float gw = 1.0 + 0.6 * uAerial;
    ground += sodium * (0.11 + 0.1 * uAerial) * (artZ * exp(-gz * gz * gw * gw) + artX * exp(-gx * gx * gw * gw) * front);
    float gzs = (x - sx) / 5.0;
    float gxs = (z - sz) / 5.0;
    ground += led * 0.03 * uAerial * ((1.0 - artZ) * exp(-gzs * gzs) + (1.0 - artX) * exp(-gxs * gxs) * front);
    // lamps: along x-running and z-running streets
    float alongZ = z - round(z / 20.0) * 20.0;
    float alongX = x - round(x / 20.0) * 20.0;
    vec2 d1 = vec2(x - sx - 4.0, alongZ) / rr;
    vec2 d2 = vec2(alongX, z - sz - 4.0) / rr;
    float lampZ = exp(-dot(d1, d1));
    float lampX = exp(-dot(d2, d2)) * front;
    vec3 artLamp = sodium * 4.4;
    vec3 lamps = lampZ * mix(led * 0.9, artLamp, artZ) + lampX * mix(led * 0.9, artLamp, artX);
    vec2 hc = floor(p / 10.0);
    vec2 hcp = vec2(hc.x, mod(hc.y, 16.0));
    float hpick = hash12(hcp);
    vec2 hp = (hc + 0.5 + (vec2(hash12(hcp + 1.7), hash12(hcp + 5.1)) - 0.5) * 0.6) * 10.0;
    vec2 d3 = (p - hp) / (rr * 0.8);
    // half as many house lights from the air, where they only add speckle
    float house = exp(-dot(d3, d3)) * step(0.86 + 0.07 * uAerial, hpick) * (1.0 - street) * smoothstep(64.0, 96.0, ax);
    vec3 homeLight = mix(vec3(1.0, 0.7, 0.4), vec3(0.6, 0.74, 0.95), step(0.96, hpick));
    // traffic on the arterials only: one light per car, 40 m apart in each
    // lane, keyed by street so the pattern still repeats every 160 m along z
    vec3 head = vec3(1.3, 1.15, 0.9);
    vec3 tail = vec3(1.5, 0.12, 0.05) * 0.5;
    vec3 cars = vec3(0.0);
    for (int s = 0; s < 2; s++) {
      float side = s == 0 ? -1.0 : 1.0;
      float hz = hash12(vec2(sx, side));
      float pz = mod(z - side * uTime * (9.0 + 6.0 * hz) + hz * 40.0, 40.0) - 20.0;
      vec2 c1 = vec2(x - sx - side * 1.7, pz) / rr;
      cars += (side > 0.0 ? tail : head) * exp(-dot(c1, c1)) * step(0.35, hz) * artZ;
      float hx = hash12(vec2(mod(sz, 160.0), side + 7.0));
      float px = mod(x - side * uTime * (9.0 + 6.0 * hx) + hx * 40.0, 40.0) - 20.0;
      vec2 c2 = vec2(px, z - sz - side * 1.7) / rr;
      cars += (side > 0.0 ? head : tail) * exp(-dot(c2, c2)) * step(0.35, hx) * artX * front;
    }
    vec3 tcol = ground + (lamps + homeLight * house * 1.3 + cars * 1.4) * energy;
    col = mix(col, tcol, town);
  }

  float dist = length(vWorld - uCam);
  col = mix(col, uFogColor, fogAmount(dist, uFogDensity));
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`,
};

export const skyShader = {
  vertexShader: /* glsl */ `
varying vec3 vDir;
void main() {
  vDir = normalize(position);
  vec4 w = modelMatrix * vec4(position, 1.0);
  gl_Position = projectionMatrix * viewMatrix * w;
  gl_Position.z = gl_Position.w; // pin to the far plane
}
`,
  fragmentShader: /* glsl */ `
uniform float uTime;
varying vec3 vDir;
${NOISE}
void main() {
  vec3 d = normalize(vDir);
  float h = d.y;
  // coastal city night: a sodium-orange band of light pollution on the
  // horizon under a deep blue-teal sky, so the green signs and the amber
  // lamps each have a complement to sit against
  vec3 horizon = vec3(0.150, 0.078, 0.034);
  vec3 low = vec3(0.034, 0.030, 0.034);
  vec3 mid = vec3(0.010, 0.022, 0.042);
  vec3 zenith = vec3(0.004, 0.011, 0.028);
  vec3 col = mix(horizon, low, smoothstep(-0.01, 0.05, h));
  col = mix(col, mid, smoothstep(0.04, 0.16, h));
  col = mix(col, zenith, smoothstep(0.14, 0.6, h));

  // low stratus: lit warm from below near the horizon, cool and thin above
  float hc = max(h, 0.035);
  vec2 cp = d.xz / hc * 0.9 + vec2(uTime * 0.0035, uTime * 0.001);
  float c = smoothstep(0.42, 0.86, fbm(cp));
  vec3 cloud = vec3(0.085, 0.048, 0.026) * (1.0 - smoothstep(0.02, 0.32, h)) + vec3(0.012, 0.019, 0.032);
  col = mix(col, cloud, c * 0.7 * smoothstep(0.0, 0.06, h));

  // below the horizon: ground haze
  col = mix(col, horizon * 0.55, smoothstep(0.0, -0.08, h));
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`,
};

export const buildingShader = {
  vertexShader: /* glsl */ `
varying vec3 vW;
varying vec3 vN;
varying float vInst;
varying float vTop;
void main() {
  vec4 w = modelMatrix * instanceMatrix * vec4(position, 1.0);
  vW = w.xyz;
  vN = normal;
  vInst = float(gl_InstanceID);
  // unrotated instances: roof height = base y + y scale (the box sits on y 0..1)
  vTop = (modelMatrix * vec4(0.0, instanceMatrix[3][1] + instanceMatrix[1][1], 0.0, 1.0)).y;
  gl_Position = projectionMatrix * viewMatrix * w;
}
`,
  fragmentShader: /* glsl */ `
uniform vec3 uCam;
// fog density and how far its colour is lifted: the distant skyline takes the
// thin sky haze (lifted toward the horizon glow), the town the ground haze
uniform float uBFog;
uniform float uFogTint;
uniform float uLitShare;
uniform float uWinGain;
// town only: street light washing up the lower floors
uniform float uStreetLit;
uniform float uAerial;
// skyline only: ambientCG night facades (ASSETS.md) instead of drawn windows
uniform float uFacadeOn;
uniform float uTexOn;
uniform sampler2D uOffice;
uniform sampler2D uOfficeE;
uniform sampler2D uGlass;
uniform sampler2D uGlassE;
varying vec3 vW;
varying vec3 vN;
varying float vInst;
varying float vTop;
${NOISE}
${FOG}
// one facade photo spans this many metres of wall
const float FACADE_M = 56.0;
void main() {
  // the per-instance id arrives interpolated; with thousands of instances the
  // drift is enough to scramble every hash below, so snap it back to integer
  float vId = floor(vInst + 0.5);
  float dist = length(vW - uCam);
  vec3 facade = vec3(0.010, 0.010, 0.012);
  vec3 col = facade;
  // light the building emits (windows, crowns): it carries through haze
  // better than the unlit mass around it
  vec3 emit = vec3(0.0);
  if (vN.y > 0.5) {
    // roofs: tar, gravel and plant decks in slightly different near-blacks
    col = vec3(0.008, 0.008, 0.009) * (0.65 + 0.9 * hash12(vec2(vId, 2.0)));
  } else if (uFacadeOn * uTexOn > 0.5) {
    float u = abs(vN.x) > 0.5 ? vW.z : vW.x;
    // each tower starts the photo somewhere else, so neighbours never match
    vec2 fuv = vec2(u, vW.y) / FACADE_M + vec2(hash12(vec2(vId, 5.0)), hash12(vec2(vId, 6.0)));
    bool glass = hash12(vec2(vId, 9.0)) > 0.62;
    vec3 fc = glass ? texture2D(uGlass, fuv).rgb : texture2D(uOffice, fuv).rgb;
    vec3 fe = glass ? texture2D(uGlassE, fuv).rgb : texture2D(uOfficeE, fuv).rgb;
    col = fc * 0.05;
    emit += fe * uWinGain * 0.55;
    float crown = step(0.86, hash12(vec2(vId, 4.0))) * step(60.0, vTop);
    float band = smoothstep(vTop - 7.0, vTop - 4.5, vW.y) * (1.0 - smoothstep(vTop - 1.2, vTop, vW.y));
    emit += vec3(0.9, 0.72, 0.45) * 0.18 * crown * band;
  } else {
    float u = abs(vN.x) > 0.5 ? vW.z : vW.x;
    vec2 g = vec2(u / 3.2, vW.y / 3.4);
    vec2 cell = floor(g);
    vec2 f = fract(g);
    float win = step(0.24, f.x) * step(f.x, 0.76) * step(0.3, f.y) * step(f.y, 0.74);
    // a mullion splits each opening into two panes
    win *= step(0.022, abs(f.x - 0.5));
    float r = hash12(cell + vec2(vId * 7.13, vId * 1.91));
    float lit = step(1.0 - uLitShare, r);
    float tone = hash12(cell * 1.7 + vId);
    // saturated room light (tungsten, LED panels): emitted, not painted on
    vec3 warm = vec3(1.0, 0.56, 0.22);
    vec3 cool = vec3(0.2, 0.33, 0.66);
    vec3 wc = mix(warm, cool, step(0.82, tone)) * (0.4 + 0.75 * hash12(cell + 3.3));
    // the nearest town façades fill a large part of the frame: a lit pane
    // there must stay a room glimpsed at night, not a flat pale quad
    wc *= mix(1.0, mix(0.42, 1.0, smoothstep(30.0, 130.0, dist)), uStreetLit);
    // a room, not a sticker: ceiling light falls off down the pane, and some
    // blinds are half drawn
    float blind = mix(0.74, 0.46, step(0.55, hash12(cell + 9.7)) * hash12(cell + 4.1));
    vec3 room = wc * (0.55 + 0.6 * smoothstep(0.3, 0.74, f.y)) * step(f.y, blind);
    // dark glass still reads as glass: a faint cool sheen marks every pane
    vec3 glass = vec3(0.006, 0.008, 0.013) * win;
    vec3 windows = room * win * lit * uWinGain + glass * (1.0 - lit);
    // a lit room spills a little light onto the wall around its window
    vec2 q = (f - vec2(0.5, 0.52)) * vec2(2.1, 2.5);
    windows += wc * lit * (1.0 - win) * 0.07 * uWinGain * exp(-dot(q, q) * 2.4);
    // windows smaller than a pixel resolve to lit and dark patches a couple of
    // bays wide and a floor high (an even average would paint the wall beige)
    float tiny = smoothstep(0.15, 0.6, max(fwidth(g.x), fwidth(g.y)));
    vec2 cg = floor(g / vec2(2.0, 1.0));
    float cl = step(1.0 - uLitShare * 0.55, hash12(cg + vec2(vId * 3.7, vId * 0.9)));
    vec3 avg = mix(warm, cool, step(0.85, hash12(cg + 7.0))) * cl * 0.16 * uWinGain;
    // from the air: half as many patches, each twice the size, so the town
    // reads as lit floors instead of shimmering speckle
    vec2 cg2 = floor(g / vec2(4.0, 2.0));
    float cl2 = step(1.0 - uLitShare * 0.28, hash12(cg2 + vec2(vId * 5.3, vId * 2.1)));
    vec3 avg2 = mix(warm, cool, step(0.85, hash12(cg2 + 7.0))) * cl2 * 0.2 * uWinGain;
    emit += mix(windows, mix(avg, avg2, uAerial), tiny);
    // sodium from the street below washes the lower floors
    col += vec3(0.05, 0.026, 0.009) * uStreetLit * exp(-vW.y / 3.6);
    // a few tall blocks carry a lit crown: a thin floodlit band under the
    // roofline, never a whole lit face
    float crown = step(0.86, hash12(vec2(vId, 4.0))) * step(60.0, vTop);
    float band = smoothstep(vTop - 7.0, vTop - 4.5, vW.y) * (1.0 - smoothstep(vTop - 1.2, vTop, vW.y));
    emit += vec3(0.9, 0.72, 0.45) * 0.18 * crown * band;
  }
  float fogA = fogAmount(dist, uBFog);
  col = mix(col, uFogColor * uFogTint, fogA) + emit * (1.0 - 0.8 * fogA);
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`,
};

export const ridgeShader = {
  vertexShader: WORLD_VERT,
  fragmentShader: /* glsl */ `
uniform vec3 uCam;
uniform float uSkyFog;
// haze lift, shared with the skyline (eases off in the aerial so the ridge
// stays a silhouette against the sky instead of a grey wall)
uniform float uFogTint;
varying vec3 vWorld;
${NOISE}
${FOG}
void main() {
  vec3 col = vec3(0.006, 0.006, 0.007);
  // scattered hillside house lights
  vec2 g = vWorld.xy / vec2(9.0, 6.0);
  vec2 cell = floor(g);
  float r = hash12(cell);
  float dotL = 1.0 - smoothstep(0.0, 0.18, length(fract(g) - 0.5));
  float band = smoothstep(0.0, 30.0, vWorld.y) * (1.0 - smoothstep(60.0, 140.0, vWorld.y));
  col += vec3(1.0, 0.72, 0.42) * dotL * step(0.93, r) * band * 0.8;
  float dist = length(vWorld - uCam);
  col = mix(col, uFogColor * uFogTint, fogAmount(dist, uSkyFog) * 0.85);
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`,
};

// Additive camera-facing point glows (lamp heads, sign luminaires, beacons).
export const glowShader = {
  vertexShader: /* glsl */ `
attribute float aSize;
attribute vec3 aColor;
attribute float aKind; // 0 sodium lamp (warms up), 1 steady, 2 blinking beacon
uniform float uPx;
uniform float uTime;
uniform vec3 uCam;
uniform float uAerial;
varying vec3 vColor;
varying float vFade;
${NOISE}
${FOG}
${LAMP}
void main() {
  vec4 mv = modelViewMatrix * vec4(position, 1.0);
  vec4 w = modelMatrix * vec4(position, 1.0);
  float k = 1.0;
  if (aKind < 0.5) {
    vColor = lampColor(lampWarm(w.z, w.x));
  } else if (aKind < 1.5) {
    vColor = aColor;
  } else {
    // aviation beacons breathe slowly rather than flash: never fully dark,
    // so any frame (and the held aerial) shows them
    float ph = hash12(w.xz);
    k = 0.45 + 0.55 * (0.5 + 0.5 * sin((uTime * 0.7 + ph) * 6.2832));
    vColor = aColor;
  }
  float dist = length(w.xyz - uCam);
  vFade = k * (1.0 - fogAmount(dist, uFogDensity * (aKind > 1.5 ? 0.25 : 1.0)));
  // from the air the freeway's lamp run must stay the brightest line in town
  float lift = aKind < 0.5 ? 1.0 + 0.7 * uAerial : 1.0;
  vFade *= lift;
  // capped: on the long lens a lamp head passing close overhead would
  // otherwise spread its halo across the whole band
  gl_PointSize = min(aSize * lift * uPx / -mv.z, 80.0);
  gl_Position = projectionMatrix * mv;
}
`,
  fragmentShader: /* glsl */ `
varying vec3 vColor;
varying float vFade;
void main() {
  vec2 q = gl_PointCoord - 0.5;
  float d2 = dot(q, q) * 4.0;
  float core = exp(-d2 * 26.0);
  float halo = exp(-d2 * 5.0) * 0.12;
  vec3 col = vColor * (core * 2.2 + halo) * vFade;
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`,
};

// Traffic lamps. Positions are computed on the GPU from a lane, a speed and a
// phase (updateCars mirrors the formula for the bodies). Two passes share the
// geometry: the lamps themselves (uReflect 0) as camera-facing glows, and
// their reflections (uReflect 1) smeared down the wet road toward the camera.
export const trailShader = {
  vertexShader: /* glsl */ `
attribute vec3 aInfo;  // lane x, speed (m/s), phase (m)
attribute vec3 aColor;
attribute float aDir;  // +1 drives away (-z), -1 drives toward (+z)
attribute vec2 aLamp;  // height (m), size (x uLampR)
uniform float uTime;
uniform float uShift;
uniform float uZmax;
uniform float uLoop;
uniform float uLen;
uniform float uWidth;
uniform float uReflect;
uniform float uPx;     // pixels per metre at 1 m (vertical)
uniform float uLampR;  // lamp half-height in metres
uniform vec3 uCam;
varying vec2 vUv;
varying vec3 vColor;
varying float vFade;
${FOG}
void main() {
  float s = aInfo.z + aInfo.y * uTime;
  float z = aDir > 0.0
    ? uZmax - mod(s - uShift, uLoop)
    : (uZmax - uLoop) + mod(s + uShift, uLoop);
  float h = aLamp.x;
  vec3 c = vec3(aInfo.x, h, z);
  float len = uLen * (aDir > 0.0 ? 1.0 : 1.35);
  // small marker lamps reflect as little as they shine
  float gain = aLamp.y;
  if (uReflect > 0.5) {
    float t = uCam.y / (uCam.y + h);
    vec3 R = uCam + (vec3(c.x, -h, c.z) - uCam) * t;
    R.y = 0.025;
    float dR = length(R.xz - uCam.xz);
    // wet asphalt stretches a reflection toward the eye, whichever way the
    // car drives (along -z, an oncoming car's ran off as a diagonal comet)
    vec3 toEye = normalize(vec3(uCam.x - R.x, 0.0, uCam.z - R.z));
    vec3 across = vec3(toEye.z, 0.0, -toEye.x);
    vec3 pos = R + toEye * (position.x * len * 2.6) + across * (position.y * (uWidth * 2.2 + dR * 0.006));
    vUv = position.xy;
    gl_Position = projectionMatrix * viewMatrix * vec4(pos, 1.0);
  } else {
    // A lamp faces the camera from every angle; seen end-on, a quad laid
    // along the road collapses to a sliver. Never under ~1.4 px tall, so a
    // far car still shows its pair, dimmed by the area it gained.
    vec4 vc = viewMatrix * vec4(c, 1.0);
    float lampR = uLampR * aLamp.y;
    float r = max(lampR, 1.4 * -vc.z / uPx);
    gain = max(0.3, (lampR * lampR) / (r * r));
    vUv = vec2(position.x * 2.0 - 1.0, position.y * 2.0);
    // the quad spans three lamp radii for the halo; tail and head lamps are
    // wider than tall, marker lamps nearly round
    vc.xy += vUv * vec2(aLamp.y < 0.9 ? 1.3 : 2.2, 1.0) * r * 3.0;
    gl_Position = projectionMatrix * vc;
  }
  vColor = aColor * gain;
  float dist = length(c - uCam);
  vFade = (1.0 - fogAmount(dist, uFogDensity)) * (uReflect > 0.5 ? 0.22 : 1.0);
}
`,
  fragmentShader: /* glsl */ `
uniform float uReflect;
varying vec2 vUv;
varying vec3 vColor;
varying float vFade;
void main() {
  vec3 col;
  if (uReflect > 0.5) {
    float across = exp(-vUv.y * vUv.y * 30.0);
    // MSAA samples can sit just outside the quad, where vUv.x passes 1: a
    // negative pow base is NaN, and bloom smears one NaN across the frame
    float tail = pow(max(1.0 - vUv.x, 0.0), 2.2);
    float head = exp(-vUv.x * vUv.x * 600.0);
    col = vColor * across * (tail * 0.75 + head * 1.8) * vFade;
  } else {
    // a hot core one third of the quad wide, and a faint halo to its edge
    float d2 = dot(vUv, vUv) * 9.0;
    col = vColor * (exp(-d2 * 2.2) * 2.2 + exp(-d2 * 0.4) * 0.12) * vFade;
  }
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`,
};

// Guide-sign face. Retroreflective sheeting under the gantry's own
// luminaires: lit from below by three fixtures on the catwalk, so the bottom
// edge is brightest with soft hot spots under each fixture and the top of
// the plate falls off. Kept under the bloom threshold so the white legend
// stays crisp, like the real sheeting does under light.
export const signShader = {
  vertexShader: /* glsl */ `
varying vec2 vUv;
varying vec3 vWorld;
void main() {
  vUv = uv;
  vec4 w = modelMatrix * vec4(position, 1.0);
  vWorld = w.xyz;
  gl_Position = projectionMatrix * viewMatrix * w;
}
`,
  fragmentShader: /* glsl */ `
uniform sampler2D map;
// the face being turned away from, and how far the turn has run (1 = done)
uniform sampler2D uPrev;
uniform float uFlip;
uniform vec3 uCam;
uniform float uBoot;
// fixture x positions across the face (0..1) and their height below its bottom edge (in face heights)
uniform vec3 uFixtures;
uniform float uDrop;
varying vec2 vUv;
varying vec3 vWorld;
${FOG}
float spot(float dx, float rise) {
  dx *= 3.0;
  return 1.0 / (1.0 + (dx * dx + rise * rise) * 16.0);
}
// A changing sign turns like a tri-vision board: vertical slats rotate one
// after another, left to right, each showing its edge halfway through.
const float SLATS = 14.0;
void main() {
  float slat = floor(vUv.x * SLATS);
  float p = clamp((uFlip - slat / SLATS * 0.45) / 0.55, 0.0, 1.0);
  float a = p * 3.14159265;
  // how square-on the slat faces the camera (1 at rest)
  float edgeOn = abs(cos(a));
  float h = (fract(vUv.x * SLATS) - 0.5) / max(edgeOn, 1e-3);
  vec2 uv = vec2((slat + 0.5 + h) / SLATS, vUv.y);
  vec4 tex = a < 1.5707963 ? texture2D(uPrev, uv) : texture2D(map, uv);
  if (abs(h) > 0.5) {
    // the prism's aluminium flank between the two faces: lit from the
    // catwalk below, brighter on the side turning toward the fixtures
    float side = clamp((abs(fract(vUv.x * SLATS) - 0.5) * 2.0 - edgeOn) / max(1.0 - edgeOn, 1e-3), 0.0, 1.0);
    vec3 alu = vec3(0.34, 0.36, 0.37) * (0.55 + 0.45 * vUv.y) * mix(0.6, 1.0, side);
    tex = vec4(alu, texture2D(map, vec2((slat + 0.5) / SLATS, vUv.y)).a);
  }
  if (tex.a < 0.5) discard;
  float rise = vUv.y + uDrop;
  float spots = spot(vUv.x - uFixtures.x, rise) + spot(vUv.x - uFixtures.y, rise) + spot(vUv.x - uFixtures.z, rise);
  float on = smoothstep(0.0, 0.8, uBoot);
  // the sheeting: bright fans over each luminaire, falling to dim at the top
  float spill = on * (0.36 * exp(-vUv.y * 3.2) + 0.95 * spots);
  float shade = 0.5 + spill;
  // the legend is brighter sheeting: it keeps reading white in the dim top
  // half and saturates (rather than greys) in the hot spots
  float legend = smoothstep(0.55, 0.8, min(tex.r, min(tex.g, tex.b)));
  shade = mix(shade, 0.92 + 0.3 * spill, legend);
  // a slat turned away from the fixtures catches less of their light
  vec3 col = tex.rgb * shade * mix(0.35, 1.0, sqrt(edgeOn));
  // halogen-white fixtures, a touch warm where they hit hardest
  col *= mix(vec3(1.0), vec3(1.06, 1.0, 0.88), clamp(spill - 0.3, 0.0, 1.0));
  float dist = length(vWorld - uCam);
  col = mix(col, uFogColor, fogAmount(dist, uFogDensity));
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`,
};
