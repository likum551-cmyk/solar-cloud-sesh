// stage_packs.mjs - lay the operator's car packs out the way the engine mounts them (apps/engine/pack_stream.cpp).
//
//   node tools/packs/stage_packs.mjs [--src=<DLC PACKS dir>] [--dest=<packs dir>] [--only=<pack>] [--force] [--dry-run]
//                                    [--keep-ng-nested]
//
//   --src      folder of packs, one sub-folder each holding a dlc.rpf (default: <repo>/DLC PACKS)
//   --dest     where the engine looks (default: %LOCALAPPDATA%\GAGE-MP\packs)
//   --only     stage just this pack folder
//   --force    restage a pack that is already staged (default: an existing one is left alone and reported)
//   --dry-run  read and report every pack, write nothing
//   --keep-ng-nested  an OPEN pack whose models sit in an NG nested archive (veh_VOLGA) keeps that archive and its
//              RPF_FILE line for the game to mount (LIVE-UNVERIFIED in folder mode; default: such a pack fails)
//   --tex-limit-mib  texture budget (tools/packs/ytd_budget.mjs): a stream\*.ytd whose compressed size is at or above
//              this loses top mips of its largest textures until it fits (default 15)
//   --tex-max-dim    ... and so does one with a texture larger than this on a side (default 2048; 0 = no size cap)
//   --gen9     the GTA V Enhanced set (default --dest %LOCALAPPDATA%\GAGE-MP\packs-gen9; published as packs-gen9.json):
//              EVERY pack becomes a loose folder (an NG/AES dlc.rpf is decrypted by CodeWalker with the keys of the
//              local game, held in that process only) and every model is converted to the gen9 resource format by
//              tools/packs/cw_gen9.ps1. Needs --codewalker=<CodeWalker30_dev48+ folder> --game-dir=<GTA V Legacy folder>.
//              Legacy-format models crash Enhanced's resource loader (0.4.26: INT3 at GTA5_Enhanced.exe+0x127c32f).
//
// A loose model file of 0xFFFFFF bytes (16 MiB) or more fails the pack: the game's loose path crashed on one
// (valkyrietp.ytd, 28 MB); only .ytd files are budgeted, so a .yft that big has to be fixed by hand.
//
// Per pack, by the dlc.rpf header's encryption word:
//   OPEN / NONE  -> a LOOSE folder: setup2.xml + a content.xml that lists only the data metas the game can take loose,
//                   those metas at their own paths, every model resource (.yft .ytd .ydr .ydd, from any nested archive)
//                   flat in stream\, and the dlc.rpf itself (AddDlcPack's gate file; it is never opened). The game
//                   refuses a nested OPEN archive, so nested .rpf entries leave content.xml.
//   NG / AES     -> the dlc.rpf as it is: the game reads those archives itself (nothing here can, by design: this tool
//                   never holds a key). Each gets gage_tune.json (a copy of handling_tune.json): the engine applies it
//                   to the pack's handling.meta at mount, read through the game's own device.
// Staging goes to <dest>\<pack>.staging and is swapped in only when complete. Exit 1 if any pack failed.
//
// Reading the archives reuses content/dlc/gage_apparel/tools/pack_dlc_rpf.mjs --extract (it only reads the TOC). No
// dependencies.

import { spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, readdirSync, readFileSync, renameSync, rmSync, statSync, writeFileSync, openSync, readSync, closeSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { budgetFile, MiB, TOC_SIZE_MARKER } from "./ytd_budget.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, "..", "..");
const packTool = path.join(repo, "content", "dlc", "gage_apparel", "tools", "pack_dlc_rpf.mjs");

const args = Object.fromEntries(process.argv.slice(2).map((a) => {
  const m = /^--([^=]+)(?:=(.*))?$/.exec(a);
  if (!m) { console.error(`unknown argument ${a}`); process.exit(2); }
  return [m[1], m[2] ?? true];
}));
const known = new Set(["src", "dest", "only", "force", "dry-run", "keep-ng-nested", "tex-limit-mib", "tex-max-dim", "gen9", "codewalker", "game-dir"]);
for (const k of Object.keys(args)) if (!known.has(k)) { console.error(`unknown option --${k}`); process.exit(2); }
const texLimit = Number(args["tex-limit-mib"] ?? 15) * MiB, texMaxDim = Number(args["tex-max-dim"] ?? 2048);
if (!(texLimit > 0 && texLimit < TOC_SIZE_MARKER)) { console.error("--tex-limit-mib must be above 0 and below 16"); process.exit(2); }
if (!(Number.isInteger(texMaxDim) && (texMaxDim === 0 || texMaxDim >= 4))) { console.error("--tex-max-dim must be 0 or an integer >= 4"); process.exit(2); }

const src = path.resolve(String(args.src ?? path.join(repo, "DLC PACKS")));
const local = process.env.LOCALAPPDATA;
if (!args.dest && !local) { console.error("LOCALAPPDATA is not set; pass --dest"); process.exit(2); }
const gen9 = !!args.gen9;
const dest = path.resolve(String(args.dest ?? path.join(local, "GAGE-MP", gen9 ? "packs-gen9" : "packs")));
const dry = !!args["dry-run"], force = !!args.force, keepNgNested = !!args["keep-ng-nested"];
const cwDir = args.codewalker ? path.resolve(String(args.codewalker)) : "";
const gameDir = args["game-dir"] ? path.resolve(String(args["game-dir"])) : "";
if (gen9 && (!cwDir || !existsSync(path.join(cwDir, "CodeWalker.Core.dll")))) { console.error("--gen9 needs --codewalker=<folder with CodeWalker.Core.dll (CodeWalker30_dev48 or newer)>"); process.exit(2); }
if (gen9 && (!gameDir || !existsSync(gameDir))) { console.error("--gen9 needs --game-dir=<GTA V Legacy folder> (CodeWalker reads the archive keys from it)"); process.exit(2); }
if (gen9 && keepNgNested) { console.error("--gen9 decrypts NG archives itself; --keep-ng-nested does not apply"); process.exit(2); }

const ENC_NONE = 0, ENC_OPEN = 0x4e45504f, ENC_AES = 0x0ffffff9, ENC_NG = 0x0fefffff, RPF7 = 0x52504637, RSC7 = 0x37435352;
// The data-file types the game takes from a loose folder mid-session (the Jesko run, 2026-11-05: handling, vehicles,
// carcols, carvariations, dlctext). Everything else (nested RPF_FILE, audio, unlocks, ...) leaves content.xml.
const KEEP_TYPES = new Set(["HANDLING_FILE", "VEHICLE_METADATA_FILE", "CARCOLS_FILE", "VEHICLE_VARIATION_FILE",
  "VEHICLE_LAYOUTS_FILE", "TEXTFILE_METAFILE"]);
const MODEL_EXT = new Set([".yft", ".ytd", ".ydr", ".ydd"]);

const stripComments = (s) => s.replace(/<!--[\s\S]*?-->/g, "");
const field = (block, tag) => { const m = new RegExp(`<${tag}>([\\s\\S]*?)</${tag}>`).exec(block); return m ? m[1].trim() : ""; };
const inPack = (name) => { const i = name.indexOf(":/"); return (i >= 0 ? name.slice(i + 2) : name).replace(/%PLATFORM%/g, "x64"); };

function header(file) {
  const fd = openSync(file, "r");
  try { const b = Buffer.alloc(16); const n = readSync(fd, b, 0, 16, 0); return n === 16 ? { magic: b.readUInt32LE(0), enc: b.readUInt32LE(12) } : null; }
  finally { closeSync(fd); }
}

function walk(dir, out = []) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, out); else out.push(p);
  }
  return out;
}

