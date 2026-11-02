// make_logo_texture.mjs - build the GAGE-MP logo tee's diffuse texture from the real brand mark.
//
//   node make_logo_texture.mjs [--body=#f2f1ee] [--chest=0.25,0.17,0.17] [--size=1024] [--out=<dir>]
//
// Inputs : brand/icon/colour/gage-mp-mark-colour-1024.png (the brand kit's blue plate with the G cut out).
// Outputs (default <pack>/out/):
//   jbib_diff_000_a_uni.png / .dds   the tee texture: fabric + the mark on the chest. DDS = BC3 (DXT5) with a
//                                    full mip chain, ready to import into a YTD (CodeWalker: YTD editor ->
//                                    add texture, name it jbib_diff_000_a_uni).
//   jbib_diff_uvprobe.png / .dds     calibration: an 8x8 grid, each cell labelled "col row" and coloured, to
//                                    be worn once in game to read where the chest / back / sleeves sit in UV.
//   preview.png                      the tee texture, the logo at 1:1, and the BC3 read-back, side by side.
//
// --chest=cx,cy,w is the logo centre (u, v in 0..1, v down) and its width as a fraction of the texture. The
// default is an ESTIMATE: the base tee's UV layout could not be read offline (the game archives are NG-encrypted
// and no key tool is here). Wear uvprobe once, read the chest cell, then re-run with the measured --chest.
//
// No dependencies (node:zlib only). Deterministic: the same arguments give the same bytes.

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { deflateSync, inflateSync } from "node:zlib";
import { fileURLToPath } from "node:url";
import path from "node:path";

const here = path.dirname(fileURLToPath(import.meta.url));
const packDir = path.resolve(here, "..");
const repoRoot = path.resolve(here, "..", "..", "..", "..");

// ---- arguments ---------------------------------------------------------------------------------
const args = Object.fromEntries(process.argv.slice(2).map((a) => {
  const m = /^--([^=]+)=(.*)$/.exec(a);
  if (!m) { console.error(`bad argument: ${a}`); process.exit(2); }
  return [m[1], m[2]];
}));
const SIZE = Number(args.size ?? 1024);
if (![256, 512, 1024, 2048].includes(SIZE)) { console.error("--size must be 256, 512, 1024 or 2048"); process.exit(2); }
const bodyHex = (args.body ?? "#f2f1ee").replace("#", "");
if (!/^[0-9a-fA-F]{6}$/.test(bodyHex)) { console.error("--body must be #rrggbb"); process.exit(2); }
const BODY = [0, 2, 4].map((i) => parseInt(bodyHex.slice(i, i + 2), 16));
const chestArg = (args.chest ?? "0.25,0.17,0.17").split(",").map(Number);
if (chestArg.length !== 3 || chestArg.some((n) => !Number.isFinite(n))) { console.error("--chest must be cx,cy,w"); process.exit(2); }
const [CHEST_CX, CHEST_CY, CHEST_W] = chestArg;
const outDir = path.resolve(args.out ?? path.join(packDir, "out"));
const markPath = path.join(repoRoot, "brand", "icon", "colour", "gage-mp-mark-colour-1024.png");

// ---- PNG decode / encode -----------------------------------------------------------------------
function crc32(buf) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function decodePng(buf) {
  const sig = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
  for (let i = 0; i < 8; i++) if (buf[i] !== sig[i]) throw new Error("not a PNG");
  let pos = 8, w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString("latin1", pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === "IHDR") { w = data.readUInt32BE(0); h = data.readUInt32BE(4); depth = data[8]; ctype = data[9]; interlace = data[12]; }
    else if (type === "IDAT") idat.push(data);
    else if (type === "IEND") break;
    pos += 12 + len;
  }
  if (depth !== 8 || interlace !== 0 || (ctype !== 6 && ctype !== 2)) throw new Error(`unsupported PNG (depth ${depth}, colour type ${ctype}, interlace ${interlace})`);
  const bpp = ctype === 6 ? 4 : 3;
  const raw = inflateSync(Buffer.concat(idat));
  const stride = w * bpp;
  if (raw.length !== (stride + 1) * h) throw new Error("PNG data length mismatch");
  const px = Buffer.alloc(w * h * 4);
  const prev = Buffer.alloc(stride), cur = Buffer.alloc(stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)];
    raw.copy(cur, 0, y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      let v = cur[i];
      if (f === 1) v += a;
      else if (f === 2) v += b;
      else if (f === 3) v += (a + b) >> 1;
      else if (f === 4) { const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
      else if (f !== 0) throw new Error(`bad PNG filter ${f}`);
      cur[i] = v & 0xff;
    }
    for (let x = 0; x < w; x++) {
      const o = (y * w + x) * 4;
      px[o] = cur[x * bpp]; px[o + 1] = cur[x * bpp + 1]; px[o + 2] = cur[x * bpp + 2];
      px[o + 3] = bpp === 4 ? cur[x * bpp + 3] : 255;
    }
    cur.copy(prev);
  }
  return { w, h, px };
}

