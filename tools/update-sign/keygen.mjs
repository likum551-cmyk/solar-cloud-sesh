#!/usr/bin/env node
// Generates the launcher-update signing key pair (ECDSA P-256, Node's built-in crypto, no deps).
//
//   node tools/update-sign/keygen.mjs --out-dir <dir> [--force]
//
// Writes <dir>/update-signing-key.pem (PKCS#8 private key, mode 0600 where the OS honours it) and
// <dir>/update-signing-pub.txt (base64 of the raw 64-byte X||Y public point). Prints the public key.
// The public key is baked into SolarLauncher.exe: cmake -DGAGE_UPDATE_PUBKEY=<base64> build/win
// The PRIVATE key never goes into the repo, a backup tarball or chat. Losing it means a launcher
// with a new baked key must be distributed by hand; leaking it lets anyone push code to every player.
import { generateKeyPairSync } from 'node:crypto';
import { existsSync, mkdirSync, writeFileSync, chmodSync } from 'node:fs';
import { join, resolve } from 'node:path';

function die(msg) { process.stderr.write(`keygen: ${msg}\n`); process.exit(1); }

const args = process.argv.slice(2);
let outDir = '';
let force = false;
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--out-dir') outDir = args[++i] ?? '';
  else if (args[i] === '--force') force = true;
  else die(`unknown argument "${args[i]}" (usage: --out-dir <dir> [--force])`);
}
if (!outDir) die('--out-dir <dir> is required');
outDir = resolve(outDir);
const keyPath = join(outDir, 'update-signing-key.pem');
const pubPath = join(outDir, 'update-signing-pub.txt');
if (existsSync(keyPath) && !force) die(`${keyPath} exists; refusing to overwrite without --force (the old key may already be baked into shipped launchers)`);

mkdirSync(outDir, { recursive: true });
const { privateKey, publicKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
const jwk = publicKey.export({ format: 'jwk' });
const x = Buffer.from(jwk.x, 'base64url');
const y = Buffer.from(jwk.y, 'base64url');
if (x.length !== 32 || y.length !== 32) die('unexpected public key size');
const pubB64 = Buffer.concat([x, y]).toString('base64');

writeFileSync(keyPath, privateKey.export({ format: 'pem', type: 'pkcs8' }), { mode: 0o600 });
try { chmodSync(keyPath, 0o600); } catch (e) { process.stderr.write(`keygen: chmod 600 failed: ${e.message}\n`); }
writeFileSync(pubPath, pubB64 + '\n');
process.stdout.write(`private key: ${keyPath}\npublic key (bake with -DGAGE_UPDATE_PUBKEY=...): ${pubB64}\n`);
