import { and, eq, gt, isNull } from 'drizzle-orm';
import { z } from 'zod';
import type { Deps } from '../deps.js';
import type { Db } from '../db/index.js';
import { accountRoles, accounts, authTokens, oauthIdentities } from '../db/schema.js';
import {
  accountExistsMessage,
  discordUnlinkedMessage,
  passwordChangedMessage,
  passwordResetMessage,
  verifyEmailMessage,
} from '../email.js';
import { ApiError, isUniqueViolation, pgErrorInfo } from '../util/errors.js';
import { ipLockGroup } from '../util/ip.js';
import { errorFields } from '../util/logger.js';
import { hashToken, randomToken } from '../util/tokens.js';
import {
  emailSchema,
  findAccountByEmail,
  findAccountById,
  findAccountByIdentifier,
  getAccountView,
  isReservedUsername,
  passwordSchema,
  usernameSchema,
  type AccountRow,
  type AccountView,
} from './accounts.js';
import {
  checkPassword,
  hashPassword,
  needsRehash,
  PASSWORD_PROBLEM_MESSAGES,
  verifyPassword,
  type Argon2Params,
} from './password.js';
import { createSession, revokeAllSessions, type CreatedSession, type SessionMeta } from './sessions.js';
import { assertNotLocked, clearFailures, clearIdentifierFailures, enforce, identifierKeyNames, loginKeys, recordFailure } from './throttle.js';

/** Per-request facts the auth flows need. */
export interface ReqCtx {
  /** Normalised client IP. */
  ip: string;
  /** ipGroup(ip): the identity rate limits apply to. */
  ipGroup: string;
  userAgent: string | null;
  requestId: string;
}

export const registerSchema = z.object({
  email: emailSchema,
  username: usernameSchema,
  password: passwordSchema,
});
export const loginSchema = z.object({
  identifier: z.string().trim().min(1).max(254),
  password: passwordSchema,
  deviceName: z.string().trim().min(1).max(64).optional(),
});
export const tokenSchema = z.object({ token: z.string().min(16).max(200) });
export const forgotPasswordSchema = z.object({ email: emailSchema });
export const resetPasswordSchema = z.object({ token: z.string().min(16).max(200), password: passwordSchema });
export const changePasswordSchema = z.object({
  currentPassword: passwordSchema.optional(),
  newPassword: passwordSchema,
});

const argon = (deps: Deps): Argon2Params => deps.config.argon2;

function weakPassword(problem: keyof typeof PASSWORD_PROBLEM_MESSAGES): ApiError {
  return new ApiError(400, 'weak_password', PASSWORD_PROBLEM_MESSAGES[problem], { details: { problem } });
}

/** Tell the caller the email could not be sent without turning a committed account into a 500 (they can resend). */
export async function sendSafely(deps: Deps, send: () => Promise<void>, what: string): Promise<void> {
  try {
    await send();
  } catch (err) {
    deps.logger.error('email send failed', { what, ...errorFields(err) });
  }
}

/**
 * Setting a FIRST password (a Discord-only account) is refused while the account's email address is
 * unverified: whoever holds an address they do not own must not attach a way in that outlives the
 * real owner's claim. An account with no email has nothing to verify. (In practice a Discord-only
 * account's email is always verified or absent: Discord signup stores only a verified one.)
 *
 * Linking and unlinking Discord no longer use this gate (Solar RP sends no email, so nobody could
 * verify): they re-authenticate with the current password instead (verifyCurrentPassword), and a link
 * started before the password changed is refused when it finishes (discord.ts finishLink). The real
 * owner's reset still strips every identity from an account whose email was never verified.
 */
export function assertEmailVerifiedForLoginChange(account: AccountRow): void {
  if (account.email !== null && account.emailVerifiedAt === null) {
    throw new ApiError(403, 'email_not_verified', 'Verify your email address before changing how you sign in.');
  }
}

// ---------------------------------------------------------------------------------------------
// Single-use tokens (email verification, password reset)

type TokenKind = 'email_verify' | 'password_reset';

/**
 * Create a token of `kind` for the account and return it (plaintext, once). Earlier unused tokens
 * of the same kind are voided so only the newest link works.
 */
