#!/usr/bin/env node
// Lists the DLLs a Windows PE file imports (static and delay-load) and, optionally, checks them against
// an allow-list. Used by tools/deploy/build-client.sh to prove that the cross-compiled launcher and the
// three injected DLLs need no runtime DLL next to GTA (no libstdc++-6, libgcc_s_seh-1, libwinpthread-1).
// Pure Node, no dependencies, runs the same on Windows and Linux.
//
//   node pe-imports.mjs [--allow <allow-list>] [--machine x64|x86] [--quiet] <file.exe|dll>...
//
// Output, one line per file:  <file>: machine=x64 subsystem=2 dllchars=0x160 imports=KERNEL32.dll,USER32.dll,...
// (dllchars = the PE DllCharacteristics word: 0x40 ASLR, 0x20 high-entropy VA, 0x100 DEP; compare the
// cross build against the WinLibs build recorded in docs/OPS.md)
// --allow  a text file, one entry per line: an exact DLL name, or a prefix ending in `*` (api-ms-win-*),
//          matched case-insensitively; `#` starts a comment. An import outside the list is a failure.
// --machine fail unless the PE is for that machine (x64 = AMD64, x86 = i386).
// Exit 0 = all files parsed and allowed, 1 = a check failed, 2 = usage error or unreadable/invalid PE.
import { readFileSync } from 'node:fs';

function usage(msg) { process.stderr.write(`pe-imports: ${msg}\nusage: pe-imports.mjs [--allow <file>] [--machine x64|x86] [--quiet] <pe>...\n`); process.exit(2); }

const files = [];
let allowPath = '';
let wantMachine = '';
let quiet = false;
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '--allow') allowPath = argv[++i] ?? usage('--allow needs a file');
  else if (a === '--machine') wantMachine = argv[++i] ?? usage('--machine needs x64 or x86');
  else if (a === '--quiet') quiet = true;
  else if (a.startsWith('--')) usage(`unknown option ${a}`);
  else files.push(a);
}
if (files.length === 0) usage('no files given');
if (wantMachine && wantMachine !== 'x64' && wantMachine !== 'x86') usage('--machine must be x64 or x86');

let allow = null;
if (allowPath) {
  allow = readFileSync(allowPath, 'utf8')
    .split(/\r?\n/)
    .map((l) => l.replace(/#.*/, '').trim().toLowerCase())
    .filter(Boolean);
  if (allow.length === 0) usage(`allow-list ${allowPath} is empty`);
}
const isAllowed = (dll) => {
  const d = dll.toLowerCase();
  return allow.some((e) => (e.endsWith('*') ? d.startsWith(e.slice(0, -1)) : d === e));
};

const MACHINES = { 0x8664: 'x64', 0x14c: 'x86', 0xaa64: 'arm64' };

function parse(path) {
  const b = readFileSync(path);
  const fail = (m) => { throw new Error(`${path}: ${m}`); };
  if (b.length < 0x40 || b.readUInt16LE(0) !== 0x5a4d) fail('not a PE file (no MZ header)');
  const pe = b.readUInt32LE(0x3c);
  if (pe + 24 > b.length || b.readUInt32LE(pe) !== 0x00004550) fail('not a PE file (no PE signature)');
  const machine = b.readUInt16LE(pe + 4);
  const nSections = b.readUInt16LE(pe + 6);
  const optSize = b.readUInt16LE(pe + 20);
  const opt = pe + 24;
  const magic = b.readUInt16LE(opt);
  if (magic !== 0x10b && magic !== 0x20b) fail(`unknown optional header magic 0x${magic.toString(16)}`);
  const plus = magic === 0x20b;
  const subsystem = b.readUInt16LE(opt + 68);
  const dllChars = b.readUInt16LE(opt + 70); // DYNAMIC_BASE 0x40, HIGH_ENTROPY_VA 0x20, NX_COMPAT 0x100, ...
  const ddBase = opt + (plus ? 112 : 96);
  const nDirs = b.readUInt32LE(ddBase - 4);
  const dir = (n) => (n < nDirs ? { rva: b.readUInt32LE(ddBase + n * 8), size: b.readUInt32LE(ddBase + n * 8 + 4) } : { rva: 0, size: 0 });
  const secBase = opt + optSize;
  const sections = [];
  for (let i = 0; i < nSections; i++) {
    const o = secBase + i * 40;
    sections.push({ va: b.readUInt32LE(o + 12), vsize: b.readUInt32LE(o + 8), raw: b.readUInt32LE(o + 20), rawSize: b.readUInt32LE(o + 16) });
  }
  const off = (rva) => {
    for (const s of sections) {
      const span = Math.max(s.vsize, s.rawSize);
      if (rva >= s.va && rva < s.va + span) return s.raw + (rva - s.va);
    }
    return -1;
  };
  const cstr = (rva) => {
    const o = off(rva);
    if (o < 0 || o >= b.length) fail(`name RVA 0x${rva.toString(16)} is outside every section`);
    let e = o;
    while (e < b.length && b[e] !== 0) e++;
    return b.toString('latin1', o, e);
  };
  const imports = [];
  const imp = dir(1);
  if (imp.rva) {
    let o = off(imp.rva);
    if (o < 0) fail('import directory is outside every section');
    for (; o + 20 <= b.length; o += 20) {
      const nameRva = b.readUInt32LE(o + 12);
      if (nameRva === 0 && b.readUInt32LE(o) === 0) break;
      imports.push(cstr(nameRva));
    }
  }
  const delay = dir(13);
  if (delay.rva) {
    let o = off(delay.rva);
    if (o < 0) fail('delay-import directory is outside every section');
    for (; o + 32 <= b.length; o += 32) {
      const attrs = b.readUInt32LE(o);
      const nameRva = b.readUInt32LE(o + 4);
      if (nameRva === 0 && attrs === 0) break;
      imports.push(cstr(nameRva));
    }
  }
  return { machine: MACHINES[machine] ?? `0x${machine.toString(16)}`, subsystem, dllChars, imports: [...new Set(imports)].sort((x, y) => x.toLowerCase().localeCompare(y.toLowerCase())) };
}

let bad = 0;
for (const f of files) {
  let info;
  try { info = parse(f); } catch (e) { process.stderr.write(`pe-imports: ${e.message}\n`); process.exit(2); }
  if (!quiet) process.stdout.write(`${f}: machine=${info.machine} subsystem=${info.subsystem} dllchars=0x${info.dllChars.toString(16)} imports=${info.imports.join(',')}\n`);
  if (wantMachine && info.machine !== wantMachine) { process.stderr.write(`pe-imports: FAIL ${f}: machine ${info.machine}, wanted ${wantMachine}\n`); bad++; }
  if (allow) {
    const extra = info.imports.filter((d) => !isAllowed(d));
    if (extra.length) { process.stderr.write(`pe-imports: FAIL ${f}: imports outside the allow-list: ${extra.join(', ')}\n`); bad++; }
  }
}
process.exit(bad ? 1 : 0);
