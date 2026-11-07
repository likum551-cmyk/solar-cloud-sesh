import { describe, expect, it } from 'vitest';
import { formatIssued, formatSize, parseManifest, splitNotes } from '../src/release';

const HOST = 'solar-rp.com';
const b64 = (s: string) => btoa(String.fromCharCode(...new TextEncoder().encode(s)));
const SHA = '8b4552e5ea5a13170fb4674126fb4fbdbe22e98eaedd09ddb9b62774e16a506a';

function manifest(payload: Record<string, unknown>) {
  const signed = b64(JSON.stringify(payload));
  return { version: 'ignored', url: 'https://evil.example/x.exe', signed, sig: 'AAAA' };
}
const good = { v: 1, version: '0.4.1', url: 'https://solar-rp.com/update/SolarRP-Setup-0.4.1.exe', sha256: SHA, size: 18662336, notes: '', mandatory: false };

describe('parseManifest', () => {
  it('reads the signed payload, not the unsigned top-level copies', () => {
    expect(parseManifest(manifest(good), HOST)).toEqual({ version: '0.4.1', url: 'https://solar-rp.com/update/SolarRP-Setup-0.4.1.exe', sha256: SHA, size: 18662336, notes: '', issued: null });
  });

  it('carries the signed notes and issue date, and never fails the release over them', () => {
    const r = parseManifest(manifest({ ...good, notes: '  Round map.  ', issued: '2026-11-06T23:40:40.607Z' }), HOST);
    expect(r?.notes).toBe('Round map.');
    expect(r?.issued?.toISOString()).toBe('2026-11-06T23:40:40.607Z');
    // unusable notes / dates drop to empty, the download stays
    for (const bad of [{ notes: 42 }, { notes: 'x'.repeat(2001) }, { issued: 'yesterday' }, { issued: '2026-13-45T00:00:00Z' }, { issued: 7 }]) {
      const b = parseManifest(manifest({ ...good, notes: 'ok', issued: '2026-11-06T23:40:40.607Z', ...bad }), HOST);
      expect(b?.version).toBe('0.4.1');
      if ('notes' in bad) expect(b?.notes).toBe('');
      if ('issued' in bad) expect(b?.issued).toBeNull();
    }
    expect(parseManifest(manifest({ ...good, notes: 'x'.repeat(2000) }), HOST)?.notes).toHaveLength(2000);
  });

  it.each([
    ['another host', { url: 'https://evil.example/update/SolarRP-Setup-0.4.1.exe' }],
    ['plain http', { url: 'http://solar-rp.com/update/SolarRP-Setup-0.4.1.exe' }],
    ['outside /update/', { url: 'https://solar-rp.com/SolarRP-Setup-0.4.1.exe' }],
    ['a different file name', { url: 'https://solar-rp.com/update/other.exe' }],
    ['a query string', { url: 'https://solar-rp.com/update/SolarRP-Setup-0.4.1.exe?x=1' }],
    ['a version the file name does not carry', { url: 'https://solar-rp.com/update/SolarRP-Setup-0.4.0.exe' }],
    ['a bad version', { version: '0.4' }],
    ['an upper-case hash', { sha256: SHA.toUpperCase() }],
    ['a short hash', { sha256: SHA.slice(2) }],
    ['a zero size', { size: 0 }],
    ['a string size', { size: '18662336' }],
  ])('refuses %s', (_name, patch) => {
    expect(parseManifest(manifest({ ...good, ...patch }), HOST)).toBeNull();
  });

  it('refuses a missing or broken signed field', () => {
    expect(parseManifest({ version: '0.4.1' }, HOST)).toBeNull();
    expect(parseManifest({ signed: '###' }, HOST)).toBeNull();
    expect(parseManifest({ signed: b64('not json') }, HOST)).toBeNull();
    expect(parseManifest(null, HOST)).toBeNull();
    expect(parseManifest('x', HOST)).toBeNull();
  });
});

describe('formatSize', () => {
  it('writes decimal megabytes with one place', () => {
    expect(formatSize(18662336)).toBe('18.7 MB');
  });
});

describe('splitNotes', () => {
  it('splits the 0.5.5 notes into two items and keeps the bracket whole', () => {
    expect(splitNotes("Round map: GTA's radar is now a circle that fills the HUD ring up to the health and hunger arcs (Legacy; Enhanced keeps the 0.5.4 map for now). Speedometer 8 % bigger.")).toEqual([
      "Round map: GTA's radar is now a circle that fills the HUD ring up to the health and hunger arcs (Legacy; Enhanced keeps the 0.5.4 map for now)",
      'Speedometer 8 % bigger',
    ]);
  });
  it('splits a semicolon list and capitalises each item', () => {
    expect(splitNotes('Round radar inside the HUD ring, zoomed out; bigger speedometer; car trunks.')).toEqual([
      'Round radar inside the HUD ring, zoomed out', 'Bigger speedometer', 'Car trunks',
    ]);
  });
  it('gives nothing for empty notes and caps a runaway list', () => {
    expect(splitNotes('')).toEqual([]);
    expect(splitNotes(' ; . ')).toEqual([]);
    expect(splitNotes(Array.from({ length: 60 }, (_, i) => `item ${i}`).join('; '))).toHaveLength(40);
  });
});

describe('formatIssued', () => {
  it('names the UTC day, so a late-evening release reads the same everywhere', () => {
    expect(formatIssued(new Date('2026-11-06T23:40:40.607Z'))).toBe('6 November 2026');
  });
});
