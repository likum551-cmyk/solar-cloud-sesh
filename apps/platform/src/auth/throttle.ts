import { and, gt, inArray, or, sql } from 'drizzle-orm';
import type { Config } from '../config.js';
import type { Db } from '../db/index.js';
import { authThrottle, rateLimits } from '../db/schema.js';
import { ApiError } from '../util/errors.js';
import { hashToken } from '../util/tokens.js';

// Two persistent throttles, both in the database so they survive restarts and work across
// processes, and both driven by an injected `now` so tests control time.
//
//   Login failures (auth_throttle). Three counters per attempt, all keyed on the TYPED identifier
//   (lower-cased, trimmed, hashed), never on the account it resolves to:
//
//     ip:<address>              any identifier from one client address           LOGIN_IP_MAX_FAILURES (20)
//     idip:<ident>:<block>      this identifier from one address block           LOGIN_ACCOUNT_IP_MAX_FAILURES (5)
//                               (IPv4 /24, IPv6 /64)
//     id:<ident>                this identifier from anywhere (a distributed     LOGIN_ACCOUNT_MAX_FAILURES (100)
//                               attack)
//
//   After `maxFailures` consecutive failures a key is locked for base * 2^(failures - maxFailures)
//   seconds, capped. The counter decays after `failureWindowSeconds` of calm (measured from the
//   later of the last failure and the end of the lock). While a key is locked the password is not
//   even checked, so a correct password during the lockout is refused too.
//
//   Why these keys. A lockout keyed on the account lets any address keep the real owner out of
//   password login (lockout DoS) and, because an unknown identifier has no account, answers
//   differently for "this email belongs to the locked username" than for any other email (an
//   oracle linking a public username to a hidden email). Keyed on what was typed and where it was
//   typed from, a lock is a function of the caller's own history only: one address block can lock
//   only itself out of an identifier, an attacker learns nothing about accounts from a 429, and
//   only a distributed attack (100 failures from at least 20 blocks) reaches the account-wide
//   lock. An account has two identifiers (username, email), each with its own budget.
//
//   Fixed-window counters (rate_limits): `hit()` for everything else (register, forgot-password,
//   join, checkout, password change).

type LoginPolicy = Config['login'];

export interface ThrottleKey {
  key: string;
  maxFailures: number;
  /** True for the keys of the typed identifier (cleared by a successful login); false for the address-wide key. */
  perIdentifier: boolean;
}

/** The identifier as the login lookup sees it (trimmed, case-insensitive), hashed so the table holds no email addresses. */
function identifierHash(identifier: string): string {
  return hashToken(identifier.trim().toLowerCase().slice(0, 254));
}

/**
 * Keys for a login attempt. `lockGroup` is ipLockGroup(client ip). The identifier is whatever was
 * typed: known and unknown identifiers, usernames and emails, all behave the same way.
 */
export function loginKeys(policy: LoginPolicy, ipGroup: string, lockGroup: string, identifier: string): ThrottleKey[] {
  const h = identifierHash(identifier);
  return [
    { key: `ip:${ipGroup}`, maxFailures: policy.ipMaxFailures, perIdentifier: false },
    { key: `idip:${h}:${lockGroup}`, maxFailures: policy.accountIpMaxFailures, perIdentifier: true },
    { key: `id:${h}`, maxFailures: policy.accountMaxFailures, perIdentifier: true },
  ];
}

/** The keys a successful login clears: this identifier's, never the address-wide one. */
export function identifierKeyNames(keys: readonly ThrottleKey[]): string[] {
  return keys.filter((k) => k.perIdentifier).map((k) => k.key);
}

/** Throws 429 `rate_limited` (with Retry-After) when any key is currently locked. */
export async function assertNotLocked(db: Db, keys: ThrottleKey[], now: Date): Promise<void> {
  const rows = await db
    .select({ lockedUntil: authThrottle.lockedUntil })
    .from(authThrottle)
    .where(and(inArray(authThrottle.key, keys.map((k) => k.key)), gt(authThrottle.lockedUntil, now)));
  let until = 0;
  for (const r of rows) if (r.lockedUntil !== null) until = Math.max(until, r.lockedUntil.getTime());
  if (until > 0) {
    const retry = Math.max(1, Math.ceil((until - now.getTime()) / 1000));
    throw new ApiError(429, 'rate_limited', 'Too many failed attempts. Try again later.', { retryAfterSeconds: retry });
  }
}

