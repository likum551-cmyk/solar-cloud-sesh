import { and, count, desc, eq, isNull, lt, or, sql } from 'drizzle-orm';
import { z } from 'zod';
import type { Actor, Deps } from '../deps.js';
import type { Db } from '../db/index.js';
import { accountRoles, accounts, auditLog, bans, gameSessions, joinTokens, ROLES, type Role } from '../db/schema.js';
import { findAccountById, getActiveBan, getRoles, hasRole, type BanView } from '../auth/accounts.js';
import { ApiError, isUniqueViolation } from '../util/errors.js';

// Roles, bans and the audit log. Every mutation here writes an audit_log row in the same
// transaction as the change, so there is no admin action without a record (the table is
// append-only by trigger).

export interface AuditEntry {
  action: string;
  targetType: string;
  targetId: string;
  details?: Record<string, unknown>;
  ip?: string | null;
  requestId?: string | null;
}

/** Append one audit row. Pass the transaction so the record commits with the change. */
export async function writeAudit(db: Db, actor: Actor, entry: AuditEntry, now: Date): Promise<void> {
  await db.insert(auditLog).values({
    at: now,
    actorAccountId: actor.accountId,
    actorLabel: actor.label,
    action: entry.action,
    targetType: entry.targetType,
    targetId: entry.targetId,
    details: entry.details ?? {},
    ip: entry.ip ?? null,
    requestId: entry.requestId ?? null,
  });
}

export interface AdminCtx {
  actor: Actor;
  /** Roles of the acting account ("admin" for the CLI). */
  actorRoles: readonly Role[];
  ip?: string | null;
  requestId?: string | null;
}

// ---------------------------------------------------------------------------------------------
// Bans

export const banSchema = z.object({
  reason: z.string().trim().min(1).max(500),
  /** Seconds from now; omit for a permanent ban. */
  durationSeconds: z.number().int().min(60).max(10 * 365 * 86_400).optional(),
});
export const unbanSchema = z.object({ reason: z.string().trim().max(500).optional() });

/**
 * Ban an account. Mods may ban players; admins may also ban mods; nobody may ban an admin (demote
 * first) or themselves. At most one ban per account is in force: banning an already banned
 * account is 409 `already_banned` (unban first to change it). An expired-but-unrevoked ban is
 * closed as "expired" and does not block a new one.
 *
 * The ban takes effect at once, in the same transaction: the account row is locked (FOR UPDATE,
 * which waits for in-flight spends and token redemptions, they hold KEY SHARE on it), the open game
 * session is closed with end reason `banned` and the account's unused join tokens are deleted. The
 * game server learns of it at its next call for the account (spend, pull and verify answer 403
 * `account_banned` / reason "banned").
 */
export async function banAccount(deps: Deps, ctx: AdminCtx, accountId: string, input: z.infer<typeof banSchema>): Promise<BanView> {
  const now = deps.clock();
  const target = await findAccountById(deps.db, accountId);
  if (!target) throw new ApiError(404, 'account_not_found', 'No such account.');
  if (ctx.actor.accountId === accountId) throw new ApiError(403, 'forbidden_target', 'You cannot ban yourself.');
  const targetRoles = await getRoles(deps.db, accountId);
  if (targetRoles.includes('admin') || (targetRoles.includes('mod') && !hasRole(ctx.actorRoles, 'admin'))) {
    throw new ApiError(403, 'forbidden_target', 'You may not ban this account.');
  }
  const expiresAt = input.durationSeconds === undefined ? null : new Date(now.getTime() + input.durationSeconds * 1000);

  try {
    return await deps.db.transaction(async (tx) => {
      await tx.select({ id: accounts.id }).from(accounts).where(eq(accounts.id, accountId)).for('update');
      await tx
        .update(bans)
        .set({ revokedAt: now, revokedReason: 'expired' })
        .where(and(eq(bans.accountId, accountId), isNull(bans.revokedAt), lt(bans.expiresAt, now)));
      const [row] = await tx
        .insert(bans)
        .values({ accountId, reason: input.reason, createdBy: ctx.actor.accountId, createdAt: now, expiresAt })
        .returning();
      if (!row) throw new Error('ban insert returned no row');
      const closed = await tx
        .update(gameSessions)
        .set({ endedAt: now, endReason: 'banned' })
        .where(and(eq(gameSessions.accountId, accountId), isNull(gameSessions.endedAt)))
        .returning({ id: gameSessions.id });
      const voided = await tx
        .delete(joinTokens)
        .where(and(eq(joinTokens.accountId, accountId), isNull(joinTokens.usedAt)))
        .returning({ id: joinTokens.id });
      await writeAudit(tx, ctx.actor, {
        action: 'account.ban',
        targetType: 'account',
        targetId: accountId,
        details: { reason: input.reason, expiresAt: expiresAt?.toISOString() ?? null, username: target.username, sessionsClosed: closed.length, joinTokensVoided: voided.length },
        ip: ctx.ip ?? null,
        requestId: ctx.requestId ?? null,
      }, now);
      return { id: row.id, reason: row.reason, createdAt: row.createdAt, expiresAt: row.expiresAt };
    });
  } catch (err) {
    if (isUniqueViolation(err, 'bans_one_open_per_account')) throw new ApiError(409, 'already_banned', 'The account is already banned.');
    throw err;
  }
}

