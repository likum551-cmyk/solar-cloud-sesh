import { and, eq, gt, isNull, lt } from 'drizzle-orm';
import { z } from 'zod';
import type { Deps } from '../deps.js';
import { accounts, gameSessions, joinTokens, type Role } from '../db/schema.js';
import { assertNotBanned, getActiveBan, getRoles, hasRole, type AccountRow } from '../auth/accounts.js';
import { enforce } from '../auth/throttle.js';
import { getServer, type GameServerRow } from '../servers/service.js';
import { characterSlots } from '../shop/service.js';
import { ApiError } from '../util/errors.js';
import { hashToken, randomToken } from '../util/tokens.js';

// Join tokens. The launcher asks the platform for a token (authenticated, 60 s, single use),
// hands it to the game client, which sends it in its Hello; the game server redeems it at
// POST /internal/game/verify. The token is bound to one server and one account, only its hash is
// stored, and redeeming is one conditional UPDATE: replays, expiry and other servers match nothing.

export const JOIN_TOKEN_PREFIX = 'gmj_';

export const joinSchema = z.object({ serverId: z.string().regex(/^[a-z0-9][a-z0-9-]{1,31}$/) });
export const verifySchema = z.object({ token: z.string().min(8).max(200) });
export const sessionEndSchema = z
  .object({
    sessionId: z.guid().optional(),
    accountId: z.guid().optional(),
    reason: z.enum(['quit', 'timeout', 'kicked', 'shutdown', 'crash']).default('quit'),
  })
  .refine((o) => o.sessionId !== undefined || o.accountId !== undefined, 'sessionId or accountId is required');

export interface JoinTicket {
  token: string;
  expiresAt: Date;
  server: { id: string; name: string; host: string; port: number };
}

/**
 * Issue a join token for `serverId`. Refused for: unverified email (when REQUIRED), a banned
 * account, an unknown or hidden server, a server in maintenance (staff may still join). An
 * account has at most one live token: asking again voids the previous one.
 */
export async function createJoinToken(deps: Deps, account: AccountRow, roles: readonly Role[], serverId: string, ip: string | null): Promise<JoinTicket> {
  const now = deps.clock();
  await enforce(deps.db, `join:${account.id}`, 10, 60, now);
  if (deps.config.requireVerifiedEmail && account.emailVerifiedAt === null) {
    throw new ApiError(403, 'email_not_verified', 'Verify your email address before joining a server.');
  }
  await assertNotBanned(deps.db, account.id, now);
  const server = await getServer(deps.db, serverId);
  const staff = hasRole(roles, 'mod');
  if (!server || (!server.visible && !staff)) throw new ApiError(404, 'server_not_found', 'No such server.');
  if (server.maintenance && !staff) {
    throw new ApiError(503, 'server_maintenance', server.maintenanceMessage ?? 'This server is down for maintenance.', {
      details: { message: server.maintenanceMessage },
    });
  }

  const token = JOIN_TOKEN_PREFIX + randomToken(32);
  const expiresAt = new Date(now.getTime() + deps.config.joinTokenTtlSeconds * 1000);
  await deps.db.transaction(async (tx) => {
    await tx.delete(joinTokens).where(and(eq(joinTokens.accountId, account.id), isNull(joinTokens.usedAt)));
    await tx.insert(joinTokens).values({ tokenHash: hashToken(token), accountId: account.id, serverId: server.id, createdAt: now, expiresAt, ip });
  });
  return { token, expiresAt, server: { id: server.id, name: server.name, host: server.host, port: server.port } };
}

export type VerifyResult =
  | {
      ok: true;
      accountId: string;
      username: string;
      roles: Role[];
      emailVerified: boolean;
      sessionId: string;
      /** Characters the account may have in game: 1 + the bought character slots, at most 3 (shop/payload.ts CHARACTER_SLOT_KEY). */
      characterSlots: number;
      /** Always null on success (a banned account is refused with reason "banned"). */
      ban: null;
    }
  | { ok: false; reason: 'invalid_token' | 'expired' | 'used' }
  | { ok: false; reason: 'banned'; accountId: string; username: string; ban: { reason: string; expiresAt: string | null } };

/**
 * Redeem a join token on behalf of `server` (already authenticated). A ban deletes the account's
 * unused tokens, so a token issued before it is plain `invalid_token`; a token that still gets here
 * for a banned account is refused with reason "banned". The token is consumed
 * whatever the outcome after the single-use check (so a banned player's token cannot be retried),
 * and a successful redeem opens the account's game session on this server, ending any session it
 * had open elsewhere ("superseded").
 *
 * A token issued for a different server is reported as plain `invalid_token` (and logged), so a
 * server holding its own secret learns nothing about other servers' tokens.
 */
