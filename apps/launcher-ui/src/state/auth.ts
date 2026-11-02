import type { Account } from '../lib/schemas';

// The auth flow as a pure reducer: every screen of the signed-out frame is a `view`, every
// network outcome an event. Side effects (API calls, token storage) live in AppState.tsx.

export type AuthView =
  | 'login'
  | 'register'
  /** After register: "check your inbox". */
  | 'verifyNotice'
  | 'forgot'
  | 'forgotSent'
  /** Paste the reset token from the email and choose a new password. */
  | 'reset'
  | 'resetDone'
  /** Waiting for the browser Discord login to finish. */
  | 'discord'
  /** First Discord login: pick a username. */
  | 'discordUsername';

export interface DiscordFlow {
  flowId: string;
  pollSecret: string;
  suggestedUsername?: string;
}

export type AuthState =
  | { status: 'booting' }
  | {
      status: 'signedOut';
      view: AuthView;
      busy: boolean;
      error: string | null;
      /** Epoch ms until which login is refused (429 Retry-After). */
      lockedUntil: number | null;
      /** The address the last register / forgot request used, for the notice copy. */
      email: string | null;
      discord: DiscordFlow | null;
      /** Why we are signed out, shown once on the login frame (session expired, logged out). */
      reason: string | null;
    }
  | { status: 'signedIn'; account: Account; token: string };

export type AuthEvent =
  | { type: 'BOOT_NONE' }
  | { type: 'SIGNED_IN'; account: Account; token: string }
  | { type: 'SHOW'; view: AuthView }
  | { type: 'SUBMIT'; now: number }
  | { type: 'FAIL'; error: string; retryAfterSeconds?: number | null; now: number }
  | { type: 'REGISTERED'; email: string }
  | { type: 'FORGOT_SENT'; email: string }
  | { type: 'RESET_DONE' }
  | { type: 'DISCORD_STARTED'; flowId: string; pollSecret: string }
  | { type: 'DISCORD_NEEDS_USERNAME'; suggestedUsername: string }
  | { type: 'ACCOUNT'; account: Account }
  | { type: 'SIGNED_OUT'; reason?: string | null }
  /** Back to the login frame with a one-line notice (an email link was handled); keeps the lockout. */
  | { type: 'NOTICE'; reason: string };

export const signedOut = (view: AuthView = 'login', reason: string | null = null): AuthState => ({
  status: 'signedOut', view, busy: false, error: null, lockedUntil: null, email: null, discord: null, reason,
});

export const initialAuth: AuthState = { status: 'booting' };

/** True when a login attempt is refused locally because of an active lockout. */
export function isLocked(s: AuthState, now: number): boolean {
  return s.status === 'signedOut' && s.lockedUntil !== null && now < s.lockedUntil;
}

export function authReducer(s: AuthState, e: AuthEvent): AuthState {
  switch (e.type) {
    case 'BOOT_NONE':
      return s.status === 'booting' ? signedOut() : s;
    case 'SIGNED_IN':
      return { status: 'signedIn', account: e.account, token: e.token };
    case 'ACCOUNT':
      return s.status === 'signedIn' ? { ...s, account: e.account } : s;
    case 'SIGNED_OUT':
      return signedOut('login', e.reason ?? null);
  }
  if (s.status !== 'signedOut') return s;
  switch (e.type) {
    case 'SHOW':
      // Leaving the Discord wait abandons that flow; the lockout survives view changes.
      return { ...s, view: e.view, busy: false, error: null, reason: null, discord: e.view === 'discord' || e.view === 'discordUsername' ? s.discord : null };
    case 'NOTICE':
      return { ...s, view: 'login', busy: false, error: null, reason: e.reason, discord: null };
    case 'SUBMIT':
      if (s.busy) return s;
      if (s.view === 'login' && isLocked(s, e.now)) return s;
      return { ...s, busy: true, error: null, reason: null };
    case 'FAIL':
      return {
        ...s,
        busy: false,
        error: e.error,
        lockedUntil: e.retryAfterSeconds ? e.now + e.retryAfterSeconds * 1000 : s.lockedUntil,
      };
    case 'REGISTERED':
      return { ...s, view: 'verifyNotice', busy: false, error: null, email: e.email };
    case 'FORGOT_SENT':
      return { ...s, view: 'forgotSent', busy: false, error: null, email: e.email };
    case 'RESET_DONE':
      return { ...s, view: 'resetDone', busy: false, error: null };
    case 'DISCORD_STARTED':
      return { ...s, view: 'discord', busy: false, error: null, discord: { flowId: e.flowId, pollSecret: e.pollSecret } };
    case 'DISCORD_NEEDS_USERNAME':
      return s.discord ? { ...s, view: 'discordUsername', busy: false, discord: { ...s.discord, suggestedUsername: e.suggestedUsername } } : s;
  }
}
