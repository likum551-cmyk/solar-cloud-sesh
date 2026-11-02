// pack_dlc_rpf.mjs - pack content/dlc/gage_apparel/source into dlc.rpf (RPF7), and check RPF7 archives.
//
//   node pack_dlc_rpf.mjs                         pack <pack>/source -> <pack>/out/dlc.rpf
//   node pack_dlc_rpf.mjs --enc=none|open         header encryption word written (default open = 0x4E45504F)
//   node pack_dlc_rpf.mjs --allow-missing         pack even though the three game assets are absent (writes
//                                                 dlc.INCOMPLETE.rpf: for checking the container only, never ship it)
//   node pack_dlc_rpf.mjs --check=<file.rpf>      parse an RPF7 independently of the writer and print its tree
//   node pack_dlc_rpf.mjs --selftest              build a synthetic archive, check it, then corrupt it and see the checker object
//
// A directory whose name ends in .rpf under source/ becomes a NESTED archive (stored as-is, like the game's own
// dlc.rpf holds x64/models/cdimages/*.rpf). A file that starts with the RSC7 magic is stored as a resource entry.
//
// Layout follows CodeWalker.Core RpfFile.cs (WriteHeader / EnsureAllEntries / CreateFile) and the RPF7 notes in
// Bobrokus/rpf7-docs and TabbedScamper/GTAV-CLAUDE-MCP RPF-REFERENCE.md. NOT verified against the game: the retail
// 3889 exe rejects an OPEN header (FiveM HookInitialMount.cpp PackfileEncryptionError); see the report.
//
// No dependencies.

import { readFileSync, writeFileSync, mkdirSync, readdirSync, statSync, existsSync } from "node:fs";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import path from "node:path";

const here = path.dirname(fileURLToPath(import.meta.url));
const packDir = path.resolve(here, "..");

const RPF7 = 0x52504637, ENC_NONE = 0, ENC_OPEN = 0x4E45504F, ENC_AES = 0x0FFFFFF9, ENC_NG = 0x0FEFFFFF;
const RSC7 = 0x37435352, DIR_MARK = 0x7FFFFF00, BLOCK = 512;

// The assets that only the game or CodeWalker can make (see the report), by path under source/.
const REQUIRED_ASSETS = [
  { rel: "x64/models/cdimages/gage_apparel_male.rpf/mp_m_freemode_01_gage_apparel.ymt", resource: false },
  { rel: "x64/models/cdimages/gage_apparel_male.rpf/mp_m_freemode_01_gage_apparel/jbib_000_u.ydd", resource: true },
  { rel: "x64/models/cdimages/gage_apparel_male.rpf/mp_m_freemode_01_gage_apparel/jbib_diff_000_a_uni.ytd", resource: true },
];

// ---- tree ----------------------------------------------------------------------------------------
// node: { name, dir: bool, children: [], data: Buffer, resource: bool }
function readTree(dirPath, name) {
  const node = { name, dir: true, children: [] };
  for (const ent of readdirSync(dirPath).sort()) {
    const p = path.join(dirPath, ent);
    if (statSync(p).isDirectory()) node.children.push(readTree(p, ent));
    else node.children.push({ name: ent, dir: false, data: readFileSync(p), resource: false });
  }
  return node;
}

function classify(node) {
  if (node.dir) { node.children.forEach(classify); return; }
  if (node.data.length >= 16 && node.data.readUInt32LE(0) === RSC7) node.resource = true;
}

