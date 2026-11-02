import { and, eq, gt, isNull, or, sql } from 'drizzle-orm';
import { z } from 'zod';
import type { Db } from '../db/index.js';
import { accountRoles, accounts, bans, oauthIdentities, ROLES, type Role } from '../db/schema.js';
import { ApiError } from '../util/errors.js';

export type AccountRow = typeof accounts.$inferSelect;

export const USERNAME_PATTERN = /^[A-Za-z0-9_]{3,20}$/;

/** Names nobody may take (they would impersonate staff or the server in chat). Case-insensitive. */
export const RESERVED_USERNAMES: ReadonlySet<string> = new Set([
  'admin', 'administrator', 'moderator', 'mod', 'staff', 'support', 'system', 'server', 'console',
  'gage', 'gagemp', 'gage_mp', 'official', 'owner', 'root', 'null', 'undefined', 'anonymous', 'everyone',
]);

export const usernameSchema = z
  .string()
  .regex(USERNAME_PATTERN, 'Username must be 3 to 20 characters: letters, digits and underscore.');

export const emailSchema = z
  .string()
  .trim()
  .max(254)
  .pipe(z.email())
  .transform((e) => e.toLowerCase());

/** The password as typed; strength is checked by checkPassword (it needs the account context). */
export const passwordSchema = z.string().min(1).max(1024);

export function isReservedUsername(username: string): boolean {
  return RESERVED_USERNAMES.has(username.toLowerCase());
}

export interface BanView {
  id: string;
  reason: string;
  createdAt: Date;
  /** null = permanent */
  expiresAt: Date | null;
}

export interface AccountView {
  id: string;
  username: string;
  email: string | null;
  emailVerified: boolean;
  roles: Role[];
  hasPassword: boolean;
  discord: { id: string; username: string | null } | null;
  ban: BanView | null;
  createdAt: Date;
}

export async function findAccountById(db: Db, id: string): Promise<AccountRow | null> {
  const [row] = await db.select().from(accounts).where(eq(accounts.id, id)).limit(1);
  return row ?? null;
}

/** Look up by the login identifier: an email when it contains "@", otherwise a username (both case-insensitive). */
export async function findAccountByIdentifier(db: Db, identifier: string): Promise<AccountRow | null> {
  const id = identifier.trim();
  const cond = id.includes('@') ? sql`lower(${accounts.email}) = ${id.toLowerCase()}` : sql`lower(${accounts.username}) = ${id.toLowerCase()}`;
  const [row] = await db.select().from(accounts).where(cond).limit(1);
  return row ?? null;
}

export async function findAccountByEmail(db: Db, email: string): Promise<AccountRow | null> {
  const [row] = await db.select().from(accounts).where(sql`lower(${accounts.email}) = ${email.toLowerCase()}`).limit(1);
  return row ?? null;
}

export async function findAccountByUsername(db: Db, username: string): Promise<AccountRow | null> {
  const [row] = await db.select().from(accounts).where(sql`lower(${accounts.username}) = ${username.toLowerCase()}`).limit(1);
  return row ?? null;
}

export async function getRoles(db: Db, accountId: string): Promise<Role[]> {
  const rows = await db.select({ role: accountRoles.role }).from(accountRoles).where(eq(accountRoles.accountId, accountId));
  const set = new Set<Role>(['player']);
  for (const r of rows) if ((ROLES as readonly string[]).includes(r.role)) set.add(r.role as Role);
  return ROLES.filter((r) => set.has(r));
}

/** The ban in force now (not revoked, not expired), or null. */
export async function getActiveBan(db: Db, accountId: string, now: Date): Promise<BanView | null> {
  const [row] = await db
    .select()
    .from(bans)
    .where(and(eq(bans.accountId, accountId), isNull(bans.revokedAt), or(isNull(bans.expiresAt), gt(bans.expiresAt, now))))
    .limit(1);
  return row ? { id: row.id, reason: row.reason, createdAt: row.createdAt, expiresAt: row.expiresAt } : null;
}

/** True when `roles` includes `needed` or a role above it (admin implies mod implies player). */
export function hasRole(roles: readonly Role[], needed: Role): boolean {
  if (roles.includes('admin')) return true;
  if (needed === 'mod') return roles.includes('mod');
  return roles.includes(needed);
}

export async function getAccountView(db: Db, account: AccountRow, now: Date): Promise<AccountView> {
  const [roles, ban, identity] = await Promise.all([
    getRoles(db, account.id),
    getActiveBan(db, account.id, now),
    db
      .select({ id: oauthIdentities.providerUserId, username: oauthIdentities.providerUsername })
      .from(oauthIdentities)
      .where(and(eq(oauthIdentities.accountId, account.id), eq(oauthIdentities.provider, 'discord')))
      .limit(1),
  ]);
  const discord = identity[0] ?? null;
  return {
    id: account.id,
    username: account.username,
    email: account.email,
    emailVerified: account.emailVerifiedAt !== null,
    roles,
    hasPassword: account.passwordHash !== null,
    discord: discord ? { id: discord.id, username: discord.username } : null,
    ban,
    createdAt: account.createdAt,
  };
}

/** Throws 403 `account_banned` (with the ban details) when the account is banned now. */
export async function assertNotBanned(db: Db, accountId: string, now: Date): Promise<void> {
  const ban = await getActiveBan(db, accountId, now);
  if (ban) {
    throw new ApiError(403, 'account_banned', 'This account is banned.', {
      details: { reason: ban.reason, expiresAt: ban.expiresAt?.toISOString() ?? null },
    });
  }
}