export async function issueAuthToken(
  db: Db,
  accountId: string,
  kind: TokenKind,
  email: string | null,
  ttlMs: number,
  now: Date,
): Promise<string> {
  await db
    .update(authTokens)
    .set({ usedAt: now })
    .where(and(eq(authTokens.accountId, accountId), eq(authTokens.kind, kind), isNull(authTokens.usedAt)));
  const token = randomToken(32);
  await db.insert(authTokens).values({
    accountId,
    kind,
    tokenHash: hashToken(token),
    email,
    createdAt: now,
    expiresAt: new Date(now.getTime() + ttlMs),
  });
  return token;
}

/**
 * Consume a token: one conditional UPDATE that matches only an unused, unexpired token of the
 * right kind. Returns the row, or null when the token is unknown, used, expired or of another
 * kind. Two concurrent callers cannot both get the row.
 */
export async function consumeAuthToken(db: Db, token: string, kind: TokenKind, now: Date) {
  const [row] = await db
    .update(authTokens)
    .set({ usedAt: now })
    .where(
      and(
        eq(authTokens.tokenHash, hashToken(token)),
        eq(authTokens.kind, kind),
        isNull(authTokens.usedAt),
        gt(authTokens.expiresAt, now),
      ),
    )
    .returning();
  return row ?? null;
}

// ---------------------------------------------------------------------------------------------
// Registration

/** Insert an account with its `player` role. Used by email registration and Discord signup. */
export async function createAccountRow(
  db: Db,
  input: { email: string | null; emailVerified: boolean; username: string; passwordHash: string | null },
  now: Date,
): Promise<AccountRow> {
  const [acct] = await db
    .insert(accounts)
    .values({
      email: input.email,
      emailVerifiedAt: input.emailVerified ? now : null,
      username: input.username,
      passwordHash: input.passwordHash,
      passwordChangedAt: input.passwordHash === null ? null : now,
      createdAt: now,
      updatedAt: now,
    })
    .returning();
  if (!acct) throw new Error('account insert returned no row');
  await db.insert(accountRoles).values({ accountId: acct.id, role: 'player', grantedAt: now });
  return acct;
}

export interface RegisterResult {
  verificationRequired: boolean;
}

/**
 * Register with email + username + password. Does not log in (the caller verifies the email and
 * then logs in). The response is identical whether or not the email already has an account, so
 * the endpoint cannot be used to find out who is registered: an existing owner gets an "you
 * already have an account" email instead. A taken username is public information (usernames are
 * the in-game names) and is reported as 409 `username_taken`.
 */
export async function registerAccount(
  deps: Deps,
  input: z.infer<typeof registerSchema>,
): Promise<RegisterResult> {
  const result: RegisterResult = { verificationRequired: deps.config.requireVerifiedEmail };
  const problem = checkPassword(input.password, { username: input.username, email: input.email });
  if (problem) throw weakPassword(problem);
  if (isReservedUsername(input.username)) {
    throw new ApiError(409, 'username_taken', 'That username is not available.');
  }

  // Hash before touching the database so a duplicate email costs the same time as a new one.
  const passwordHash = await hashPassword(input.password, argon(deps));
  const now = deps.clock();
  try {
    const { account, token } = await deps.db.transaction(async (tx) => {
      const account = await createAccountRow(tx, { email: input.email, emailVerified: false, username: input.username, passwordHash }, now);
      const token = await issueAuthToken(tx, account.id, 'email_verify', input.email, deps.config.emailVerifyTtlHours * 3_600_000, now);
      return { account, token };
    });
    await sendSafely(
      deps,
      () => deps.email.send(verifyEmailMessage(deps.config.emailLinkBaseUrl, input.email, account.username, token, deps.config.emailVerifyTtlHours)),
      'verify_email',
    );
    return result;
  } catch (err) {
    if (isUniqueViolation(err, 'accounts_username_lower_uq')) {
      throw new ApiError(409, 'username_taken', 'That username is not available.');
    }
    if (isUniqueViolation(err, 'accounts_email_lower_uq')) {
      const existing = await findAccountByEmail(deps.db, input.email);
      if (existing) {
        await sendSafely(deps, () => deps.email.send(accountExistsMessage(input.email, existing.username)), 'account_exists');
      }
      return result;
    }
    if (isUniqueViolation(err)) {
      deps.logger.error('unexpected unique violation while registering', { constraint: pgErrorInfo(err)?.constraint });
    }
    throw err;
  }
}

// ---------------------------------------------------------------------------------------------
// Login

