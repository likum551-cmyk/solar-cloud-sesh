// make_decals.mjs - bake the Solar RP brand decals for the branded tee into texture space and write
// apps/engine/brand_apparel_data.inc (the data brand_apparel.cpp composites onto the stock texture at run time).
//
//   node make_decals.mjs --mesh=<uvdump.json> --tail-hash=<16 hex> [--out=<file.inc>] [--debug-dir=<dir>]
//
// What it does: the stock "Sky Blue Tee" (mp_m_freemode_01, component 11, drawable 8, texture 10 =
// jbib_008_u.ydd + jbib_diff_008_k_uni.ytd) is a front panel and a back panel laid side by side in UV space.
// Each logo is placed on the garment in metres (x = sideways, z = up, orthographic along y, like a screen print)
// and every UV texel of the panel is mapped to that plane through the mesh's own triangles (barycentric), so the
// decal lands exactly where the mesh puts that part of the shirt. The result is a single-colour alpha mask per decal
// at the 512 px base level (what brand_apparel.cpp blends over the BC1/BC3 blocks it touches).
//
// Inputs that are NOT in the repo: --mesh is a JSON dump of the game's jbib_008_u.ydd vertices
// ([{v:[x,y,z,u,v]...,t:[[i,j,k]...]}], see tools/brand_apparel/derive/ydd_uv.mjs, run against your own game
// install); --tail-hash is the fingerprint of the stock texture's small mips (derive/dump_ytd.mjs). Only the
// numbers and the generated decals are committed, never the mesh or the texture.
//
// No dependencies. Deterministic.

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { decodePng, encodePng } from "./png.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..", "..");
const args = Object.fromEntries(process.argv.slice(2).map((a) => {
  const m = /^--([^=]+)=(.*)$/.exec(a);
  if (!m) { console.error(`bad argument: ${a}`); process.exit(2); }
  return [m[1], m[2]];
}));
if (!args.mesh || !args["tail-hash"]) { console.error("usage: make_decals.mjs --mesh=<uvdump.json> --tail-hash=<16 hex> [--out=...] [--debug-dir=...]"); process.exit(2); }
if (!/^[0-9a-f]{16}$/.test(args["tail-hash"])) { console.error("--tail-hash must be 16 lowercase hex digits"); process.exit(2); }
const outFile = path.resolve(args.out ?? path.join(repoRoot, "apps", "engine", "brand_apparel_data.inc"));

// ---- the design (metres on the garment; z up, x = ped's right) --------------------------------------
const INK = [0xf7, 0xf2, 0xe8];             // Solar RP NOON ink #f7f2e8 (brand/solar/solar-rp-noon/README.md)
const BASE = 512;                           // the texture's full-size width/height the decals are authored at
const SS = 4;                               // supersampling per texel while baking
// The Solar mark is 1:2 (viewBox 50 x 100), so the chest mark is sized by its height: 0.10 m wide x 0.20 m tall keeps the
// old square mark's vertical extent (0.196 m) around the same centre. Sources are made by derive_solar.mjs (brand/solar/derived);
// only their alpha is used (a decal is a single-colour mask), so the ink variants are the right inputs.
const DECALS = [
  { name: "front chest mark", side: "front", src: "brand/solar/derived/solar-rp-mark-ink-1024.png",
    cx: 0.0, cz: 0.36, widthM: 0.10 },
  { name: "back wordmark lockup", side: "back", src: "brand/solar/derived/solar-rp-lockup-horizontal-ink-256h.png",
    cx: 0.0, cz: 0.40, widthM: 0.30 },
];

// ---- mesh ---------------------------------------------------------------------------------------------
const dump = JSON.parse(readFileSync(args.mesh, "utf8"));
const tris = [];                            // {p:[[x,y,z,u,v] x3]}
for (const g of dump) for (const t of g.t) {
  const v = t.map((i) => g.v[i]);
  if (v.some((x) => !x)) continue;
  tris.push(v);
}
console.error(`mesh: ${tris.length} triangles`);