function encodePng(w, h, rgba) {
  const stride = w * 4;
  const raw = Buffer.alloc((stride + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (stride + 1)] = 0; rgba.copy(raw, y * (stride + 1) + 1, y * stride, (y + 1) * stride); }
  const chunk = (type, data) => {
    const out = Buffer.alloc(12 + data.length);
    out.writeUInt32BE(data.length, 0);
    out.write(type, 4, "latin1");
    data.copy(out, 8);
    out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
    return out;
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw, { level: 9 })), chunk("IEND", Buffer.alloc(0))]);
}

// ---- small helpers -----------------------------------------------------------------------------
function mulberry32(seed) {
  let a = seed >>> 0;
  return () => { a = (a + 0x6d2b79f5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}
const clamp = (v, lo = 0, hi = 255) => (v < lo ? lo : v > hi ? hi : v);

// Bilinear sample of the source mark (RGBA) at fractional pixel coordinates.
function sample(src, fx, fy) {
  const x0 = Math.floor(fx), y0 = Math.floor(fy), tx = fx - x0, ty = fy - y0;
  const out = [0, 0, 0, 0];
  for (let j = 0; j < 2; j++) for (let i = 0; i < 2; i++) {
    const x = Math.min(src.w - 1, Math.max(0, x0 + i)), y = Math.min(src.h - 1, Math.max(0, y0 + j));
    const wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
    const o = (y * src.w + x) * 4;
    // premultiply so a transparent texel's colour does not bleed into the edge
    const a = src.px[o + 3] / 255;
    out[0] += src.px[o] * a * wgt; out[1] += src.px[o + 1] * a * wgt; out[2] += src.px[o + 2] * a * wgt; out[3] += a * wgt;
  }
  return out[3] > 0 ? [out[0] / out[3], out[1] / out[3], out[2] / out[3], out[3]] : [0, 0, 0, 0];
}

// The brand PNG as ink coverage: blue ink over transparent or over an opaque white G. alpha = the share of the
// brand blue in the pixel (unmixed against white), times the PNG's own alpha. R of #2d6eb9 is the smallest channel (45).
function inkCoverage(r, g, b, a) {
  const blueMin = 45;
  const unmix = clamp((255 - Math.min(r, g, b)) / (255 - blueMin), 0, 1);
  return unmix * a;
}

// ---- fabric ------------------------------------------------------------------------------------
// A jersey-knit tee: fine vertical ribs, a little per-thread noise and a slow shading drift, +-3% in all.
function fabricTexel(x, y, rnd, drift) {
  const rib = 0.014 * Math.sin((x / SIZE) * Math.PI * 2 * (SIZE / 3.2));
  const weave = 0.007 * Math.sin((y / SIZE) * Math.PI * 2 * (SIZE / 5.1) + (x & 1) * Math.PI);
  const noise = (rnd() - 0.5) * 0.02;
  return 1 + rib + weave + noise + drift;
}

// ---- the tee texture ---------------------------------------------------------------------------
function buildTee(mark) {
  const px = Buffer.alloc(SIZE * SIZE * 4);
  const rnd = mulberry32(0x6a6a6a6a);
  const logoW = CHEST_W * SIZE;                 // px
  const logoH = logoW * (mark.h / mark.w);
  const x0 = CHEST_CX * SIZE - logoW / 2, y0 = CHEST_CY * SIZE - logoH / 2;
  const INK_OPACITY = 0.94;                     // a screen print: the fabric still shows through the ink
  let inkPixels = 0;
  for (let y = 0; y < SIZE; y++) {
    for (let x = 0; x < SIZE; x++) {
      const o = (y * SIZE + x) * 4;
      const drift = 0.012 * Math.sin((x + y) / SIZE * Math.PI * 1.3);
      const f = fabricTexel(x, y, rnd, drift);
      let r = BODY[0] * f, g = BODY[1] * f, b = BODY[2] * f;
      // logo, 3x3 supersampled so the chamfers are clean at the print size
      const u = (x - x0) / logoW, v = (y - y0) / logoH;
      if (u > -0.02 && u < 1.02 && v > -0.02 && v < 1.02) {
        let cov = 0;
        for (let sj = 0; sj < 3; sj++) for (let si = 0; si < 3; si++) {
          const fx = ((x + (si + 0.5) / 3) - x0) / logoW * mark.w - 0.5;
          const fy = ((y + (sj + 0.5) / 3) - y0) / logoH * mark.h - 0.5;
          if (fx < -0.5 || fy < -0.5 || fx > mark.w - 0.5 || fy > mark.h - 0.5) continue;
          const s = sample(mark, fx, fy);
          cov += inkCoverage(s[0], s[1], s[2], s[3]);   // s = [r, g, b, alpha 0..1]
        }
        cov /= 9;
        if (cov > 0) {
          const a = cov * INK_OPACITY;
          const ink = [0x2d, 0x6e, 0xb9].map((c) => c * (0.9 + 0.1 * f));   // the knit modulates the ink a little
          r = r * (1 - a) + ink[0] * a; g = g * (1 - a) + ink[1] * a; b = b * (1 - a) + ink[2] * a;
          if (cov > 0.5) ++inkPixels;
        }
      }
      px[o] = clamp(Math.round(r)); px[o + 1] = clamp(Math.round(g)); px[o + 2] = clamp(Math.round(b)); px[o + 3] = 255;
    }
  }
  return { px, inkPixels, logoBox: [x0, y0, logoW, logoH] };
}

// ---- the UV probe ------------------------------------------------------------------------------
const FONT = { // 3x5 digits, rows top to bottom, bit 2 = left column
  0: [7, 5, 5, 5, 7], 1: [2, 6, 2, 2, 7], 2: [7, 1, 7, 4, 7], 3: [7, 1, 7, 1, 7], 4: [5, 5, 7, 1, 1],
  5: [7, 4, 7, 1, 7], 6: [7, 4, 7, 5, 7], 7: [7, 1, 1, 1, 1], 8: [7, 5, 7, 5, 7], 9: [7, 5, 7, 1, 7],
};
function buildProbe() {
  const px = Buffer.alloc(SIZE * SIZE * 4);
  const N = 8, cell = SIZE / N;
  for (let y = 0; y < SIZE; y++) for (let x = 0; x < SIZE; x++) {
    const cx = Math.floor(x / cell), cy = Math.floor(y / cell);
    const o = (y * SIZE + x) * 4;
    // hue by column, lightness by row, so neighbours differ and the cell is findable by colour alone
    const hue = (cx / N) * 360, light = 0.38 + 0.07 * cy;
    const c = (1 - Math.abs(2 * light - 1)) * 0.75, hp = hue / 60, xx = c * (1 - Math.abs((hp % 2) - 1)), m = light - c / 2;
    const [r1, g1, b1] = hp < 1 ? [c, xx, 0] : hp < 2 ? [xx, c, 0] : hp < 3 ? [0, c, xx] : hp < 4 ? [0, xx, c] : hp < 5 ? [xx, 0, c] : [c, 0, xx];
    let rgb = [(r1 + m) * 255, (g1 + m) * 255, (b1 + m) * 255];
    const lx = x % cell, ly = y % cell;
    if (lx < 3 || ly < 3) rgb = [255, 255, 255];                    // cell border
    // label "col row" in black, scale s
    const s = Math.floor(cell / 14);
    const lw = (3 * 2 + 1) * s, lh = 5 * s, ox = Math.floor((cell - lw) / 2), oy = Math.floor((cell - lh) / 2);
    const gx = lx - ox, gy = ly - oy;
    if (gx >= 0 && gy >= 0 && gy < lh) {
      const glyph = Math.floor(gx / (4 * s)), within = gx % (4 * s);
      if (glyph < 2 && within < 3 * s) {
        const d = glyph === 0 ? cx : cy;
        if ((FONT[d][Math.floor(gy / s)] >> (2 - Math.floor(within / s))) & 1) rgb = [0, 0, 0];
      }
    }
    px[o] = Math.round(clamp(rgb[0])); px[o + 1] = Math.round(clamp(rgb[1])); px[o + 2] = Math.round(clamp(rgb[2])); px[o + 3] = 255;
  }
  return px;
}

// ---- BC3 (DXT5) encode / decode, DDS container --------------------------------------------------
const to565 = (r, g, b) => ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
function from565(c) {
  const r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)];
}