// ---- writer --------------------------------------------------------------------------------------
function buildArchive(root, encWord) {
  // nested archives first: a directory named *.rpf becomes a file whose data is the built child archive
  const prep = (n) => {
    if (!n.dir) return;
    n.children = n.children.map((c) => {
      if (c.dir && c.name.toLowerCase().endsWith(".rpf")) return { name: c.name, dir: false, data: buildArchive(c, encWord), resource: false, nested: true };
      if (c.dir) prep(c);
      return c;
    });
  };
  prep(root);

  // entry order: CodeWalker EnsureAllEntries (root = 0; a popped directory's children are appended as one run,
  // sorted by ordinal name with files and directories merged; sub-directories are pushed in order and popped LIFO)
  const entries = [root];
  const stack = [root];
  while (stack.length) {
    const d = stack.pop();
    const kids = [...d.children].sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
    d.entriesIndex = entries.length;
    d.entriesCount = kids.length;
    for (const k of kids) entries.push(k);
    for (const k of kids) if (k.dir) stack.push(k);
  }

  // names table: root "" first (offset 0), names deduplicated, padded to 16
  const nameBytes = [];
  const nameOff = new Map();
  let namesLen = 0;
  for (const e of entries) {
    const key = e === root ? "" : e.name;
    if (!nameOff.has(key)) {
      nameOff.set(key, namesLen);
      const b = Buffer.from(key + "\0", "latin1");
      if (Buffer.from(key, "latin1").toString("latin1") !== key) throw new Error(`name not latin1: ${key}`);
      nameBytes.push(b);
      namesLen += b.length;
    }
    e.nameOffset = nameOff.get(key);
  }
  const namesPad = (16 - (namesLen % 16)) % 16;
  if (namesPad) { nameBytes.push(Buffer.alloc(namesPad)); namesLen += namesPad; }
  if (namesLen > 0xFFFF) throw new Error("names table exceeds the 16-bit name offset range");

  const headerBytes = 16 + entries.length * 16 + namesLen;
  const headerBlocks = Math.ceil(headerBytes / BLOCK);
  let nextBlock = headerBlocks;
  const dataParts = [];
  for (const e of entries) {
    if (e.dir) continue;
    const len = e.data.length;
    const blocks = Math.max(1, Math.ceil(len / BLOCK));
    e.fileOffset = nextBlock;
    nextBlock += blocks;
    const padded = Buffer.alloc(blocks * BLOCK);
    e.data.copy(padded);
    dataParts.push(padded);
    if (e.fileOffset >= 0x7FFFFF) throw new Error("archive too large");
  }

  const toc = Buffer.alloc(entries.length * 16);
  entries.forEach((e, i) => {
    const o = i * 16;
    if (e.dir) {
      toc.writeUInt32LE(e.nameOffset, o); toc.writeUInt32LE(DIR_MARK, o + 4);
      toc.writeUInt32LE(e.entriesIndex, o + 8); toc.writeUInt32LE(e.entriesCount, o + 12);
    } else if (e.resource) {
      const size = e.data.length;
      if (size >= 0xFFFFFF) throw new Error(`resource ${e.name} is too large for this writer`);
      toc.writeUInt16LE(e.nameOffset, o);
      toc[o + 2] = size & 0xFF; toc[o + 3] = (size >> 8) & 0xFF; toc[o + 4] = (size >> 16) & 0xFF;
      toc[o + 5] = e.fileOffset & 0xFF; toc[o + 6] = (e.fileOffset >> 8) & 0xFF; toc[o + 7] = ((e.fileOffset >> 16) & 0x7F) | 0x80;
      toc.writeUInt32LE(e.data.readUInt32LE(8), o + 8);      // SystemFlags from the RSC7 header
      toc.writeUInt32LE(e.data.readUInt32LE(12), o + 12);    // GraphicsFlags
    } else {
      // binary, stored uncompressed: FileSize 0, uncompressed size = length, encryption 0
      const packed = BigInt(e.nameOffset) | (0n << 16n) | (BigInt(e.fileOffset) << 40n);
      toc.writeBigUInt64LE(packed, o);
      toc.writeUInt32LE(e.data.length, o + 8); toc.writeUInt32LE(0, o + 12);
    }
  });

  const head = Buffer.alloc(headerBlocks * BLOCK);
  head.writeUInt32LE(RPF7, 0); head.writeUInt32LE(entries.length, 4); head.writeUInt32LE(namesLen, 8); head.writeUInt32LE(encWord, 12);
  toc.copy(head, 16);
  Buffer.concat(nameBytes).copy(head, 16 + toc.length);
  return Buffer.concat([head, ...dataParts]);
}

