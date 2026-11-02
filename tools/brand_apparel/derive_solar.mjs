// derive_solar.mjs - rasterise the Solar RP NOON brand kit (brand/solar/solar-rp-noon/*.svg) into the PNGs the
// in-game side embeds, into brand/solar/derived/. Nothing else under brand/ is read for writing or touched.
//
//   node derive_solar.mjs [--out=<dir>] [--only=<name>]      (Node 22; no dependencies)
//
// Outputs (transparent PNG, straight alpha, the kit's own shapes, no resampling of a raster):
//   solar-rp-mark-full-64.png   32 x 64      overlay corner mark, small      (ink + one gold hook, for #0a0907 plates)
//   solar-rp-mark-full-256.png  128 x 256    overlay corner mark, large
//   solar-rp-mark-ink-1024.png  512 x 1024   overlay loader backdrop mark + the tee's front chest decal source
//   solar-rp-lockup-horizontal-full-256h.png  1038 x 256   overlay loader lockup (ink + one gold hook), 4 units of clear space
//   solar-rp-lockup-horizontal-ink-256h.png   1038 x 256   the tee's back decal source (single colour; make_decals crops to the alpha box)
// The mark is 1:2 (viewBox 50 x 100), so its textures are 1:2 as well: the overlay draws them at that aspect.
//
// The SVGs are plain polygon paths (M L H V Z, absolute) under translate()/scale() groups, with fill and
// fill-rule on the path or an ancestor. The rasteriser fills them with an exact horizontal span coverage and
// 16 sub-scanlines per pixel (the same antialiasing quality as a browser), compositing paths in paint order.
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { encodePng } from "./png.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..", "..");
const kit = path.join(repoRoot, "brand", "solar", "solar-rp-noon");
const args = Object.fromEntries(process.argv.slice(2).map((a) => {
  const m = /^--([^=]+)=(.*)$/.exec(a);
  if (!m) { console.error(`bad argument: ${a}`); process.exit(2); }
  return [m[1], m[2]];
}));
const outDir = path.resolve(args.out ?? path.join(repoRoot, "brand", "solar", "derived"));
// Refuse to write anywhere under brand/ except brand/solar/derived (the owner's rule: the kit and the retired art stay as they are).
{
  const brand = path.join(repoRoot, "brand") + path.sep;
  const derived = path.join(repoRoot, "brand", "solar", "derived");
  const rel = path.relative(derived, outDir);
  if ((outDir + path.sep).startsWith(brand) && (rel.startsWith("..") || path.isAbsolute(rel))) {
    console.error(`refusing to write into ${outDir}: under brand/ only brand/solar/derived is allowed`);
    process.exit(2);
  }
}

