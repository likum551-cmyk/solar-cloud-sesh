#!/usr/bin/env node
// Merge the kit's settings.json into the VPS's own, key by key. Runs on the VPS from install.sh (needs node 18 or newer).
//
//   node merge-settings.mjs KIT.json TARGET.json OUT.json
//
// Rules: a key the VPS file lacks is added. Objects merge recursively. Arrays (permission rules, hook groups) are united:
// everything the VPS file has stays, in its order, and kit entries it lacks are appended. A scalar the VPS file already
// has is KEPT (the VPS wins, so a login-time or hand-made choice is never undone). Nothing is ever removed.
// Prints "CHANGED" or "UNCHANGED" as its first line, then key PATHS only (never values). OUT.json is written only on CHANGED.
// Exit codes: 0 ok, 1 usage or unreadable kit file, 3 TARGET is not valid JSON (nothing written; the caller leaves it alone).
import fs from 'node:fs';

const [kitFile, targetFile, outFile] = process.argv.slice(2);
if (!kitFile || !targetFile || !outFile) { console.error('usage: merge-settings.mjs KIT.json TARGET.json OUT.json'); process.exit(1); }

const read = (f) => JSON.parse(fs.readFileSync(f, 'utf8'));
let kit;
try { kit = read(kitFile); } catch (e) { console.error(`merge-settings: kit file unreadable (${e.message.split('\n')[0]})`); process.exit(1); }
let target;
try { target = read(targetFile); } catch (e) { console.error(`merge-settings: ${targetFile} is not valid JSON (${e.message.split('\n')[0]})`); process.exit(3); }

const isObj = (x) => x !== null && typeof x === 'object' && !Array.isArray(x);
const added = [];
const keptExisting = [];
const appended = [];

function merge(t, k, p) {
  if (isObj(t) && isObj(k)) {
    const out = { ...t };
    for (const key of Object.keys(k)) {
      const kp = p ? `${p}.${key}` : key;
      if (!(key in t)) { out[key] = k[key]; added.push(kp); }
      else out[key] = merge(t[key], k[key], kp);
    }
    return out;
  }
  if (Array.isArray(t) && Array.isArray(k)) {
    const seen = new Set(t.map((x) => JSON.stringify(x)));
    const out = [...t];
    for (const x of k) {
      const s = JSON.stringify(x);
      if (!seen.has(s)) { seen.add(s); out.push(x); appended.push(p); }
    }
    return out;
  }
  if (JSON.stringify(t) !== JSON.stringify(k)) keptExisting.push(p);
  return t;
}

const merged = merge(isObj(target) ? target : {}, kit, '');
const changed = JSON.stringify(merged) !== JSON.stringify(target);
console.log(changed ? 'CHANGED' : 'UNCHANGED');
if (added.length) console.log(`added: ${added.join(', ')}`);
if (appended.length) {
  const counts = {};
  for (const p of appended) counts[p] = (counts[p] || 0) + 1;
  console.log(`appended to arrays: ${Object.entries(counts).map(([p, n]) => `${p} (+${n})`).join(', ')}`);
}
if (keptExisting.length) console.log(`kept the VPS value (differs from the kit): ${keptExisting.join(', ')}`);
if (changed) fs.writeFileSync(outFile, JSON.stringify(merged, null, 2) + '\n', { encoding: 'utf8' });