function encodeColorBlock(blk /* 16 x [r,g,b] */) {
  // principal axis by power iteration on the block's covariance, endpoints = extreme projections
  const mean = [0, 0, 0];
  for (const p of blk) { mean[0] += p[0]; mean[1] += p[1]; mean[2] += p[2]; }
  for (let k = 0; k < 3; k++) mean[k] /= 16;
  const cov = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  for (const p of blk) { const d = [p[0] - mean[0], p[1] - mean[1], p[2] - mean[2]]; for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) cov[i][j] += d[i] * d[j]; }
  let axis = [1, 1, 1];
  for (let it = 0; it < 6; it++) {
    const n = [0, 1, 2].map((i) => cov[i][0] * axis[0] + cov[i][1] * axis[1] + cov[i][2] * axis[2]);
    const len = Math.hypot(n[0], n[1], n[2]);
    if (len < 1e-6) break;
    axis = n.map((v) => v / len);
  }
  let lo = Infinity, hi = -Infinity, pLo = blk[0], pHi = blk[0];
  for (const p of blk) { const t = (p[0] - mean[0]) * axis[0] + (p[1] - mean[1]) * axis[1] + (p[2] - mean[2]) * axis[2]; if (t < lo) { lo = t; pLo = p; } if (t > hi) { hi = t; pHi = p; } }
  let c0 = to565(pHi[0], pHi[1], pHi[2]), c1 = to565(pLo[0], pLo[1], pLo[2]);
  if (c0 < c1) { const t = c0; c0 = c1; c1 = t; }
  const out = Buffer.alloc(8);
  out.writeUInt16LE(c0, 0); out.writeUInt16LE(c1, 2);
  if (c0 === c1) return out;                       // flat block: all indices 0
  const e0 = from565(c0), e1 = from565(c1);
  const pal = [e0, e1, e0.map((v, i) => Math.round((2 * v + e1[i]) / 3)), e0.map((v, i) => Math.round((v + 2 * e1[i]) / 3))];
  let bits = 0;
  for (let i = 0; i < 16; i++) {
    let best = 0, bd = Infinity;
    for (let k = 0; k < 4; k++) { const d = (blk[i][0] - pal[k][0]) ** 2 + (blk[i][1] - pal[k][1]) ** 2 + (blk[i][2] - pal[k][2]) ** 2; if (d < bd) { bd = d; best = k; } }
    bits |= best << (2 * i);
  }
  out.writeUInt32LE(bits >>> 0, 4);
  return out;
}

