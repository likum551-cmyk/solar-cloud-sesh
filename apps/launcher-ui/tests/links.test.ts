import { describe, expect, it } from 'vitest';
import { parseAuthLink, tokenFromPaste } from '../src/lib/links';

// A token shaped like the platform's: base64url of 32 random bytes (43 chars, mixed case, - and _).
const TOKEN = 'Xq3-Lw_9aZbK0pQrS7tUvW1yZ2cD4eF6gH8iJkLmNoP';

describe('parseAuthLink', () => {
  it('reads the platform mail links (default base gage-mp://auth)', () => {
    expect(TOKEN).toHaveLength(43);
    expect(parseAuthLink(`gage-mp://auth/verify-email?token=${TOKEN}`)).toEqual({ kind: 'verifyEmail', token: TOKEN });
    expect(parseAuthLink(`gage-mp://auth/reset-password?token=${encodeURIComponent(TOKEN)}`)).toEqual({ kind: 'resetPassword', token: TOKEN });
  });

  it('accepts an https base, a trailing slash and surrounding whitespace', () => {
    expect(parseAuthLink(`  https://solar-rp.com/auth/verify-email/?token=${TOKEN}\n`)).toEqual({ kind: 'verifyEmail', token: TOKEN });
  });

  it('limits schemes when asked (the native deep link takes gage-mp: only)', () => {
    const https = `https://solar-rp.com/auth/verify-email?token=${TOKEN}`;
    expect(parseAuthLink(https, ['gage-mp:'])).toBeNull();
    expect(parseAuthLink(`gage-mp://auth/verify-email?token=${TOKEN}`, ['gage-mp:'])).not.toBeNull();
    expect(parseAuthLink(`http://solar-rp.com/auth/verify-email?token=${TOKEN}`)).toBeNull();
    expect(parseAuthLink(`javascript://auth/verify-email?token=${TOKEN}`)).toBeNull();
  });

  it('refuses unknown paths, missing or malformed tokens and non-URLs', () => {
    expect(parseAuthLink(`gage-mp://auth/delete-account?token=${TOKEN}`)).toBeNull();
    expect(parseAuthLink('gage-mp://auth/verify-email')).toBeNull();
    expect(parseAuthLink('gage-mp://auth/verify-email?token=short')).toBeNull();
    expect(parseAuthLink(`gage-mp://auth/verify-email?token=${TOKEN}%3Cscript%3E`)).toBeNull();
    expect(parseAuthLink(`gage-mp://auth/verify-email?token=${'a'.repeat(201)}`)).toBeNull();
    expect(parseAuthLink('not a link')).toBeNull();
    expect(parseAuthLink('')).toBeNull();
  });
});

describe('tokenFromPaste', () => {
  it('takes the whole link of the right kind or the bare token', () => {
    expect(tokenFromPaste(`gage-mp://auth/reset-password?token=${TOKEN}`, 'resetPassword')).toBe(TOKEN);
    expect(tokenFromPaste(` ${TOKEN} `, 'resetPassword')).toBe(TOKEN);
    expect(tokenFromPaste(`gage-mp://auth/verify-email?token=${TOKEN}`, 'verifyEmail')).toBe(TOKEN);
  });

  it('refuses a link of the other kind and junk', () => {
    expect(tokenFromPaste(`gage-mp://auth/verify-email?token=${TOKEN}`, 'resetPassword')).toBeNull();
    expect(tokenFromPaste('hello', 'verifyEmail')).toBeNull();
    expect(tokenFromPaste('', 'verifyEmail')).toBeNull();
  });
});