function isRsc7(file) {
  const fd = openSync(file, "r");
  try { const b = Buffer.alloc(4); return readSync(fd, b, 0, 4, 0) === 4 && b.readUInt32LE(0) === RSC7; }
  finally { closeSync(fd); }
}

// A Legacy (gen8) resource: RSC7 with the Legacy version for its type. Gen9 output keeps RSC7 and changes the version
// (CodeWalker dev48: yft 162 -> 171, ytd 13 -> 5). Mirrors apps/engine/pack_enh_mount_core.h IsLegacyResource.
const LEGACY_VER = { ".yft": 162, ".ytd": 13, ".ydr": 165, ".ydd": 165 };
function isLegacyResource(file) {
  const fd = openSync(file, "r");
  try {
    const b = Buffer.alloc(8);
    if (readSync(fd, b, 0, 8, 0) !== 8 || b.readUInt32LE(0) !== RSC7) return false;
    return b.readUInt32LE(4) === LEGACY_VER[path.extname(file).toLowerCase()];
  } finally { closeSync(fd); }
}

// --gen9: run tools/packs/cw_gen9.ps1 (CodeWalker.Core, no window). Throws with the script's last lines on failure.
function codeWalker(mode, inPath, outPath) {
  const a = ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", path.join(here, "cw_gen9.ps1"),
    "-Mode", mode, "-CodeWalker", cwDir, "-In", inPath, "-Out", outPath];
  if (mode === "extract") a.push("-GameDir", gameDir);
  const r = spawnSync("powershell.exe", a, { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  const lines = `${r.stdout || ""}${r.stderr || ""}`.trim().split(/\r?\n/);
  if (r.status !== 0) throw new Error(`CodeWalker ${mode} failed (exit ${r.status}): ${lines.filter((l) => /FAIL|error|problem/i.test(l)).slice(0, 3).join(" | ") || lines.slice(-2).join(" | ")}`);
  return lines;
}

function models(vehiclesMeta) {
  const s = stripComments(vehiclesMeta);
  return [...s.matchAll(/<modelName>\s*([^<]*?)\s*<\/modelName>/g)].map((m) => m[1]).filter((m) => /^[A-Za-z0-9_-]{1,63}$/.test(m));
}

// Rewrite content.xml: drop the dataFiles items not kept, and their lines in every change set's file lists.
function filterContent(xml, keepNames) {
  const keep = (n) => keepNames.has(n.trim().toLowerCase());
  let out = xml.replace(/<dataFiles>([\s\S]*?)<\/dataFiles>/, (all, body) =>
    "<dataFiles>" + body.replace(/[ \t]*<Item>[\s\S]*?<\/Item>[ \t]*\r?\n?/g, (item) => (keep(field(item, "filename")) ? item : "")) + "</dataFiles>");
  out = out.replace(/<(filesToEnable|filesToDisable)>([\s\S]*?)<\/\1>/g, (all, tag, body) =>
    `<${tag}>` + body.replace(/[ \t]*<Item>([^<]*)<\/Item>[ \t]*\r?\n?/g, (line, name) => (keep(name) ? line : "")) + `</${tag}>`);
  return out;
}

// tools/packs/handling_tune.json: per-handlingName scale/set rules for the loose packs' HANDLING_FILEs ('*' = every one).
const tune = JSON.parse(readFileSync(path.join(here, "handling_tune.json"), "utf8"));
const fmt = (v) => String(Math.round(v * 1e6) / 1e6);

// Apply the tune to one handling.meta text. Each rule must hit exactly one <field value="..."> inside that item's block,
// else this throws (a silent no-op patch is how a tune "applies" and changes nothing).
function tuneHandling(xml, report) {
  return xml.replace(/<Item type="CHandlingData">[\s\S]*?<\/Item>/g, (block) => {
    const hname = field(block, "handlingName");
    const named = Object.keys(tune).find((k) => k !== "*" && !k.startsWith("_") && k.toLowerCase() === hname.toLowerCase());
    const rules = [tune["*"], named ? tune[named] : null].filter(Boolean);
    let out = block;
    const done = [];
    for (const r of rules) {
      for (const [op, fields] of [["scale", r.scale || {}], ["set", r.set || {}]]) {
        for (const [f, n] of Object.entries(fields)) {
          const rx = new RegExp(`(<${f}\\s+value=")([-0-9.eE+]+)(")`, "g");
          const hits = [...out.matchAll(rx)];
          if (hits.length !== 1) throw new Error(`handling ${hname}: ${f} found ${hits.length} times (want exactly 1)`);
          out = out.replace(rx, (all, a, v, b) => {
            const nv = op === "scale" ? Number(v) * n : n;
            done.push(`${f} ${v}->${fmt(nv)}`);
            return a + fmt(nv) + b;
          });
        }
      }
    }
    if (done.length) report.tuned.push(`${hname}: ${done.join(", ")}`);
    return out;
  });
}

function stageLoose(name, rpf, staging, report, encrypted) {
  const x = path.join(staging, ".extract");
  if (encrypted) {
    codeWalker("extract", rpf, x);   // --gen9 only: an NG/AES pack, decrypted by CodeWalker with the local game's keys
  } else {
    const r = spawnSync(process.execPath, [packTool, `--extract=${rpf}`, `--out=${x}`], { encoding: "utf8" });
    if (r.status !== 0) throw new Error(`extract failed (exit ${r.status}): ${(r.stderr || r.stdout || "").trim().split("\n").slice(-2).join(" | ")}`);
    // A TOC problem means a file came out wrong or not at all (valkyrietp.ytd before the 16 MiB fix): never stage that.
    if (/TOC problem/.test(r.stderr || "")) throw new Error(`extract reported problems: ${r.stderr.trim().split(/\r?\n/).slice(0, 3).join(" | ")}`);
  }
  for (const f of ["setup2.xml", "content.xml"])
    if (!existsSync(path.join(x, f))) throw new Error(`${f} is missing at the archive root`);
  const content = readFileSync(path.join(x, "content.xml"), "utf8");
  const items = [...stripComments(content).matchAll(/<dataFiles>([\s\S]*?)<\/dataFiles>/g)].flatMap((m) => [...m[1].matchAll(/<Item>[\s\S]*?<\/Item>/g)].map((i) => i[0]));
  const kept = new Set(), dropped = [];
  for (const it of items) {
    const fname = field(it, "filename"), type = field(it, "fileType").toUpperCase();
    const rel = inPack(fname);
    const file = path.join(x, ...rel.split("/"));
    if (type === "RPF_FILE" && keepNgNested && existsSync(file) && statSync(file).isFile() && header(file)?.enc === ENC_NG) {
      // --keep-ng-nested: an NG archive inside an OPEN pack (veh_VOLGA's x64/vehicles.rpf holds its models). Nothing here
      // can read it, so it stays a blob and content.xml keeps it: the game mounts it with its own NG keys (name + length
      // pick the key, both unchanged). LIVE-UNVERIFIED in folder mode (an OPEN nested RPF_FILE crashed the game there).
      kept.add(fname.trim().toLowerCase());
      const to = path.join(staging, ...rel.split("/"));
      mkdirSync(path.dirname(to), { recursive: true });
      copyFileSync(file, to);
      report.ngNested.push(rel);
      continue;
    }
    if (!KEEP_TYPES.has(type)) { dropped.push(`${rel} (${type})`); continue; }
    if (!existsSync(file) || !statSync(file).isFile()) { dropped.push(`${rel} (${type}, not in the archive)`); continue; }
    kept.add(fname.trim().toLowerCase());
    const to = path.join(staging, ...rel.split("/"));
    mkdirSync(path.dirname(to), { recursive: true });
    if (type === "HANDLING_FILE") writeFileSync(to, tuneHandling(readFileSync(file, "utf8"), report));
    else copyFileSync(file, to);
    if (type === "VEHICLE_METADATA_FILE") report.models.push(...models(readFileSync(file, "utf8")));
  }
  writeFileSync(path.join(staging, "content.xml"), filterContent(content, kept));
  copyFileSync(path.join(x, "setup2.xml"), path.join(staging, "setup2.xml"));
  const stream = path.join(staging, "stream");
  mkdirSync(stream, { recursive: true });
  const seen = new Map();
  for (const f of walk(x)) {
    if (!MODEL_EXT.has(path.extname(f).toLowerCase())) continue;
    if (!isRsc7(f)) { report.warnings.push(`${path.relative(x, f)} has no RSC7 header; not streamed`); continue; }
    const base = path.basename(f), key = base.toLowerCase();
    if (seen.has(key)) { report.warnings.push(`${base} appears twice (${seen.get(key)} kept)`); continue; }
    seen.set(key, path.relative(x, f));
    copyFileSync(f, path.join(stream, base));
  }
  // Texture budget, then the hard gate: no loose model file may reach the 24-bit size the game's loose path handles.
  for (const base of [...seen.keys()].map((k) => path.basename(seen.get(k))).sort()) {
    const file = path.join(stream, base);
    const isYtd = path.extname(base).toLowerCase() === ".ytd";
    if (isYtd) {
      let res;
      try { res = budgetFile(file, { limit: texLimit, maxDim: texMaxDim || 0xFFFF }); }
      catch (e) { throw new Error(`texture budget for ${base}: ${e.message}`); }
      if (res) {
        const dropped = res.after.textures.reduce((s, t, i) => s + (res.before.textures[i].levels - t.levels), 0);
        report.textures.push(`${base} ${(res.beforeBytes / 1e6).toFixed(1)} MB -> ${(res.afterBytes / 1e6).toFixed(1)} MB (${res.halved} texture${res.halved === 1 ? "" : "s"} halved, ${dropped} top mip${dropped === 1 ? "" : "s"} dropped)`);
      }
    }
    const size = statSync(file).size;
    if (size >= TOC_SIZE_MARKER)
      throw new Error(`${base} is ${size} bytes compressed (16 MiB or more): the game's loose path cannot take it${isYtd ? "" : " (only .ytd files are budgeted)"}`);
  }
  if (gen9 && seen.size) {
    // GTA V Enhanced: every model converted to the gen9 resource format (the Legacy one crashes Enhanced's resource loader).
    const stream9 = path.join(staging, "stream.gen9");
    codeWalker("convert", stream, stream9);
    for (const f of walk(stream9)) {
      if (!MODEL_EXT.has(path.extname(f).toLowerCase())) continue;
      if (isLegacyResource(f)) throw new Error(`${path.basename(f)} is still Legacy-format after the gen9 conversion`);
      if (statSync(f).size >= TOC_SIZE_MARKER) throw new Error(`${path.basename(f)} is 16 MiB or more after the gen9 conversion`);
    }
    if (walk(stream9).length !== walk(stream).length) throw new Error("the gen9 conversion lost files");
    rmSync(stream, { recursive: true, force: true });
    renameSync(stream9, stream);
    report.gen9 = seen.size;
  }
  report.stream = seen.size;
  report.dropped = dropped;
  copyFileSync(rpf, path.join(staging, "dlc.rpf"));
  const ngBlobs = walk(x).filter((f) => f.toLowerCase().endsWith(".rpf") && header(f)?.enc === ENC_NG).length;
  rmSync(x, { recursive: true, force: true });
  if (!report.models.length) report.warnings.push("no <modelName> in any kept vehicles.meta");
  if (!seen.size && !report.ngNested.length)
    throw new Error("no model resources found" + (ngBlobs ? ` (${ngBlobs} NG nested archive(s) hold them; --keep-ng-nested keeps them for the game to read)` : ""));
}

if (!existsSync(src)) { console.error(`no source folder ${src}`); process.exit(1); }
const packs = readdirSync(src, { withFileTypes: true }).filter((e) => e.isDirectory()).map((e) => e.name)
  .filter((n) => !args.only || n.toLowerCase() === String(args.only).toLowerCase()).sort((a, b) => a.localeCompare(b));
if (!packs.length) { console.error(`no pack folders in ${src}${args.only ? ` named ${args.only}` : ""}`); process.exit(1); }
if (!dry) mkdirSync(dest, { recursive: true });

let failed = 0, staged = 0, skipped = 0;
for (const name of packs) {
  const rpf = path.join(src, name, "dlc.rpf");
  const target = path.join(dest, name);
  const report = { models: [], warnings: [], dropped: [], stream: 0, tuned: [], ngNested: [], textures: [] };
  try {
    if (!/^[\x20-\x7e]+$/.test(name)) throw new Error("the folder name is not plain ASCII (the game takes narrow paths)");
    if (!existsSync(rpf)) throw new Error("no dlc.rpf");
    const h = header(rpf);
    if (!h || h.magic !== RPF7) throw new Error(`not an RPF7 archive${h ? ` (magic 0x${h.magic.toString(16)})` : ""}`);
    // --gen9: every pack becomes a loose folder (an encrypted one is decrypted by CodeWalker), its models converted.
    const mode = h.enc === ENC_OPEN || h.enc === ENC_NONE ? "loose" : h.enc === ENC_NG || h.enc === ENC_AES ? (gen9 ? "loose" : "rpf") : null;
    if (!mode) throw new Error(`unknown encryption word 0x${h.enc.toString(16)}`);
    const encName = h.enc === ENC_OPEN ? "OPEN" : h.enc === ENC_NONE ? "NONE" : h.enc === ENC_NG ? "NG" : "AES";
    if (existsSync(target) && !force) { console.log(`= ${name}: already staged (${encName}); --force restages it`); skipped++; continue; }
    if (dry) { console.log(`~ ${name}: ${encName} -> ${mode === "loose" ? "loose folder" : "dlc.rpf as is"} (dry run)`); continue; }
    const staging = target + ".staging";
    rmSync(staging, { recursive: true, force: true });
    mkdirSync(staging, { recursive: true });
    if (mode === "loose") stageLoose(name, rpf, staging, report, h.enc === ENC_NG || h.enc === ENC_AES);
    else {
      copyFileSync(rpf, path.join(staging, "dlc.rpf"));
      copyFileSync(path.join(here, "handling_tune.json"), path.join(staging, "gage_tune.json"));
    }
    if (existsSync(target)) { rmSync(target + ".old", { recursive: true, force: true }); renameSync(target, target + ".old"); }
    renameSync(staging, target);
    rmSync(target + ".old", { recursive: true, force: true });
    staged++;
    console.log(`+ ${name}: ${encName} -> ${mode === "loose" ? `loose folder, ${report.stream} model file(s) in stream\\${report.ngNested.length ? ` + ${report.ngNested.length} NG nested archive(s)` : ""}, cars: ${report.models.join(", ") || "-"}` : "dlc.rpf as is (the game reads it; its cars are listed in game)"}`);
    for (const t of report.textures) console.log(`    textures: ${t}`);
    for (const t of report.tuned) console.log(`    tuned: ${t}`);
    for (const d of report.dropped) console.log(`    dropped from content.xml: ${d}`);
    for (const n of report.ngNested) console.log(`    kept NG nested archive (the game reads it): ${n}`);
    if (mode !== "loose") console.log("    gage_tune.json: the engine tunes its handling at mount");
    for (const w of report.warnings) console.log(`    WARNING: ${w}`);
  } catch (e) {
    failed++;
    console.error(`! ${name}: ${e.message}`);
    rmSync(target + ".staging", { recursive: true, force: true });
  }
}
console.log(`${staged} staged, ${skipped} already staged, ${failed} failed -> ${dest}`);
process.exit(failed ? 1 : 0);
