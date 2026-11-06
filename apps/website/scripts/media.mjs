// Copies the site's GTA V footage and key art into public/media from where the project already keeps
// them (one copy of each file in the repo; provenance stays with the source folders):
//   apps/launcher-ui/public/assets   the launcher's trailer loops and store stills (see its ASSETS.md)
//   docs/archive/web-legacy/src/assets/gta   the official loading-screen splashes (see PROVENANCE.md there)
// Fails loudly when a file is missing. Skips files that are already identical.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SITE = path.resolve(HERE, '..');
const REPO = path.resolve(SITE, '..', '..');
const OUT = path.join(SITE, 'public', 'media');
const LAUNCHER = path.join(REPO, 'apps', 'launcher-ui', 'public', 'assets');
const KEYART = path.join(REPO, 'docs', 'archive', 'web-legacy', 'src', 'assets', 'gta');

// The page's footage is the site's own encode (scripts/encode-media.mjs: graded boomerang loops, committed
// in public/media because their source recordings are not in the repo); this script only checks it is
// there. The account band's still comes from the launcher.
const FILES = [
  [LAUNCHER, 'video/vinewood.webp', 'vinewood.webp'],
  [LAUNCHER, 'stills/vehicle.webp', 'vehicle.webp'],
  [LAUNCHER, 'stills/downtown.webp', 'downtown.webp'],
  [LAUNCHER, 'stills/settings.webp', 'dusk.webp'],
  [KEYART, 'splash-michael.webp', 'michael.webp'],
  [KEYART, 'splash-franklin.webp', 'franklin.webp'],
  [KEYART, 'splash-trevor.webp', 'trevor.webp'],
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
const OWN = ['hero', 'beach', 'streets', 'canal', 'close'].flatMap((r) => [`${r}.webm`, `${r}.mp4`, `${r}.webp`]).concat(['sol.webp']);
const ownMissing = OWN.filter((f) => !fs.existsSync(path.join(OUT, f)));
if (ownMissing.length) {
  console.error(`media: ${ownMissing.length} encoded file(s) missing from public/media (run node scripts/encode-media.mjs):\n  ${ownMissing.join('\n  ')}`);
  process.exit(1);
}
console.log(`media: ${copied} copied, ${same} unchanged, ${FILES.length} total -> ${path.relative(SITE, OUT)}`);
