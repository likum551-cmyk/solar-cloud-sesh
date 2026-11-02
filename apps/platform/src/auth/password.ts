import { hash, parseOptions, verify } from '@node-rs/argon2';
import { ApiError } from '../util/errors.js';
import { isCommonPassword } from './common-passwords.js';

export const PASSWORD_MIN_LENGTH = 10;
/** Upper bound so an attacker cannot make argon2 chew on megabytes. */
export const PASSWORD_MAX_LENGTH = 128;

export interface Argon2Params {
  memoryKib: number;
  timeCost: number;
  parallelism: number;
}

/**
 * NFKC-normalise so visually identical passwords typed on different systems hash alike (NIST
 * SP 800-63B). Applied on every hash and verify; never stored.
 */
export function normalizePassword(password: string): string {
  return password.normalize('NFKC');
}

export type PasswordProblem = 'too_short' | 'too_long' | 'common' | 'contains_username' | 'contains_email';

/**
 * The first reason `password` is unacceptable for this account, or null. Rules: at least 10 and
 * at most 128 characters (code points, after NFKC), not on the bundled common-password list, and
 * not containing the username (3+ chars) or the email's local part (4+ chars).
 */
export function checkPassword(password: string, ctx: { username?: string; email?: string } = {}): PasswordProblem | null {
  const p = normalizePassword(password);
  const length = [...p].length;
  if (length < PASSWORD_MIN_LENGTH) return 'too_short';
  if (length > PASSWORD_MAX_LENGTH) return 'too_long';
  if (isCommonPassword(p)) return 'common';
  const lower = p.toLowerCase();
  if (ctx.username !== undefined && ctx.username.length >= 3 && lower.includes(ctx.username.toLowerCase())) return 'contains_username';
  const local = ctx.email?.split('@')[0]?.toLowerCase();
  if (local !== undefined && local.length >= 4 && lower.includes(local)) return 'contains_email';
  return null;
}

export const PASSWORD_PROBLEM_MESSAGES: Record<PasswordProblem, string> = {
  too_short: `Password must be at least ${PASSWORD_MIN_LENGTH} characters.`,
  too_long: `Password must be at most ${PASSWORD_MAX_LENGTH} characters.`,
  common: 'That password is too common. Choose something less guessable.',
  contains_username: 'Password must not contain your username.',
  contains_email: 'Password must not contain your email name.',
};

export interface HashLimits {
  /** Hashes running at once. Keep it below the libuv thread pool (4 by default): argon2 runs there, next to file and DNS work. */
  maxConcurrent: number;
  /** Hashes allowed to wait for a slot; one more is refused with 503 `busy`. */
  maxQueue: number;
}

/**
 * Process-wide bound on concurrent argon2 work. Each hash costs memory (ARGON2_MEMORY_KIB) and a
 * thread-pool slot for tens of milliseconds, and the public routes that reach one (login,
 * register, reset, password change) are unauthenticated or cheap to call, so an unbounded burst
 * could exhaust memory or starve the pool. Beyond `maxConcurrent` running and `maxQueue` waiting,
 * work is refused at once with 503 `busy` and a Retry-After instead of piling up.
 *
 * `run` takes its slot synchronously when one is free (callers that start together are admitted in
 * call order), and a finishing task hands its slot straight to the oldest waiter.
 */
export class HashLimiter {
  private active = 0;
  private readonly waiters: (() => void)[] = [];
  private limits: HashLimits;

  constructor(limits: HashLimits) {
    this.limits = { ...limits };
  }

  configure(limits: HashLimits): void {
    this.limits = { ...limits };
  }

  stats(): { active: number; queued: number } {
    return { active: this.active, queued: this.waiters.length };
  }

  async run<T>(task: () => Promise<T>): Promise<T> {
    if (this.active < this.limits.maxConcurrent) {
      this.active++;
    } else if (this.waiters.length < this.limits.maxQueue) {
      await new Promise<void>((resolve) => this.waiters.push(resolve)); // the finishing task passes its slot on
    } else {
      throw new ApiError(503, 'busy', 'The server is busy checking passwords. Try again in a moment.', { retryAfterSeconds: 2 });
    }
    try {
      return await task();
    } finally {
      const next = this.waiters.shift();
      if (next) next();
      else this.active--;
    }
  }
}

/** The one limiter every hash and verify goes through; buildDeps() applies ARGON2_MAX_CONCURRENT / ARGON2_MAX_QUEUE. */
export const hashLimiter = new HashLimiter({ maxConcurrent: 2, maxQueue: 64 });

/** argon2id hash (PHC string with random salt) of the normalised password. */
export function hashPassword(password: string, params: Argon2Params): Promise<string> {
  // @node-rs/argon2 defaults to Argon2id; the tests assert the "$argon2id$" prefix.
  return hashLimiter.run(() =>
    hash(normalizePassword(password), {
      memoryCost: params.memoryKib,
      timeCost: params.timeCost,
      parallelism: params.parallelism,
    }),
  );
}

/**
 * True when `password` matches the PHC string `hashed`. A malformed hash is a plain false, never a
 * throw. Throws 503 `busy` (not false) when the limiter is saturated: refusing to check is not a
 * wrong password and must not be counted as one.
 */
export function verifyPassword(hashed: string, password: string): Promise<boolean> {
  return hashLimiter.run(async () => {
    try {
      return await verify(hashed, normalizePassword(password));
    } catch {
      return false;
    }
  });
}

/** True when `hashed` was made with weaker parameters than the current policy (rehash after a successful login). */
export function needsRehash(hashed: string, params: Argon2Params): boolean {
  try {
    const o = parseOptions(hashed);
    return o.memoryCost < params.memoryKib || o.timeCost < params.timeCost || o.parallelism !== params.parallelism;
  } catch {
    return true;
  }
}