// ---- independent reader / checker -----------------------------------------------------------------
// Does not share structures with the writer: it re-derives everything from the bytes, the way CodeWalker reads.
function readArchive(buf, label = "root") {
  const errs = [];
  const err = (m) => errs.push(`${label}: ${m}`);
  if (buf.length < 16) { err("shorter than the 16-byte header"); return { errs, files: [], enc: 0 }; }
  if (buf.readUInt32LE(0) !== RPF7) err(`bad magic 0x${buf.readUInt32LE(0).toString(16)}`);
  const count = buf.readUInt32LE(4), namesLen = buf.readUInt32LE(8), enc = buf.readUInt32LE(12);
  if (![ENC_NONE, ENC_OPEN, ENC_AES, ENC_NG].includes(enc)) err(`unknown encryption word 0x${enc.toString(16)}`);
  const tocEnd = 16 + count * 16, namesEnd = tocEnd + namesLen;
  if (namesEnd > buf.length) { err("TOC + names run past the end of the file"); return { errs, files: [], enc }; }
  if (namesLen % 16) err("names length is not a multiple of 16");
  const name = (off) => { let e = tocEnd + off; if (off >= namesLen) { err(`name offset ${off} out of range`); return "?"; } let s = ""; while (e < namesEnd && buf[e]) s += String.fromCharCode(buf[e++]); return s; };
  const ent = [];
  for (let i = 0; i < count; i++) {
    const o = 16 + i * 16;
    const x = buf.readUInt32LE(o + 4);
    if (x === DIR_MARK) ent.push({ i, kind: "dir", name: name(buf.readUInt32LE(o)), index: buf.readUInt32LE(o + 8), count: buf.readUInt32LE(o + 12) });
    else if ((x & 0x80000000) === 0) {
      const q = buf.readBigUInt64LE(o);
      ent.push({ i, kind: "bin", name: name(Number(q & 0xFFFFn)), size: Number((q >> 16n) & 0xFFFFFFn), off: Number((q >> 40n) & 0xFFFFFFn), usize: buf.readUInt32LE(o + 8), enc: buf.readUInt32LE(o + 12) });
    } else {
      const size = buf[o + 2] | (buf[o + 3] << 8) | (buf[o + 4] << 16);
      const off = (buf[o + 5] | (buf[o + 6] << 8) | (buf[o + 7] << 16)) & 0x7FFFFF;
      ent.push({ i, kind: "rsc", name: name(buf.readUInt16LE(o)), size, off, sys: buf.readUInt32LE(o + 8), gfx: buf.readUInt32LE(o + 12) });
    }
  }
  if (ent.length && (ent[0].kind !== "dir" || ent[0].name !== "")) err("entry 0 is not the unnamed root directory");
  const files = [];
  const seen = new Set([0]);
  const walk = (d, p) => {
    if (d.index + d.count > ent.length) { err(`directory ${p} children ${d.index}+${d.count} outside the TOC`); return; }
    let prev = null;
    for (let k = 0; k < d.count; k++) {
      const c = ent[d.index + k];
      if (seen.has(c.i)) err(`entry ${c.i} (${c.name}) is reachable twice`);
      seen.add(c.i);
      if (prev !== null && !(prev < c.name)) err(`directory ${p}: "${prev}" is not before "${c.name}" in ordinal order`);
      prev = c.name;
      const full = p ? `${p}/${c.name}` : c.name;
      if (c.kind === "dir") { walk(c, full); continue; }
      const start = c.off * BLOCK;
      const len = c.kind === "bin" ? (c.size ? c.size : c.usize) : c.size;
      if (c.off < Math.ceil((16 + count * 16 + namesLen) / BLOCK)) err(`${full}: data starts inside the header blocks`);
      if (start + len > buf.length) { err(`${full}: data ${start}+${len} runs past the end of the archive (${buf.length})`); continue; }
      const data = buf.subarray(start, start + len);
      if (c.kind === "rsc" && (data.length < 16 || data.readUInt32LE(0) !== RSC7)) err(`${full}: resource entry without RSC7 magic`);
      if (c.kind === "rsc" && (data.readUInt32LE(8) !== c.sys || data.readUInt32LE(12) !== c.gfx)) err(`${full}: TOC flags differ from the RSC7 header`);
      files.push({ path: full, kind: c.kind, len, data });
      if (c.kind === "bin" && c.name.toLowerCase().endsWith(".rpf") && c.size === 0) {
        const inner = readArchive(data, `${label}/${full}`);
        errs.push(...inner.errs);
        files.push(...inner.files.map((f) => ({ ...f, path: `${full}/${f.path}` })));
      }
    }
  };
  if (ent.length) walk(ent[0], "");
  if (seen.size !== count) err(`${count - seen.size} entr${count - seen.size === 1 ? "y is" : "ies are"} not reachable from the root`);
  return { errs, files, enc };
}

