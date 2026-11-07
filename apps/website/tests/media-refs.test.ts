import { describe, expect, it } from 'vitest';
import indexHtml from '../index.html?raw';
import loginHtml from '../login/index.html?raw';
import signupHtml from '../signup/index.html?raw';
import accountHtml from '../account/index.html?raw';
import partialsTs from '../partials.ts?raw';
import mainTs from '../src/main.ts?raw';

// Every /media/ file a page or script names must be in public/media (a launcher clip swap once left
// the site pointing at five deleted loops, and only the build's copy step noticed).
const SOURCES: Record<string, string> = {
  'index.html': indexHtml,
  'login/index.html': loginHtml,
  'signup/index.html': signupHtml,
  'account/index.html': accountHtml,
  'partials.ts': partialsTs,
  'src/main.ts': mainTs,
};
// the file names in public/media (the glob only lists them; nothing is loaded)
const PRESENT = new Set(Object.keys(import.meta.glob('../public/media/*')).map((p) => p.slice(p.lastIndexOf('/') + 1)));

function refs(): Map<string, string[]> {
  const out = new Map<string, string[]>();
  for (const [rel, text] of Object.entries(SOURCES)) {
    for (const m of text.matchAll(/\/media\/([A-Za-z0-9_.-]+)/g)) {
      const name = m[1]!;
      out.set(name, [...(out.get(name) ?? []), rel]);
    }
  }
  return out;
}

describe('media references', () => {
  it('finds the references and the files it checks (the hero still and the reel among them)', () => {
    const r = refs();
    expect(r.size).toBeGreaterThanOrEqual(5); // 2 on the landing page (2026-11-07 launcher product page) + 3 account backdrops
    for (const name of ['still-beach.webp', 'reel-car.webp', 'vinewood.webp', 'downtown.webp']) expect(r.has(name)).toBe(true);
    expect(PRESENT.has('still-beach.webp')).toBe(true);
  });

  // GI, 2026-11-06: the servers "without default gta poster": our own emblems, no key art on the landing page
  it('shows no GTA V key-art poster on the landing page', () => {
    expect(indexHtml).not.toMatch(/\/media\/(server-|cast-|franklin|michael|trevor)/);
  });

  it('names only files that exist in public/media', () => {
    const missing = [...refs()].filter(([name]) => !PRESENT.has(name)).map(([n, from]) => `${n} (${from.join(', ')})`);
    expect(missing).toEqual([]);
  });

  // GI, 2026-11-06: "too many videos, a bit laggy", then "FULL REDESIGN, remove this disgusting 3D S": the
  // landing page is stills only, with no WebGL stage
  it('plays no video and mounts no 3D stage', () => {
    expect(indexHtml).not.toMatch(/<video\b/);
    expect(indexHtml).not.toMatch(/stage3d|data-stage=/);
    expect(mainTs).not.toMatch(/mark3d|from 'three'/);
  });
});