let dummyHashPromise: Promise<string> | null = null;
/**
 * A valid hash of a random password, verified against when the account does not exist (equal work
 * for known and unknown names). A refused attempt (503 busy) is not cached: the next call retries.
 */
function dummyHash(deps: Deps): Promise<string> {
  if (dummyHashPromise === null) {
    const pending = hashPassword(randomToken(24), argon(deps));
    dummyHashPromise = pending;
    pending.catch(() => {
      if (dummyHashPromise === pending) dummyHashPromise = null;
    });
  }
  return dummyHashPromise;
}

export interface LoginResult {
  session: CreatedSession;
  account: AccountView;
}

/**
 * Password login. Throttled per client address and per TYPED identifier (see throttle.ts): a locked
 * key refuses before the password is checked (429 `rate_limited`), a wrong password or unknown
 * identifier is 401 `invalid_credentials` and counts as a failure. The keys never depend on whether
 * the identifier belongs to an account, so a 429 reveals nothing about accounts. Banned accounts
 * can log in (so the launcher can show the ban); they cannot join or buy.
 */
export async function login(deps: Deps, input: z.infer<typeof loginSchema>, ctx: ReqCtx): Promise<LoginResult> {
  const now = deps.clock();
  const keys = loginKeys(deps.config.login, ctx.ipGroup, ipLockGroup(ctx.ip), input.identifier);
  await assertNotLocked(deps.db, keys, now);
  const account = await findAccountByIdentifier(deps.db, input.identifier);

  const hash = account?.passwordHash ?? (await dummyHash(deps));
  const ok = (await verifyPassword(hash, input.password)) && account !== null && account.passwordHash !== null;
  if (!ok || account === null) {
    await recordFailure(deps.db, deps.config.login, keys, now);
    deps.logger.info('login failed', { ip: ctx.ip, known: account !== null, requestId: ctx.requestId });
    throw new ApiError(401, 'invalid_credentials', 'Wrong username/email or password.');
  }

  // Success clears this identifier's counters (not the address-wide one: alternating a good login
  // with bad guesses must not reset the per-address budget).
  await clearFailures(deps.db, identifierKeyNames(keys));
  if (account.passwordHash !== null && needsRehash(account.passwordHash, argon(deps))) {
    const upgraded = await hashPassword(input.password, argon(deps));
    await deps.db.update(accounts).set({ passwordHash: upgraded }).where(eq(accounts.id, account.id));
  }
  await deps.db.update(accounts).set({ lastLoginAt: now }).where(eq(accounts.id, account.id));
  const session = await createSession(
    deps.db,
    deps.config,
    account.id,
    { ip: ctx.ip, userAgent: ctx.userAgent, deviceName: input.deviceName ?? null },
    now,
  );
  return { session, account: await getAccountView(deps.db, account, now) };
}

/** Start a session for an account that has already proven itself (Discord login). */
export async function startSession(deps: Deps, accountId: string, meta: SessionMeta): Promise<LoginResult> {
  const now = deps.clock();
  const account = await findAccountById(deps.db, accountId);
  if (!account) throw new ApiError(404, 'account_not_found', 'No such account.');
  await deps.db.update(accounts).set({ lastLoginAt: now }).where(eq(accounts.id, accountId));
  const session = await createSession(deps.db, deps.config, accountId, meta, now);
  return { session, account: await getAccountView(deps.db, account, now) };
}

// ---------------------------------------------------------------------------------------------
// Email verification

/** Mark the email verified. Single-use token; void if the account's email changed since it was issued. */
export async function verifyEmail(deps: Deps, token: string): Promise<AccountView> {
  const now = deps.clock();
  const updated = await deps.db.transaction(async (tx) => {
    const row = await consumeAuthToken(tx, token, 'email_verify', now);
    if (!row || row.email === null) throw invalidToken();
    const [acct] = await tx
      .update(accounts)
      .set({ emailVerifiedAt: now, updatedAt: now })
      .where(and(eq(accounts.id, row.accountId), eq(accounts.email, row.email)))
      .returning();
    if (!acct) throw invalidToken(); // the email changed after the token was issued: roll the consumption back
    return acct;
  });
  return getAccountView(deps.db, updated, now);
}

function invalidToken(): ApiError {
  return new ApiError(400, 'invalid_token', 'This link or code is invalid, expired or already used.');
}

