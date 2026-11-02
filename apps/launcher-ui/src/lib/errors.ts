// One failure type for everything the API client can hit, so screens switch on `kind` and
// `code` instead of parsing messages. Human copy for each case lives in describeFailure().

/** Why an API call did not produce a validated result. */
export type FailureKind =
  /** No API base configured (the native host has not sent one and no build default exists). */
  | 'not_configured'
  /** The request never reached the API: no network, DNS, TLS, CORS or connection refused. */
  | 'offline'
  /** No answer within the client timeout. */
  | 'timeout'
  /** 401: the session token is missing, expired or revoked. */
  | 'unauthorized'
  /** 429: too many attempts; `retryAfterSeconds` says when to try again (login lockout). */
  | 'rate_limited'
  /** 5xx, or the API answered with something that is not its error shape. */
  | 'server'
  /** A deliberate refusal with a stable `code` (weak_password, server_maintenance, ...). */
  | 'refused'
  /** 2xx with a body that does not match the expected schema. */
  | 'invalid_response';

export class ApiFailure extends Error {
  readonly kind: FailureKind;
  readonly status: number | null;
  readonly code: string | null;
  readonly retryAfterSeconds: number | null;
  readonly details: unknown;

  constructor(kind: FailureKind, message: string, opts: { status?: number | null; code?: string | null; retryAfterSeconds?: number | null; details?: unknown } = {}) {
    super(message);
    this.name = 'ApiFailure';
    this.kind = kind;
    this.status = opts.status ?? null;
    this.code = opts.code ?? null;
    this.retryAfterSeconds = opts.retryAfterSeconds ?? null;
    this.details = opts.details;
  }
}

/** True when `e` is an ApiFailure of one of the given kinds. */
export function isFailure(e: unknown, ...kinds: FailureKind[]): e is ApiFailure {
  return e instanceof ApiFailure && (kinds.length === 0 || kinds.includes(e.kind));
}

/** Copy for a failure, by code first (the API's own refusals), then by kind. */
export function describeFailure(e: unknown): string {
  if (!(e instanceof ApiFailure)) return 'Something went wrong in the client. Try again.';
  if (e.kind === 'refused' && e.code) {
    const byCode: Record<string, string> = {
      invalid_credentials: 'That email or username and password do not match.',
      weak_password: e.message,
      username_taken: 'That username is taken. Pick another.',
      invalid_token: 'This link has expired or was already used. Request a new one.',
      email_not_verified: 'Verify your email first. We sent you a link when you registered.',
      account_banned: e.message,
      server_maintenance: 'This server is in maintenance. Pick another or try again later.',
      server_not_found: 'This server is no longer listed. Refresh and pick another.',
      discord_disabled: 'Discord login is switched off on this network right now.',
      flow_expired: 'The Discord login took too long. Start it again.',
      flow_used: 'That Discord login was already used. Start it again.',
      payment_unavailable: 'Payments are unavailable right now. Nothing was charged.',
      product_inactive: 'This item is no longer on sale.',
      product_not_found: 'This item is no longer on sale.',
      current_password_required: 'Enter your current password.',
      last_login_method: 'Set a password before unlinking Discord, or you could not log in again.',
      session_not_found: 'That device was already signed out.',
      version_conflict: 'Another device changed these settings first. Reload and try again.',
    };
    return byCode[e.code] ?? e.message;
  }
  switch (e.kind) {
    case 'not_configured': return 'This client has no network address yet. Update Solar Launcher or check your install.';
    case 'offline': return 'Cannot reach the Solar RP network. Check your connection; we will keep trying.';
    case 'timeout': return 'The Solar RP network did not answer in time. Try again.';
    case 'unauthorized': return 'Your session ended. Log in again.';
    case 'rate_limited': return e.retryAfterSeconds
      ? `Too many attempts. Try again in ${formatWait(e.retryAfterSeconds)}.`
      : 'Too many attempts. Wait a moment and try again.';
    case 'server': return 'The Solar RP network had a problem answering. Try again in a minute.';
    case 'invalid_response': return 'The Solar RP network sent an answer this client does not understand. Update Solar Launcher.';
    case 'refused': return e.message;
  }
}

/** "45 s", "3 min", "2 h 5 min". */
export function formatWait(seconds: number): string {
  const s = Math.max(0, Math.ceil(seconds));
  if (s < 60) return `${s} s`;
  const m = Math.ceil(s / 60);
  if (m < 60) return `${m} min`;
  const h = Math.floor(m / 60);
  const rest = m % 60;
  return rest ? `${h} h ${rest} min` : `${h} h`;
}
