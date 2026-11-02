import { and, desc, eq, gt, isNull, ne, sql } from 'drizzle-orm';
import type { Db } from '../db/index.js';
import { accounts, sessions } from '../db/schema.js';
import type { Config } from '../config.js';
import { hashToken, randomToken } from '../util/tokens.js';
import type { AccountRow } from './accounts.js';

// Sessions are opaque bearer tokens ("gms_" + 256 random bits). Only the SHA-256 is stored.
// Lifetime is sliding: every use pushes expires_at to now + SESSION_TTL_DAYS (at most once an
// hour, so reads are not writes), never beyond absolute_expires_at (created + SESSION_MAX_DAYS).

export const SESSION_TOKEN_PREFIX = 'gms_';
const DAY_MS = 86_400_000;
/** How much sliding progress is worth a write. */
const REFRESH_GRANULARITY_MS = 3_600_000;

export interface SessionMeta {
  ip: string | null;
  userAgent: string | null;
  deviceName: string | null;
}

export interface CreatedSession {
  id: string;
  /** The bearer token. Shown once; only its hash is stored. */
  token: string;
  expiresAt: Date;
}

export interface AuthenticatedSession {
  sessionId: string;
  account: AccountRow;
}

const clip = (s: string | null, n: number): string | null => (s === null ? null : s.slice(0, n));

/** Create a session for `accountId` and return its token (the only time it exists in plaintext on the server). */
export async function createSession(
  db: Db,
  config: Pick<Config, 'sessionTtlDays' | 'sessionMaxDays'>,
  accountId: string,
  meta: SessionMeta,
  now: Date,
): Promise<CreatedSession> {
  const token = SESSION_TOKEN_PREFIX + randomToken(32);
  const absolute = new Date(now.getTime() + config.sessionMaxDays * DAY_MS);
  const expiresAt = new Date(Math.min(now.getTime() + config.sessionTtlDays * DAY_MS, absolute.getTime()));
  const [row] = await db
    .insert(sessions)
    .values({
      accountId,
      tokenHash: hashToken(token),
      createdAt: now,
      lastSeenAt: now,
      expiresAt,
      absoluteExpiresAt: absolute,
      userAgent: clip(meta.userAgent, 256),
      deviceName: clip(meta.deviceName, 64),
      ip: meta.ip,
    })
    .returning({ id: sessions.id });
  if (!row) throw new Error('session insert returned no row');
  return { id: row.id, token, expiresAt };
}

/**
 * The session and account behind a bearer token, or null when the token is unknown, revoked or
 * expired. Slides the expiry forward as a side effect.
 */
export async function authenticate(
  db: Db,
  config: Pick<Config, 'sessionTtlDays'>,
  token: string,
  now: Date,
): Promise<AuthenticatedSession | null> {
  if (!token.startsWith(SESSION_TOKEN_PREFIX) || token.length > 200) return null;
  const [row] = await db
    .select({ session: sessions, account: accounts })
    .from(sessions)
    .innerJoin(accounts, eq(accounts.id, sessions.accountId))
    .where(and(eq(sessions.tokenHash, hashToken(token)), isNull(sessions.revokedAt), gt(sessions.expiresAt, now), gt(sessions.absoluteExpiresAt, now)))
    .limit(1);
  if (!row) return null;

  const slid = Math.min(now.getTime() + config.sessionTtlDays * DAY_MS, row.session.absoluteExpiresAt.getTime());
  if (slid - row.session.expiresAt.getTime() >= REFRESH_GRANULARITY_MS) {
    await db
      .update(sessions)
      .set({ lastSeenAt: now, expiresAt: new Date(slid) })
      .where(and(eq(sessions.id, row.session.id), isNull(sessions.revokedAt)));
  }
  return { sessionId: row.session.id, account: row.account };
}

export interface SessionView {
  id: string;
  deviceName: string | null;
  userAgent: string | null;
  ip: string | null;
  createdAt: Date;
  lastSeenAt: Date;
  expiresAt: Date;
  current: boolean;
}

/** The account's live sessions (devices), newest use first. */
export async function listSessions(db: Db, accountId: string, currentSessionId: string, now: Date): Promise<SessionView[]> {
  const rows = await db
    .select()
    .from(sessions)
    .where(and(eq(sessions.accountId, accountId), isNull(sessions.revokedAt), gt(sessions.expiresAt, now), gt(sessions.absoluteExpiresAt, now)))
    .orderBy(desc(sessions.lastSeenAt));
  return rows.map((r) => ({
    id: r.id,
    deviceName: r.deviceName,
    userAgent: r.userAgent,
    ip: r.ip,
    createdAt: r.createdAt,
    lastSeenAt: r.lastSeenAt,
    expiresAt: r.expiresAt,
    current: r.id === currentSessionId,
  }));
}

/** Revoke one of the account's sessions. True when a live session was revoked. */
export async function revokeSession(db: Db, accountId: string, sessionId: string, reason: string, now: Date): Promise<boolean> {
  const rows = await db
    .update(sessions)
    .set({ revokedAt: now, revokedReason: reason })
    .where(and(eq(sessions.id, sessionId), eq(sessions.accountId, accountId), isNull(sessions.revokedAt)))
    .returning({ id: sessions.id });
  return rows.length > 0;
}

/**
 * Revoke every live session of the account, optionally keeping one (the device that is changing
 * the password stays signed in). Returns how many were revoked.
 */
export async function revokeAllSessions(
  db: Db,
  accountId: string,
  opts: { exceptSessionId?: string; reason: string },
  now: Date,
): Promise<number> {
  const conds = [eq(sessions.accountId, accountId), isNull(sessions.revokedAt)];
  if (opts.exceptSessionId !== undefined) conds.push(ne(sessions.id, opts.exceptSessionId));
  const rows = await db
    .update(sessions)
    .set({ revokedAt: now, revokedReason: opts.reason })
    .where(and(...conds))
    .returning({ id: sessions.id });
  return rows.length;
}

/** Delete sessions that can never be used again (expired or revoked more than `keepDays` ago). Returns the count. */
export async function purgeSessions(db: Db, now: Date, keepDays = 7): Promise<number> {
  const cutoff = new Date(now.getTime() - keepDays * DAY_MS);
  const rows = await db
    .delete(sessions)
    .where(sql`(${sessions.revokedAt} is not null and ${sessions.revokedAt} < ${cutoff.toISOString()}::timestamptz) or ${sessions.absoluteExpiresAt} < ${cutoff.toISOString()}::timestamptz or ${sessions.expiresAt} < ${cutoff.toISOString()}::timestamptz`)
    .returning({ id: sessions.id });
  return rows.length;
}