const sha = (b) => createHash("sha256").update(b).digest("hex").slice(0, 12);

function printTree(r, buf) {
  console.log(`RPF7, ${buf.length} bytes, encryption 0x${r.enc.toString(16).padStart(8, "0")} (${{ [ENC_OPEN]: "OPEN", [ENC_NONE]: "none", [ENC_NG]: "NG", [ENC_AES]: "AES" }[r.enc] ?? "?"})`);
  for (const f of r.files) console.log(`  ${f.kind.padEnd(3)} ${String(f.len).padStart(8)}  ${f.path}  ${sha(f.data)}`);
}

// ---- self test ------------------------------------------------------------------------------------
function selftest() {
  const rsc = (sys, gfx, n) => { const b = Buffer.alloc(16 + n, 0xAB); b.writeUInt32LE(RSC7, 0); b.writeUInt32LE(13, 4); b.writeUInt32LE(sys, 8); b.writeUInt32LE(gfx, 12); return b; };
  const mk = (name, data) => ({ name, dir: false, data, resource: false });
  const dir = (name, ...children) => ({ name, dir: true, children });
  const files = {
    setup: Buffer.from("<SSetupData/>\n"), content: Buffer.from("<CDataFileMgr__ContentsOfDataFileXml/>\n"),
    meta: Buffer.from("<ShopPedApparel/>\n"), ydd: rsc(0x00000001, 0x20000002, 1500), ytd: rsc(0x00000003, 0x50000004, 70000),
    ymt: Buffer.from("ymt-bytes-" + "x".repeat(700)),
  };
  const root = dir("", mk("setup2.xml", files.setup), mk("content.xml", files.content),
    dir("common", dir("data", mk("b_shop.meta", files.meta))),
    dir("x64", dir("models", dir("cdimages", dir("pack_male.rpf", mk("a.ymt", files.ymt), dir("a", mk("jbib_000_u.ydd", files.ydd), mk("jbib_diff_000_a_uni.ytd", files.ytd)))))));
  classify(root);
  const buf = buildArchive(root, ENC_OPEN);
  const r = readArchive(buf);
  let bad = 0;
  const check = (ok, what) => { console.log(`${ok ? "PASS" : "FAIL"}  ${what}`); if (!ok) bad++; };
  check(r.errs.length === 0, `checker accepts the writer's archive (${r.errs.length} problem(s)${r.errs.length ? ": " + r.errs[0] : ""})`);
  const byPath = new Map(r.files.map((f) => [f.path, f]));
  const expect = { "setup2.xml": files.setup, "content.xml": files.content, "common/data/b_shop.meta": files.meta,
    "x64/models/cdimages/pack_male.rpf/a.ymt": files.ymt, "x64/models/cdimages/pack_male.rpf/a/jbib_000_u.ydd": files.ydd,
    "x64/models/cdimages/pack_male.rpf/a/jbib_diff_000_a_uni.ytd": files.ytd };
  check(r.files.filter((f) => !f.path.endsWith(".rpf")).length === Object.keys(expect).length, `reads back ${Object.keys(expect).length} files (nested archive included)`);
  for (const [p, data] of Object.entries(expect)) { const f = byPath.get(p); check(!!f && f.data.subarray(0, data.length).equals(data) && f.len === data.length, `round trip ${p}`); }
  check(byPath.get("x64/models/cdimages/pack_male.rpf/a/jbib_000_u.ydd")?.kind === "rsc", "RSC7 file is stored as a resource entry");
  check(buf.length % BLOCK === 0, "archive length is a multiple of 512");

  // The checker must object to damage (otherwise a clean result means nothing).
  const mutate = (what, fn) => { const b = Buffer.from(buf); fn(b); const rr = readArchive(b); check(rr.errs.length > 0, `checker rejects: ${what} (${rr.errs[0] ?? "no complaint"})`); };
  mutate("bad magic", (b) => b.writeUInt32LE(0x12345678, 0));
  mutate("a file offset pointing past the end", (b) => {
    for (let i = 0; i < b.readUInt32LE(4); i++) {   // the first binary (non-directory, non-resource) entry
      const o = 16 + i * 16, x = b.readUInt32LE(o + 4);
      if (x !== DIR_MARK && (x & 0x80000000) === 0) { b.writeBigUInt64LE(b.readBigUInt64LE(o) | (0x7000n << 40n), o); return; }
    }
    throw new Error("selftest: no binary entry to damage");
  });
  mutate("two children swapped (unsorted)", (b) => { const t = Buffer.from(b.subarray(16 + 1 * 16, 16 + 2 * 16)); b.copy(b, 16 + 1 * 16, 16 + 2 * 16, 16 + 3 * 16); t.copy(b, 16 + 2 * 16); });
  mutate("a name offset out of range", (b) => b.writeUInt16LE(0xFFF0, 16 + 1 * 16));
  mutate("RSC7 magic overwritten", (b) => { const f = r.files.find((x) => x.path.endsWith(".ytd")); const i = b.indexOf(f.data.subarray(0, 32)); b.writeUInt32LE(0, i); });
  mutate("an entry not reachable from the root", (b) => b.writeUInt32LE(b.readUInt32LE(4) + 1, 4));
  console.log(bad ? `FAILED (${bad})` : "ALL PASS");
  process.exit(bad ? 1 : 0);
}

