import { describe, expect, it } from 'vitest';
import { createApi } from '../src/lib/api';
import { createMockFetch } from '../src/lib/mock-api';
import type { Product } from '../src/lib/schemas';
import { canPlayLoops, CARD_STILLS, CLIPS, clipThumbUrl, clipUrl, posterUrl, productStill, serverArt, STILLS, stillUrl, thumbUrl } from '../src/media/catalog';

const srv = (name: string, tags: string[] = []) => ({ name, tags });
const prod = (kind: Product['kind'], grants: Product['grants']): Pick<Product, 'kind' | 'grants'> => ({ kind, grants });

describe('serverArt', () => {
  it('picks the footage from the server name', () => {
    expect(serverArt(srv('Downtown Roleplay', ['roleplay']), 9)).toEqual({ clip: 'city' });
    expect(serverArt(srv('Harbor Roleplay', ['roleplay']), 9)).toEqual({ clip: 'harbor' });
    expect(serverArt(srv('Harbour Life'), 9)).toEqual({ clip: 'harbor' });
    expect(serverArt(srv('Freeroam', ['freeroam']), 9)).toEqual({ clip: 'wild' });
    expect(serverArt(srv('Night Racing', ['racing']), 9)).toEqual({ clip: 'night' });
    expect(serverArt(srv('Vinewood Roleplay', ['roleplay']), 9)).toEqual({ clip: 'vinewood' });
  });

  it('reads the tags when the name says nothing', () => {
    expect(serverArt(srv('Alpha One', ['drift']), 0)).toEqual({ clip: 'night' });
    expect(serverArt(srv('Alpha Two', ['sandbox']), 0)).toEqual({ clip: 'wild' });
  });

  it('matches whole words for short keywords', () => {
    // "support" and "airport" contain "port" but are not a port; they fall through to rotation.
    expect(serverArt(srv('Support Test'), 0)).toEqual({ clip: 'city' });
    expect(serverArt(srv('Support Test'), 2)).toEqual({ clip: 'wild' });
    expect(serverArt(srv('Port Side'), 2)).toEqual({ clip: 'harbor' });
  });

  it('rotates unmatched servers by their place in the list, wrapping and never undefined', () => {
    const got = [0, 1, 2, 3, 4, 5, -1].map((i) => serverArt(srv('Alpha'), i).clip);
    expect(got).toEqual(['city', 'harbor', 'wild', 'night', 'vinewood', 'city', 'vinewood']);
  });

  it('gives the three Solar RP servers (SOL, LUNA, TERRA) three different loops', async () => {
    const api = createApi({ baseUrl: 'mock://platform', getToken: () => null, fetchImpl: createMockFetch() });
    const { servers } = await api.servers();
    expect(servers.length).toBe(3);
    const clips = servers.map((s, i) => serverArt(s, i).clip);
    expect(new Set(clips).size).toBe(3);
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
    expect(clipUrl('city')).toMatch(/assets\/video\/city\.mp4$/);
    expect(posterUrl('city')).toMatch(/assets\/video\/city\.webp$/);
    expect(clipThumbUrl('city')).toMatch(/assets\/video\/thumb\/city\.webp$/);
    expect(stillUrl('vip')).toMatch(/assets\/stills\/vip\.webp$/);
    expect(thumbUrl('vip')).toMatch(/assets\/stills\/thumb\/vip\.webp$/);
    expect(CLIPS.length).toBe(5);
    expect(STILLS.length).toBe(7);
    expect(CARD_STILLS.length).toBe(5);
  });

  it('every loop, poster, card thumbnail and still the UI can ask for is on disk', () => {
    // The keys of a lazy glob are the files on disk; nothing is loaded.
    const onDisk = new Set(Object.keys(import.meta.glob('../public/assets/**/*.{mp4,webp}')).map((k) => k.slice('../public/assets/'.length)));
    const rel = (u: string) => u.slice(u.indexOf('assets/') + 'assets/'.length);
    const files = [
      ...CLIPS.flatMap((c) => [clipUrl(c), posterUrl(c), clipThumbUrl(c)]),
      ...STILLS.map(stillUrl),
      ...CARD_STILLS.map(thumbUrl),
    ].map(rel);
    expect(files.length).toBe(5 * 3 + 7 + 5);
    expect(onDisk.size).toBeGreaterThanOrEqual(files.length);
    const missing = files.filter((f) => !onDisk.has(f));
    expect(missing).toEqual([]);
  });

  it('ships nothing the UI cannot ask for', () => {
    const onDisk = Object.keys(import.meta.glob('../public/assets/**/*.{mp4,webp,jpg,png,glb,hdr}')).map((k) => k.slice('../public/assets/'.length));
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