// ---- SVG subset -> filled polygons ---------------------------------------------------------------------
function parseAttrs(s) {
  const o = {};
  for (const m of s.matchAll(/([\w:-]+)\s*=\s*"([^"]*)"/g)) o[m[1]] = m[2];
  return o;
}
// translate(x y) / scale(s) / scale(sx sy) chains -> [a, d, e, f] (x' = a x + e, y' = d y + f); anything else throws.
function parseTransform(t) {
  let a = 1, d = 1, e = 0, f = 0;
  if (!t) return [a, d, e, f];
  let left = t;
  for (const m of t.matchAll(/(translate|scale)\(([^)]*)\)/g)) {
    const n = m[2].trim().split(/[\s,]+/).map(Number);
    if (n.some((v) => !Number.isFinite(v))) throw new Error(`bad transform ${t}`);
    // compose: current * next, where next is applied first (SVG order, left to right = outer to inner)
    if (m[1] === "translate") { e += a * n[0]; f += d * (n[1] ?? 0); }
    else { a *= n[0]; d *= n.length > 1 ? n[1] : n[0]; }
    left = left.replace(m[0], "");
  }
  if (left.trim() !== "") throw new Error(`unsupported transform ${t}`);
  return [a, d, e, f];
}
const compose = (p, c) => [p[0] * c[0], p[1] * c[1], p[0] * c[2] + p[2], p[1] * c[3] + p[3]];
const apply = (m, x, y) => [m[0] * x + m[2], m[1] * y + m[3]];
function parseHex(h) {
  const m = /^#([0-9a-f]{6})$/i.exec(h ?? "");
  if (!m) throw new Error(`unsupported fill ${h}`);
  const v = parseInt(m[1], 16);
  return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
}
function parsePath(d) {
  const toks = [...d.matchAll(/([MLHVZ])|(-?\d*\.?\d+(?:e-?\d+)?)/gi)];
  const subs = [];
  let cur = null, x = 0, y = 0, cmd = null, i = 0;
  const num = () => { const t = toks[i++]; if (!t || t[2] === undefined) throw new Error(`path ${d}: number expected`); return Number(t[2]); };
  while (i < toks.length) {
    if (toks[i][1]) { cmd = toks[i][1]; i++; if (cmd === "Z") { if (cur) { subs.push(cur); cur = null; } continue; } }
    else if (!cmd) throw new Error(`path ${d}: no command`);
    if (cmd === "M") { if (cur) subs.push(cur); x = num(); y = num(); cur = [[x, y]]; cmd = "L"; }
    else if (cmd === "L") { x = num(); y = num(); if (!cur) throw new Error("L before M"); cur.push([x, y]); }
    else if (cmd === "H") { x = num(); cur.push([x, y]); }
    else if (cmd === "V") { y = num(); cur.push([x, y]); }
    else throw new Error(`path ${d}: unsupported command ${cmd}`);
  }
  if (cur) subs.push(cur);
  return subs;
}
function parseSvg(text) {
  const vb = /viewBox="([^"]+)"/.exec(text)[1].split(/\s+/).map(Number);
  const items = [];
  const stack = [{ m: [1, 1, 0, 0], fill: null, rule: "nonzero" }];
  for (const t of text.matchAll(/<(\/?)(g|path)\b([^>]*?)(\/?)>/g)) {
    const [, close, tag, attrText, selfClose] = t;
    if (tag === "g") {
      if (close) { stack.pop(); continue; }
      const a = parseAttrs(attrText), top = stack[stack.length - 1];
      stack.push({ m: compose(top.m, parseTransform(a.transform)), fill: a.fill ?? top.fill, rule: a["fill-rule"] ?? top.rule });
      if (selfClose) stack.pop();
    } else {
      const a = parseAttrs(attrText), top = stack[stack.length - 1];
      const m = compose(top.m, parseTransform(a.transform));
      const fill = a.fill ?? top.fill;
      if (!fill) throw new Error("path without fill");
      items.push({ polys: parsePath(a.d).map((s) => s.map(([px, py]) => apply(m, px, py))), fill: parseHex(fill), rule: a["fill-rule"] ?? top.rule });
    }
  }
  if (stack.length !== 1) throw new Error("unbalanced <g>");
  return { vb, items };
}

