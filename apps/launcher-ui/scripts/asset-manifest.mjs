// Writes public/assets/manifest.json: every file the native host must serve under
// https://app.gage/assets/, with its size and SHA-256. Vite copies it to dist/assets/ with the
// rest, so the installer and the host can verify the folder they ship (NATIVE-INTEGRATION.md).
// Idempotent: the same tree always produces byte-identical output (sorted, no timestamps).

import { createHash } from 'node:crypto';
import { readdir, readFile, stat, writeFile } from 'node:fs/promises';
import { dirname, join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', 'public', 'assets');
const out = join(root, 'manifest.json');

const MIME = {
  '.glb': 'model/gltf-binary',
  '.hdr': 'image/vnd.radiance',
  '.webp': 'image/webp',
  '.avif': 'image/avif',
  '.mp4': 'video/mp4',
  '.webm': 'video/webm',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.ktx2': 'image/ktx2',
  '.json': 'application/json',
  '.woff2': 'font/woff2',
};

async function walk(dir) {
  const entries = await readdir(dir, { withFileTypes: true });
  const files = [];
  for (const e of entries) {
    const p = join(dir, e.name);
    if (e.isDirectory()) files.push(...(await walk(p)));
    else if (e.isFile() && p !== out) files.push(p);
  }
  return files;
}

const files = (await walk(root)).sort();
if (files.length === 0) {
  console.error(`asset-manifest: no files under ${root}`);
  process.exit(1);
}

const entries = [];
let total = 0;
for (const f of files) {
  const rel = relative(root, f).split(sep).join('/');
  const ext = rel.slice(rel.lastIndexOf('.')).toLowerCase();
  const type = MIME[ext];
  if (!type) {
    console.error(`asset-manifest: no MIME type for ${rel}; add "${ext}" to MIME and to NATIVE-INTEGRATION.md`);
    process.exit(1);
  }
  const bytes = (await stat(f)).size;
  const sha256 = createHash('sha256').update(await readFile(f)).digest('hex');
  total += bytes;
  entries.push({ path: `assets/${rel}`, bytes, sha256, type });
}

const body = `${JSON.stringify({ version: 1, totalBytes: total, files: entries }, null, 2)}\n`;
await writeFile(out, body, 'utf8');
console.log(`asset-manifest: ${entries.length} files, ${total} bytes -> ${relative(join(here, '..'), out).split(sep).join('/')}`);
