#!/usr/bin/env node
// Make a Linux-safe copy of ~/.claude/settings.json for the VPS kit. Runs on GI's PC from tools/deploy/pack-vps-kit.sh.
//
//   node sanitize-settings.mjs IN.json OUT.json REPORT.txt [--keep-env NAME,NAME]
//
// Kept as they are: permissions, model, effort, statusLine, plugins, UI preferences and every other key.
// Dropped, and listed in REPORT.txt by KEY NAME or PATH only (a value is never printed, logged or written to the report):
//   - the whole `env` block (names are listed). --keep-env NAME,NAME keeps the named entries when the name does not look
//     like a credential and the value is a short plain token such as 1, 0 or 32.
//   - account/credential helpers (apiKeyHelper, awsAuthRefresh, awsCredentialExport, gcpAuthRefresh, otelHeadersHelper, forceLogin*)
//   - any property whose NAME looks like a credential (token, secret, password, api key, ...) and holds a value
//   - any string that looks like a credential (sk-ant-..., ghp_..., a JWT, a private key block, ...)
//   - any hook, status line, permission rule or setting whose string points at Windows (a drive path, .cmd/.bat/.exe/.ps1,
//     powershell, %VAR%, a backslash). Nothing is rewritten: a Windows entry is dropped, never guessed into a Linux one.
// Exit codes: 0 written, 1 input or output problem (including a credential-looking string left in the output), 2 usage.
import fs from 'node:fs';
import { SECRET_KEY_NAME, windowsReason, contentHits } from './kit-patterns.mjs';

const args = process.argv.slice(2);
const files = [];
let keepEnv = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--keep-env') keepEnv = (args[++i] ?? '').split(',').map((s) => s.trim()).filter(Boolean);
  else if (args[i].startsWith('--')) { console.error(`sanitize-settings: unknown option ${args[i]}`); process.exit(2); }
  else files.push(args[i]);
}
if (files.length !== 3) { console.error('usage: sanitize-settings.mjs IN.json OUT.json REPORT.txt [--keep-env NAME,NAME]'); process.exit(2); }
const [inFile, outFile, reportFile] = files;

let settings;
try { settings = JSON.parse(fs.readFileSync(inFile, 'utf8')); } catch (e) {
  console.error(`sanitize-settings: cannot read ${inFile} as JSON (${e.message.split('\n')[0]})`);
  process.exit(1);
}
if (settings === null || typeof settings !== 'object' || Array.isArray(settings)) {
  console.error('sanitize-settings: settings.json is not a JSON object');
  process.exit(1);
}

const dropped = []; // { path, reason }
const kept = [];    // { path, note }
const drop = (p, reason) => dropped.push({ path: p, reason });

// ---- env ------------------------------------------------------------------------------------------------------------
const envNames = settings.env && typeof settings.env === 'object' ? Object.keys(settings.env) : [];
if ('env' in settings) {
  const keptEnv = {};
  for (const name of envNames) {
    const v = settings.env[name];
    const plain = (typeof v === 'string' || typeof v === 'number') && /^[0-9A-Za-z._:-]{1,16}$/.test(String(v));
    if (keepEnv.includes(name) && !SECRET_KEY_NAME.test(name) && plain) {
      keptEnv[name] = v;
      kept.push({ path: `env.${name}`, note: 'kept by --keep-env (plain flag-like value)' });
    } else {
      drop(`env.${name}`, keepEnv.includes(name) ? 'refused by --keep-env (credential-like name or long value)' : 'env value (never copied)');
    }
  }
  for (const n of keepEnv) if (!envNames.includes(n)) console.error(`sanitize-settings: --keep-env ${n}: no such env key`);
  if (Object.keys(keptEnv).length) settings.env = keptEnv; else delete settings.env;
}

// ---- account / credential helpers ---------------------------------------------------------------------------------------
for (const k of ['apiKeyHelper', 'awsAuthRefresh', 'awsCredentialExport', 'gcpAuthRefresh', 'otelHeadersHelper', 'forceLoginMethod', 'forceLoginOrgUUID']) {
  if (k in settings) { delete settings[k]; drop(k, 'account or credential helper'); }
}

// ---- hooks: drop a handler that points at Windows, then any matcher group or event left empty ---------------------------------
const stringsOf = (o) => {
  const out = [];
  const w = (x) => {
    if (typeof x === 'string') out.push(x);
    else if (Array.isArray(x)) x.forEach(w);
    else if (x && typeof x === 'object') Object.values(x).forEach(w);
  };
  w(o);
  return out;
};
const badString = (s) => {
  const w = windowsReason(s);
  if (w) return `points at Windows (${w})`;
  const h = contentHits(s).find((x) => x.block);
  if (h) return `credential-looking string (${h.id})`;
  return null;
};
const badIn = (o) => { for (const s of stringsOf(o)) { const r = badString(s); if (r) return r; } return null; };