// ---- logo sources (cropped to the alpha bounding box) -------------------------------------------------
function loadLogo(rel) {
  const { w, h, px } = decodePng(readFileSync(path.join(repoRoot, rel)));
  let minx = w, miny = h, maxx = -1, maxy = -1;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    if (px[(y * w + x) * 4 + 3] > 8) { minx = Math.min(minx, x); maxx = Math.max(maxx, x); miny = Math.min(miny, y); maxy = Math.max(maxy, y); }
  }
  if (maxx < 0) throw new Error(`${rel}: no opaque pixels`);
  const cw = maxx - minx + 1, ch = maxy - miny + 1;
  const a = new Float32Array(cw * ch);
  for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) a[y * cw + x] = px[((miny + y) * w + (minx + x)) * 4 + 3] / 255;
  return { w: cw, h: ch, a };
}
function sample(logo, s, t) {                // bilinear alpha at s,t in [0,1]; 0 outside
  if (s < 0 || t < 0 || s > 1 || t > 1) return 0;
  const x = s * logo.w - 0.5, y = t * logo.h - 0.5;
  const x0 = Math.floor(x), y0 = Math.floor(y), fx = x - x0, fy = y - y0;
  const at = (xx, yy) => (xx < 0 || yy < 0 || xx >= logo.w || yy >= logo.h) ? 0 : logo.a[yy * logo.w + xx];
  return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

// ---- bake ---------------------------------------------------------------------------------------------
function bake(d) {
  const logo = loadLogo(d.src);
  const heightM = d.widthM * logo.h / logo.w;
  const N = BASE * SS;
  const acc = new Float32Array(N * N);
  const front = d.side === "front";
  for (const v of tris) {
    const yavg = (v[0][1] + v[1][1] + v[2][1]) / 3;
    const uavg = (v[0][3] + v[1][3] + v[2][3]) / 3, vavg = (v[0][4] + v[1][4] + v[2][4]) / 3;
    if (front ? !(yavg > 0 && uavg < 0.5 && vavg > 0.38) : !(yavg < 0 && uavg > 0.5 && vavg > 0.38)) continue;
    // supersample-grid bounding box of the triangle in UV space
    const us = v.map((p) => p[3] * N), vs = v.map((p) => p[4] * N);
    const x0 = Math.max(0, Math.floor(Math.min(...us))), x1 = Math.min(N - 1, Math.ceil(Math.max(...us)));
    const y0 = Math.max(0, Math.floor(Math.min(...vs))), y1 = Math.min(N - 1, Math.ceil(Math.max(...vs)));
    const den = (vs[1] - vs[2]) * (us[0] - us[2]) + (us[2] - us[1]) * (vs[0] - vs[2]);
    if (Math.abs(den) < 1e-9) continue;
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
      const px_ = x + 0.5, py_ = y + 0.5;
      const l0 = ((vs[1] - vs[2]) * (px_ - us[2]) + (us[2] - us[1]) * (py_ - vs[2])) / den;
      const l1 = ((vs[2] - vs[0]) * (px_ - us[2]) + (us[0] - us[2]) * (py_ - vs[2])) / den;
      const l2 = 1 - l0 - l1;
      if (l0 < -1e-6 || l1 < -1e-6 || l2 < -1e-6) continue;
      const gx = l0 * v[0][0] + l1 * v[1][0] + l2 * v[2][0];
      const gz = l0 * v[0][2] + l1 * v[1][2] + l2 * v[2][2];
      // front: the viewer faces the ped, so the ped's right (+x) is on the viewer's left; back: +x is on the right.
      const s = front ? 0.5 + (d.cx - gx) / d.widthM : 0.5 + (gx - d.cx) / d.widthM;
      const t = 0.5 + (d.cz - gz) / heightM;
      const a = sample(logo, s, t);
      if (a > 0) acc[y * N + x] = a;
    }
  }
  // box-downsample to BASE x BASE
  const a8 = new Uint8Array(BASE * BASE);
  for (let y = 0; y < BASE; y++) for (let x = 0; x < BASE; x++) {
    let sum = 0;
    for (let sy = 0; sy < SS; sy++) for (let sx = 0; sx < SS; sx++) sum += acc[(y * SS + sy) * N + (x * SS + sx)];
    a8[y * BASE + x] = Math.min(255, Math.round(255 * sum / (SS * SS)));
  }
  // bounding rect, grown to multiples of 16 so every mip down to 32 px has an integer-aligned rect
  let minx = BASE, miny = BASE, maxx = -1, maxy = -1;
  for (let y = 0; y < BASE; y++) for (let x = 0; x < BASE; x++) if (a8[y * BASE + x] > 0) { minx = Math.min(minx, x); maxx = Math.max(maxx, x); miny = Math.min(miny, y); maxy = Math.max(maxy, y); }
  if (maxx < 0) throw new Error(`${d.name}: the baked decal is empty (mesh/side filter matched nothing)`);
  const rx = Math.max(0, Math.floor(minx / 16) * 16), ry = Math.max(0, Math.floor(miny / 16) * 16);
  const rx1 = Math.min(BASE, Math.ceil((maxx + 1) / 16) * 16), ry1 = Math.min(BASE, Math.ceil((maxy + 1) / 16) * 16);
  const rw = rx1 - rx, rh = ry1 - ry;
  const crop = new Uint8Array(rw * rh);
  for (let y = 0; y < rh; y++) for (let x = 0; x < rw; x++) crop[y * rw + x] = a8[(ry + y) * BASE + (rx + x)];
  return { name: d.name, src: d.src, x: rx, y: ry, w: rw, h: rh, a8: crop, full: a8, widthM: d.widthM, heightM, cx: d.cx, cz: d.cz };
}

