import { describe, expect, it } from 'vitest';
import { IP_A, IP_B, IP_C, IP_V6, makeHarness, validBody } from './helpers.js';
import { LAUNCHER_MAX_SERVERS, launcherParseServerList } from './launcher-parser.js';

const u = (cp: number): string => String.fromCodePoint(cp);

async function listBody(h: ReturnType<typeof makeHarness>): Promise<string> {
  return (await h.get('/servers')).text();
}

describe('GET /servers against the launcher parser (serverfinder.cpp port)', () => {
  it('every announced server comes out with the same host, port, name, url, counts, mode and lang', async () => {
    const h = makeHarness({ MAX_PER_IP: '10' });
    await h.announce(validBody({ port: 24800, name: 'Alpha', players: 5, maxPlayers: 32, gamemode: 'freeroam', lang: 'en', url: 'https://alpha.example/' }), { ip: IP_A });
    await h.announce(validBody({ port: 24801, name: 'Beta no site', players: 0, maxPlayers: 1, gamemode: 'dm', lang: 'pt-BR', url: undefined }), { ip: IP_B });
    await h.announce(validBody({ port: 25000, name: `Gamma ${u(0x1f600)}`, players: 1024, maxPlayers: 1024, gamemode: u(0xe9).repeat(16), lang: 'zh-Hans', url: '' }), { ip: IP_C });
    await h.announce(validBody({ port: 24800, name: 'Delta v6', players: 2, maxPlayers: 8, gamemode: 'race', lang: 'de', url: 'http://delta.example/x?y=1' }), { ip: IP_V6 });

    const parsed = launcherParseServerList(await listBody(h));
    expect(parsed).not.toBeNull();
    expect(parsed?.dropped).toBe(0);
    expect(parsed?.servers).toEqual([
      { host: IP_C, port: 25000, name: `Gamma ${u(0x1f600)}`, url: '', players: 1024, maxPlayers: 1024, mode: u(0xe9).repeat(16), lang: 'zh-Hans' },
      { host: IP_A, port: 24800, name: 'Alpha', url: 'https://alpha.example/', players: 5, maxPlayers: 32, mode: 'freeroam', lang: 'en' },
      { host: IP_V6, port: 24800, name: 'Delta v6', url: 'http://delta.example/x?y=1', players: 2, maxPlayers: 8, mode: 'race', lang: 'de' },
      { host: IP_B, port: 24801, name: 'Beta no site', url: '', players: 0, maxPlayers: 1, mode: 'dm', lang: 'pt-BR' },
    ]);
  });

  it('the launcher sees the same number of servers as the master lists (none dropped or merged)', async () => {
    const h = makeHarness({ MAX_PER_IP: '50' });
    for (let i = 0; i < 40; i++) await h.announce(validBody({ port: 24800 + i, name: `Server ${i}` }), { ip: IP_A });
    const parsed = launcherParseServerList(await listBody(h));
    expect(parsed?.servers).toHaveLength(40);
    expect(parsed?.dropped).toBe(0);
  });

  it('one entry is an object with exactly the RAGE:MP fields plus protocol', async () => {
    const h = makeHarness();
    await h.announce(validBody());
    const list = JSON.parse(await listBody(h)) as Record<string, Record<string, unknown>>;
    expect(Object.keys(list)).toEqual([`${IP_A}:24800`]);
    expect(Object.keys(list[`${IP_A}:24800`] ?? {}).sort()).toEqual(['gamemode', 'lang', 'maxplayers', 'name', 'players', 'protocol', 'url']);
    expect(list[`${IP_A}:24800`]).toEqual({ name: 'GAGE Test Server', url: 'https://example.org/gage', lang: 'en', gamemode: 'freeroam', players: 3, maxplayers: 32, protocol: 1 });
  });

  it('the oracle is not vacuous: it blanks a label over the launcher cap and rejects an empty list', () => {
    const entry = (over: Record<string, unknown>): string =>
      JSON.stringify({ '8.8.8.8:24800': { name: 'n', url: '', lang: 'en', gamemode: 'g', players: 1, maxplayers: 2, ...over } });
    expect(launcherParseServerList(entry({ gamemode: 'g'.repeat(33) }))?.servers[0]?.mode).toBe('');
    expect(launcherParseServerList(entry({ lang: 'en-US-xyz' }))?.servers[0]?.lang).toBe('');
    expect(launcherParseServerList(entry({ gamemode: 'g'.repeat(32) }))?.servers[0]?.mode).toBe('g'.repeat(32));
    expect(launcherParseServerList('{}')).toBeNull();
    expect(launcherParseServerList('not json')).toBeNull();
  });

  it('never lists more servers than the launcher reads', async () => {
    // MAX_ENTRIES defaults to exactly what the launcher keeps; the config range allows more,
    // so pin the default here.
    const h = makeHarness();
    expect(h.config.maxEntries).toBe(LAUNCHER_MAX_SERVERS);
  });
});
