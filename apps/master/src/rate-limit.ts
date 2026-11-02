// In-memory token bucket per key. A bucket starts full, holds at most `capacity` tokens and
// refills continuously at `refillPerSecond`; one request costs one token.

export interface TokenBucketOptions {
  capacity: number;
  refillPerSecond: number;
}

export type TakeResult = { allowed: true } | { allowed: false; retryAfterMs: number };

interface Bucket {
  tokens: number;
  updatedAt: number;
}

export class TokenBucketLimiter {
  private readonly buckets = new Map<string, Bucket>();
  private readonly capacity: number;
  private readonly refillPerSecond: number;

  constructor(opts: TokenBucketOptions) {
    if (!(opts.capacity >= 1) || !(opts.refillPerSecond > 0)) {
      throw new RangeError('capacity must be >= 1 and refillPerSecond > 0');
    }
    this.capacity = opts.capacity;
    this.refillPerSecond = opts.refillPerSecond;
  }

  take(key: string, now: number): TakeResult {
    let b = this.buckets.get(key);
    if (!b) {
      b = { tokens: this.capacity, updatedAt: now };
      this.buckets.set(key, b);
    }
    this.refill(b, now);
    if (b.tokens >= 1) {
      b.tokens -= 1;
      return { allowed: true };
    }
    return {
      allowed: false,
      retryAfterMs: Math.ceil(((1 - b.tokens) / this.refillPerSecond) * 1000),
    };
  }

  /** Drop buckets that have refilled completely (an idle key is indistinguishable from a new one). Returns how many. */
  prune(now: number): number {
    let removed = 0;
    for (const [key, b] of this.buckets) {
      this.refill(b, now);
      if (b.tokens >= this.capacity) {
        this.buckets.delete(key);
        removed++;
      }
    }
    return removed;
  }

  get size(): number {
    return this.buckets.size;
  }

  private refill(b: Bucket, now: number): void {
    const elapsedMs = Math.max(0, now - b.updatedAt); // a clock step backwards must not mint or burn tokens
    b.tokens = Math.min(this.capacity, b.tokens + (elapsedMs / 1000) * this.refillPerSecond);
    b.updatedAt = now;
  }
}
