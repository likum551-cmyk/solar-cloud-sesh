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
  it('finds the references and the files it checks (the hero reel among them)', () => {
    const r = refs();
    expect(r.size).toBeGreaterThanOrEqual(10);
    expect(r.has('hero.webm')).toBe(true);
    expect(r.has('hero.mp4')).toBe(true);
    expect(PRESENT.has('hero.webm')).toBe(true);
  });

  it('names only files that exist in public/media', () => {
    const missing = [...refs()].filter(([name]) => !PRESENT.has(name)).map(([n, from]) => `${n} (${from.join(', ')})`);
    expect(missing).toEqual([]);
  });

  // GI, 2026-11-06: "too many videos and it's a bit laggy": the hero is the one moving shot
  it('plays one video, the hero, with a VP9 WebM source before its H.264 MP4 and a poster', () => {
    const videos = [...indexHtml.matchAll(/<video\b[^>]*>([\s\S]*?)<\/video>/g)];
    expect(videos.length).toBe(1);
    expect(videos[0]![0]).toMatch(/class="hero__video\b/);
    for (const v of videos) {
      const tag = v[0];
      expect(tag).toMatch(/poster="\/media\/[a-z]+\.webp"/);
      expect(tag).not.toMatch(/<video\b[^>]*\ssrc=/);
      const webm = tag.indexOf('type="video/webm; codecs=vp9"');
      const mp4 = tag.indexOf('type="video/mp4"');
      expect(webm).toBeGreaterThan(0);
      expect(mp4).toBeGreaterThan(webm);
    }
  });
});