// ---- rasteriser ---------------------------------------------------------------------------------------------
const SUB = 16;
function coverage(polys, rule, W, H, sx, sy, ox, oy) {
  const cov = new Float32Array(W * H);
  const edges = [];
  for (const poly of polys) {
    for (let i = 0; i < poly.length; i++) {
      const [x0, y0] = poly[i], [x1, y1] = poly[(i + 1) % poly.length];
      const ax = (x0 - ox) * sx, ay = (y0 - oy) * sy, bx = (x1 - ox) * sx, by = (y1 - oy) * sy;
      if (ay === by) continue;
      edges.push(ay < by ? { y0: ay, y1: by, x: ax, dx: (bx - ax) / (by - ay), dir: 1 } : { y0: by, y1: ay, x: bx, dx: (ax - bx) / (ay - by), dir: -1 });
    }
  }
  const xs = [];
  for (let py = 0; py < H; py++) {
    for (let k = 0; k < SUB; k++) {
      const yy = py + (k + 0.5) / SUB;
      xs.length = 0;
      for (const e of edges) if (yy >= e.y0 && yy < e.y1) xs.push([e.x + (yy - e.y0) * e.dx, e.dir]);
      xs.sort((p, q) => p[0] - q[0]);
      let wind = 0;
      for (let j = 0; j < xs.length - 1; j++) {
        wind += rule === "evenodd" ? 1 : xs[j][1];
        const inside = rule === "evenodd" ? (wind & 1) === 1 : wind !== 0;
        if (!inside) continue;
        const xa = Math.max(0, xs[j][0]), xb = Math.min(W, xs[j + 1][0]);
        if (xb <= xa) continue;
        const ia = Math.floor(xa), ib = Math.min(W - 1, Math.floor(xb));
        for (let x = ia; x <= ib; x++) cov[py * W + x] += (Math.min(xb, x + 1) - Math.max(xa, x)) / SUB;
      }
    }
  }
  return cov;
}
// padUnits: transparent clear space added on every side, in the SVG's own units (the canvas is W x H pixels).
export function rasterise(svgText, W, H, padUnits = 0) {
  const parsed = parseSvg(svgText);
  const items = parsed.items;
  const vb = [parsed.vb[0] - padUnits, parsed.vb[1] - padUnits, parsed.vb[2] + 2 * padUnits, parsed.vb[3] + 2 * padUnits];
  const sx = W / vb[2], sy = H / vb[3];
  const premul = new Float32Array(W * H * 4);        // r g b (premultiplied, 0..255) a (0..1)
  for (const it of items) {
    const cov = coverage(it.polys, it.rule, W, H, sx, sy, vb[0], vb[1]);
    for (let i = 0; i < W * H; i++) {
      const c = Math.min(1, cov[i]);
      if (c <= 0) continue;
      for (let k = 0; k < 3; k++) premul[i * 4 + k] = it.fill[k] * c + premul[i * 4 + k] * (1 - c);
      premul[i * 4 + 3] = c + premul[i * 4 + 3] * (1 - c);
    }
  }
  const last = items[items.length - 1].fill;
  const out = Buffer.alloc(W * H * 4);
  for (let i = 0; i < W * H; i++) {
    const a = premul[i * 4 + 3];
    for (let k = 0; k < 3; k++) out[i * 4 + k] = a > 0 ? Math.max(0, Math.min(255, Math.round(premul[i * 4 + k] / a))) : last[k];
    out[i * 4 + 3] = Math.round(a * 255);
  }
  return out;
}

// ---- jobs ----------------------------------------------------------------------------------------------------
const JOBS = [
  { name: "solar-rp-mark-full-64", svg: "solar-rp-mark-noon-full.svg", h: 64 },
  { name: "solar-rp-mark-full-256", svg: "solar-rp-mark-noon-full.svg", h: 256 },
  { name: "solar-rp-mark-ink-1024", svg: "solar-rp-mark-noon-ink.svg", h: 1024 },
  // The lockups carry 4 units of clear space on every side (the overlay's loader lays its tagline and progress row
  // out against the artwork's 4 px design inset inside the image, as it did with the retired art: 4 / 108 of 112 px).
  { name: "solar-rp-lockup-horizontal-full-256h", svg: "solar-rp-horizontal-noon-full.svg", h: 256, pad: 4 },
  { name: "solar-rp-lockup-horizontal-ink-256h", svg: "solar-rp-horizontal-noon-ink.svg", h: 256, pad: 4 },
];
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  mkdirSync(outDir, { recursive: true });
  let n = 0;
  for (const j of JOBS) {
    if (args.only && args.only !== j.name) continue;
    const text = readFileSync(path.join(kit, j.svg), "utf8");
    const vb = /viewBox="([^"]+)"/.exec(text)[1].split(/\s+/).map(Number);
    const pad = j.pad ?? 0;
    const W = Math.round(j.h * (vb[2] + 2 * pad) / (vb[3] + 2 * pad));
    const px = rasterise(text, W, j.h, pad);
    writeFileSync(path.join(outDir, j.name + ".png"), encodePng(W, j.h, px));
    console.error(`${j.name}.png  ${W} x ${j.h}  from ${j.svg}${pad ? `, ${pad} units clear space` : ""}`);
    n++;
  }
  if (n === 0) { console.error("nothing written"); process.exit(1); }
}