function encodeBC3Level(px, w, h) {
  const bw = Math.max(1, w >> 2), bh = Math.max(1, h >> 2);
  const out = Buffer.alloc(bw * bh * 16);
  const blk = new Array(16);
  for (let by = 0; by < bh; by++) for (let bx = 0; bx < bw; bx++) {
    for (let i = 0; i < 16; i++) {
      const x = Math.min(w - 1, bx * 4 + (i & 3)), y = Math.min(h - 1, by * 4 + (i >> 2));
      const o = (y * w + x) * 4;
      blk[i] = [px[o], px[o + 1], px[o + 2]];
    }
    const o = (by * bw + bx) * 16;
    // alpha block: opaque texture, alpha0 = alpha1 = 255, indices 0
    out[o] = 255; out[o + 1] = 255;
    encodeColorBlock(blk).copy(out, o + 8);
  }
  return out;
}

function decodeBC3Level(data, w, h) {
  const bw = Math.max(1, w >> 2), bh = Math.max(1, h >> 2);
  const px = Buffer.alloc(w * h * 4);
  for (let by = 0; by < bh; by++) for (let bx = 0; bx < bw; bx++) {
    const o = (by * bw + bx) * 16 + 8;
    const c0 = data.readUInt16LE(o), c1 = data.readUInt16LE(o + 2), bits = data.readUInt32LE(o + 4);
    const e0 = from565(c0), e1 = from565(c1);
    const pal = c0 > c1
      ? [e0, e1, e0.map((v, i) => Math.round((2 * v + e1[i]) / 3)), e0.map((v, i) => Math.round((v + 2 * e1[i]) / 3))]
      : [e0, e1, e0.map((v, i) => Math.round((v + e1[i]) / 2)), [0, 0, 0]];
    for (let i = 0; i < 16; i++) {
      const x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
      if (x >= w || y >= h) continue;
      const p = pal[(bits >>> (2 * i)) & 3], q = (y * w + x) * 4;
      px[q] = p[0]; px[q + 1] = p[1]; px[q + 2] = p[2]; px[q + 3] = 255;
    }
  }
  return px;
}

