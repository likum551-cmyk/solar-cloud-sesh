import { describe, expect, it } from 'vitest';
import { MemoryStore, type NewServerRecord, type StoreLimits } from '../src/store.js';

const limits: StoreLimits = { maxPerIp: 2, maxEntries: 3, ttlMs: 1000 };

function rec(ip: string, port: number, over: Partial<NewServerRecord> = {}): NewServerRecord {
  return {
    key: `${ip}:${port}`,
    ip,
    ipGroup: ip,
    port,
    name: `s${port}`,
    gamemode: 'm',
    lang: 'en',
    url: null,
    players: 0,
    maxPlayers: 10,
    protocol: 1,
    ...over,
  };
}

describe('MemoryStore', () => {
  it('creates, then refreshes keeping firstSeen and moving the expiry', async () => {
    const s = new MemoryStore();
    const first = await s.upsert(rec('1.1.1.1', 1), limits, 100);
    expect(first).toMatchObject({ ok: true, created: true, record: { firstSeen: 100, lastSeen: 100, expiresAt: 1100 } });
    const again = await s.upsert(rec('1.1.1.1', 1, { players: 5 }), limits, 600);
    expect(again).toMatchObject({ ok: true, created: false, record: { firstSeen: 100, lastSeen: 600, expiresAt: 1600, players: 5 } });
    expect(await s.list(700)).toHaveLength(1);
  });

  it('a live entry is listed up to, not including, its expiry instant', async () => {
    const s = new MemoryStore();
    await s.upsert(rec('1.1.1.1', 1), limits, 0);
    expect(await s.has('1.1.1.1:1', 999)).toBe(true);
    expect(await s.has('1.1.1.1:1', 1000)).toBe(false);
    expect(await s.list(999)).toHaveLength(1);
    expect(await s.list(1000)).toHaveLength(0);
  });

  it('refuses a new entry beyond maxPerIp for the same group, but never a refresh', async () => {
    const s = new MemoryStore();
    expect((await s.upsert(rec('1.1.1.1', 1), limits, 0)).ok).toBe(true);
    expect((await s.upsert(rec('1.1.1.1', 2), limits, 0)).ok).toBe(true);
    expect(await s.upsert(rec('1.1.1.1', 3), limits, 0)).toEqual({ ok: false, reason: 'ip-cap' });
    expect((await s.upsert(rec('1.1.1.1', 2, { players: 9 }), limits, 10)).ok).toBe(true);
    expect((await s.upsert(rec('2.2.2.2', 1), limits, 0)).ok).toBe(true);
  });

  it('expired entries stop counting toward the per-address cap', async () => {
    const s = new MemoryStore();
    await s.upsert(rec('1.1.1.1', 1), limits, 0);
    await s.upsert(rec('1.1.1.1', 2), limits, 0);
    expect((await s.upsert(rec('1.1.1.1', 3), limits, 1000)).ok).toBe(true);
  });

  it('refuses beyond maxEntries, after collecting expired entries first', async () => {
    const s = new MemoryStore();
    await s.upsert(rec('1.1.1.1', 1), limits, 0);
    await s.upsert(rec('2.2.2.2', 1), limits, 0);
    await s.upsert(rec('3.3.3.3', 1), limits, 0);
    expect(await s.upsert(rec('4.4.4.4', 1), limits, 10)).toEqual({ ok: false, reason: 'store-full' });
    const afterExpiry = await s.upsert(rec('4.4.4.4', 1), limits, 1000);
    expect(afterExpiry.ok).toBe(true);
    expect(s.size).toBe(1);
  });

  it('an expired entry under the same key is replaced as a new one', async () => {
    const s = new MemoryStore();
    await s.upsert(rec('1.1.1.1', 1), limits, 0);
    const r = await s.upsert(rec('1.1.1.1', 1), limits, 5000);
    expect(r).toMatchObject({ ok: true, created: true, record: { firstSeen: 5000 } });
  });

  it('sweep removes exactly the expired entries and reports the count', async () => {
    const s = new MemoryStore();
    await s.upsert(rec('1.1.1.1', 1), limits, 0);
    await s.upsert(rec('2.2.2.2', 1), limits, 500);
    expect(await s.sweep(999)).toBe(0);
    expect(await s.sweep(1000)).toBe(1);
    expect(s.size).toBe(1);
    expect(await s.sweep(1500)).toBe(1);
    expect(s.size).toBe(0);
  });

  it('list returns copies', async () => {
    const s = new MemoryStore();
    await s.upsert(rec('1.1.1.1', 1), limits, 0);
    const [copy] = await s.list(1);
    if (!copy) throw new Error('expected one entry');
    copy.name = 'tampered';
    expect((await s.list(1))[0]?.name).toBe('s1');
  });
});
