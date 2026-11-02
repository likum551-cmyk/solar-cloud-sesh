#!/usr/bin/env node
// Checks a signed manifest the way the launcher does: signature over the "signed" bytes with the
// baked public key, then (optionally) the installer's size and SHA-256. CI runs it after sign.mjs.
//
//   node tools/update-sign/verify.mjs --pub <base64 X||Y | path to update-signing-pub.txt>
//        --manifest <version.json> [--installer <setup.exe>]
import { createHash, createPublicKey, verify } from 'node:crypto';
import { readFileSync, existsSync } from 'node:fs';

function die(msg) { process.stderr.write(`verify: FAIL ${msg}\n`); process.exit(1); }

const opt = {};
const args = process.argv.slice(2);
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  const v = args[++i];
  if (v === undefined) die(`${a} needs a value`);
  if (a === '--pub') opt.pub = v;
  else if (a === '--manifest') opt.manifest = v;
  else if (a === '--installer') opt.installer = v;
  else die(`unknown argument "${a}"`);
}
if (!opt.pub || !opt.manifest) die('--pub and --manifest are required');
const pubB64 = existsSync(opt.pub) ? readFileSync(opt.pub, 'utf8').trim() : opt.pub;
const raw = Buffer.from(pubB64, 'base64');
if (raw.length !== 64) die(`public key must be 64 bytes (X||Y), got ${raw.length}`);
const key = createPublicKey({
  key: { kty: 'EC', crv: 'P-256', x: raw.subarray(0, 32).toString('base64url'), y: raw.subarray(32).toString('base64url') },
  format: 'jwk',
});

const m = JSON.parse(readFileSync(opt.manifest, 'utf8'));
if (typeof m.signed !== 'string' || typeof m.sig !== 'string') die('manifest has no "signed"/"sig"');
const payloadBytes = Buffer.from(m.signed, 'base64');
const sig = Buffer.from(m.sig, 'base64');
if (sig.length !== 64) die(`signature must be 64 bytes, got ${sig.length}`);
if (!verify('sha256', payloadBytes, { key, dsaEncoding: 'ieee-p1363' }, sig)) die('signature does not verify');
const p = JSON.parse(payloadBytes.toString('utf8'));
if (p.v !== 1) die(`unknown payload format v=${p.v}`);
if (m.version !== p.version || m.url !== p.url) die('unsigned display fields disagree with the signed payload');
if (opt.installer) {
  const bytes = readFileSync(opt.installer);
  if (bytes.length !== p.size) die(`installer size ${bytes.length} != signed ${p.size}`);
  const h = createHash('sha256').update(bytes).digest('hex');
  if (h !== p.sha256) die(`installer sha256 ${h} != signed ${p.sha256}`);
}
process.stdout.write(`verify: OK ${p.version} ${p.url} sha256 ${p.sha256} size ${p.size} mandatory ${p.mandatory}\n`);
