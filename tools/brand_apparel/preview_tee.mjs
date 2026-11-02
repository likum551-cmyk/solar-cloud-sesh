// preview_tee.mjs - render the stock tee and the branded tee (front and back) from their raw BC1 mip chains.
//
//   node preview_tee.mjs --mesh=<uvdump.json> --stock=<real_k.bin> --branded=<composited_k.bin> --out=<dir> [--scale=700]
//
// --stock / --branded are the 8 raw BC1 mips (512 px, 174760 bytes) of jbib_diff_008_k_uni, the second one as dumped by
// tests/brand_apparel_test.cpp (the bytes the GPU is actually given). --mesh is the vertex dump make_decals.mjs uses.
// Output: <dir>/preview_tee.png with, left to right, front stock, front branded, front branded sampled at mip 2 (128 px,
// what a medium distance shows), back stock, back branded; <dir>/front_branded_zoom.png (2x crop of the chest).
// Nothing from the game is written next to the repo: pass scratch paths.
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { encodePng } from "./png.mjs";

const args = Object.fromEntries(process.argv.slice(2).map((a) => { const m = a.match(/^--([^=]+)=(.*)$/); return m ? [m[1], m[2]] : [a, true]; }));
if (!args.mesh || !args.stock || !args.branded || !args.out) { console.error("usage: preview_tee.mjs --mesh=... --stock=... --branded=... --out=<dir> [--scale=700]"); process.exit(2); }
mkdirSync(args.out, { recursive: true });
const SCALE = Number(args.scale || 700);

function decodeLevel(buf, level) {
  let off = 0;
  for (let l = 0; l < level; l++) { const w = Math.max(1, 512 >> l), bw = Math.max(1, w >> 2); off += bw * bw * 8; }
  const w = Math.max(1, 512 >> level), bw = Math.max(1, w >> 2);
  const px = new Uint8Array(w * w * 4);
  for (let by = 0; by < bw; by++) for (let bx = 0; bx < bw; bx++) {
    const o = off + (by * bw + bx) * 8;
    const c0 = buf[o] | (buf[o + 1] << 8), c1 = buf[o + 2] | (buf[o + 3] << 8);
    const e = (c) => { const r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31; return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)]; };
    const p0 = e(c0), p1 = e(c1);
    const pal = [p0, p1];
    if (c0 > c1) { pal.push(p0.map((v, i) => (2 * v + p1[i]) / 3), p0.map((v, i) => (v + 2 * p1[i]) / 3)); }
    else { pal.push(p0.map((v, i) => (v + p1[i]) / 2), [0, 0, 0]); }
    const idx = buf[o + 4] | (buf[o + 5] << 8) | (buf[o + 6] << 16) | (buf[o + 7] << 24);
    for (let i = 0; i < 16; i++) {
      const c = pal[(idx >>> (2 * i)) & 3];
      const x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
      if (x >= w || y >= w) continue;
      const q = (y * w + x) * 4;
      px[q] = c[0]; px[q + 1] = c[1]; px[q + 2] = c[2]; px[q + 3] = 255;
    }
  }
  return { w, px };
}

function sample(tex, u, v) {
  const { w, px } = tex;
  const fx = Math.min(Math.max(u * w - 0.5, 0), w - 1.001), fy = Math.min(Math.max(v * w - 0.5, 0), w - 1.001);
  const x0 = Math.floor(fx), y0 = Math.floor(fy), tx = fx - x0, ty = fy - y0;
  const out = [0, 0, 0];
  for (let c = 0; c < 3; c++) {
    const a = px[(y0 * w + x0) * 4 + c], b = px[(y0 * w + x0 + 1) * 4 + c], d = px[((y0 + 1) * w + x0) * 4 + c], e = px[((y0 + 1) * w + x0 + 1) * 4 + c];
    out[c] = (a * (1 - tx) + b * tx) * (1 - ty) + (d * (1 - tx) + e * tx) * ty;
  }
  return out;
}

const dump = JSON.parse(readFileSync(args.mesh, "utf8"));
const tris = [];
for (const g of dump) for (const t of g.t) tris.push(t.map((i) => g.v[i]));   // [x,y,z,u,v]
let minX = 1e9, maxX = -1e9, minZ = 1e9, maxZ = -1e9;
for (const t of tris) for (const p of t) { minX = Math.min(minX, p[0]); maxX = Math.max(maxX, p[0]); minZ = Math.min(minZ, p[2]); maxZ = Math.max(maxZ, p[2]); }
const pad = 0.04;
const spanX = maxX - minX + 2 * pad, spanZ = maxZ - minZ + 2 * pad;
const pxPerM = Math.min(SCALE / spanZ, (SCALE * 0.9) / spanX);
const W = Math.ceil(spanX * pxPerM), H = Math.ceil(spanZ * pxPerM);