function downsample(px, w, h) {
  const nw = Math.max(1, w >> 1), nh = Math.max(1, h >> 1);
  const out = Buffer.alloc(nw * nh * 4);
  for (let y = 0; y < nh; y++) for (let x = 0; x < nw; x++) for (let c = 0; c < 4; c++) {
    let s = 0;
    for (let j = 0; j < 2; j++) for (let i = 0; i < 2; i++) s += px[(Math.min(h - 1, y * 2 + j) * w + Math.min(w - 1, x * 2 + i)) * 4 + c];
    out[(y * nw + x) * 4 + c] = Math.round(s / 4);
  }
  return { px: out, w: nw, h: nh };
}

function buildDds(px0) {
  const levels = [];
  let cur = { px: px0, w: SIZE, h: SIZE };
  for (;;) {
    levels.push({ ...cur, data: encodeBC3Level(cur.px, cur.w, cur.h) });
    if (cur.w === 1 && cur.h === 1) break;
    cur = downsample(cur.px, cur.w, cur.h);
  }
  const hdr = Buffer.alloc(128);
  hdr.write("DDS ", 0, "latin1");
  hdr.writeUInt32LE(124, 4);
  hdr.writeUInt32LE(0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, 8);   // CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT|LINEARSIZE
  hdr.writeUInt32LE(SIZE, 12); hdr.writeUInt32LE(SIZE, 16);
  hdr.writeUInt32LE(levels[0].data.length, 20);
  hdr.writeUInt32LE(levels.length, 28);
  hdr.writeUInt32LE(32, 76); hdr.writeUInt32LE(0x4, 80); hdr.write("DXT5", 84, "latin1");   // pixel format: FourCC DXT5
  hdr.writeUInt32LE(0x1000 | 0x8 | 0x400000, 108);                         // TEXTURE|COMPLEX|MIPMAP
  return { dds: Buffer.concat([hdr, ...levels.map((l) => l.data)]), levels };
}

// ---- go ----------------------------------------------------------------------------------------
const mark = decodePng(readFileSync(markPath));
if (mark.w !== 1024 || mark.h !== 1024) throw new Error(`unexpected mark size ${mark.w}x${mark.h}`);
{ // the source must contain both the brand blue and an interior that is not blue, or the unmix is meaningless
  let blue = 0, clear = 0;
  for (let i = 0; i < mark.w * mark.h; i++) {
    const o = i * 4;
    if (mark.px[o + 3] > 250 && Math.abs(mark.px[o] - 0x2d) < 4 && Math.abs(mark.px[o + 1] - 0x6e) < 4 && Math.abs(mark.px[o + 2] - 0xb9) < 4) blue++;
    else if (mark.px[o + 3] < 5 || (mark.px[o + 3] > 250 && mark.px[o] > 250 && mark.px[o + 1] > 250 && mark.px[o + 2] > 250)) clear++;
  }
  console.log(`brand mark: ${blue} px of #2d6eb9, ${clear} px transparent-or-white`);
  if (blue < 100000 || clear < 100000) throw new Error("brand mark does not look like the blue plate with a cut-out G");
}

