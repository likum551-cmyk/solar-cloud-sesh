// ytd_budget.mjs - texture budget for loose car-pack texture dictionaries (.ytd, RSC7 v13, GTA V Legacy PC).
//
//   node tools/packs/ytd_budget.mjs --list=<file.ytd>                      parse a .ytd and print its textures + checks
//   node tools/packs/ytd_budget.mjs --in=<a.ytd> --out=<b.ytd> [--limit-mib=15] [--max-dim=2048]
//                                                                           drop top mips until it fits; write b.ytd
//   node tools/packs/ytd_budget.mjs --selftest [--mutant=<name>[,<name>]]   synthetic round trip + rejected mutants
//
// Why: the game's loose-file path has no 0xFFFFFF big-resource marker; a model file of 16 MiB or more compressed
// crashed the game (valkyrietp.ytd, 28 MB compressed, 152 MiB of textures). A .ytd whose compressed size is >= the
// limit, or whose textures are larger than max-dim on a side, loses the top mip level(s) of its largest textures:
// width/height halve, levels drop by one, the stride is recomputed for the format and the data starts at the next
// mip. The system (virtual) segment is kept byte for byte except those texture fields, the data pointers and the
// pages-info graphics page count; the graphics (physical) segment is rebuilt compactly with new page flags.
//
// Format facts (CodeWalker.Core, github.com/dexyfex/CodeWalker, master, fetched 2026-11-05):
//   ResourceBuilder.cs   RSC7 header = magic, version, sysFlags + (version>>4 << 28), gfxFlags + (version&15 << 28);
//                        body = raw deflate of (system pages ++ graphics pages); AssignPositions2 = the page packer
//                        ported in packPhysical() (a block never straddles a page; 16-byte alignment; page counts
//                        0x7F/0x3F/0xF/3/1 for base<<0..base<<4 with base = 0x2000 << shift; 128 pages in total).
//   RpfFile.cs           RpfResourcePageFlags: base = 0x200 << flags[3:0]; page counts (largest first) at bit 4 (x1,
//                        base<<8), 5-6 (base<<7), 7-10 (base<<6), 11-16 (base<<5), 17-23 (base<<4), 24, 25, 26, 27
//                        (base<<3 .. base<<0); pages are laid out in that order.
//   ResourceData.cs      pointers 0x50000000 + offset (system) and 0x60000000 + offset (graphics).
//   ResourceFile.cs      ResourceFileBase (VFT, 1, PagesInfo pointer) and ResourcePagesInfo (16 bytes + 8 per page:
//                        +8 system page count, +9 graphics page count).
//   Texture.cs           TextureDictionary (64 bytes: name-hash list at 0x20, texture pointer list at 0x30; lists are
//                        ptr u64, count u16, capacity u16, pad); Texture legacy (144 bytes): name ptr 0x28, usage 0x40,
//                        width 0x50, height 0x52, depth 0x54, stride 0x56, format 0x58 (D3DFMT / FourCC), levels 0x5D,
//                        data ptr 0x70; TextureData is a graphics block holding the whole mip chain.
//   Utils/DDSIO.cs       ComputePitch (block-compressed: max(1,(w+3)/4) blocks x 8 or 16 bytes, rows likewise) and
//                        GetTexture's Stride = slicePitch(mip 0) / height.
//
// No dependencies (node:zlib raw deflate).