const baked = DECALS.map(bake);
for (const b of baked) console.error(`${b.name}: ${b.w}x${b.h} px at (${b.x},${b.y}) of ${BASE}, ${b.widthM} x ${b.heightM.toFixed(3)} m, ${b.a8.reduce((n, a) => n + (a > 0 ? 1 : 0), 0)} covered texels`);

// ---- debug images ---------------------------------------------------------------------------------------
if (args["debug-dir"]) {
  mkdirSync(args["debug-dir"], { recursive: true });
  const img = Buffer.alloc(BASE * BASE * 4);
  for (let i = 0; i < BASE * BASE; i++) { img[i * 4] = 20; img[i * 4 + 1] = 120; img[i * 4 + 2] = 200; img[i * 4 + 3] = 255; }
  for (const b of baked) for (let i = 0; i < BASE * BASE; i++) {
    const a = b.full[i] / 255;
    for (let c = 0; c < 3; c++) img[i * 4 + c] = Math.round(img[i * 4 + c] * (1 - a) + INK[c] * a);
  }
  writeFileSync(path.join(args["debug-dir"], "decals_over_flat_blue.png"), encodePng(BASE, BASE, img));
}

// ---- write the .inc -------------------------------------------------------------------------------------
function arr(name, a8) {
  const rows = [];
  for (let i = 0; i < a8.length; i += 32) rows.push("    " + Array.from(a8.subarray(i, i + 32), (v) => String(v)).join(","));
  return `static const uint8_t ${name}[${a8.length}] = {\n${rows.join(",\n")}\n};\n`;
}
let out = `// GENERATED by tools/brand_apparel/make_decals.mjs - do not edit by hand. Included once by brand_apparel.cpp.
// Single-colour alpha decals baked into the UV space of the stock tee (base ${BASE} px), plus the fingerprint of
// the stock texture they go on. No Rockstar pixels or geometry are stored here: the fingerprint is a 64-bit hash
// of the stock texture's small mips and the decals are our own brand artwork warped through the mesh's UV mapping.
//
// Target: mp_m_freemode_01 component 11, drawable 8, texture 10 (Sky Blue Tee: jbib_008_u.ydd + jbib_diff_008_k_uni.ytd),
// 512x512 BC1 (DXT1), 8 mips down to 4x4. Tail fingerprint = FNV-1a 64 over the bytes of every mip whose width is <= 64.
`;
baked.forEach((b, i) => { out += `\n// ${b.name}: ${b.src}, ${b.widthM} m wide on the garment, centre x ${b.cx} m z ${b.cz} m\n` + arr(`kDecalData${i}`, b.a8); });
out += `\nstatic const Decal kDecals[] = {\n`;
baked.forEach((b, i) => { out += `    { ${b.x}, ${b.y}, ${b.w}, ${b.h}, kDecalData${i}, { ${INK.join(", ")} } },   // ${b.name}\n`; });
out += `};\n\nstatic const Target kTargets[] = {\n    { "jbib_diff_008_k_uni (top 11/8/10)", 0x${args["tail-hash"]}ULL, kFmtBC1, ${BASE}, 8, 11, 8, 10, kDecals, ${baked.length} },\n};\n`;
mkdirSync(path.dirname(outFile), { recursive: true });
writeFileSync(outFile, out);
console.error(`wrote ${outFile} (${out.length} bytes)`);