/**
 * Lift the ban in force. 404 `not_banned` when there is none. Nobody may lift their own ban, and
 * only an admin may lift the ban of a mod or an admin (the mirror of who may place it), both
 * 403 `forbidden_target`; every refusal is written to the audit log as `account.unban_refused`.
 */
export async function unbanAccount(deps: Deps, ctx: AdminCtx, accountId: string, input: z.infer<typeof unbanSchema>): Promise<void> {
  const now = deps.clock();
  const refuse = async (why: 'self' | 'needs_admin'): Promise<never> => {
    await writeAudit(deps.db, ctx.actor, {
      action: 'account.unban_refused',
      targetType: 'account',
      targetId: accountId,
      details: { why },
      ip: ctx.ip ?? null,
      requestId: ctx.requestId ?? null,
    }, now);
    throw new ApiError(403, 'forbidden_target', why === 'self' ? 'You cannot lift your own ban.' : 'Only an admin may lift this ban.');
  };
  if (ctx.actor.accountId === accountId) return refuse('self');
  const targetRoles = await getRoles(deps.db, accountId);
  if ((targetRoles.includes('admin') || targetRoles.includes('mod')) && !hasRole(ctx.actorRoles, 'admin')) return refuse('needs_admin');
  await deps.db.transaction(async (tx) => {
    const rows = await tx
      .update(bans)
      .set({ revokedAt: now, revokedBy: ctx.actor.accountId, revokedReason: input.reason ?? 'unbanned' })
      .where(and(eq(bans.accountId, accountId), isNull(bans.revokedAt)))
      .returning({ id: bans.id });
    if (rows.length === 0) throw new ApiError(404, 'not_banned', 'The account has no ban to lift.');
    await writeAudit(tx, ctx.actor, {
      action: 'account.unban',
      targetType: 'account',
      targetId: accountId,
      details: { reason: input.reason ?? null },
      ip: ctx.ip ?? null,
      requestId: ctx.requestId ?? null,
    }, now);
  });
}

// ---------------------------------------------------------------------------------------------
// Roles

export const rolesSchema = z.object({ roles: z.array(z.enum(ROLES)).max(3) });

/**
 * Replace an account's roles (admin only; `player` is always kept). The last admin cannot lose
 * the admin role, so the platform cannot be locked out of its own admin API.
 */
