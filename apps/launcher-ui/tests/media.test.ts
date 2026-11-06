import { describe, expect, it } from 'vitest';
import { createApi } from '../src/lib/api';
import { createMockFetch } from '../src/lib/mock-api';
import type { Product } from '../src/lib/schemas';
import { canPlayLoops, CARD_STILLS, CLIPS, clipThumbUrl, clipUrl, posterUrl, productStill, QUESTS_REEL, reelFor, REEL_FADE_MS, serverArt, SIGNATURE_CLIPS, STILLS, stillUrl, thumbUrl } from '../src/media/catalog';

const srv = (name: string, tags: string[] = []) => ({ name, tags });
const prod = (kind: Product['kind'], grants: Product['grants']): Pick<Product, 'kind' | 'grants'> => ({ kind, grants });

describe('serverArt', () => {
  it('picks the footage from the server name', () => {
    expect(serverArt(srv('Downtown Life'), 9)).toEqual({ clip: 'skyline' });
    expect(serverArt(srv('Harbor Roleplay', ['roleplay']), 9)).toEqual({ clip: 'beach' });
    expect(serverArt(srv('Harbour Life'), 9)).toEqual({ clip: 'beach' });
    expect(serverArt(srv('Freeroam', ['freeroam']), 9)).toEqual({ clip: 'canal' });
    expect(serverArt(srv('Night Racing', ['racing']), 9)).toEqual({ clip: 'observatory' });
    expect(serverArt(srv('Street Racing', ['racing']), 9)).toEqual({ clip: 'streets' });
    expect(serverArt(srv('Vinewood Roleplay', ['roleplay']), 9)).toEqual({ clip: 'vinewood' });
    expect(serverArt(srv('Pool Party'), 9)).toEqual({ clip: 'pool' });
  });

  it('reads the tags when the name says nothing', () => {
    expect(serverArt(srv('Alpha One', ['drift']), 0)).toEqual({ clip: 'streets' });
    expect(serverArt(srv('Alpha Two', ['sandbox']), 0)).toEqual({ clip: 'canal' });
    expect(serverArt(srv('Alpha Three', ['roleplay']), 0)).toEqual({ clip: 'boulevard' });
  });

  it('matches whole words for short keywords', () => {
    // "support" and "airport" contain "port", "solo" and "console" contain "sol": none of them is a
    // port or SOL; they fall through to rotation.
    expect(serverArt(srv('Support Test'), 0)).toEqual({ clip: 'skyline' });
    expect(serverArt(srv('Support Test'), 2)).toEqual({ clip: 'vinewood' });
    expect(serverArt(srv('Console Solo'), 1)).toEqual({ clip: 'canal' });
    expect(serverArt(srv('Port Side'), 2)).toEqual({ clip: 'beach' });
  });

  it('rotates unmatched servers by their place in the list, wrapping, never undefined and never a Solar RP server shot', () => {
    const got = [0, 1, 2, 3, 4, 5, -1].map((i) => serverArt(srv('Alpha'), i).clip);
    expect(got).toEqual(['skyline', 'canal', 'vinewood', 'boulevard', 'skyline', 'canal', 'boulevard']);
    for (const c of got) expect(SIGNATURE_CLIPS as readonly string[]).not.toContain(c);
  });

  it('gives SOL the beach at sunset, LUNA the observatory and TERRA the streets', async () => {
    const api = createApi({ baseUrl: 'mock://platform', getToken: () => null, fetchImpl: createMockFetch() });
    const { servers } = await api.servers();
    expect(servers.map((s, i) => [s.name, serverArt(s, i).clip])).toEqual([['SOL', 'beach'], ['LUNA', 'observatory'], ['TERRA', 'streets']]);
  });
});

describe('reelFor', () => {
  it('leads with the clip, holds two or three different shots, and never dissolves a shot into itself', () => {
    expect(reelFor('skyline')).toEqual(['skyline', 'canal']);   // sign-in: no Vinewood girders behind the card
    expect(reelFor('beach')).toEqual(['beach', 'skyline']);   // SOL: two low dusk horizons
    expect(reelFor('streets')).toEqual(['streets', 'pool']);
    // Quests' own reel: the dusk city, then the beach; no girders, haze or billboards behind the list.
    expect(QUESTS_REEL).toEqual(['skyline', 'beach']);
    for (const c of CLIPS) {
      const r = reelFor(c);
      expect(r[0]).toBe(c);
      expect(r.length).toBeGreaterThanOrEqual(2);
      expect(r.length).toBeLessThanOrEqual(3);
      expect(new Set(r).size).toBe(r.length);
      for (const shot of r) expect(CLIPS as readonly string[]).toContain(shot);
    }
  });

  it('never plays the signature shot of another server (SOL beach, LUNA observatory, TERRA streets)', () => {
    expect([...SIGNATURE_CLIPS].sort()).toEqual(['beach', 'observatory', 'streets']);
    for (const c of CLIPS) {
      const borrowed = reelFor(c).slice(1).filter((s) => (SIGNATURE_CLIPS as readonly string[]).includes(s));
      expect(borrowed, c).toEqual([]);
    }
  });

  it('dissolves for longer than a blink (GI: no hard cut, no 0.8 s dissolve)', () => {
    expect(REEL_FADE_MS).toBeGreaterThanOrEqual(1500);
  });
});

