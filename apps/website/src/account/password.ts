// The platform's password rules (apps/platform/src/auth/password.ts checkPassword), mirrored so the form
// can show them while the player types. The common-password list is NOT mirrored: only the server knows
// it, and its `common` answer comes back as an error on submit. The server stays the authority.

export const PASSWORD_MIN = 10;
export const PASSWORD_MAX = 128;
export const USERNAME_PATTERN = /^[A-Za-z0-9_]{3,20}$/;

export interface PasswordRules {
  /** at least PASSWORD_MIN code points after NFKC */
  long: boolean;
  /** at most PASSWORD_MAX code points after NFKC */
  notTooLong: boolean;
  /** does not contain the username (only checked for usernames of 3+ characters) */
  noUsername: boolean;
  /** does not contain the email's local part (only checked for 4+ characters) */
  noEmailName: boolean;
}

export type LocalProblem = 'too_short' | 'too_long' | 'contains_username' | 'contains_email';

/** Code points after NFKC, as the server counts them. */
export function passwordLength(password: string): number {
  return [...password.normalize('NFKC')].length;
}

export function checkPasswordRules(password: string, ctx: { username?: string; email?: string } = {}): PasswordRules {
  const p = password.normalize('NFKC');
  const n = [...p].length;
  const lower = p.toLowerCase();
  const user = ctx.username?.trim() ?? '';
  // the server lower-cases and trims the email before it compares (emailSchema)
  const local = ctx.email?.trim().toLowerCase().split('@')[0] ?? '';
  return {
    long: n >= PASSWORD_MIN,
    notTooLong: n <= PASSWORD_MAX,
    noUsername: !(user.length >= 3 && lower.includes(user.toLowerCase())),
    noEmailName: !(local.length >= 4 && lower.includes(local)),
  };
}

/** The first problem in the server's own order (length, then username, then email), or null. */
export function firstPasswordProblem(password: string, ctx: { username?: string; email?: string } = {}): LocalProblem | null {
  const r = checkPasswordRules(password, ctx);
  if (!r.long) return 'too_short';
  if (!r.notTooLong) return 'too_long';
  if (!r.noUsername) return 'contains_username';
  if (!r.noEmailName) return 'contains_email';
  return null;
}

/** A loose shape check only; the server's validator decides. */
export function looksLikeEmail(email: string): boolean {
  const e = email.trim();
  return e.length <= 254 && /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(e);
}
