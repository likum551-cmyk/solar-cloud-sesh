// tail_hash.mjs - print the brand_apparel fingerprint of a raw texture mip chain.
//
//   node tail_hash.mjs <chain.bin> [width=512] [bytesPerBlock=8]
//
// <chain.bin> is every mip of the texture back to back, largest first (BC1: 8 bytes per 4x4 block, BC3: 16), as
// tests/brand_apparel_test.cpp and the scratch dump tools write it. The fingerprint is FNV-1a 64 over the bytes of every
// mip whose width is <= 64, the same value brand_apparel.cpp computes at run time from a GPU read-back (a tail-only hash
// survives a texture-quality setting that drops the top mips). Use it to add another stock variant to kTargets in
// brand_apparel_data.inc (make_decals.mjs --tail-hash=<value>). Expected for the stock Sky Blue Tee: 67964881c72ba99d.
import { readFileSync } from "node:fs";

const [file, widthArg, bpbArg] = process.argv.slice(2);
if (!file) { console.error("usage: tail_hash.mjs <chain.bin> [width=512] [bytesPerBlock=8]"); process.exit(2); }
const width = Number(widthArg || 512), bpb = Number(bpbArg || 8);
if (!Number.isInteger(width) || width < 1 || (bpb !== 8 && bpb !== 16)) { console.error("bad width or block size"); process.exit(2); }
const buf = readFileSync(file);

let h = 0xcbf29ce484222325n;
const MASK = 0xffffffffffffffffn, PRIME = 0x100000001b3n;
let off = 0, hashed = 0;
for (let w = width; w >= 1 && off < buf.length; w >>= 1) {
  const bw = Math.max(1, (w + 3) >> 2), bytes = bw * bw * bpb;
  if (off + bytes > buf.length) { console.error(`chain ends inside the ${w} px mip (${buf.length} bytes)`); process.exit(1); }
  if (w <= 64) {
    for (let i = 0; i < bytes; i++) h = ((h ^ BigInt(buf[off + i])) * PRIME) & MASK;
    hashed += bytes;
  }
  off += bytes;
}
if (off !== buf.length) console.error(`note: ${buf.length - off} trailing bytes ignored`);
if (!hashed) { console.error("no mip of width <= 64 in this chain"); process.exit(1); }
console.log(h.toString(16).padStart(16, "0"));
