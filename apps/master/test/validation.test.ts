import { describe, expect, it } from 'vitest';
import { json, makeHarness, validBody } from './helpers.js';

const u = (cp: number): string => String.fromCodePoint(cp);

// [label, body override, path the error must name]
const REJECTED: Array<[string, Record<string, unknown>, string]> = [
  ['port 0', { port: 0 }, 'port'],
  ['port 65536', { port: 65536 }, 'port'],
  ['port fractional', { port: 24800.5 }, 'port'],
  ['port as a string', { port: '24800' }, 'port'],
  ['port missing', { port: undefined }, 'port'],
  ['name empty', { name: '' }, 'name'],
  ['name only spaces', { name: '   ' }, 'name'],
  ['name 65 characters', { name: 'a'.repeat(65) }, 'name'],
  ['name 65 code points outside the BMP', { name: u(0x1f600).repeat(65) }, 'name'],
  ['name with a control character', { name: `bad${u(7)}name` }, 'name'],
  ['name with a newline', { name: 'two\nlines' }, 'name'],
  ['name with a bidi override', { name: `evil${u(0x202e)}gnp` }, 'name'],
  ['name with a line separator', { name: `a${u(0x2028)}b` }, 'name'],
  ['name with an unpaired surrogate', { name: `a${u(0xd800)}b` }, 'name'],
  ['name not a string', { name: 12 }, 'name'],
  ['gamemode empty', { gamemode: '' }, 'gamemode'],
  ['gamemode 33 bytes', { gamemode: 'g'.repeat(33) }, 'gamemode'],
  ['gamemode 34 bytes in 17 characters', { gamemode: u(0xe9).repeat(17) }, 'gamemode'],
  ['gamemode with a control character', { gamemode: `a${u(0)}b` }, 'gamemode'],
  ['lang empty', { lang: '' }, 'lang'],
  ['lang a full word', { lang: 'english' }, 'lang'],
  ['lang with an underscore', { lang: 'en_US' }, 'lang'],
  ['lang one letter', { lang: 'e' }, 'lang'],
  ['lang too long for the launcher', { lang: 'en-US-xyz' }, 'lang'],
  ['url not a url', { url: 'not a url' }, 'url'],
  ['url ftp', { url: 'ftp://example.org/x' }, 'url'],
  ['url javascript', { url: 'javascript:alert(1)' }, 'url'],
  ['url ws (the launcher would treat it as an address)', { url: 'ws://example.org:1234' }, 'url'],
  ['url with credentials', { url: 'https://user:pw@example.org/' }, 'url'],
  ['url over 256 characters', { url: `https://example.org/${'a'.repeat(250)}` }, 'url'],
  ['players negative', { players: -1 }, 'players'],
  ['players fractional', { players: 1.5 }, 'players'],
  ['players above maxPlayers', { players: 33, maxPlayers: 32 }, 'players'],
  ['players above 1024', { players: 1025, maxPlayers: 1024 }, 'players'],
  ['maxPlayers 0', { players: 0, maxPlayers: 0 }, 'maxPlayers'],
  ['maxPlayers above 1024', { maxPlayers: 1025 }, 'maxPlayers'],
  ['maxPlayers missing', { maxPlayers: undefined }, 'maxPlayers'],
  ['protocol fractional', { protocol: 1.5 }, 'protocol'],
  ['protocol negative', { protocol: -1 }, 'protocol'],
  ['protocol as a string', { protocol: '1' }, 'protocol'],
  ['protocol above u16', { protocol: 65536 }, 'protocol'],
  ['protocol missing', { protocol: undefined }, 'protocol'],
];

describe('POST /announce validation', () => {
  it.each(REJECTED)('rejects %s', async (_label, override, path) => {
    const h = makeHarness();
    const res = await h.announce(validBody(override));
    expect(res.status).toBe(400);
    const body = await json(res);
    expect(body.error).toBe('validation_failed');
    expect(body.issues.map((i: { path: string }) => i.path)).toContain(path);
    // a rejected announce must leave nothing behind
    expect(await h.store.list(Date.now())).toHaveLength(0);
  });

  it('accepts the exact boundaries', async () => {
    const h = makeHarness();
    const boundary = [
      { port: 1, name: 'a', gamemode: 'g', lang: 'en', players: 0, maxPlayers: 1, protocol: 0, url: '' },
      { port: 65535, name: 'x'.repeat(64), gamemode: 'g'.repeat(32), lang: 'zh-Hans', players: 1024, maxPlayers: 1024, protocol: 65535 },
      // 64 characters that are 128 UTF-16 units and 256 UTF-8 bytes: the limit counts characters
      { port: 2, name: u(0x1f600).repeat(64), gamemode: u(0xe9).repeat(16), lang: 'pt-BR', players: 5, maxPlayers: 5, protocol: 1 },
    ];
    for (const [i, b] of boundary.entries()) {
      const res = await h.announce(b, { ip: i === 0 ? '8.8.8.8' : i === 1 ? '1.1.1.1' : '9.9.9.9' });
      expect(res.status, JSON.stringify(await json(res.clone()))).toBe(200);
    }
    expect(await h.store.list(Date.now())).toHaveLength(3);
  });

  it('trims text fields before checking and storing them', async () => {
    const h = makeHarness();
    const res = await h.announce(validBody({ name: '  Padded  ', gamemode: ' dm ', lang: ' en ', url: '  https://example.org/  ' }));
    expect(res.status).toBe(200);
    expect(await h.store.list(Date.now())).toMatchObject([{ name: 'Padded', gamemode: 'dm', lang: 'en', url: 'https://example.org/' }]);
  });

  it.each([
    ['an array', '[]'],
    ['null', 'null'],
    ['a string', '"hello"'],
    ['a number', '42'],
  ])('rejects a JSON body that is %s', async (_label, raw) => {
    const h = makeHarness();
    const res = await h.announce(undefined, { raw });
    expect(res.status).toBe(400);
    expect((await json(res)).error).toBe('validation_failed');
  });

  it('rejects a body that is not JSON', async () => {
    const h = makeHarness();
    const res = await h.announce(undefined, { raw: '{"port": 24800,' });
    expect(res.status).toBe(400);
    expect((await json(res)).error).toBe('invalid_json');
  });

  it('caps how many issues one response reports', async () => {
    const h = makeHarness();
    const res = await h.announce({ port: 'x', name: 1, gamemode: 2, lang: 3, url: 4, players: 'p', maxPlayers: 'm', protocol: 'v' });
    expect(res.status).toBe(400);
    const issues = (await json(res)).issues as unknown[];
    expect(issues.length).toBeGreaterThan(0);
    expect(issues.length).toBeLessThanOrEqual(10);
  });
});