mkdirSync(outDir, { recursive: true });
const tee = buildTee(mark);
console.log(`tee ${SIZE}x${SIZE}: logo box x=${tee.logoBox[0].toFixed(1)} y=${tee.logoBox[1].toFixed(1)} w=${tee.logoBox[2].toFixed(1)} h=${tee.logoBox[3].toFixed(1)}, ${tee.inkPixels} ink px`);
if (tee.inkPixels < 1000) throw new Error("the logo did not land on the texture (check --chest)");
const probe = buildProbe();

const teeDds = buildDds(tee.px);
const probeDds = buildDds(probe);
writeFileSync(path.join(outDir, "jbib_diff_000_a_uni.png"), encodePng(SIZE, SIZE, tee.px));
writeFileSync(path.join(outDir, "jbib_diff_000_a_uni.dds"), teeDds.dds);
writeFileSync(path.join(outDir, "jbib_diff_uvprobe.png"), encodePng(SIZE, SIZE, probe));
writeFileSync(path.join(outDir, "jbib_diff_uvprobe.dds"), probeDds.dds);

// Verify the DDS the way the game would read it: header fields, exact size, and a decode of level 0 against the source.
function psnr(a, b) { let se = 0, n = 0; for (let i = 0; i < a.length; i += 4) for (let c = 0; c < 3; c++) { const d = a[i + c] - b[i + c]; se += d * d; n++; } return se === 0 ? 99 : 10 * Math.log10(255 * 255 / (se / n)); }
let expectSize = 128;
for (let s = SIZE; ; s = Math.max(1, s >> 1)) { expectSize += Math.max(1, s >> 2) ** 2 * 16; if (s === 1) break; }
const mips = 1 + Math.log2(SIZE);
const rb = decodeBC3Level(teeDds.levels[0].data, SIZE, SIZE);
const pTee = psnr(tee.px, rb);
const pProbe = psnr(probe, decodeBC3Level(probeDds.levels[0].data, SIZE, SIZE));
const checks = [
  [teeDds.dds.length === expectSize, `DDS size ${teeDds.dds.length} == expected ${expectSize}`],
  [teeDds.levels.length === mips, `${teeDds.levels.length} mip levels == ${mips}`],
  [teeDds.dds.readUInt32LE(84) === 0x35545844, "FourCC is DXT5"],
  [pTee > 40, `BC3 read-back PSNR of the tee ${pTee.toFixed(1)} dB > 40`],
  [pProbe > 28, `BC3 read-back PSNR of the probe ${pProbe.toFixed(1)} dB > 28`],
];
let bad = 0;
for (const [ok, what] of checks) { console.log(`${ok ? "PASS" : "FAIL"}  ${what}`); if (!ok) bad++; }

// preview: tee | logo crop at 1:1 (x2 nearest) | BC3 read-back crop
{
  const pw = SIZE * 2, ph = SIZE;
  const pv = Buffer.alloc(pw * ph * 4, 0);
  const put = (src, sw, sx, sy, dx, dy, w, h, scale) => {
    for (let y = 0; y < h * scale; y++) for (let x = 0; x < w * scale; x++) {
      const so = ((sy + Math.floor(y / scale)) * sw + (sx + Math.floor(x / scale))) * 4, d = ((dy + y) * pw + dx + x) * 4;
      if (dx + x < pw && dy + y < ph) { pv[d] = src[so]; pv[d + 1] = src[so + 1]; pv[d + 2] = src[so + 2]; pv[d + 3] = 255; }
    }
  };
  put(tee.px, SIZE, 0, 0, 0, 0, SIZE, SIZE, 1);
  const cw = Math.min(SIZE / 2, Math.ceil(tee.logoBox[2] * 1.4 / 2) * 2), cx = Math.max(0, Math.round(tee.logoBox[0] - tee.logoBox[2] * 0.2)), cy = Math.max(0, Math.round(tee.logoBox[1] - tee.logoBox[2] * 0.2));
  const crop = Math.min(cw, SIZE - cx, SIZE - cy), sc = Math.max(1, Math.floor((SIZE / 2) / crop));
  put(tee.px, SIZE, cx, cy, SIZE, 0, crop, crop, sc);                         // source crop, top right
  put(rb, SIZE, cx, cy, SIZE, Math.floor(SIZE / 2), crop, crop, sc);         // BC3 read-back crop, bottom right
  writeFileSync(path.join(outDir, "preview.png"), encodePng(pw, ph, pv));
}
console.log(`wrote ${outDir}`);
process.exit(bad ? 1 : 0);
