// Platform error codes (apps/platform/docs/api.md, "Error codes") as plain words for players. The
// platform's own `message` is never shown: it is written for developers and may change. Each answer
// names the problem and what to do, and says which field it belongs to when there is one.

import type { ApiFailure } from './api';
import { PASSWORD_MAX, PASSWORD_MIN, type LocalProblem } from './password';

export type FieldName = 'username' | 'email' | 'password' | 'identifier' | 'currentPassword' | 'newPassword';
export type Action = 'register' | 'login' | 'change-password' | 'sessions' | 'load' | 'logout' | 'quests' | 'discord';

export interface Problem {
  /** the field the message belongs under; absent = a message for the whole form */
  field?: FieldName;
  text: string;
}

export const PASSWORD_TEXT: Record<LocalProblem | 'common', string> = {
  too_short: `Use at least ${PASSWORD_MIN} characters.`,
  too_long: `Use at most ${PASSWORD_MAX} characters.`,
  common: 'That password is too common. Pick something less guessable.',
  contains_username: 'Your password can’t contain your username.',
  contains_email: 'Your password can’t contain the part of your email before the @.',
};

export const OFFLINE_TEXT = 'Accounts are offline right now. Try again in a few minutes.';
export const NETWORK_TEXT = 'Couldn’t reach Solar RP. Check your connection and try again.';
export const QUESTS_OFFLINE_TEXT = 'Quests aren’t available right now. Try again in a few minutes.';

/** "12 seconds", "1 minute", "3 minutes", "2 hours" (rounded up: never tell someone to retry too early). */
export function waitText(seconds: number): string {
  const s = Math.max(1, Math.ceil(seconds));
  if (s < 60) return `${s} second${s === 1 ? '' : 's'}`;
  const m = Math.ceil(s / 60);
  if (m < 60) return `${m} minute${m === 1 ? '' : 's'}`;
  const h = Math.ceil(m / 60);
  return `${h} hour${h === 1 ? '' : 's'}`;
}

function ref(requestId: string | null): string {
  return requestId ? ` (reference ${requestId.slice(0, 8)})` : '';
}

const ISSUE_FIELD: Record<string, FieldName> = {
  username: 'username',
  email: 'email',
  password: 'password',
  identifier: 'identifier',
  currentPassword: 'currentPassword',
  newPassword: 'newPassword',
};
/** What to type in a field the server (or the form) refused as malformed. */
export const ISSUE_TEXT_FOR: Record<FieldName, string> = {
  username: 'Use 3 to 20 characters: letters, digits and underscore.',
  email: 'Enter a valid email address.',
  password: 'Enter a password.',
  identifier: 'Enter your username or email.',
  currentPassword: 'Enter your current password.',
  newPassword: 'Enter a new password.',
};

/** Why Discord did not link, by the code a finished link flow reports (`{ status: "failed", code }`). */
export const LINK_FAILED_TEXT: Record<string, string> = {
  access_denied: 'You cancelled on Discord, so nothing was linked.',
  discord_already_linked: 'That Discord account is already linked to another Solar RP account.',
  already_has_discord: 'This account already has a Discord account linked.',
  password_changed: 'Your password changed while you were on Discord. Link Discord again.',
};
export const LINK_FAILED_DEFAULT = 'Discord didn’t confirm the link. Try again.';

export function linkFailedText(code: string): string {
  return Object.hasOwn(LINK_FAILED_TEXT, code) ? (LINK_FAILED_TEXT[code] as string) : LINK_FAILED_DEFAULT;
}