function render(tex, front) {
  const img = new Uint8Array(W * H * 4);
  for (let i = 0; i < W * H; i++) { img[i * 4] = 38; img[i * 4 + 1] = 40; img[i * 4 + 2] = 46; img[i * 4 + 3] = 255; }
  const depth = new Float32Array(W * H).fill(-1e9);
  const sx = (x) => ((front ? -x : x) - (front ? -maxX : minX) + pad) * pxPerM;
  const sy = (z) => (maxZ - z + pad) * pxPerM;
  for (const t of tris) {
    const P = t.map((p) => ({ x: sx(p[0]), y: sy(p[2]), d: front ? p[1] : -p[1], u: p[3], v: p[4] }));
    // face normal vs view direction for a little shading
    const ax = t[1][0] - t[0][0], ay = t[1][1] - t[0][1], az = t[1][2] - t[0][2], bx = t[2][0] - t[0][0], by = t[2][1] - t[0][1], bz = t[2][2] - t[0][2];
    let nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
    const nl = Math.hypot(nx, ny, nz) || 1;
    const facing = Math.abs((front ? ny : -ny) / nl);
    const shade = 0.62 + 0.38 * facing;
    const minx = Math.max(0, Math.floor(Math.min(P[0].x, P[1].x, P[2].x))), maxx = Math.min(W - 1, Math.ceil(Math.max(P[0].x, P[1].x, P[2].x)));
    const miny = Math.max(0, Math.floor(Math.min(P[0].y, P[1].y, P[2].y))), maxy = Math.min(H - 1, Math.ceil(Math.max(P[0].y, P[1].y, P[2].y)));
    const den = (P[1].y - P[2].y) * (P[0].x - P[2].x) + (P[2].x - P[1].x) * (P[0].y - P[2].y);
    if (Math.abs(den) < 1e-9) continue;
    for (let y = miny; y <= maxy; y++) for (let x = minx; x <= maxx; x++) {
      const px = x + 0.5, py = y + 0.5;
      const l0 = ((P[1].y - P[2].y) * (px - P[2].x) + (P[2].x - P[1].x) * (py - P[2].y)) / den;
      const l1 = ((P[2].y - P[0].y) * (px - P[2].x) + (P[0].x - P[2].x) * (py - P[2].y)) / den;
      const l2 = 1 - l0 - l1;
      if (l0 < -1e-4 || l1 < -1e-4 || l2 < -1e-4) continue;
      const d = l0 * P[0].d + l1 * P[1].d + l2 * P[2].d;
      if (d <= depth[y * W + x]) continue;
      depth[y * W + x] = d;
      const c = sample(tex, l0 * P[0].u + l1 * P[1].u + l2 * P[2].u, l0 * P[0].v + l1 * P[1].v + l2 * P[2].v);
      const o = (y * W + x) * 4;
      img[o] = Math.min(255, c[0] * shade); img[o + 1] = Math.min(255, c[1] * shade); img[o + 2] = Math.min(255, c[2] * shade);
    }
  }
  return img;
}

const stock = readFileSync(args.stock), branded = readFileSync(args.branded);
if (stock.length !== 174760 || branded.length !== 174760) { console.error("both chains must be 174760 bytes"); process.exit(1); }
const panels = [
  render(decodeLevel(stock, 0), true),
  render(decodeLevel(branded, 0), true),
  render(decodeLevel(branded, 2), true),
  render(decodeLevel(stock, 0), false),
  render(decodeLevel(branded, 0), false),
];
const gap = 8, TW = panels.length * W + (panels.length - 1) * gap;
const out = new Uint8Array(TW * H * 4);
for (let i = 0; i < TW * H; i++) { out[i * 4] = 12; out[i * 4 + 1] = 12; out[i * 4 + 2] = 14; out[i * 4 + 3] = 255; }
panels.forEach((p, k) => { for (let y = 0; y < H; y++) out.set(p.subarray(y * W * 4, (y + 1) * W * 4), (y * TW + k * (W + gap)) * 4); });
writeFileSync(args.out + "/preview_tee.png", encodePng(TW, H, Buffer.from(out.buffer)));

// 2x crop of the branded front chest
const zx0 = Math.floor(W * 0.18), zx1 = Math.floor(W * 0.82), zy0 = Math.floor(H * 0.12), zy1 = Math.floor(H * 0.78);
const zw = (zx1 - zx0) * 2, zh = (zy1 - zy0) * 2;
const zoom = new Uint8Array(zw * zh * 4);
for (let y = 0; y < zh; y++) for (let x = 0; x < zw; x++) {
  const s = ((zy0 + (y >> 1)) * W + zx0 + (x >> 1)) * 4, d = (y * zw + x) * 4;
  zoom[d] = panels[1][s]; zoom[d + 1] = panels[1][s + 1]; zoom[d + 2] = panels[1][s + 2]; zoom[d + 3] = 255;
}
writeFileSync(args.out + "/front_branded_zoom.png", encodePng(zw, zh, Buffer.from(zoom.buffer)));
console.log(`wrote ${args.out}/preview_tee.png (${TW}x${H}) and front_branded_zoom.png (${zw}x${zh})`);