import zlib from "node:zlib";
import { readFileSync, writeFileSync, renameSync, rmSync, mkdirSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const RSC7 = 0x37435352;
export const MiB = 1048576;
export const TOC_SIZE_MARKER = 0xFFFFFF;    // RPF7 24-bit resource size; at or above it the archive needs the big marker
const SYS_BASE = 0x50000000, GFX_BASE = 0x60000000;
const TEX_LEN = 144, DICT_LEN = 64;

// Legacy (non-gen9) PC texture formats, TextureFormat enum in Texture.cs.
const FORMATS = new Map([
  [21, { name: "A8R8G8B8", bpp: 4 }], [22, { name: "X8R8G8B8", bpp: 4 }], [25, { name: "A1R5G5B5", bpp: 2 }],
  [28, { name: "A8", bpp: 1 }], [32, { name: "A8B8G8R8", bpp: 4 }], [50, { name: "L8", bpp: 1 }],
  [0x31545844, { name: "DXT1", block: 8 }], [0x33545844, { name: "DXT3", block: 16 }], [0x35545844, { name: "DXT5", block: 16 }],
  [0x31495441, { name: "ATI1", block: 8 }], [0x32495441, { name: "ATI2", block: 16 }], [0x20374342, { name: "BC7", block: 16 }],
]);
export const FMT = Object.fromEntries([...FORMATS].map(([code, f]) => [f.name, code]));

// Core-logic mutants for the selftest (each must make the selftest fail): offset, stride, levels, straddle, pagecount,
// novalidate (skip budgetYtd's own output validation, so the selftest's independent checks must catch the others).
const mutants = new Set();

// ---- format arithmetic ----------------------------------------------------------------------------
export function slicePitch(f, w, h) {
  if (f.block) return Math.max(1, (w + 3) >> 2) * f.block * Math.max(1, (h + 3) >> 2);
  return w * f.bpp * h;
}
export const expectedStride = (f, w, h) => Math.floor(slicePitch(f, w, h) / h);
// Bytes of the first `levels` mips of a w x h texture (mip i is max(1, w >> i) x max(1, h >> i)).
export function chainBytes(f, w, h, levels, depth = 1) {
  let n = 0;
  for (let i = 0; i < levels; i++) n += slicePitch(f, Math.max(1, w >> i), Math.max(1, h >> i));
  return n * depth;
}

// Jenkins one-at-a-time (joaat): the game's name hash; TextureDictionary keys are joaat(lowercase name).
export function joaat(s) {
  let h = 0;
  for (const c of Buffer.from(s.toLowerCase(), "latin1")) {
    h = (h + c) >>> 0; h = (h + (h << 10)) >>> 0; h = (h ^ (h >>> 6)) >>> 0;
  }
  h = (h + (h << 3)) >>> 0; h = (h ^ (h >>> 11)) >>> 0; h = (h + (h << 15)) >>> 0;
  return h;
}

// ---- page flags -----------------------------------------------------------------------------------
// The pages a flags word describes, largest first (CodeWalker RpfResourcePageFlags.Pages).
export function pageList(flags) {
  const base = 0x200 * 2 ** (flags & 0xF);
  const counts = [(flags >>> 4) & 1, (flags >>> 5) & 3, (flags >>> 7) & 0xF, (flags >>> 11) & 0x3F, (flags >>> 17) & 0x7F,
    (flags >>> 24) & 1, (flags >>> 25) & 1, (flags >>> 26) & 1, (flags >>> 27) & 1];
  const pages = [];
  let off = 0;
  counts.forEach((c, i) => { const size = base * 2 ** (8 - i); for (let k = 0; k < c; k++) { pages.push({ off, size }); off += size; } });
  return { pages, size: off, count: pages.length };
}

// Port of CodeWalker ResourceBuilder.AssignPositions2 for a graphics segment (no root block, legacy lengths).
// Returns block offsets (relative to the segment), the flags value (without the version nibble) and the page count.
// `align` is CodeWalker's ALIGN_SIZE (16); texture data uses 4096, the block alignment of the game-loaded dictionaries
// this tool was checked against (every page starts on a multiple of 8 KiB, so in-page alignment is absolute).
export function packPhysical(lengths, maxPageCount, align = 16) {
  const ALIGN = align, MULT = 16;
  if (!lengths.length) return { offsets: [], flags: 0, count: 0 };
  const maxBlock = Math.max(...lengths), minBlock = Math.min(...lengths);
  let baseShift = 0, baseSize = 0x2000;
  while ((baseSize < minBlock || baseSize * MULT < maxBlock) && baseShift < 0xF) { baseShift++; baseSize = 0x2000 * 2 ** baseShift; }
  if (baseSize * MULT < maxBlock) throw new Error(`a ${maxBlock}-byte block does not fit the largest page`);
  const order = lengths.map((len, i) => ({ len, i })).sort((a, b) => b.len - a.len || a.i - b.i);
  for (;;) {
    const lists = [null, null, null, null, null];
    const place = new Array(lengths.length);
    let largestI = 0, largest = baseSize;
    while (largest < maxBlock) { largestI++; largest *= 2; }
    order.forEach(({ len, i }, k) => {
      if (k === 0) { lists[largestI] = [len]; place[i] = [largestI, 0, 0]; return; }
      let psi = 0, ps = baseSize;
      while (len > ps && psi < largestI) { psi++; ps *= 2; }
      for (let ti = psi, ts = ps; ti <= largestI; ti++, ts *= 2) {
        const list = lists[ti];
        if (!list) continue;
        for (let p = 0; p < list.length; p++) {
          const o = list[p] + ((ALIGN - (list[p] % ALIGN)) % ALIGN);
          if (o + len <= ts || mutants.has("straddle")) { list[p] = o + len; place[i] = [ti, p, o]; return; }
        }
      }
      (lists[psi] ??= []).push(len);
      place[i] = [psi, lists[psi].length - 1, 0];
    });
    const counts = lists.map((l) => (l ? l.length : 0));
    const total = counts.reduce((a, b) => a + b, 0);
    const ok = total <= maxPageCount && counts[0] <= 0x7F && counts[1] <= 0x3F && counts[2] <= 0xF && counts[3] <= 3 && counts[4] <= 1;
    if (ok) {
      const pageOff = [];
      let off = 0;
      for (let i = 4; i >= 0; i--) { pageOff[i] = off; off += baseSize * 2 ** i * counts[i]; }
      const offsets = place.map(([si, p, o]) => pageOff[si] + baseSize * 2 ** si * p + o);
      const flags = ((baseShift & 0xF) | ((counts[4] & 1) << 4) | ((counts[3] & 3) << 5) | ((counts[2] & 0xF) << 7)
        | ((counts[1] & 0x3F) << 11) | ((counts[0] & 0x7F) << 17)) >>> 0;
      return { offsets, flags, count: total };
    }
    if (baseShift >= 0xF) throw new Error(`cannot pack ${lengths.length} blocks into ${maxPageCount} page(s)`);
    baseShift++; baseSize = 0x2000 * 2 ** baseShift;
  }
}

// ---- RSC7 container -------------------------------------------------------------------------------
export function openRsc(buf) {
  if (buf.length < 16 || buf.readUInt32LE(0) !== RSC7) throw new Error("not an RSC7 resource");
  const version = buf.readUInt32LE(4), sysFlags = buf.readUInt32LE(8), gfxFlags = buf.readUInt32LE(12);
  const sysPages = pageList(sysFlags), gfxPages = pageList(gfxFlags);
  let data;
  try { data = zlib.inflateRawSync(buf.subarray(16)); } catch (e) { throw new Error(`inflate failed: ${e.message}`); }
  if (data.length !== sysPages.size + gfxPages.size)
    throw new Error(`inflated ${data.length} bytes, the flags describe ${sysPages.size} + ${gfxPages.size} = ${sysPages.size + gfxPages.size}`);
  return { version, sysFlags, gfxFlags, sysPages, gfxPages, sys: data.subarray(0, sysPages.size), gfx: data.subarray(sysPages.size) };
}

// ---- independent reader ---------------------------------------------------------------------------
// Re-derives everything from the bytes (it shares only the format arithmetic with the writer): every pointer lands
// inside its segment, every block sits inside one page and overlaps no other, every mip chain's size follows from
// width/height/levels/format, the stride matches the format, the page-info counts match the flags, the name hashes
// match the names. Also measures what a rebuild would lose: nonzero graphics bytes outside texture data and system
// qwords that point into the graphics segment without being a texture's data pointer.
export function readYtd(buf) {
  const errs = [];
  const err = (m) => errs.push(m);
  let r;
  try { r = openRsc(buf); } catch (e) { return { errs: [e.message], textures: [] }; }
  const { sys, gfx } = r;
  const expectVer = (((r.sysFlags >>> 28) & 0xF) << 4) | ((r.gfxFlags >>> 28) & 0xF);
  if (r.version !== expectVer) err(`header version ${r.version} differs from the flags' version nibbles (${expectVer})`);
  if (r.version !== 13) err(`version ${r.version}, expected 13 (texture dictionary)`);
  const blocks = { sys: [], gfx: [] };
  const u64 = (o) => sys.readBigUInt64LE(o);
  const at = (seg, ptr, len, what) => {
    const base = seg === "sys" ? BigInt(SYS_BASE) : BigInt(GFX_BASE), size = BigInt(seg === "sys" ? sys.length : gfx.length);
    if (ptr < base || ptr - base + BigInt(len) > size) { err(`${what}: pointer 0x${ptr.toString(16)}+${len} is outside the ${seg === "sys" ? "system" : "graphics"} segment (${size} bytes)`); return -1; }
    const off = Number(ptr - base);
    blocks[seg].push({ off, len, what });
    return off;
  };
  if (sys.length < DICT_LEN) { err("system segment shorter than a texture dictionary"); return { errs, textures: [] }; }
  blocks.sys.push({ off: 0, len: DICT_LEN, what: "dictionary" });
  const piPtr = u64(8);
  let pagesInfo = null;
  const piOff = at("sys", piPtr, 16, "pages info");
  if (piOff >= 0) {
    const sc = sys[piOff + 8], gc = sys[piOff + 9];
    blocks.sys[blocks.sys.length - 1].len = 16 + 8 * (sc + gc);
    if (piOff + 16 + 8 * (sc + gc) > sys.length) err("pages info runs past the system segment");
    if (sc !== r.sysPages.count) err(`pages info says ${sc} system page(s), the flags ${r.sysPages.count}`);
    if (gc !== r.gfxPages.count) err(`pages info says ${gc} graphics page(s), the flags ${r.gfxPages.count}`);
    pagesInfo = { off: piOff, sysCount: sc, gfxCount: gc };
  }
  const hp = u64(0x20), hc = sys.readUInt16LE(0x28);
  const tp = u64(0x30), tc = sys.readUInt16LE(0x38), tcap = sys.readUInt16LE(0x3A);
  if (hc !== tc) err(`${hc} name hashes for ${tc} textures`);
  if (tcap < tc) err(`texture list capacity ${tcap} < count ${tc}`);
  const hOff = hc ? at("sys", hp, hc * 4, "name hashes") : -1;
  const lOff = tc ? at("sys", tp, Math.max(tc, tcap) * 8, "texture pointers") : -1;
  const textures = [];
  let prevHash = null;
  if (lOff >= 0) {
    for (let i = 0; i < tc; i++) {
      const to = at("sys", u64(lOff + i * 8), TEX_LEN, `texture ${i}`);
      if (to < 0) continue;
      const t = { i, sysOff: to, name: "", hash: hOff >= 0 && i < hc ? sys.readUInt32LE(hOff + i * 4) : null };
      const np = u64(to + 0x28);
      if (np) {
        const no = at("sys", np, 1, `texture ${i} name`);
        if (no >= 0) {
          const end = sys.indexOf(0, no);
          if (end < 0) err(`texture ${i}: name is not terminated`);
          else { t.name = sys.toString("latin1", no, end); blocks.sys[blocks.sys.length - 1].len = end - no + 1; }
        }
      } else err(`texture ${i}: no name`);
      t.width = sys.readUInt16LE(to + 0x50); t.height = sys.readUInt16LE(to + 0x52); t.depth = sys.readUInt16LE(to + 0x54);
      t.stride = sys.readUInt16LE(to + 0x56); t.format = sys.readUInt32LE(to + 0x58); t.levels = sys[to + 0x5D];
      t.usage = sys.readUInt32LE(to + 0x40); t.dataPtr = u64(to + 0x70);
      const f = FORMATS.get(t.format);
      t.fmtName = f ? f.name : `0x${t.format.toString(16)}`;
      const label = `texture ${i} (${t.name})`;
      if (!f) { err(`${label}: unknown format ${t.fmtName}`); textures.push(t); continue; }
      if (!t.width || !t.height) err(`${label}: zero width or height`);
      if (t.depth !== 1) err(`${label}: depth ${t.depth} (only 2D textures are handled)`);
      const maxLevels = 1 + Math.floor(Math.log2(Math.max(t.width, t.height, 1)));
      if (t.levels < 1 || t.levels > maxLevels) err(`${label}: ${t.levels} levels for ${t.width}x${t.height} (1..${maxLevels})`);
      if (t.stride !== expectedStride(f, t.width, t.height)) err(`${label}: stride ${t.stride}, ${t.fmtName} ${t.width}x${t.height} needs ${expectedStride(f, t.width, t.height)}`);
      t.dataLen = chainBytes(f, t.width, t.height, t.levels, t.depth);
      t.dataOff = at("gfx", t.dataPtr, t.dataLen, `${label} data`);
      if (t.hash !== null && t.name && t.hash !== joaat(t.name)) err(`${label}: name hash 0x${t.hash.toString(16)} is not joaat("${t.name.toLowerCase()}") = 0x${joaat(t.name).toString(16)}`);
      if (t.hash !== null && prevHash !== null && !(t.hash > prevHash)) err(`${label}: name hashes are not in ascending order`);
      if (t.hash !== null) prevHash = t.hash;
      textures.push(t);
    }
  }
  // Blocks: no overlap (a graphics block shared by two textures at the same offset and length is allowed), each inside
  // one page.
  for (const [seg, pages] of [["sys", r.sysPages.pages], ["gfx", r.gfxPages.pages]]) {
    const list = [...blocks[seg]].sort((a, b) => a.off - b.off || a.len - b.len);
    for (let k = 1; k < list.length; k++) {
      const a = list[k - 1], b = list[k];
      if (seg === "gfx" && a.off === b.off && a.len === b.len) continue;
      if (b.off < a.off + a.len) err(`${a.what} [${a.off}+${a.len}] overlaps ${b.what} [${b.off}+${b.len}]`);
    }
    for (const b of list) {
      const p = pages.find((pg) => b.off >= pg.off && b.off < pg.off + pg.size);
      if (!p) err(`${b.what} starts outside every page`);
      else if (b.off + b.len > p.off + p.size) err(`${b.what} [${b.off}+${b.len}] straddles the page [${p.off}+${p.size}]`);
    }
  }
  // What a rebuild of the graphics segment would drop. Real dictionaries (lp700r.ytd, valkyrietp.ytd) start every data
  // block on a 4 KiB boundary and fill the gaps and page tails with 0xCD; zero and 0xCD outside the texture data are
  // filler, anything else is data this reader does not know about.
  let unaccountedGfx = 0;
  {
    const list = [...blocks.gfx].sort((a, b) => a.off - b.off);
    let pos = 0;
    const scan = (from, to) => { for (let k = from; k < to; k++) if (gfx[k] !== 0 && gfx[k] !== 0xCD) unaccountedGfx++; };
    for (const b of list) { if (b.off > pos) scan(pos, b.off); pos = Math.max(pos, b.off + b.len); }
    scan(pos, gfx.length);
  }
  const dataPtrs = new Set(textures.map((t) => t.dataPtr));
  const strayGfxPtrs = [];
  for (let o = 0; o + 8 <= sys.length; o += 8) {
    const v = sys.readBigUInt64LE(o);
    if (v >= BigInt(GFX_BASE) && v < BigInt(GFX_BASE) + BigInt(gfx.length) && !dataPtrs.has(v)) strayGfxPtrs.push(o);
  }
  return { errs, textures, rsc: r, pagesInfo, sysBlocks: blocks.sys, unaccountedGfx, strayGfxPtrs };
}

// ---- budget -----------------------------------------------------------------------------------------
// Returns { changed: false, reason } when nothing has to change, or { changed: true, out, before, after, halved }.
// Throws when the file cannot be read safely or cannot be brought under the limit.
export function budgetYtd(buf, opts = {}) {
  const limit = opts.limit ?? 15 * MiB, maxDim = opts.maxDim ?? 2048, level = opts.level ?? 9;
  const src = readYtd(buf);
  if (src.errs.length) throw new Error(`input does not parse cleanly: ${src.errs.slice(0, 3).join("; ")}`);
  const texs = src.textures;
  const tooBig = texs.filter((t) => Math.max(t.width, t.height) > maxDim);
  if (buf.length < limit && !tooBig.length) return { changed: false, reason: `${buf.length} bytes < limit, no texture over ${maxDim}` };
  if (src.unaccountedGfx) throw new Error(`the graphics segment holds ${src.unaccountedGfx} nonzero byte(s) outside texture data; refusing to rebuild it`);
  if (src.strayGfxPtrs.length) throw new Error(`${src.strayGfxPtrs.length} system qword(s) point into the graphics segment without being texture data pointers; refusing`);
  if (!src.pagesInfo) throw new Error("no pages info");

  // A data block shared by two textures stays as it is (both keep their mips).
  const ptrUse = new Map();
  for (const t of texs) ptrUse.set(t.dataPtr, (ptrUse.get(t.dataPtr) || 0) + 1);
  const locked = (t) => ptrUse.get(t.dataPtr) > 1;
  const fmt = (t) => FORMATS.get(t.format);
  // Drop d top mips: the new top mip is (w >> d) x (h >> d); a block-compressed top mip must stay a whole number of
  // 4x4 blocks (D3D11 rejects a BC texture whose mip-0 size is not a multiple of 4).
  const canDrop = (t, d) => {
    if (locked(t) || d >= t.levels) return false;
    const w = t.width >> d, h = t.height >> d;
    if (w < 1 || h < 1) return false;
    return !(fmt(t).block && (w % 4 || h % 4));
  };
  const bytesAt = (t, d) => chainBytes(fmt(t), Math.max(1, t.width >> d), Math.max(1, t.height >> d), t.levels - d);
  const drop = texs.map(() => 0);
  for (const t of texs) while (Math.max(t.width >> drop[t.i], t.height >> drop[t.i]) > maxDim && canDrop(t, drop[t.i] + 1)) drop[t.i]++;

  // Estimate the compressed size from each texture's own deflate ratio, then build and measure for real.
  const ratio = texs.map((t) => zlib.deflateRawSync(src.rsc.gfx.subarray(t.dataOff, t.dataOff + t.dataLen), { level: 1 }).length / t.dataLen);
  const sysComp = zlib.deflateRawSync(src.rsc.sys, { level: 1 }).length + 16;
  const estimate = () => sysComp + texs.reduce((s, t) => s + ratio[t.i] * bytesAt(t, drop[t.i]), 0);
  const nextDrop = () => {
    let best = null, bestCost = -1;
    for (const t of texs) {
      if (!canDrop(t, drop[t.i] + 1)) continue;
      const cost = ratio[t.i] * bytesAt(t, drop[t.i]);
      if (cost > bestCost) { best = t; bestCost = cost; }
    }
    if (!best) return false;
    drop[best.i]++;
    return true;
  };
  while (estimate() >= limit * 0.95 && nextDrop());
  for (;;) {
    const out = buildYtd(src, drop, level);
    if (out.length < limit) {
      if (!mutants.has("novalidate")) validateBudget(src, drop, out, limit);
      const after = readYtd(out);
      return { changed: true, out, before: src, after, halved: drop.filter((d) => d > 0).length, drops: drop };
    }
    if (!nextDrop()) throw new Error(`cannot bring it under ${limit} bytes: ${out.length} bytes with every droppable mip dropped`);
  }
}

function buildYtd(src, drop, level) {
  const { rsc, textures: texs, pagesInfo } = src;
  const sys = Buffer.from(rsc.sys);
  // One graphics block per distinct data pointer.
  const blockOf = new Map(), blocks = [];
  for (const t of texs) {
    if (blockOf.has(t.dataPtr)) continue;
    const f = FORMATS.get(t.format), d = drop[t.i];
    const skip = mutants.has("offset") ? 0 : chainBytes(f, t.width, t.height, d);
    const len = chainBytes(f, Math.max(1, t.width >> d), Math.max(1, t.height >> d), t.levels - d);
    if (chainBytes(f, t.width, t.height, d) + len !== t.dataLen) throw new Error(`internal: mip tail of ${t.name} does not end where its chain does`);
    blockOf.set(t.dataPtr, blocks.length);
    blocks.push(rsc.gfx.subarray(t.dataOff + skip, t.dataOff + skip + len));
  }
  // The pages info holds 8 bytes per page: it may not grow into the next system block.
  const after = src.sysBlocks.filter((b) => b.off > pagesInfo.off).map((b) => b.off);
  const piPage = rsc.sysPages.pages.find((p) => pagesInfo.off >= p.off && pagesInfo.off < p.off + p.size);
  const roomEnd = Math.min(piPage.off + piPage.size, ...after);
  const maxTotal = Math.floor((roomEnd - pagesInfo.off - 16) / 8);
  const maxGfx = Math.min(128 - pagesInfo.sysCount, maxTotal - pagesInfo.sysCount);
  if (maxGfx < 1) throw new Error("the pages info has no room for a graphics page");
  const packed = packPhysical(blocks.map((b) => b.length), mutants.has("pagecount") ? 128 : maxGfx, 4096);
  const gfxSize = pageList(packed.flags).size;
  const gfx = Buffer.alloc(gfxSize);
  blocks.forEach((b, k) => b.copy(gfx, packed.offsets[k]));
  for (const t of texs) {
    const f = FORMATS.get(t.format), d = drop[t.i];
    const w = Math.max(1, t.width >> d), h = Math.max(1, t.height >> d);
    sys.writeUInt16LE(w, t.sysOff + 0x50);
    sys.writeUInt16LE(h, t.sysOff + 0x52);
    if (!mutants.has("stride")) sys.writeUInt16LE(expectedStride(f, w, h), t.sysOff + 0x56);
    if (!mutants.has("levels")) sys[t.sysOff + 0x5D] = t.levels - d;
    sys.writeBigUInt64LE(BigInt(GFX_BASE + packed.offsets[blockOf.get(t.dataPtr)]), t.sysOff + 0x70);
  }
  sys[pagesInfo.off + 9] = packed.count;
  sys.fill(0, pagesInfo.off + 16 + 8 * pagesInfo.sysCount, pagesInfo.off + 16 + 8 * (pagesInfo.sysCount + Math.max(packed.count, pagesInfo.gfxCount)));
  const head = Buffer.alloc(16);
  head.writeUInt32LE(RSC7, 0);
  head.writeUInt32LE(rsc.version, 4);
  head.writeUInt32LE(rsc.sysFlags, 8);
  head.writeUInt32LE(((rsc.gfxFlags & 0xF0000000) | packed.flags) >>> 0, 12);
  return Buffer.concat([head, zlib.deflateRawSync(Buffer.concat([sys, gfx]), { level })]);
}

// The output must parse cleanly with the independent reader, keep every texture (same names, formats, order), carry
// exactly the expected mip tail of each texture's data, leave nothing unaccounted, and fit the limit.
function validateBudget(src, drop, out, limit) {
  const r = readYtd(out);
  const fail = (m) => { throw new Error(`rebuilt dictionary failed validation: ${m}`); };
  if (r.errs.length) fail(r.errs.slice(0, 3).join("; "));
  if (out.length >= limit) fail(`${out.length} bytes >= limit ${limit}`);
  if (r.textures.length !== src.textures.length) fail("texture count changed");
  if (r.unaccountedGfx) fail(`${r.unaccountedGfx} graphics byte(s) outside texture data`);
  for (const t of src.textures) {
    const n = r.textures[t.i], f = FORMATS.get(t.format), d = drop[t.i];
    if (n.name !== t.name || n.format !== t.format || n.hash !== t.hash) fail(`texture ${t.i} identity changed`);
    if (n.width !== Math.max(1, t.width >> d) || n.height !== Math.max(1, t.height >> d) || n.levels !== t.levels - d) fail(`texture ${t.name} has the wrong size`);
    const skip = chainBytes(f, t.width, t.height, d);
    const want = src.rsc.gfx.subarray(t.dataOff + skip, t.dataOff + t.dataLen);
    if (!r.rsc.gfx.subarray(n.dataOff, n.dataOff + n.dataLen).equals(want)) fail(`texture ${t.name} data is not its mip tail`);
    // every system byte other than the patched fields is unchanged
  }
  const patched = new Set();
  for (const t of src.textures) for (const o of [0x50, 0x51, 0x52, 0x53, 0x56, 0x57, 0x5D, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77]) patched.add(t.sysOff + o);
  patched.add(src.pagesInfo.off + 9);
  // the page-pointer slots (runtime-filled, zero in the file) of the larger of the old and the new page count
  const piSlots = [src.pagesInfo.off + 16, src.pagesInfo.off + 16 + 8 * (src.pagesInfo.sysCount + Math.max(src.pagesInfo.gfxCount, r.pagesInfo.gfxCount))];
  const a = src.rsc.sys, b = r.rsc.sys;
  if (a.length !== b.length) fail("system segment size changed");
  for (let k = 0; k < a.length; k++) if (a[k] !== b[k] && !patched.has(k) && !(k >= piSlots[0] && k < piSlots[1])) fail(`system byte ${k} changed`);
}

// ---- reporting ------------------------------------------------------------------------------------
const mb = (n) => (n / 1e6).toFixed(1);
const hex = (n) => `0x${(n >>> 0).toString(16).padStart(8, "0")}`;
export function describe(r) {
  const lines = [];
  for (const t of r.textures) lines.push(`    ${t.name.padEnd(32)} ${String(t.fmtName).padEnd(8)} ${`${t.width}x${t.height}`.padStart(9)}  levels ${String(t.levels).padStart(2)}  ${String(t.dataLen ?? "?").padStart(9)} B`);
  return lines.join("\n");
}
export function summary(buf, r) {
  return `${buf.length} B compressed, ${r.rsc ? r.rsc.sys.length + r.rsc.gfx.length : "?"} B inflated (sys ${r.rsc?.sys.length} + gfx ${r.rsc?.gfx.length}), flags sys ${hex(r.rsc?.sysFlags)} gfx ${hex(r.rsc?.gfxFlags)}, ${r.textures.length} textures, pages ${r.pagesInfo ? `${r.pagesInfo.sysCount}+${r.pagesInfo.gfxCount}` : "?"}`;
}

// Budget a file in place (atomic: temp file + rename). Returns null when unchanged, else { before, after, halved }.
export function budgetFile(file, opts = {}) {
  const buf = readFileSync(file);
  const res = budgetYtd(buf, opts);
  if (!res.changed) return null;
  const tmp = file + ".budget.tmp";
  writeFileSync(tmp, res.out);
  renameSync(tmp, file);
  return { beforeBytes: buf.length, afterBytes: res.out.length, halved: res.halved, before: res.before, after: res.after };
}

// ---- selftest -----------------------------------------------------------------------------------------
// Builds texture dictionaries with its own naive layout (one system page, one graphics page, blocks in order), so
// the reader and the budget are checked against bytes the budget's writer did not lay out.
function prng(seed) { let x = seed >>> 0 || 1; return () => { x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; return x; }; }
function fixture(specs, { roomyPagesInfo = false, seed = 7 } = {}) {
  const rnd = prng(seed);
  const sorted = specs.map((s) => ({ ...s, hash: joaat(s.name) })).sort((a, b) => a.hash - b.hash);
  const n = sorted.length;
  const al = (x) => (x + 15) & ~15;
  const piLen = roomyPagesInfo ? 16 + 8 * 128 : 16 + 8 * 2;
  let o = DICT_LEN;
  const piOff = o; o = al(o + piLen);
  const hashOff = o; o = al(o + 4 * n);
  const listOff = o; o = al(o + 8 * n);
  const texOff = []; for (let i = 0; i < n; i++) { texOff.push(o); o = al(o + TEX_LEN); }
  const nameOff = []; for (const s of sorted) { nameOff.push(o); o = al(o + s.name.length + 1); }
  if (o > 0x2000) throw new Error("fixture: system data exceeds one page");
  const sys = Buffer.alloc(0x2000);
  const dataOff = [];
  let g = 0;
  for (const s of sorted) { dataOff.push(g); s.len = chainBytes(FORMATS.get(s.fmt), s.w, s.h, s.levels); g = al(g + s.len); }
  let shift = 0; while (0x2000 * 2 ** shift < g) shift++;
  const gfx = Buffer.alloc(0x2000 * 2 ** shift);
  sorted.forEach((s, i) => {
    const data = gfx.subarray(dataOff[i], dataOff[i] + s.len);
    if (s.fill === "zero") data.fill(0x11);
    else for (let k = 0; k + 4 <= data.length; k += 4) data.writeUInt32LE(rnd(), k);
    // mark each mip's first bytes with its level so a misplaced mip tail is visible
    const f = FORMATS.get(s.fmt);
    for (let lv = 0, off = 0; lv < s.levels; lv++) { data[off] = 0xA0 + lv; off += slicePitch(f, Math.max(1, s.w >> lv), Math.max(1, s.h >> lv)); }
  });
  sys.writeUInt32LE(0x40B5A5A0, 0); sys.writeUInt32LE(1, 4); sys.writeBigUInt64LE(BigInt(SYS_BASE + piOff), 8); sys.writeUInt32LE(1, 0x18);
  sys.writeBigUInt64LE(BigInt(SYS_BASE + hashOff), 0x20); sys.writeUInt16LE(n, 0x28); sys.writeUInt16LE(n, 0x2A);
  sys.writeBigUInt64LE(BigInt(SYS_BASE + listOff), 0x30); sys.writeUInt16LE(n, 0x38); sys.writeUInt16LE(n, 0x3A);
  sys[piOff + 8] = 1; sys[piOff + 9] = 1;
  sorted.forEach((s, i) => {
    const f = FORMATS.get(s.fmt), t = texOff[i];
    sys.writeUInt32LE(s.hash, hashOff + 4 * i);
    sys.writeBigUInt64LE(BigInt(SYS_BASE + t), listOff + 8 * i);
    sys.writeUInt32LE(0x40B5A5B0, t); sys.writeUInt32LE(1, t + 4);
    sys.writeBigUInt64LE(BigInt(SYS_BASE + nameOff[i]), t + 0x28); sys.writeUInt16LE(1, t + 0x30);
    sys.writeUInt32LE(0x00800014, t + 0x40);
    sys.writeUInt16LE(s.w, t + 0x50); sys.writeUInt16LE(s.h, t + 0x52); sys.writeUInt16LE(1, t + 0x54);
    sys.writeUInt16LE(expectedStride(f, s.w, s.h), t + 0x56); sys.writeUInt32LE(s.fmt, t + 0x58); sys[t + 0x5D] = s.levels;
    sys.writeBigUInt64LE(BigInt(GFX_BASE + dataOff[i]), t + 0x70);
    sys.write(s.name + "\0", nameOff[i], "latin1");
  });
  const head = Buffer.alloc(16);
  head.writeUInt32LE(RSC7, 0); head.writeUInt32LE(13, 4);
  head.writeUInt32LE((1 << 17) >>> 0, 8);
  head.writeUInt32LE(((0xD << 28) | (1 << 17) | shift) >>> 0, 12);
  return Buffer.concat([head, zlib.deflateRawSync(Buffer.concat([sys, gfx]), { level: 1 })]);
}

// Rewrite a resource's inflated body with `fn(sys, gfx, header)` (for reader mutants).
function mutateRsc(buf, fn) {
  const r = openRsc(buf);
  const sys = Buffer.from(r.sys), gfx = Buffer.from(r.gfx), head = Buffer.from(buf.subarray(0, 16));
  fn(sys, gfx, head);
  return Buffer.concat([head, zlib.deflateRawSync(Buffer.concat([sys, gfx]), { level: 1 })]);
}

function selftest() {
  let bad = 0;
  const check = (ok, what) => { console.log(`${ok ? "PASS" : "FAIL"}  ${what}`); if (!ok) bad++; };
  const guard = (what, fn) => { try { return fn(); } catch (e) { check(false, `${what}: threw ${e.message}`); return null; } };

  // format arithmetic against hand-computed sizes
  // 512..1: 131072 32768 8192 2048 512 128 32 8 8 8 (the 4x4, 2x2 and 1x1 mips are one 8-byte block each)
  check(chainBytes(FORMATS.get(FMT.DXT1), 512, 512, 10) === 174776, "DXT1 512x512 x10 chain = 174776 bytes");
  check(chainBytes(FORMATS.get(FMT.DXT5), 4, 4, 1) === 16 && chainBytes(FORMATS.get(FMT.DXT5), 1, 1, 1) === 16, "DXT5 4x4 and 1x1 are one 16-byte block");
  check(expectedStride(FORMATS.get(FMT.DXT5), 1024, 1024) === 1024 && expectedStride(FORMATS.get(FMT.A8R8G8B8), 256, 64) === 1024, "stride = slice pitch / height (DDSIO.GetTexture)");
  check(joaat("") === 0 && joaat("a") === 0xCA2E9442, "joaat(\"a\") = 0xca2e9442");
  check(pageList(0xd308000c).size === 159383552 && pageList(0x20000).size === 8192, "page flags 0xd308000c = 152 MiB, 0x20000 = 8 KiB (valkyrietp.ytd)");

  const specs = [
    { name: "big_diffuse", fmt: FMT.DXT5, w: 2048, h: 2048, levels: 12 },
    { name: "Wide_Normal", fmt: FMT.BC7, w: 2048, h: 1024, levels: 12 },
    { name: "odd_dxt1", fmt: FMT.DXT1, w: 1000, h: 1000, levels: 10 },
    { name: "small_rgba", fmt: FMT.A8R8G8B8, w: 128, h: 64, levels: 8 },
    { name: "flat_spec", fmt: FMT.DXT1, w: 512, h: 512, levels: 10, fill: "zero" },
  ];
  for (const roomy of [true, false]) {
    const tag = roomy ? "roomy pages info" : "tight pages info";
    const inBuf = fixture(specs, { roomyPagesInfo: roomy });
    const src = readYtd(inBuf);
    check(src.errs.length === 0 && src.textures.length === 5, `[${tag}] reader accepts the fixture (${src.errs[0] ?? "5 textures"})`);
    const limit = 3 * MiB, maxDim = 1024;
    check(inBuf.length >= limit, `[${tag}] fixture is over the limit (${inBuf.length} >= ${limit})`);
    const res = guard(`[${tag}] budget`, () => budgetYtd(inBuf, { limit, maxDim, level: 6 }));
    if (!res) continue;
    const out = res.out, r = readYtd(out);
    check(res.changed && r.errs.length === 0, `[${tag}] output parses with the independent reader (${r.errs[0] ?? "clean"})`);
    check(out.length < limit, `[${tag}] output ${out.length} < limit ${limit}`);
    check(r.rsc && r.rsc.sys.length + r.rsc.gfx.length === pageList(out.readUInt32LE(8)).size + pageList(out.readUInt32LE(12)).size, `[${tag}] inflated size equals the size the new flags describe`);
    check(r.unaccountedGfx === 0 && r.strayGfxPtrs.length === 0, `[${tag}] no graphics byte or pointer unaccounted`);
    check(r.pagesInfo && r.pagesInfo.gfxCount === pageList(out.readUInt32LE(12)).count, `[${tag}] pages info graphics count = flags page count (${r.pagesInfo?.gfxCount})`);
    if (!roomy) check(r.pagesInfo?.gfxCount === 1, "[tight pages info] packer kept to the 1 page the pages info has room for");
    // Independent data check: each texture's new data must be the input's chain from mip d on, where d is
    // log2(old width / new width); recomputed here from the fixture spec, not from the budget's plan.
    const byName = new Map(r.textures.map((t) => [t.name, t]));
    for (const t of src.textures) {
      const n = byName.get(t.name);
      if (!n) { check(false, `[${tag}] ${t.name} kept`); continue; }
      const d = Math.round(Math.log2(t.width / n.width));
      const f = FORMATS.get(t.format);
      let skip = 0; for (let lv = 0; lv < d; lv++) skip += slicePitch(f, Math.max(1, t.width >> lv), Math.max(1, t.height >> lv));
      const want = src.rsc.gfx.subarray(t.dataOff + skip, t.dataOff + t.dataLen);
      const got = r.rsc.gfx.subarray(n.dataOff, n.dataOff + n.dataLen);
      check(n.levels === t.levels - d && n.height === t.height >> d && got.equals(want) && got[0] === 0xA0 + d,
        `[${tag}] ${t.name}: ${t.width}x${t.height}/${t.levels} -> ${n.width}x${n.height}/${n.levels}, data = mip ${d}.. of the input`);
    }
    const dims = Object.fromEntries(r.textures.map((t) => [t.name, `${t.width}x${t.height}`]));
    check(dims.big_diffuse === "1024x1024" || dims.big_diffuse === "512x512", `[${tag}] 2048 DXT5 capped to max-dim (${dims.big_diffuse})`);
    check(dims.odd_dxt1 === "1000x1000" || dims.odd_dxt1 === "500x500", `[${tag}] 1000x1000 DXT1 never goes below a multiple of 4 (${dims.odd_dxt1})`);
    check(dims.small_rgba === "128x64", `[${tag}] small texture untouched (${dims.small_rgba})`);
    const again = guard(`[${tag}] second run`, () => budgetYtd(out, { limit, maxDim }));
    check(again && again.changed === false, `[${tag}] budget is idempotent (second run: ${again?.reason ?? "changed"})`);

    if (!roomy) continue;
    // Reader mutants: each damaged copy of the budget's output must be rejected.
    const t0 = r.textures.find((t) => t.name === "big_diffuse");
    const tw = r.textures.find((t) => t.name === "Wide_Normal");
    const rej = (what, fn) => { const m = readYtd(mutateRsc(out, fn)); check(m.errs.length > 0, `reader rejects: ${what} (${m.errs[0] ?? "no complaint"})`); };
    rej("data pointer past the graphics segment", (s, g) => s.writeBigUInt64LE(BigInt(GFX_BASE + g.length), t0.sysOff + 0x70));
    rej("levels + 1 (chain longer than the data block)", (s) => { s[t0.sysOff + 0x5D] = t0.levels + 1; });
    rej("width doubled with the old data", (s) => s.writeUInt16LE(t0.width * 2, t0.sysOff + 0x50));
    rej("stale stride", (s) => s.writeUInt16LE(t0.stride * 2, t0.sysOff + 0x56));
    rej("pages info graphics count off by one", (s) => { s[r.pagesInfo.off + 9]++; });
    rej("name hash not joaat(name)", (s) => s.writeUInt32LE(0x12345678, s.readUInt32LE(0x20) - SYS_BASE + 0));
    rej("texture pointer outside the system segment", (s) => s.writeBigUInt64LE(BigInt(SYS_BASE + 0x10000000), Number(s.readBigUInt64LE(0x30) - BigInt(SYS_BASE))));
    rej("unknown format", (s) => s.writeUInt32LE(0x99, t0.sysOff + 0x58));
    {
      const pages = r.rsc.gfxPages.pages;
      if (pages.length > 1) rej("a data block moved to straddle a page boundary", (s) => s.writeBigUInt64LE(BigInt(GFX_BASE + pages[1].off - 16), tw.sysOff + 0x70));
      else check(false, "fixture output has more than one graphics page (needed for the straddle mutant)");
    }
    {
      const h = Buffer.from(out); h.writeUInt32LE((h.readUInt32LE(12) + (1 << 17)) >>> 0, 12);
      const m = readYtd(h); check(m.errs.length > 0, `reader rejects: graphics flags one page bigger than the data (${m.errs[0] ?? "no complaint"})`);
    }
    {
      const m = readYtd(mutateRsc(out, (s, g) => { g[g.length - 1] = 1; }));
      check(m.errs.length === 0 && m.unaccountedGfx === 1, "reader counts a stray graphics byte outside texture data");
      let threw = null; try { budgetYtd(mutateRsc(fixture(specs, { roomyPagesInfo: true }), (s, g) => { g[g.length - 1] = 1; }), { limit, maxDim }); } catch (e) { threw = e.message; }
      check(!!threw && /outside texture data/.test(threw), `budget refuses a graphics segment with unaccounted bytes (${threw ?? "did not refuse"})`);
    }
  }
  // no-op below the limit; refusal when it cannot fit
  {
    const small = fixture([{ name: "tiny", fmt: FMT.DXT1, w: 256, h: 256, levels: 9 }]);
    const res = guard("no-op", () => budgetYtd(small, { limit: 15 * MiB, maxDim: 2048 }));
    check(res && res.changed === false, `a small dictionary is left alone (${res?.reason})`);
    let threw = null; try { budgetYtd(fixture([{ name: "dense", fmt: FMT.BC7, w: 512, h: 512, levels: 1 }]), { limit: 64 * 1024, maxDim: 2048 }); } catch (e) { threw = e.message; }
    check(!!threw && /cannot bring it under/.test(threw), `a dictionary that cannot fit is refused (${threw ?? "did not throw"})`);
    const odd = guard("odd size", () => budgetYtd(fixture([{ name: "odd_dxt1", fmt: FMT.DXT1, w: 1000, h: 1000, levels: 10 }]), { limit: 15 * MiB, maxDim: 256 }));
    const ot = odd?.changed ? readYtd(odd.out).textures[0] : null;
    check(ot && ot.width === 500 && ot.height === 500 && ot.levels === 9, `max-dim 256 on a 1000x1000 DXT1 stops at 500x500 (250 is not a whole number of 4x4 blocks): ${ot ? `${ot.width}x${ot.height}/${ot.levels}` : "unchanged"}`);
  }
  console.log(bad ? `FAILED (${bad})` : "ALL PASS");
  return bad;
}

// ---- main -------------------------------------------------------------------------------------------
const isMain = process.argv[1] && path.resolve(process.argv[1]).toLowerCase() === fileURLToPath(import.meta.url).toLowerCase();
if (isMain) {
  const args = Object.fromEntries(process.argv.slice(2).map((a) => {
    const m = /^--([^=]+)(?:=(.*))?$/.exec(a);
    if (!m) { console.error(`bad argument ${a}`); process.exit(2); }
    return [m[1], m[2] ?? true];
  }));
  const known = new Set(["selftest", "mutant", "list", "in", "out", "limit-mib", "max-dim"]);
  for (const k of Object.keys(args)) if (!known.has(k)) { console.error(`unknown option --${k}`); process.exit(2); }
  if (args.mutant) {
    const known = new Set(["offset", "stride", "levels", "straddle", "pagecount", "novalidate"]);
    for (const m of String(args.mutant).split(",")) { if (!known.has(m)) { console.error(`unknown mutant ${m}`); process.exit(2); } mutants.add(m); }
    console.log(`MUTANTS ACTIVE: ${[...mutants].join(", ")}`);
  }
  const opts = { limit: Number(args["limit-mib"] ?? 15) * MiB, maxDim: Number(args["max-dim"] ?? 2048) };
  if (!(opts.limit > 0) || !(opts.maxDim >= 4)) { console.error("bad --limit-mib or --max-dim"); process.exit(2); }
  if (args.selftest) process.exit(selftest() ? 1 : 0);
  if (args.list) {
    const buf = readFileSync(path.resolve(String(args.list)));
    const r = readYtd(buf);
    console.log(`${path.basename(String(args.list))}: ${summary(buf, r)}`);
    console.log(describe(r));
    if (r.unaccountedGfx) console.log(`  note: ${r.unaccountedGfx} nonzero graphics byte(s) outside texture data`);
    if (r.strayGfxPtrs?.length) console.log(`  note: ${r.strayGfxPtrs.length} system qword(s) point into graphics outside texture data`);
    for (const e of r.errs) console.error(`PROBLEM ${e}`);
    console.log(r.errs.length ? `${r.errs.length} problem(s)` : "structure OK");
    process.exit(r.errs.length ? 1 : 0);
  }
  if (args.in) {
    if (!args.out) { console.error("--in needs --out"); process.exit(2); }
    const inPath = path.resolve(String(args.in)), outPath = path.resolve(String(args.out));
    if (inPath.toLowerCase() === outPath.toLowerCase()) { console.error("--out must differ from --in"); process.exit(2); }
    const buf = readFileSync(inPath);
    let res;
    try { res = budgetYtd(buf, opts); } catch (e) { console.error(`! ${e.message}`); process.exit(1); }
    if (!res.changed) { console.log(`unchanged: ${res.reason}`); process.exit(0); }
    console.log(`before: ${summary(buf, res.before)}`);
    console.log(describe(res.before));
    console.log(`after:  ${summary(res.out, res.after)}`);
    console.log(describe(res.after));
    mkdirSync(path.dirname(outPath), { recursive: true });
    writeFileSync(outPath + ".tmp", res.out);
    rmSync(outPath, { force: true });
    renameSync(outPath + ".tmp", outPath);
    console.log(`${path.basename(inPath)} ${mb(buf.length)} MB -> ${mb(res.out.length)} MB (${res.halved} texture(s) reduced) -> ${outPath}`);
    process.exit(0);
  }
  console.error("nothing to do: --selftest, --list=<ytd> or --in=<ytd> --out=<ytd>");
  process.exit(2);
}