/** The words for a failed call made for `action`. */
export function describeFailure(f: ApiFailure, action: Action): Problem {
  if (f.kind === 'network') return { text: NETWORK_TEXT };
  if (f.kind === 'bad-response') {
    // the static site's 404 page or a proxy error page: the API is not there (yet, or right now)
    if (f.status === 404 || f.status >= 500) return { text: action === 'quests' ? QUESTS_OFFLINE_TEXT : OFFLINE_TEXT };
    return { text: `Something went wrong on our side. Try again in a moment.` };
  }
  const pwField: FieldName = action === 'change-password' ? 'newPassword' : 'password';
  switch (f.code) {
    case 'weak_password': {
      const p = f.details?.problem;
      const text = typeof p === 'string' && Object.hasOwn(PASSWORD_TEXT, p) ? PASSWORD_TEXT[p as keyof typeof PASSWORD_TEXT] : 'Pick a stronger password.';
      return { field: pwField, text };
    }
    case 'username_taken':
      return { field: 'username', text: 'That username is taken or reserved. Try another.' };
    case 'invalid_credentials':
      if (action === 'change-password') return { field: 'currentPassword', text: 'That isn’t your current password.' };
      if (action === 'discord') return { field: 'password', text: 'That isn’t your password.' };
      return { text: 'That username or email and password don’t match.' };
    case 'current_password_required':
      return { field: 'currentPassword', text: 'Enter your current password.' };
    case 'email_not_verified':
      return { text: 'This needs a verified email address, and email verification isn’t available yet.' };
    case 'email_required':
      return { text: 'This account has no email address, so it can’t have a password yet.' };
    case 'rate_limited':
      return {
        text: f.retryAfterSeconds !== null
          ? `Too many attempts. Try again in ${waitText(f.retryAfterSeconds)}.`
          : 'Too many attempts. Wait a while, then try again.',
      };
    case 'busy':
      return { text: 'The server is busy checking passwords. Try again in a few seconds.' };
    case 'unauthorized':
      return { text: 'You’re signed out. Log in again.' };
    case 'session_not_found':
      return { text: 'That session has already ended.' };
    case 'invalid_request': {
      const issues = f.details?.issues;
      if (Array.isArray(issues)) {
        for (const i of issues) {
          const path = typeof i === 'object' && i !== null ? (i as { path?: unknown }).path : undefined;
          const key = typeof path === 'string' ? (path.split('.')[0] ?? '') : '';
          const field = Object.hasOwn(ISSUE_FIELD, key) ? ISSUE_FIELD[key] : undefined;
          if (field) return { field, text: ISSUE_TEXT_FOR[field] };
        }
      }
      return { text: 'Check the form and try again.' };
    }
    case 'not_found':
      return { text: action === 'quests' ? QUESTS_OFFLINE_TEXT : OFFLINE_TEXT };
    // quests (POST /v1/me/quests/:slug/start and /claim)
    case 'quest_not_found':
    case 'quest_inactive':
      return { text: 'This quest has ended.' };
    case 'already_claimed':
      return { text: 'You already claimed this quest.' };
    case 'discord_required':
      return { text: 'Claiming quests needs a linked Discord account.' };
    case 'discord_already_claimed':
      return { text: 'Your Discord account already claimed this quest on another Solar RP account.' };
    case 'quest_not_started':
      return { text: 'Start the quest first, then claim it.' };
    case 'quest_wait': {
      const w = f.details?.waitSeconds;
      const s = typeof w === 'number' ? w : f.retryAfterSeconds;
      return { text: s !== null && s > 0 ? `Not yet: you can claim this in ${waitText(s)}.` : 'Not yet: try again in a moment.' };
    }
    case 'quest_unavailable':
      return { text: 'This quest can’t be completed yet.' };
    case 'account_banned':
      return { text: 'A banned account can’t claim quests.' };
    // linking Discord (POST /v1/me/discord/link and /link/finish)
    case 'discord_disabled':
      return { text: 'Discord linking isn’t switched on yet. Try again later.' };
    case 'already_has_discord':
      return { text: linkFailedText('already_has_discord') };
    case 'password_required':
      return { text: 'This account has no password yet. Set one under Change password, then link Discord.' };
    case 'flow_not_found':
      return { text: 'That Discord link didn’t finish. Link Discord again from this page.' };
    case 'flow_expired':
      return { text: 'The link took longer than 10 minutes. Link Discord again.' };
    case 'flow_used':
      return { text: 'This Discord link was already finished. If Discord doesn’t show as linked, link it again.' };
    default:
      if (f.status >= 500) return { text: `Something went wrong on our side. Try again in a moment${ref(f.requestId)}.` };
      return { text: `That didn’t work${ref(f.requestId)}. Try again.` };
  }
}
