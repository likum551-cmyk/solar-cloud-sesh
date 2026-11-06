import { describe, expect, it } from 'vitest';
import { STATIC_STATUS, describeStatus, findServer, parseServers } from '../src/servers';

// the live shape (apps/platform/src/http/routes/player.ts, GET /v1/servers)
const row = (over: Record<string, unknown> = {}, status: Record<string, unknown> = {}, maintenance = { enabled: false, message: '' }) => ({
  id: 'sol', name: 'SOL', region: 'eu', host: 'x', port: 24800, tags: [], sort: 0, maintenance,
  status: { state: 'online', players: 12, maxPlayers: 128, pingMs: 3, protocol: 8, checkedAt: '2026-11-06T15:00:00Z', ...status },
  ...over,
});

describe('parseServers', () => {
  it('reads the live shape', () => {
    const list = parseServers({ servers: [row()] });
    expect(list).toEqual([{ id: 'sol', name: 'SOL', state: 'online', players: 12, maxPlayers: 128, maintenance: null }]);
  });

  it('takes an empty list as a list (the live answer today), and refuses other shapes', () => {
    expect(parseServers({ servers: [] })).toEqual([]);
    expect(parseServers(null)).toBeNull();
    expect(parseServers({ servers: 'x' })).toBeNull();
    expect(parseServers([])).toBeNull();
  });

  it('skips rows without an id or a name', () => {
    expect(parseServers({ servers: [{ name: 'SOL' }, row({ id: 'luna', name: 'LUNA' })] })?.map((s) => s.id)).toEqual(['luna']);
  });

  it('drops counts that are not a believable pair, and every count of a server not online', () => {
    for (const status of [{ players: 200, maxPlayers: 128 }, { players: -1 }, { players: 1.5 }, { maxPlayers: 0 }, { players: null }, { state: 'offline' }, { state: 'unknown' }]) {
      const s = parseServers({ servers: [row({}, status)] })![0]!;
      expect([s.players, s.maxPlayers]).toEqual([null, null]);
    }
  });

  it('reads an unknown state as unknown', () => {
    expect(parseServers({ servers: [row({}, { state: 'exploded' })] })![0]!.state).toBe('unknown');
  });
});

describe('findServer', () => {
  it('matches the id or the name in any case, and misses cleanly', () => {
    const list = parseServers({ servers: [row({ id: 'srv-1', name: 'Sol' })] })!;
    expect(findServer(list, 'sol')?.id).toBe('srv-1');
    expect(findServer(list, 'SRV-1')?.id).toBe('srv-1');
    expect(findServer(list, 'luna')).toBeUndefined();
  });
});

describe('describeStatus', () => {
  it('keeps the static claim when SOL is not listed (the live list is empty until the server is registered)', () => {
    expect(describeStatus(undefined)).toBe(STATIC_STATUS);
    expect(STATIC_STATUS.count).toBeNull();
    expect(describeStatus(findServer(parseServers({ servers: [] })!, 'sol'))).toBe(STATIC_STATUS);
  });

  it('shows the live count when online', () => {
    const v = describeStatus(parseServers({ servers: [row()] })![0]);
    expect(v).toMatchObject({ tone: 'online', label: 'Online', count: '12 / 128' });
  });

  it('never invents a count: online without counts, or not probed yet, stays on the static claim', () => {
    expect(describeStatus(parseServers({ servers: [row({}, { players: null })] })![0])).toBe(STATIC_STATUS);
    expect(describeStatus(parseServers({ servers: [row({}, { state: 'unknown' })] })![0])).toBe(STATIC_STATUS);
  });

  it('says offline and maintenance, maintenance first, with its message', () => {
    expect(describeStatus(parseServers({ servers: [row({}, { state: 'offline' })] })![0])).toMatchObject({ tone: 'offline', count: null });
    const m = describeStatus(parseServers({ servers: [row({}, { state: 'offline' }, { enabled: true, message: ' Back at 20:00 ' })] })![0]);
    expect(m).toMatchObject({ tone: 'maintenance', note: 'Back at 20:00' });
    const blank = describeStatus(parseServers({ servers: [row({}, {}, { enabled: true, message: '' })] })![0]);
    expect(blank.note).toMatch(/maintenance/);
  });
});