/** Send a fresh verification email (voids the previous one). */
export async function resendVerification(deps: Deps, accountId: string): Promise<void> {
  const now = deps.clock();
  await enforce(deps.db, `resend-verify:${accountId}`, 3, 3600, now);
  const account = await findAccountById(deps.db, accountId);
  if (!account) throw new ApiError(404, 'account_not_found', 'No such account.');
  if (account.email === null) throw new ApiError(409, 'no_email', 'This account has no email address.');
  if (account.emailVerifiedAt !== null) throw new ApiError(409, 'already_verified', 'The email is already verified.');
  const token = await issueAuthToken(deps.db, account.id, 'email_verify', account.email, deps.config.emailVerifyTtlHours * 3_600_000, now);
  await deps.email.send(verifyEmailMessage(deps.config.emailLinkBaseUrl, account.email, account.username, token, deps.config.emailVerifyTtlHours));
}

// ---------------------------------------------------------------------------------------------
// Password reset and change

/**
 * Email a reset code if the address has an account. Always resolves the same way, so the
 * endpoint does not reveal which emails are registered. The per-address budget (3 per hour per
 * email) is charged here; the per-client limit (route `forgot-password`, 10 per hour per IP) is
 * applied by the route BEFORE this runs, so an address that is already limited cannot spend the
 * victim's budget and block their reset.
 */
export async function forgotPassword(deps: Deps, email: string): Promise<void> {
  const now = deps.clock();
  await enforce(deps.db, `forgot:email:${email}`, 3, 3600, now);
  const account = await findAccountByEmail(deps.db, email);
  if (!account || account.email === null) return;
  const token = await issueAuthToken(deps.db, account.id, 'password_reset', account.email, deps.config.passwordResetTtlMinutes * 60_000, now);
  await sendSafely(
    deps,
    () => deps.email.send(passwordResetMessage(deps.config.emailLinkBaseUrl, account.email as string, account.username, token, deps.config.passwordResetTtlMinutes)),
    'password_reset',
  );
}

/**
 * Set a new password from a reset token. The password is checked first, so a rejected password
 * leaves the token usable. On success every session of the account is revoked, the email counts
 * as verified (the owner just proved control of it) and the login lockout is cleared. When the
 * email was NOT verified before, whoever registered the account may be a squatter: every external
 * identity (Discord) attached to it is removed in the same transaction and the owner is told.
 */
export async function resetPassword(deps: Deps, input: z.infer<typeof resetPasswordSchema>): Promise<void> {
  const now = deps.clock();
  const [peek] = await deps.db
    .select({ accountId: authTokens.accountId })
    .from(authTokens)
    .where(and(eq(authTokens.tokenHash, hashToken(input.token)), eq(authTokens.kind, 'password_reset'), isNull(authTokens.usedAt), gt(authTokens.expiresAt, now)))
    .limit(1);
  if (!peek) throw invalidToken();
  const account = await findAccountById(deps.db, peek.accountId);
  if (!account) throw invalidToken();
  const problem = checkPassword(input.password, { username: account.username, ...(account.email === null ? {} : { email: account.email }) });
  if (problem) throw weakPassword(problem);

  const passwordHash = await hashPassword(input.password, argon(deps));
  const removed = await deps.db.transaction(async (tx) => {
    const row = await consumeAuthToken(tx, input.token, 'password_reset', now);
    if (!row) throw invalidToken();
    // Serialise with a Discord link finishing on this account (it takes the same lock and re-checks the email).
    const [locked] = await tx.select().from(accounts).where(eq(accounts.id, row.accountId)).for('update');
    if (!locked) throw invalidToken();
    await tx
      .update(accounts)
      .set({
        passwordHash,
        passwordChangedAt: now,
        updatedAt: now,
        ...(locked.email !== null && row.email === locked.email ? { emailVerifiedAt: locked.emailVerifiedAt ?? now } : {}),
      })
      .where(eq(accounts.id, row.accountId));
    await revokeAllSessions(tx, row.accountId, { reason: 'password_reset' }, now);
    if (locked.emailVerifiedAt !== null) return [];
    return tx.delete(oauthIdentities).where(eq(oauthIdentities.accountId, row.accountId)).returning({ providerUsername: oauthIdentities.providerUsername });
  });
  await clearIdentifierFailures(deps.db, [account.username, ...(account.email === null ? [] : [account.email])]);
  if (account.email !== null) {
    const to = account.email;
    await sendSafely(deps, () => deps.email.send(passwordChangedMessage(to, account.username)), 'password_changed');
    for (const identity of removed) {
      await sendSafely(deps, () => deps.email.send(discordUnlinkedMessage(to, account.username, identity.providerUsername)), 'discord_unlinked');
    }
  }
}