export async function verifyJoinToken(deps: Deps, server: GameServerRow, token: string): Promise<VerifyResult> {
  const now = deps.clock();
  const hash = hashToken(token);

  const outcome = await deps.db.transaction(async (tx) => {
    const [consumed] = await tx
      .update(joinTokens)
      .set({ usedAt: now })
      .where(and(eq(joinTokens.tokenHash, hash), eq(joinTokens.serverId, server.id), isNull(joinTokens.usedAt), gt(joinTokens.expiresAt, now)))
      .returning({ id: joinTokens.id, accountId: joinTokens.accountId });
    if (!consumed) return { kind: 'rejected' as const };

    // KEY SHARE on the account row: a ban takes it exclusively, so either the ban is visible to the
    // check below or the ban waits until this session exists and then closes it.
    const [account] = await tx.select().from(accounts).where(eq(accounts.id, consumed.accountId)).for('key share');
    if (!account) return { kind: 'rejected' as const };
    const ban = await getActiveBan(tx, account.id, now);
    if (ban) return { kind: 'banned' as const, account, ban };

    await tx
      .update(gameSessions)
      .set({ endedAt: now, endReason: 'superseded' })
      .where(and(eq(gameSessions.accountId, account.id), isNull(gameSessions.endedAt)));
    const [session] = await tx
      .insert(gameSessions)
      .values({ accountId: account.id, serverId: server.id, joinTokenId: consumed.id, startedAt: now })
      .returning({ id: gameSessions.id });
    if (!session) throw new Error('game session insert returned no row');
    return { kind: 'ok' as const, account, sessionId: session.id, roles: await getRoles(tx, account.id), characterSlots: await characterSlots(tx, account.id) };
  });

  if (outcome.kind === 'banned') {
    return {
      ok: false,
      reason: 'banned',
      accountId: outcome.account.id,
      username: outcome.account.username,
      ban: { reason: outcome.ban.reason, expiresAt: outcome.ban.expiresAt?.toISOString() ?? null },
    };
  }
  if (outcome.kind === 'ok') {
    return {
      ok: true,
      accountId: outcome.account.id,
      username: outcome.account.username,
      roles: outcome.roles,
      emailVerified: outcome.account.emailVerifiedAt !== null,
      sessionId: outcome.sessionId,
      characterSlots: outcome.characterSlots,
      ban: null,
    };
  }
  return diagnoseRejection(deps, server, hash, now);
}

/** Why a token could not be redeemed (for the caller and the log); runs only on the failure path. */
async function diagnoseRejection(deps: Deps, server: GameServerRow, hash: string, now: Date): Promise<VerifyResult> {
  const [row] = await deps.db.select().from(joinTokens).where(eq(joinTokens.tokenHash, hash)).limit(1);
  if (!row) return { ok: false, reason: 'invalid_token' };
  if (row.serverId !== server.id) {
    deps.logger.warn('join token presented to the wrong server', { presentedBy: server.id, issuedFor: row.serverId });
    return { ok: false, reason: 'invalid_token' };
  }
  if (row.usedAt !== null) return { ok: false, reason: 'used' };
  if (row.expiresAt.getTime() <= now.getTime()) return { ok: false, reason: 'expired' };
  return { ok: false, reason: 'invalid_token' };
}

/**
 * The player left (or the server dropped them): close their open game session on this server.
 * Idempotent: ending a session that is already closed (or unknown) reports `ended: 0`.
 */
export async function endGameSession(deps: Deps, server: GameServerRow, input: z.infer<typeof sessionEndSchema>): Promise<{ ended: number }> {
  const now = deps.clock();
  const conds = [eq(gameSessions.serverId, server.id), isNull(gameSessions.endedAt)];
  if (input.sessionId !== undefined) conds.push(eq(gameSessions.id, input.sessionId));
  if (input.accountId !== undefined) conds.push(eq(gameSessions.accountId, input.accountId));
  const rows = await deps.db
    .update(gameSessions)
    .set({ endedAt: now, endReason: input.reason })
    .where(and(...conds))
    .returning({ id: gameSessions.id });
  return { ended: rows.length };
}

/** True when `accountId` has an open game session on `serverId` (the gate for grant pulls and wallet calls). */
export async function hasOpenSession(deps: Deps, serverId: string, accountId: string): Promise<boolean> {
  const [row] = await deps.db
    .select({ id: gameSessions.id })
    .from(gameSessions)
    .where(and(eq(gameSessions.serverId, serverId), eq(gameSessions.accountId, accountId), isNull(gameSessions.endedAt)))
    .limit(1);
  return row !== undefined;
}

/** Housekeeping: delete join tokens a day past expiry, close game sessions older than GAME_SESSION_MAX_HOURS. */
export async function purgeGameData(deps: Deps, now: Date): Promise<{ tokens: number; staleSessions: number }> {
  const cutoff = new Date(now.getTime() - 86_400_000);
  const tokens = await deps.db.delete(joinTokens).where(lt(joinTokens.expiresAt, cutoff)).returning({ id: joinTokens.id });
  const staleBefore = new Date(now.getTime() - deps.config.gameSessionMaxHours * 3_600_000);
  const stale = await deps.db
    .update(gameSessions)
    .set({ endedAt: now, endReason: 'stale' })
    .where(and(isNull(gameSessions.endedAt), lt(gameSessions.startedAt, staleBefore)))
    .returning({ id: gameSessions.id });
  return { tokens: tokens.length, staleSessions: stale.length };
}
