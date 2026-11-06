// Copies the account pages' GTA V stills into public/media from where the project already keeps them
// (one copy of each file in the repo; provenance stays with the source folder):
//   apps/launcher-ui/public/assets   the launcher's store stills (see its ASSETS.md)
// Fails loudly when a file is missing. Skips files that are already identical.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SITE = path.resolve(HERE, '..');
const REPO = path.resolve(SITE, '..', '..');
const OUT = path.join(SITE, 'public', 'media');
const LAUNCHER = path.join(REPO, 'apps', 'launcher-ui', 'public', 'assets');

// The landing page's media is the site's own encode (scripts/encode-media.mjs: the hero loop, the graded
// stills and the key-art crops, committed in public/media because their sources are not in the repo); this
// script only checks it is there. The account pages' stills come from the launcher.
const FILES = [
  [LAUNCHER, 'video/vinewood.webp', 'vinewood.webp'],
  [LAUNCHER, 'stills/downtown.webp', 'downtown.webp'],
  [LAUNCHER, 'stills/settings.webp', 'dusk.webp'],
];

fs.mkdirSync(OUT, { recursive: true });
let copied = 0;
let same = 0;
const missing = [];
for (const [root, rel, name] of FILES) {
  const src = path.join(root, rel);
  const dst = path.join(OUT, name);
  if (!fs.existsSync(src)) { missing.push(src); continue; }
  const a = fs.readFileSync(src);
  if (fs.existsSync(dst) && Buffer.compare(a, fs.readFileSync(dst)) === 0) { same++; continue; }
  fs.writeFileSync(dst, a);
  copied++;
}
if (missing.length) {
  console.error(`media: ${missing.length} source file(s) missing:\n  ${missing.join('\n  ')}`);
  process.exit(1);
}
if (copied + same !== FILES.length) { console.error('media: count mismatch'); process.exit(1); }
// the site's own encodes must be there too (a fresh checkout has them; encode-media.mjs remakes them)
const OWN = [
  'hero.webm', 'hero.mp4', 'hero.webp',
  ...['skyline', 'beach', 'pool', 'canal', 'close'].map((n) => `still-${n}.webp`),
  ...['michael', 'franklin', 'trevor'].map((n) => `cast-${n}.webp`),
  ...['sol', 'luna', 'terra'].map((n) => `server-${n}.webp`),
  // the HUD plates: crops of a render of the client's own overlay code (ASSETS.md); not remade by encode-media.mjs
  'hud-ring.webp', 'hud-car.webp',
];
const ownMissing = OWN.filter((f) => !fs.existsSync(path.join(OUT, f)));
if (ownMissing.length) {
  console.error(`media: ${ownMissing.length} encoded file(s) missing from public/media (run node scripts/encode-media.mjs):\n  ${ownMissing.join('\n  ')}`);
  process.exit(1);
}
console.log(`media: ${copied} copied, ${same} unchanged, ${FILES.length} total -> ${path.relative(SITE, OUT)}`);
