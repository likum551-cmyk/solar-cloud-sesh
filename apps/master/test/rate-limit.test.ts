import { describe, expect, it } from 'vitest';
import { TokenBucketLimiter } from '../src/rate-limit.js';

describe('TokenBucketLimiter', () => {
  it('allows `capacity` requests at once, then refuses with an exact retry delay', () => {
    const l = new TokenBucketLimiter({ capacity: 3, refillPerSecond: 0.5 });
    for (let i = 0; i < 3; i++) expect(l.take('a', 0)).toEqual({ allowed: true });
    expect(l.take('a', 0)).toEqual({ allowed: false, retryAfterMs: 2000 });
  });

  it('refills over time and never beyond capacity', () => {
    const l = new TokenBucketLimiter({ capacity: 2, refillPerSecond: 1 });
    l.take('a', 0);
    l.take('a', 0);
    expect(l.take('a', 0).allowed).toBe(false);
    expect(l.take('a', 999).allowed).toBe(false);
    expect(l.take('a', 1000).allowed).toBe(true);
    // a long idle period only refills to capacity
    expect(l.take('a', 3_600_000).allowed).toBe(true);
    expect(l.take('a', 3_600_000).allowed).toBe(true);
    expect(l.take('a', 3_600_000).allowed).toBe(false);
  });

  it('keeps one bucket per key', () => {
    const l = new TokenBucketLimiter({ capacity: 1, refillPerSecond: 0.1 });
    expect(l.take('a', 0).allowed).toBe(true);
    expect(l.take('a', 0).allowed).toBe(false);
    expect(l.take('b', 0).allowed).toBe(true);
  });

  it('ignores a clock that steps backwards', () => {
    const l = new TokenBucketLimiter({ capacity: 1, refillPerSecond: 1 });
    expect(l.take('a', 10_000).allowed).toBe(true);
    expect(l.take('a', 0).allowed).toBe(false);
  });

  it('prune drops only fully refilled buckets', () => {
    const l = new TokenBucketLimiter({ capacity: 2, refillPerSecond: 1 });
    l.take('idle', 0);
    l.take('busy', 0);
    l.take('busy', 0);
    expect(l.size).toBe(2);
    expect(l.prune(1000)).toBe(1); // idle is back to 2 tokens, busy has 1
    expect(l.size).toBe(1);
    expect(l.prune(2000)).toBe(1);
    expect(l.size).toBe(0);
  });

  it('refuses nonsensical settings', () => {
    expect(() => new TokenBucketLimiter({ capacity: 0, refillPerSecond: 1 })).toThrow(RangeError);
    expect(() => new TokenBucketLimiter({ capacity: 1, refillPerSecond: 0 })).toThrow(RangeError);
  });
});
