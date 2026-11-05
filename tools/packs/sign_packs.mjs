#!/usr/bin/env node
// Publishes car packs for the launcher's download (apps/launcher/pack_sync.h, docs/UPDATES.md "Car packs").
//
//   node tools/packs/sign_packs.mjs --key <private pem> --pub <update-signing-pub.txt> --root <packs folder>
//        --blobs <web dir for the files> --out <packs.json> [--dry-run]
//
// <root> holds one folder per pack (each with a dlc.rpf at its top: tools/packs/stage_packs.mjs output). Every file is
// copied into <blobs>/<sha256> (content-addressed: an existing blob is never rewritten) BEFORE the manifest is written,
// so a launcher never sees a manifest whose files are not served yet. The manifest is signed with the update key,
// verified against <pub> (the key every launcher has baked in), written to <out>.tmp and renamed over <out>.
// Names and paths follow the launcher's rules exactly; anything else stops the run (nothing is skipped silently).
// Never prints the private key. Exit code != 0 on any error.
import { createHash, createPrivateKey, createPublicKey, sign, verify } from 'node:crypto';
import { createReadStream, readFileSync, writeFileSync, renameSync, readdirSync, statSync, lstatSync, existsSync,
         copyFileSync, mkdirSync, chmodSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';

function die(msg) { process.stderr.write(`sign_packs: ${msg}\n`); process.exit(1); }

const opt = { dryRun: false };
const args = process.argv.slice(2);
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  const next = () => { const v = args[++i]; if (v === undefined) die(`${a} needs a value`); return v; };
  if (a === '--key') opt.key = next();
  else if (a === '--pub') opt.pub = next();
  else if (a === '--root') opt.root = next();
  else if (a === '--blobs') opt.blobs = next();
  else if (a === '--out') opt.out = next();
  else if (a === '--dry-run') opt.dryRun = true;
  else die(`unknown argument "${a}"`);
}
for (const k of ['key', 'pub', 'root', 'blobs', 'out']) if (!opt[k]) die(`--${k} is required`);

// ---- the launcher's rules (pack_sync.cpp ValidSegment / ValidPackName / ValidRelPath) -----------------------
const DEVICE = /^(con|prn|aux|nul|conin\$|conout\$|com[0-9]|lpt[0-9])$/i;
function validSegment(s, max) {
  if (!s || s.length > max || s.startsWith('.') || s.endsWith('.')) return false;
  if (!/^[A-Za-z0-9_.-]+$/.test(s)) return false;
  return !DEVICE.test(s.split('.')[0]);
}
const validPackName = (n) => validSegment(n, 64);
function validRelPath(p) {
  if (!p || p.length > 200) return false;
  const segs = p.split('/');
  return segs.length <= 8 && segs.every((s) => validSegment(s, 100));
}
const LIMITS = { packs: 4096, files: 100000, fileBytes: 2 * 1024 ** 3, totalBytes: 128 * 1024 ** 3 };

// ---- walk ------------------------------------------------------------------------------------------------------
function listFiles(dir, prefix, out) {
  for (const name of readdirSync(dir).sort()) {
    if (name.startsWith('.')) continue;  // the launcher ignores dot-names too
    const full = join(dir, name);
    const st = lstatSync(full);
    const rel = prefix ? `${prefix}/${name}` : name;
    if (st.isSymbolicLink()) die(`${full} is a symbolic link; packs must be plain files`);
    if (st.isDirectory()) listFiles(full, rel, out);
    else if (st.isFile()) out.push(rel);
  }
}

function sha256File(path) {
  return new Promise((resolve, reject) => {
    const h = createHash('sha256');
    createReadStream(path).on('data', (d) => h.update(d)).on('end', () => resolve(h.digest('hex'))).on('error', reject);
  });
}

