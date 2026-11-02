#!/usr/bin/env node
// Writes a signed launcher-update manifest (docs/UPDATES.md has the format).
//
//   node tools/update-sign/sign.mjs --key <pem> --installer <setup.exe> --version <x.y.z>
//        --url <installer URL> --out <version.json> [--notes <text>] [--mandatory]
//        [--update-url <new manifest URL>]
//
// The installer's SHA-256 and size are computed here. The manifest is written to <out>.tmp and
// renamed over <out>, so a web server never serves a half-written file. Exit code != 0 on any error.
import { createHash, createPrivateKey, sign } from 'node:crypto';
import { readFileSync, writeFileSync, renameSync, statSync } from 'node:fs';

function die(msg) { process.stderr.write(`sign: ${msg}\n`); process.exit(1); }

const opt = { notes: '', mandatory: false, updateUrl: '' };
const args = process.argv.slice(2);
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  const next = () => { const v = args[++i]; if (v === undefined) die(`${a} needs a value`); return v; };
  if (a === '--key') opt.key = next();
  else if (a === '--installer') opt.installer = next();
  else if (a === '--version') opt.version = next();
  else if (a === '--url') opt.url = next();
  else if (a === '--out') opt.out = next();
  else if (a === '--notes') opt.notes = next();
  else if (a === '--update-url') opt.updateUrl = next();
  else if (a === '--mandatory') opt.mandatory = true;
  else die(`unknown argument "${a}"`);
}
for (const k of ['key', 'installer', 'version', 'url', 'out']) if (!opt[k]) die(`--${k} is required`);
if (!/^\d+\.\d+\.\d+$/.test(opt.version)) die(`version "${opt.version}" must be x.y.z`);
const isHttp = (u) => /^https?:\/\/[^\s/?#]+[^\s]*$/.test(u);
if (!isHttp(opt.url)) die(`url "${opt.url}" must be an absolute http(s) URL`);
if (opt.updateUrl && !isHttp(opt.updateUrl)) die(`update-url "${opt.updateUrl}" must be an absolute http(s) URL`);
if (opt.notes.length > 2000) die('notes longer than 2000 characters');

const bytes = readFileSync(opt.installer);
if (bytes.length !== statSync(opt.installer).size || bytes.length === 0) die('installer is empty or changed while reading');
const payload = {
  v: 1,
  version: opt.version,
  url: opt.url,
  sha256: createHash('sha256').update(bytes).digest('hex'),
  size: bytes.length,
  notes: opt.notes,
  mandatory: opt.mandatory,
  updateUrl: opt.updateUrl,
  issued: new Date().toISOString(),
};
const payloadBytes = Buffer.from(JSON.stringify(payload), 'utf8');
const key = createPrivateKey(readFileSync(opt.key));
if (key.asymmetricKeyType !== 'ec' || key.asymmetricKeyDetails?.namedCurve !== 'prime256v1') die('key must be an ECDSA P-256 private key');
const sig = sign('sha256', payloadBytes, { key, dsaEncoding: 'ieee-p1363' });
if (sig.length !== 64) die(`unexpected signature length ${sig.length}`);

// Top-level version/url/notes are UNSIGNED display fields for launchers older than 0.4.0, which
// only read "version" and show "update available". 0.4.0+ trusts nothing outside "signed".
const manifest = {
  version: opt.version,
  url: opt.url,
  notes: opt.notes,
  signed: payloadBytes.toString('base64'),
  sig: sig.toString('base64'),
};
const tmp = `${opt.out}.tmp`;
writeFileSync(tmp, JSON.stringify(manifest, null, 2) + '\n');
renameSync(tmp, opt.out);
process.stdout.write(`signed ${opt.version}: ${payload.size} bytes, sha256 ${payload.sha256} -> ${opt.out}\n`);
