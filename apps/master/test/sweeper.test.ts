import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { TokenBucketLimiter } from '../src/rate-limit.js';
import { MemoryStore, type NewServerRecord, type ServerStore } from '../src/store.js';
import { startSweeper } from '../src/sweeper.js';
import { memoryLogger } from './helpers.js';

const T0 = 1_700_000_000_000;
const limits = { maxPerIp: 8, maxEntries: 100, ttlMs: 90_000 };
const rec = (port: number): NewServerRecord => ({
  key: `8.8.8.8:${port}`, ip: '8.8.8.8', ipGroup: '8.8.8.8', port, name: 'n', gamemode: 'g', lang: 'en', url: null, players: 0, maxPlayers: 1, protocol: 1,
});

beforeEach(() => {
  vi.useFakeTimers({ now: T0 });
});
afterEach(() => {
  vi.useRealTimers();
});

describe('sweeper', () => {
  it('removes expired entries on its interval, and only those', async () => {
    const store = new MemoryStore();
    const limiter = new TokenBucketLimiter({ capacity: 5, refillPerSecond: 1 });
    const logs = memoryLogger();
    const sweeper = startSweeper({ store, limiter, clock: () => Date.now(), logger: logs.logger, intervalMs: 10_000 });

    await store.upsert(rec(1), limits, Date.now());
    vi.advanceTimersByTime(60_000);
    await store.upsert(rec(2), limits, Date.now());

    await vi.advanceTimersByTimeAsync(10_000); // t = 70 s: nothing expired yet
    expect(store.size).toBe(2);
    await vi.advanceTimersByTimeAsync(20_000); // t = 90 s: entry 1 expired, entry 2 not
    expect(store.size).toBe(1);
    expect(logs.entries().some((e) => e.msg === 'expired servers removed' && e.removed === 1)).toBe(true);
    await vi.advanceTimersByTimeAsync(60_000); // t = 150 s: entry 2 expired at 150 s
    expect(store.size).toBe(0);
    sweeper.stop();
  });

  it('drops idle rate-limit buckets', async () => {
    const store = new MemoryStore();
    const limiter = new TokenBucketLimiter({ capacity: 2, refillPerSecond: 1 });
    limiter.take('8.8.8.8', Date.now());
    expect(limiter.size).toBe(1);
    const sweeper = startSweeper({ store, limiter, clock: () => Date.now(), logger: memoryLogger().logger, intervalMs: 10_000 });
    await vi.advanceTimersByTimeAsync(10_000);
    expect(limiter.size).toBe(0);
    sweeper.stop();
  });

  it('stop() ends the timer', async () => {
    const store = new MemoryStore();
    const sweeper = startSweeper({ store, limiter: new TokenBucketLimiter({ capacity: 1, refillPerSecond: 1 }), clock: () => Date.now(), logger: memoryLogger().logger, intervalMs: 10_000 });
    await store.upsert(rec(1), limits, Date.now());
    sweeper.stop();
    await vi.advanceTimersByTimeAsync(300_000);
    expect(store.size).toBe(1);
  });

  it('logs a failing store instead of throwing, and keeps sweeping', async () => {
    let calls = 0;
    const store: ServerStore = {
      upsert: () => Promise.reject(new Error('unused')),
      has: () => Promise.reject(new Error('unused')),
      list: () => Promise.reject(new Error('unused')),
      sweep: () => {
        calls++;
        return Promise.reject(new Error('db down'));
      },
    };
    const logs = memoryLogger();
    const sweeper = startSweeper({ store, limiter: new TokenBucketLimiter({ capacity: 1, refillPerSecond: 1 }), clock: () => Date.now(), logger: logs.logger, intervalMs: 10_000 });
    await vi.advanceTimersByTimeAsync(30_000);
    expect(calls).toBe(3);
    expect(logs.entries().filter((e) => e.level === 'error' && e.msg === 'sweep failed' && e.err === 'db down')).toHaveLength(3);
    sweeper.stop();
  });

  it('does not start a sweep while the previous one is still running', async () => {
    let calls = 0;
    let release: () => void = () => undefined;
    const store: ServerStore = {
      upsert: () => Promise.reject(new Error('unused')),
      has: () => Promise.reject(new Error('unused')),
      list: () => Promise.reject(new Error('unused')),
      sweep: () => {
        calls++;
        return new Promise<number>((resolve) => {
          release = () => resolve(0);
        });
      },
    };
    const sweeper = startSweeper({ store, limiter: new TokenBucketLimiter({ capacity: 1, refillPerSecond: 1 }), clock: () => Date.now(), logger: memoryLogger().logger, intervalMs: 10_000 });
    await vi.advanceTimersByTimeAsync(30_000);
    expect(calls).toBe(1);
    release();
    await vi.advanceTimersByTimeAsync(10_000);
    expect(calls).toBe(2);
    release();
    sweeper.stop();
  });
});