describe('productStill', () => {
  it('maps each product to a GTA V still by its grant, then its kind', () => {
    expect(productStill(prod('cosmetic', [{ type: 'item', key: 'vehicle.vanta_gt', quantity: 1 }]))).toBe('vehicle');
    expect(productStill(prod('cosmetic', [{ type: 'item', key: 'outfit.field_jacket', quantity: 1 }]))).toBe('outfit');
    expect(productStill(prod('cosmetic', [{ type: 'item', key: 'cosmetic.glow_trim', quantity: 1 }]))).toBe('item');
    expect(productStill(prod('vip', [{ type: 'entitlement', key: 'vip', durationDays: 30 }]))).toBe('vip');
    expect(productStill(prod('currency_pack', [{ type: 'wallet_credit', currency: 'coins', amount: 5000 }]))).toBe('coins');
  });

  it('only ever answers with a still that has a card thumbnail', () => {
    const answers = new Set([
      productStill(prod('cosmetic', [])),
      productStill(prod('vip', [])),
      productStill(prod('currency_pack', [])),
      productStill(prod('cosmetic', [{ type: 'item', key: 'unknown.thing', quantity: 1 }])),
    ]);
    expect(answers.size).toBeGreaterThan(0);
    for (const a of answers) expect(CARD_STILLS as readonly string[]).toContain(a);
  });
});

describe('asset URLs', () => {
  it('point under assets/ with the shipped layout', () => {
    expect(clipUrl('skyline')).toMatch(/assets\/video\/skyline\.webm$/);
    expect(posterUrl('skyline')).toMatch(/assets\/video\/skyline\.webp$/);
    expect(clipThumbUrl('skyline')).toMatch(/assets\/video\/thumb\/skyline\.webp$/);
    expect(stillUrl('vip')).toMatch(/assets\/stills\/vip\.webp$/);
    expect(thumbUrl('vip')).toMatch(/assets\/stills\/thumb\/vip\.webp$/);
    expect(CLIPS.length).toBe(8);
    expect(STILLS.length).toBe(7);
    expect(CARD_STILLS.length).toBe(5);
  });

  it('every loop, poster, card thumbnail and still the UI can ask for is on disk', () => {
    // The keys of a lazy glob are the files on disk; nothing is loaded.
    const onDisk = new Set(Object.keys(import.meta.glob('../public/assets/**/*.{webm,webp}')).map((k) => k.slice('../public/assets/'.length)));
    const rel = (u: string) => u.slice(u.indexOf('assets/') + 'assets/'.length);
    const files = [
      ...CLIPS.flatMap((c) => [clipUrl(c), posterUrl(c), clipThumbUrl(c)]),
      ...STILLS.map(stillUrl),
      ...CARD_STILLS.map(thumbUrl),
    ].map(rel);
    expect(files.length).toBe(8 * 3 + 7 + 5);
    expect(onDisk.size).toBeGreaterThanOrEqual(files.length);
    const missing = files.filter((f) => !onDisk.has(f));
    expect(missing).toEqual([]);
  });

  it('ships nothing the UI cannot ask for', () => {
    const onDisk = Object.keys(import.meta.glob('../public/assets/**/*.{mp4,webm,webp,avif,jpg,png,glb,hdr}')).map((k) => k.slice('../public/assets/'.length));
    const rel = (u: string) => u.slice(u.indexOf('assets/') + 'assets/'.length);
    const wanted = new Set([
      ...CLIPS.flatMap((c) => [clipUrl(c), posterUrl(c), clipThumbUrl(c)]),
      ...STILLS.map(stillUrl),
      ...CARD_STILLS.map(thumbUrl),
    ].map(rel));
    expect(onDisk.length).toBeGreaterThan(0);
    expect(onDisk.filter((f) => !wanted.has(f))).toEqual([]);
  });

  it('reports no loop playback where there is no DOM (posters only)', () => {
    expect(canPlayLoops()).toBe(false);
  });
});