export async function setRoles(deps: Deps, ctx: AdminCtx, accountId: string, input: z.infer<typeof rolesSchema>): Promise<Role[]> {
  const now = deps.clock();
  if (!hasRole(ctx.actorRoles, 'admin')) throw new ApiError(403, 'forbidden', 'Only an admin can change roles.');
  const target = await findAccountById(deps.db, accountId);
  if (!target) throw new ApiError(404, 'account_not_found', 'No such account.');
  const wanted = ROLES.filter((r) => r === 'player' || input.roles.includes(r));

  return deps.db.transaction(async (tx) => {
    const before = await getRoles(tx, accountId);
    if (before.includes('admin') && !wanted.includes('admin')) {
      // Serialise concurrent demotions: lock the admin rows, then count.
      const admins = await tx.select({ accountId: accountRoles.accountId }).from(accountRoles).where(eq(accountRoles.role, 'admin')).for('update');
      if (admins.length <= 1) throw new ApiError(409, 'last_admin', 'This is the last admin; promote another account first.');
    }
    await tx.delete(accountRoles).where(eq(accountRoles.accountId, accountId));
    await tx.insert(accountRoles).values(wanted.map((role) => ({ accountId, role, grantedAt: now })));
    await writeAudit(tx, ctx.actor, {
      action: 'account.roles',
      targetType: 'account',
      targetId: accountId,
      details: { before, after: wanted, username: target.username },
      ip: ctx.ip ?? null,
      requestId: ctx.requestId ?? null,
    }, now);
    return wanted;
  });
}

// ---------------------------------------------------------------------------------------------
// Account lookup and audit listing

export interface AdminAccountRow {
  id: string;
  username: string;
  email: string | null;
  emailVerified: boolean;
  roles: Role[];
  ban: BanView | null;
  createdAt: Date;
  lastLoginAt: Date | null;
}

async function toAdminRow(deps: Deps, a: typeof accounts.$inferSelect): Promise<AdminAccountRow> {
  const now = deps.clock();
  return {
    id: a.id,
    username: a.username,
    email: a.email,
    emailVerified: a.emailVerifiedAt !== null,
    roles: await getRoles(deps.db, a.id),
    ban: await getActiveBan(deps.db, a.id, now),
    createdAt: a.createdAt,
    lastLoginAt: a.lastLoginAt,
  };
}

export const accountSearchSchema = z.object({
  q: z.string().trim().min(1).max(100),
  limit: z.coerce.number().int().min(1).max(50).default(20),
});

/** Accounts whose username starts with `q` (case-insensitive) or whose email equals it. */
export async function searchAccounts(deps: Deps, input: z.infer<typeof accountSearchSchema>): Promise<AdminAccountRow[]> {
  const q = input.q.toLowerCase();
  const escaped = q.replace(/[\\%_]/g, (c) => `\\${c}`);
  const rows = await deps.db
    .select()
    .from(accounts)
    .where(or(sql`lower(${accounts.username}) like ${`${escaped}%`}`, sql`lower(${accounts.email}) = ${q}`))
    .orderBy(accounts.username)
    .limit(input.limit);
  return Promise.all(rows.map((r) => toAdminRow(deps, r)));
}

export async function getAdminAccount(deps: Deps, id: string): Promise<AdminAccountRow> {
  const a = await findAccountById(deps.db, id);
  if (!a) throw new ApiError(404, 'account_not_found', 'No such account.');
  return toAdminRow(deps, a);
}

export const auditQuerySchema = z.object({
  limit: z.coerce.number().int().min(1).max(200).default(50),
  /** Return entries with id below this (the previous page's last id). */
  before: z.coerce.number().int().positive().optional(),
  action: z.string().max(64).optional(),
  targetId: z.string().max(200).optional(),
});

export async function listAudit(deps: Deps, q: z.infer<typeof auditQuerySchema>) {
  const conds = [];
  if (q.before !== undefined) conds.push(lt(auditLog.id, q.before));
  if (q.action !== undefined) conds.push(eq(auditLog.action, q.action));
  if (q.targetId !== undefined) conds.push(eq(auditLog.targetId, q.targetId));
  return deps.db
    .select()
    .from(auditLog)
    .where(conds.length > 0 ? and(...conds) : undefined)
    .orderBy(desc(auditLog.id))
    .limit(q.limit);
}

/** How many accounts hold the admin role (the CLI uses it to say whether a bootstrap is needed). */
export async function countAdmins(db: Db): Promise<number> {
  const [row] = await db.select({ n: count() }).from(accountRoles).where(eq(accountRoles.role, 'admin'));
  return row?.n ?? 0;
}