/**
 * The throttle identifier of failed current-password checks: the account, not its login names. One
 * budget for every re-authentication (changing the password, linking and unlinking Discord), so a
 * stolen session gets no extra guesses by spreading them over several endpoints.
 */
const reauthIdentifier = (accountId: string): string => `chpw:${accountId}`;

/**
 * Re-authentication of a signed-in account: its CURRENT password, throttled like a login (per client
 * address and per account, the same lockout policy; a locked key is 429 `rate_limited` before the
 * password is checked). A wrong password is 401 `invalid_credentials` and counts as a failure; a right
 * one clears this account's re-authentication counters (not the address-wide one, as in login).
 */
export async function verifyCurrentPassword(
  deps: Deps,
  account: { id: string; passwordHash: string },
  password: string,
  ctx: ReqCtx,
  now: Date,
): Promise<void> {
  const keys = loginKeys(deps.config.login, ctx.ipGroup, ipLockGroup(ctx.ip), reauthIdentifier(account.id));
  await assertNotLocked(deps.db, keys, now);
  if (!(await verifyPassword(account.passwordHash, password))) {
    await recordFailure(deps.db, deps.config.login, keys, now);
    deps.logger.info('re-authentication failed', { ip: ctx.ip, requestId: ctx.requestId });
    throw new ApiError(401, 'invalid_credentials', 'The current password is wrong.');
  }
  await clearFailures(deps.db, identifierKeyNames(keys));
}

/**
 * Change (or, for a Discord-only account, set) the password. Requires the current password when
 * there is one. The route limits the call per account (`change-password`, 10 per hour: each call
 * costs an argon2 verify and a stolen session must not be a free guessing oracle), and wrong
 * current passwords also count towards a lockout of their own (keyed on the account, separate from
 * the login identifiers). Revokes every other session of the account and voids outstanding
 * password-reset links; the calling device stays signed in.
 */
export async function changePassword(
  deps: Deps,
  accountId: string,
  currentSessionId: string,
  input: z.infer<typeof changePasswordSchema>,
  ctx: ReqCtx,
): Promise<{ revokedSessions: number }> {
  const now = deps.clock();
  const account = await findAccountById(deps.db, accountId);
  if (!account) throw new ApiError(404, 'account_not_found', 'No such account.');

  if (account.passwordHash !== null) {
    if (input.currentPassword === undefined) throw new ApiError(400, 'current_password_required', 'The current password is required.');
    await verifyCurrentPassword(deps, { id: account.id, passwordHash: account.passwordHash }, input.currentPassword, ctx, now);
    if (input.currentPassword === input.newPassword) {
      throw new ApiError(400, 'weak_password', 'The new password must differ from the current one.', { details: { problem: 'unchanged' } });
    }
  } else if (account.email === null) {
    throw new ApiError(409, 'email_required', 'Add an email address before setting a password.');
  } else {
    assertEmailVerifiedForLoginChange(account);
  }

  const problem = checkPassword(input.newPassword, { username: account.username, ...(account.email === null ? {} : { email: account.email }) });
  if (problem) throw weakPassword(problem);

  const passwordHash = await hashPassword(input.newPassword, argon(deps));
  const revoked = await deps.db.transaction(async (tx) => {
    await tx.update(accounts).set({ passwordHash, passwordChangedAt: now, updatedAt: now }).where(eq(accounts.id, accountId));
    // A reset link emailed before this change must not be able to undo it.
    await tx
      .update(authTokens)
      .set({ usedAt: now })
      .where(and(eq(authTokens.accountId, accountId), eq(authTokens.kind, 'password_reset'), isNull(authTokens.usedAt)));
    return revokeAllSessions(tx, accountId, { exceptSessionId: currentSessionId, reason: 'password_changed' }, now);
  });
  await clearIdentifierFailures(deps.db, [reauthIdentifier(accountId), account.username, ...(account.email === null ? [] : [account.email])]);
  if (account.email !== null) {
    await sendSafely(deps, () => deps.email.send(passwordChangedMessage(account.email as string, account.username)), 'password_changed');
  }
  return { revokedSessions: revoked };
}
