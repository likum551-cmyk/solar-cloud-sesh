// Email links. The platform mails `<EMAIL_LINK_BASE_URL>/verify-email?token=…` and
// `…/reset-password?token=…` (default base `gage-mp://auth`). Windows hands a clicked
// `gage-mp://` link to SolarLauncher.exe, which forwards it as a `deepLink` bridge message
// (NATIVE-INTEGRATION.md); a link opened on another device is pasted into the launcher instead.
// Tokens are only ever POSTed, never fetched by GET.

export type AuthLinkKind = 'verifyEmail' | 'resetPassword';

export interface AuthLink {
  kind: AuthLinkKind;
  token: string;
}

/** The platform's token shape: base64url, 16..200 chars (apps/platform tokenSchema). */
const TOKEN = /^[A-Za-z0-9_-]{16,200}$/;

const PATHS: Record<string, AuthLinkKind> = {
  'verify-email': 'verifyEmail',
  'reset-password': 'resetPassword',
};

/**
 * Parse an emailed auth link. `scheme` limits which schemes count: the native deep link only
 * accepts `gage-mp:`; a pasted link may also be the https form of a custom EMAIL_LINK_BASE_URL.
 */
export function parseAuthLink(raw: string, schemes: readonly string[] = ['gage-mp:', 'https:']): AuthLink | null {
  const text = raw.trim();
  if (text.length === 0 || text.length > 2048) return null;
  let url: URL;
  try {
    url = new URL(text);
  } catch {
    return null;
  }
  if (!schemes.includes(url.protocol.toLowerCase())) return null;
  const last = url.pathname.replace(/\/+$/, '').split('/').pop() ?? '';
  const kind = PATHS[last.toLowerCase()];
  const token = url.searchParams.get('token');
  if (!kind || !token || !TOKEN.test(token)) return null;
  return { kind, token };
}

/**
 * The token from a pasted value: a full link of the expected kind, or the bare token.
 * Null for a link of the other kind, or anything that is not a token.
 */
export function tokenFromPaste(raw: string, kind: AuthLinkKind): string | null {
  const text = raw.trim();
  if (TOKEN.test(text)) return text;
  const link = parseAuthLink(text);
  return link && link.kind === kind ? link.token : null;
}