if (!existsSync(opt.root) || !statSync(opt.root).isDirectory()) die(`--root ${opt.root} is not a folder`);
const packNames = readdirSync(opt.root).filter((n) => !n.startsWith('.') && statSync(join(opt.root, n)).isDirectory()).sort();
const packs = [];
const seenNames = new Set();
let totalBytes = 0;
let totalFiles = 0;
for (const name of packNames) {
  if (!existsSync(join(opt.root, name, 'dlc.rpf'))) die(`${name}: no dlc.rpf at the top of the folder (not a pack)`);
  if (!validPackName(name)) die(`pack folder name "${name}" breaks the launcher's rules (1..64 of A-Za-z0-9_.-, no leading dot)`);
  if (seenNames.has(name.toLowerCase())) die(`two packs named "${name}" (case-insensitively)`);
  seenNames.add(name.toLowerCase());
  const rels = [];
  listFiles(join(opt.root, name), '', rels);
  const seenPaths = new Set();
  const files = [];
  for (const rel of rels) {
    if (!validRelPath(rel)) die(`${name}/${rel}: the path breaks the launcher's rules (A-Za-z0-9_.- segments, no leading dot, at most 8 deep)`);
    if (seenPaths.has(rel.toLowerCase())) die(`${name}/${rel}: two files with this path (case-insensitively)`);
    seenPaths.add(rel.toLowerCase());
    const full = join(opt.root, name, ...rel.split('/'));
    const size = statSync(full).size;
    if (size > LIMITS.fileBytes) die(`${name}/${rel}: ${size} bytes is over the 2 GiB per-file limit`);
    const sha256 = await sha256File(full);
    if (statSync(full).size !== size) die(`${name}/${rel} changed while hashing`);
    files.push({ path: rel, size, sha256, full });
    totalBytes += size;
    if (++totalFiles > LIMITS.files) die('more than 100000 files');
  }
  packs.push({ name, files });
}
if (packs.length === 0) die(`no packs under ${opt.root} (a pack is a folder with a dlc.rpf)`);
if (packs.length > LIMITS.packs) die('more than 4096 packs');
if (totalBytes > LIMITS.totalBytes) die('more than 128 GiB in all');

// ---- blobs first -----------------------------------------------------------------------------------------------
let copied = 0;
let copiedBytes = 0;
if (!opt.dryRun) mkdirSync(opt.blobs, { recursive: true });
for (const p of packs) {
  for (const f of p.files) {
    const dest = join(opt.blobs, f.sha256);
    if (existsSync(dest)) {
      if (statSync(dest).size !== f.size) die(`${dest} exists with a different size: the blob store is damaged`);
      continue;
    }
    copied++;
    copiedBytes += f.size;
    if (opt.dryRun) continue;
    const tmp = `${dest}.tmp${process.pid}`;
    copyFileSync(f.full, tmp);
    if ((await sha256File(tmp)) !== f.sha256) { unlinkSync(tmp); die(`${p.name}/${f.path} changed while copying`); }
    chmodSync(tmp, 0o644);
    renameSync(tmp, dest);
  }
}

// ---- sign, check against the launchers' key, write -------------------------------------------------------------
const payload = {
  v: 1,
  kind: 'packs',
  issued: new Date().toISOString(),
  packs: packs.map((p) => ({ name: p.name, files: p.files.map(({ path, size, sha256 }) => ({ path, size, sha256 })) })),
};
const payloadBytes = Buffer.from(JSON.stringify(payload), 'utf8');
if (payloadBytes.length > 16 * 1024 * 1024 - 4096) die('packs.json would exceed the launcher\'s 16 MiB limit');
const key = createPrivateKey(readFileSync(opt.key));
if (key.asymmetricKeyType !== 'ec' || key.asymmetricKeyDetails?.namedCurve !== 'prime256v1') die('key must be an ECDSA P-256 private key');
const jwk = createPublicKey(key).export({ format: 'jwk' });
const raw = Buffer.concat([Buffer.from(jwk.x, 'base64url'), Buffer.from(jwk.y, 'base64url')]).toString('base64');
const baked = readFileSync(opt.pub, 'utf8').trim();
if (raw !== baked) die(`the private key does not belong to the public key in ${opt.pub}: launchers would reject this manifest`);
const sig = sign('sha256', payloadBytes, { key, dsaEncoding: 'ieee-p1363' });
if (sig.length !== 64) die(`unexpected signature length ${sig.length}`);
const pubKey = createPublicKey({ key: { kty: 'EC', crv: 'P-256', x: jwk.x, y: jwk.y }, format: 'jwk' });
if (!verify('sha256', payloadBytes, { key: pubKey, dsaEncoding: 'ieee-p1363' }, sig)) die('the fresh signature does not verify');

const mb = (n) => (n / 1048576).toFixed(1);
const summary = `${packs.length} pack(s), ${totalFiles} file(s), ${mb(totalBytes)} MiB; ${copied} new blob(s) (${mb(copiedBytes)} MiB)`;
if (opt.dryRun) {
  process.stdout.write(`[dry-run] would write ${opt.out}: ${summary}\n`);
  for (const p of packs) process.stdout.write(`  ${p.name}: ${p.files.length} file(s)\n`);
  process.exit(0);
}
const manifest = { signed: payloadBytes.toString('base64'), sig: sig.toString('base64') };
const tmp = `${opt.out}.tmp`;
writeFileSync(tmp, JSON.stringify(manifest) + '\n');
chmodSync(tmp, 0o644);
renameSync(tmp, opt.out);
process.stdout.write(`signed ${opt.out}: ${summary}\n`);
for (const p of packs) process.stdout.write(`  ${p.name}: ${p.files.length} file(s)\n`);
