import { describe, expect, it } from 'vitest';
import { formatSize, parseManifest } from '../src/release';

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
    expect(parseManifest(manifest(good), HOST)).toEqual({ version: '0.4.1', url: 'https://solar-rp.com/update/SolarRP-Setup-0.4.1.exe', sha256: SHA, size: 18662336 });
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