/** Count one failure on every key and lock any that reached its threshold. */
export async function recordFailure(db: Db, policy: LoginPolicy, keys: ThrottleKey[], now: Date): Promise<void> {
  for (const k of keys) {
    const t = authThrottle;
    const windowStart = new Date(now.getTime() - policy.failureWindowSeconds * 1000);
    const calmSince = sql`greatest(${t.updatedAt}, coalesce(${t.lockedUntil}, ${t.updatedAt}))`;
    const decayed = sql`(${calmSince} <= ${windowStart.toISOString()}::timestamptz)`;
    const nextFailures = sql`(case when ${decayed} then 1 else ${t.failures} + 1 end)`;
    const lockSeconds = sql`least(${policy.lockoutBaseSeconds}::numeric * power(2::numeric, least(${nextFailures} - ${k.maxFailures}, 20)), ${policy.lockoutMaxSeconds}::numeric)`;
    const insertLock = k.maxFailures <= 1 ? new Date(now.getTime() + policy.lockoutBaseSeconds * 1000) : null;
    await db
      .insert(t)
      .values({ key: k.key, failures: 1, windowStart: now, lockedUntil: insertLock, updatedAt: now })
      .onConflictDoUpdate({
        target: t.key,
        set: {
          failures: sql`${nextFailures}`,
          windowStart: sql`(case when ${decayed} then ${now.toISOString()}::timestamptz else ${t.windowStart} end)`,
          lockedUntil: sql`(case when ${nextFailures} >= ${k.maxFailures} then ${now.toISOString()}::timestamptz + (${lockSeconds} * interval '1 second') else null end)`,
          updatedAt: sql`${now.toISOString()}::timestamptz`,
        },
      });
  }
}

/** Forget the failures of these keys (after a successful login: the attempt's own keys, never the address-wide one). */
export async function clearFailures(db: Db, keys: readonly string[]): Promise<void> {
  if (keys.length === 0) return;
  await db.delete(authThrottle).where(inArray(authThrottle.key, [...keys]));
}

/**
 * Forget every login failure recorded for these typed identifiers, from any address (after a
 * password reset or change: whoever just proved control of the account may sign in again at once).
 */
export async function clearIdentifierFailures(db: Db, identifiers: readonly string[]): Promise<void> {
  const hashes = [...new Set(identifiers.map(identifierHash))];
  if (hashes.length === 0) return;
  await db
    .delete(authThrottle)
    .where(or(inArray(authThrottle.key, hashes.map((h) => `id:${h}`)), ...hashes.map((h) => sql`starts_with(${authThrottle.key}, ${`idip:${h}:`})`)));
}

export interface HitResult {
  allowed: boolean;
  count: number;
  retryAfterSeconds: number;
}

/**
 * Fixed-window counter. Counts this call and reports whether it is within `limit` for the
 * current `windowSeconds` window of `key`. One atomic upsert, so concurrent callers are counted
 * exactly.
 */
export async function hit(db: Db, key: string, limit: number, windowSeconds: number, now: Date): Promise<HitResult> {
  const threshold = new Date(now.getTime() - windowSeconds * 1000).toISOString();
  const nowIso = now.toISOString();
  const t = rateLimits;
  const [row] = await db
    .insert(t)
    .values({ key, windowStart: now, count: 1 })
    .onConflictDoUpdate({
      target: t.key,
      set: {
        count: sql`(case when ${t.windowStart} <= ${threshold}::timestamptz then 1 else ${t.count} + 1 end)`,
        windowStart: sql`(case when ${t.windowStart} <= ${threshold}::timestamptz then ${nowIso}::timestamptz else ${t.windowStart} end)`,
      },
    })
    .returning({ count: t.count, windowStart: t.windowStart });
  if (!row) throw new Error('rate limit upsert returned no row');
  const retry = Math.max(1, Math.ceil((row.windowStart.getTime() + windowSeconds * 1000 - now.getTime()) / 1000));
  return { allowed: row.count <= limit, count: row.count, retryAfterSeconds: retry };
}

/** `hit()` that throws 429 `rate_limited` when over the limit. */
export async function enforce(db: Db, key: string, limit: number, windowSeconds: number, now: Date): Promise<void> {
  const r = await hit(db, key, limit, windowSeconds, now);
  if (!r.allowed) {
    throw new ApiError(429, 'rate_limited', 'Too many requests. Try again later.', { retryAfterSeconds: r.retryAfterSeconds });
  }
}