// ---- main ----------------------------------------------------------------------------------------
const args = Object.fromEntries(process.argv.slice(2).map((a) => { const m = /^--([^=]+)(?:=(.*))?$/.exec(a); if (!m) { console.error(`bad argument ${a}`); process.exit(2); } return [m[1], m[2] ?? true]; }));
if (args.selftest) selftest();
if (args.check) {
  const buf = readFileSync(path.resolve(String(args.check)));
  const r = readArchive(buf);
  printTree(r, buf);
  if (r.errs.length) { for (const e of r.errs) console.error("PROBLEM " + e); process.exit(1); }
  console.log("structure OK");
  process.exit(0);
}

const encWord = { open: ENC_OPEN, none: ENC_NONE }[String(args.enc ?? "open")];
if (encWord === undefined) { console.error("--enc must be open or none (NG/AES cannot be written headlessly: see the report)"); process.exit(2); }
const srcDir = path.join(packDir, "source");
const outDir = path.join(packDir, "out");
const root = readTree(srcDir, "");
classify(root);
const missing = REQUIRED_ASSETS.filter((a) => !existsSync(path.join(srcDir, ...a.rel.split("/"))));
for (const a of REQUIRED_ASSETS.filter((x) => x.resource && existsSync(path.join(srcDir, ...x.rel.split("/"))))) {
  const head = readFileSync(path.join(srcDir, ...a.rel.split("/"))).subarray(0, 4);
  if (head.length < 4 || head.readUInt32LE(0) !== RSC7) { console.error(`${a.rel} is not an RSC7 resource (it must come from CodeWalker/OpenIV, not a source file)`); process.exit(1); }
}
if (missing.length && !args["allow-missing"]) {
  console.error("refusing to pack: these game assets are missing under source/ (they need CodeWalker or OpenIV and the game's keys):");
  for (const a of missing) console.error(`  ${a.rel}`);
  console.error("pass --allow-missing to build dlc.INCOMPLETE.rpf for a container check only");
  process.exit(1);
}
const buf = buildArchive(root, encWord);
const r = readArchive(buf);
mkdirSync(outDir, { recursive: true });
const outFile = path.join(outDir, missing.length ? "dlc.INCOMPLETE.rpf" : "dlc.rpf");
writeFileSync(outFile, buf);
printTree(r, buf);
if (r.errs.length) { for (const e of r.errs) console.error("PROBLEM " + e); process.exit(1); }
console.log(`structure OK -> ${outFile}${missing.length ? "  (INCOMPLETE: missing game assets)" : ""}`);
