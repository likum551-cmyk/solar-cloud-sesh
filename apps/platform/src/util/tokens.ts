import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';

// Opaque random tokens. Only a SHA-256 of a token is ever stored: tokens carry 256 bits of
// entropy, so a fast hash is sufficient (unlike passwords, which use argon2id), and a database
// read leak does not yield usable tokens.

/** A URL-safe random token with `bytes` bytes (default 32 = 256 bits) of entropy. */
export function randomToken(bytes = 32): string {
  return randomBytes(bytes).toString('base64url');
}

/** Hex SHA-256 of a token (what the database stores and looks up). */
export function hashToken(token: string): string {
  return createHash('sha256').update(token, 'utf8').digest('hex');
}

/** Constant-time equality of two strings of any length (compares fixed-size digests, so the length does not leak). */
export function safeEqual(a: string, b: string): boolean {
  const da = createHash('sha256').update(a, 'utf8').digest();
  const db = createHash('sha256').update(b, 'utf8').digest();
  return timingSafeEqual(da, db);
}

/** base64url(SHA-256(input)), the PKCE S256 transform (RFC 7636). */
export function pkceChallenge(verifier: string): string {
  return createHash('sha256').update(verifier, 'ascii').digest('base64url');
}