if (settings.hooks && typeof settings.hooks === 'object') {
  for (const event of Object.keys(settings.hooks)) {
    const groups = Array.isArray(settings.hooks[event]) ? settings.hooks[event] : [];
    const newGroups = [];
    groups.forEach((g, gi) => {
      const handlers = Array.isArray(g.hooks) ? g.hooks : [];
      const keepHandlers = [];
      handlers.forEach((h, hi) => {
        const why = badIn(h);
        const where = `hooks.${event}[${gi}]${g.matcher ? `(${g.matcher})` : ''}.hooks[${hi}]`;
        if (why) drop(where, why);
        else { keepHandlers.push(h); kept.push({ path: where, note: `type ${h.type ?? '?'}, no Windows path, no credential` }); }
      });
      if (keepHandlers.length) newGroups.push({ ...g, hooks: keepHandlers });
    });
    if (newGroups.length) settings.hooks[event] = newGroups;
    else { delete settings.hooks[event]; }
  }
  if (Object.keys(settings.hooks).length === 0) delete settings.hooks;
}

// ---- statusLine ---------------------------------------------------------------------------------------------------------
if (settings.statusLine) {
  const why = badIn(settings.statusLine);
  if (why) { delete settings.statusLine; drop('statusLine', why); }
  else kept.push({ path: 'statusLine', note: 'no Windows path, no credential' });
}

// ---- everything else: credential-looking names and values, Windows strings ----------------------------------------------------
const isEmptyScalar = (v) => v === '' || v === null || v === false || v === 0;
function prune(node, p) {
  if (Array.isArray(node)) {
    const out = [];
    node.forEach((el, i) => {
      if (typeof el === 'string') {
        const why = badString(el);
        if (why) { drop(`${p}[${i}]`, why); return; }
        out.push(el);
      } else if (el && typeof el === 'object') {
        const r = prune(el, `${p}[${i}]`);
        out.push(r);
      } else out.push(el);
    });
    return out;
  }
  if (node && typeof node === 'object') {
    const out = {};
    for (const [k, v] of Object.entries(node)) {
      const kp = p ? `${p}.${k}` : k;
      const scalar = v === null || typeof v !== 'object';
      if (scalar && SECRET_KEY_NAME.test(k) && !isEmptyScalar(v)) { drop(kp, 'property name looks like a credential'); continue; }
      if (typeof v === 'string') {
        const why = badString(v);
        if (why) { drop(kp, why); continue; }
        out[k] = v;
      } else if (v && typeof v === 'object') out[k] = prune(v, kp);
      else out[k] = v;
    }
    return out;
  }
  return node;
}
// hooks and statusLine were already vetted above; run the generic pass over the rest only
const hooks = settings.hooks; delete settings.hooks;
const statusLine = settings.statusLine; delete settings.statusLine;
const env = settings.env; delete settings.env;
let clean = prune(settings, '');
if (hooks !== undefined) clean = { ...clean, hooks };
if (statusLine !== undefined) clean = { ...clean, statusLine };
if (env !== undefined) clean = { ...clean, env };

// ---- output check: re-parse, and nothing credential-looking may remain ----------------------------------------------------------
const text = JSON.stringify(clean, null, 2) + '\n';
JSON.parse(text);
const left = contentHits(text).filter((h) => h.block);
if (left.length) {
  console.error(`sanitize-settings: output still holds a credential-looking string (${left.map((h) => h.id).join(', ')}); nothing written`);
  process.exit(1);
}
if (text.includes('\r')) { console.error('sanitize-settings: CR in output'); process.exit(1); }
fs.writeFileSync(outFile, text, { encoding: 'utf8' });

const report = [];
report.push('# settings.json sanitised for the Linux VPS (key names and paths only; no values)');
report.push(`kept top-level keys: ${Object.keys(clean).join(', ')}`);
for (const k of kept) report.push(`kept    ${k.path}: ${k.note}`);
for (const d of dropped) report.push(`dropped ${d.path}: ${d.reason}`);
if (envNames.length) report.push(`env names seen: ${envNames.join(', ')}`);
if (!dropped.length) report.push('dropped nothing');
fs.writeFileSync(reportFile, report.join('\n') + '\n', { encoding: 'utf8' });
console.log(report.join('\n'));
