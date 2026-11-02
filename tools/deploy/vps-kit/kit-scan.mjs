#!/usr/bin/env node
// Scan a folder (or a list of names) for things that must never be in the VPS kit. Prints counts and relative FILE NAMES only,
// never matched text. Runs on GI's PC from tools/deploy/pack-vps-kit.sh.
//
//   node kit-scan.mjs DIR [--strict] [--label TEXT] [--words]
//   node kit-scan.mjs --names-from FILE [--strict] [--label TEXT]
//
//   --strict         also apply the Claude-setup name rules (token/secret in a name, transcripts, history)
//   --lenient        source-tree mode: assignment-style rules (secret-assignment, jwt, apikey-json-literal) only warn
//   --words          also print how many text files mention token / secret / password (information only)
//   --names-from F   check the names listed in F (one per line, from `tar -tzf`) and read no content
// Exit codes: 0 clean (warnings may be listed), 1 a blocking hit, 2 usage.
import fs from 'node:fs';
import path from 'node:path';
import { nameHits, contentHits, REPORT_WORDS } from './kit-patterns.mjs';

const argv = process.argv.slice(2);
const flags = new Set();
let dir = '';
let namesFrom = '';
let label = 'kit';
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '--strict' || a === '--words' || a === '--lenient') flags.add(a);
  else if (a === '--label') label = argv[++i] ?? 'kit';
  else if (a === '--names-from') namesFrom = argv[++i] ?? '';
  else if (a.startsWith('--')) { console.error(`kit-scan: unknown option ${a}`); process.exit(2); }
  else dir = a;
}
if (!dir && !namesFrom) { console.error('kit-scan: give a folder or --names-from FILE'); process.exit(2); }
const strict = flags.has('--strict');
// --lenient is for the repository's source tarball, whose tests and API docs hold made-up passwords and example tokens
// ("password": "another-...", "token": "gmj_..."). Real key SHAPES (private key blocks, sk-ant-, ghp_, AKIA, xox, AIza) still block;
// the three assignment-style rules only warn there (the files are listed so they can be looked at).
const lenient = flags.has('--lenient');
const LENIENT_WARN = new Set(['secret-assignment', 'jwt', 'apikey-json-literal']);

const byClass = new Map(); // "name:pem" / "content:jwt" -> { block, files:Set }
const add = (key, block, file) => {
  if (!byClass.has(key)) byClass.set(key, { block, files: new Set() });
  byClass.get(key).files.add(file);
};

let nFiles = 0;
let nText = 0;
let nBinary = 0;
let nTooBig = 0;
const wordFiles = REPORT_WORDS.map(() => 0);

if (namesFrom) {
  const names = fs.readFileSync(namesFrom, 'utf8').split('\n').map((s) => s.replace(/\r$/, '')).filter(Boolean);
  for (const n of names) {
    nFiles++;
    for (const id of nameHits(n.replace(/^\.\//, ''), strict)) add(`name:${id}`, true, n);
  }
} else {
  const root = path.resolve(dir);
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const full = path.join(d, e.name);
      const rel = path.relative(root, full).split(path.sep).join('/');
      if (e.isDirectory()) {
        for (const id of nameHits(rel + '/', strict)) add(`name:${id}`, true, rel + '/');
        walk(full);
        continue;
      }
      if (!e.isFile() && !e.isSymbolicLink()) continue;
      nFiles++;
      for (const id of nameHits(rel, strict)) add(`name:${id}`, true, rel);
      let st;
      try { st = fs.statSync(full); } catch { continue; }
      if (st.size > 64 * 1024 * 1024) { nTooBig++; continue; }
      const fd = fs.openSync(full, 'r');
      const head = Buffer.alloc(Math.min(8192, st.size));
      fs.readSync(fd, head, 0, head.length, 0);
      fs.closeSync(fd);
      if (head.includes(0)) { nBinary++; continue; }
      nText++;
      const text = fs.readFileSync(full, 'utf8');
      for (const h of contentHits(text)) add(`content:${h.id}`, h.block && !(lenient && LENIENT_WARN.has(h.id)), rel);
      REPORT_WORDS.forEach((re, i) => { if (re.test(text)) wordFiles[i]++; });
    }
  };
  walk(root);
}

let blocked = 0;
const lines = [];
for (const [key, v] of [...byClass.entries()].sort()) {
  const files = [...v.files].sort();
  if (v.block) blocked += files.length;
  lines.push(`${v.block ? 'BLOCK' : 'warn '} ${key}: ${files.length} file(s)`);
  for (const f of files.slice(0, 20)) lines.push(`        ${f}`);
  if (files.length > 20) lines.push(`        ... and ${files.length - 20} more`);
}
console.log(`[kit-scan] ${label}: ${nFiles} entries${namesFrom ? ' (names only)' : `, ${nText} text, ${nBinary} binary${nTooBig ? `, ${nTooBig} over 64 MiB not read` : ''}`}; ${blocked} blocking hit(s)`);
for (const l of lines) console.log(l);
if (flags.has('--words')) {
  console.log(`[kit-scan] ${label}: files mentioning token=${wordFiles[0]} secret=${wordFiles[1]} password=${wordFiles[2]} (prose; the blocking rules above look for assigned values)`);
}
process.exit(blocked > 0 ? 1 : 0);
