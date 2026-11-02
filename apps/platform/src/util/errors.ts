// One error type for everything the API refuses on purpose, plus helpers for reading Postgres
// errors through drizzle's wrapper.

export type ErrorStatus = 400 | 401 | 402 | 403 | 404 | 405 | 409 | 410 | 413 | 415 | 422 | 423 | 429 | 500 | 501 | 503;

/**
 * An expected refusal. The HTTP layer renders it as `{ "error": { "code", "message", "requestId" } }`
 * with the given status. `code` is the stable machine-readable part clients switch on; `message`
 * is for humans and may change. `retryAfterSeconds` also sets the Retry-After header.
 */
export class ApiError extends Error {
  readonly status: ErrorStatus;
  readonly code: string;
  readonly details?: unknown;
  readonly retryAfterSeconds?: number;

  constructor(status: ErrorStatus, code: string, message: string, opts: { details?: unknown; retryAfterSeconds?: number } = {}) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.code = code;
    if (opts.details !== undefined) this.details = opts.details;
    if (opts.retryAfterSeconds !== undefined) this.retryAfterSeconds = opts.retryAfterSeconds;
  }
}

export interface PgErrorInfo {
  code?: string;
  constraint?: string;
  message: string;
}

/**
 * The Postgres error behind `err`. drizzle wraps driver errors in DrizzleQueryError (the driver
 * error is `cause`), and PGlite and node-postgres both expose `code`, `constraint` and `message`.
 */
export function pgErrorInfo(err: unknown): PgErrorInfo | null {
  let cur: unknown = err;
  for (let depth = 0; depth < 5 && cur instanceof Error; depth++) {
    const e = cur as Error & { code?: unknown; constraint?: unknown };
    if (typeof e.code === 'string' && /^[0-9A-Z]{5}$/.test(e.code)) {
      return {
        code: e.code,
        ...(typeof e.constraint === 'string' ? { constraint: e.constraint } : {}),
        message: e.message,
      };
    }
    cur = e.cause;
  }
  return null;
}

/** True when `err` is a unique violation (23505), optionally on one named constraint or index. */
export function isUniqueViolation(err: unknown, constraint?: string): boolean {
  const info = pgErrorInfo(err);
  if (info?.code !== '23505') return false;
  return constraint === undefined || info.constraint === constraint;
}

/** True when `err` is a check violation (23514) whose message contains `needle`, or any check violation without one. */
export function isCheckViolation(err: unknown, needle?: string): boolean {
  const info = pgErrorInfo(err);
  if (info?.code !== '23514') return false;
  return needle === undefined || info.message.includes(needle) || info.constraint === needle;
}

/** True when `err` is a foreign-key violation (23503), or a RESTRICT violation (23001), optionally on one named constraint. */
export function isForeignKeyViolation(err: unknown, constraint?: string): boolean {
  const info = pgErrorInfo(err);
  if (info?.code !== '23503' && info?.code !== '23001') return false;
  return constraint === undefined || info.constraint === constraint;
}
