import { describe, expect, it } from 'vitest';
import { ipGroup, isLoopbackHost, lastForwardedFor, normalizeIp } from '../src/util/ip.js';
import { ApiError, isCheckViolation, isForeignKeyViolation, isUniqueViolation, pgErrorInfo } from '../src/util/errors.js';
import { createLogger, errorFields } from '../src/util/logger.js';
import { hashToken, pkceChallenge, randomToken, safeEqual } from '../src/util/tokens.js';

describe('ip helpers', () => {
  it('canonicalises addresses so one client cannot hide behind two spellings', () => {
    expect(normalizeIp('203.0.113.7')).toBe('203.0.113.7');
    expect(normalizeIp(' 203.0.113.7 ')).toBe('203.0.113.7');
    expect(normalizeIp('::ffff:203.0.113.7')).toBe('203.0.113.7');
    expect(normalizeIp('::FFFF:CB00:7107')).toBe('203.0.113.7');
    expect(normalizeIp('2001:DB8:0:0:0:0:0:1')).toBe('2001:db8::1');
    expect(normalizeIp('2001:db8::0:1')).toBe('2001:db8::1');
    for (const bad of ['', 'localhost', '203.0.113.7:8080', '[::1]', 'fe80::1%eth0', '300.1.1.1', '1.2.3', 'example.com', "1.1.1.1'; drop table"]) {
      expect(normalizeIp(bad), bad).toBeNull();
    }
  });

  it('groups IPv6 by /64 and leaves IPv4 alone', () => {
    expect(ipGroup('203.0.113.7')).toBe('203.0.113.7');
    expect(ipGroup('2001:db8:aaaa:1::1')).toBe(ipGroup('2001:db8:aaaa:1:ffff:ffff:ffff:ffff'));
    expect(ipGroup('2001:db8:aaaa:1::1')).not.toBe(ipGroup('2001:db8:aaaa:2::1'));
    expect(ipGroup('2001:db8:aaaa:1::1')).toBe('2001:0db8:aaaa:0001::/64');
    expect(ipGroup('::1')).toBe('0000:0000:0000:0000::/64');
    expect(ipGroup('unknown')).toBe('unknown');
  });

  it('takes the last X-Forwarded-For entry, never one the client wrote', () => {
    expect(lastForwardedFor(undefined)).toBeNull();
    expect(lastForwardedFor('198.51.100.1')).toBe('198.51.100.1');
    expect(lastForwardedFor('6.6.6.6, 198.51.100.1')).toBe('198.51.100.1');
    expect(lastForwardedFor('6.6.6.6,198.51.100.1 ')).toBe('198.51.100.1');
    expect(lastForwardedFor('198.51.100.1, not-an-ip')).toBeNull();
    expect(lastForwardedFor('')).toBeNull();
    expect(lastForwardedFor('::ffff:198.51.100.1')).toBe('198.51.100.1');
  });

  it('knows which listen addresses only the local machine can reach', () => {
    for (const h of ['127.0.0.1', '127.1.2.3', 'localhost', '::1', '[::1]']) expect(isLoopbackHost(h), h).toBe(true);
    for (const h of ['0.0.0.0', '::', '10.0.0.5', '192.168.1.2', '203.0.113.7', 'example.com']) expect(isLoopbackHost(h), h).toBe(false);
  });
});

describe('tokens', () => {
  it('random tokens are URL safe, long and unique', () => {
    const seen = new Set<string>();
    for (let i = 0; i < 200; i++) {
      const t = randomToken();
      expect(t).toMatch(/^[A-Za-z0-9_-]{43}$/);
      seen.add(t);
    }
    expect(seen.size).toBe(200);
    expect(randomToken(48)).toHaveLength(64);
  });

  it('hashes are hex SHA-256 and compare in constant time over any length', () => {
    expect(hashToken('abc')).toBe('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    expect(safeEqual('abc', 'abc')).toBe(true);
    expect(safeEqual('abc', 'abd')).toBe(false);
    expect(safeEqual('abc', 'abcd')).toBe(false);
    expect(safeEqual('', '')).toBe(true);
    expect(safeEqual('', 'a')).toBe(false);
  });

  it('the PKCE transform matches RFC 7636 appendix B', () => {
    expect(pkceChallenge('dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk')).toBe('E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM');
  });
});

describe('error helpers', () => {
  const pgError = (code: string, extra: Record<string, unknown> = {}) => Object.assign(new Error(`pg ${code}`), { code, ...extra });
  const wrapped = (inner: Error) => Object.assign(new Error('Failed query: select 1'), { cause: inner });

  it('finds the Postgres error behind drizzle\'s wrapper', () => {
    const inner = pgError('23505', { constraint: 'accounts_username_lower_uq' });
    expect(pgErrorInfo(wrapped(inner))).toEqual({ code: '23505', constraint: 'accounts_username_lower_uq', message: 'pg 23505' });
    expect(pgErrorInfo(inner)?.code).toBe('23505');
    expect(pgErrorInfo(new Error('plain'))).toBeNull();
    expect(pgErrorInfo('not an error')).toBeNull();
    expect(pgErrorInfo(Object.assign(new Error('x'), { code: 'ECONNRESET' }))).toBeNull(); // Node codes are not SQLSTATE
  });

  it('classifies unique, check and foreign-key violations, optionally by constraint', () => {
    const unique = wrapped(pgError('23505', { constraint: 'a_uq' }));
    expect(isUniqueViolation(unique)).toBe(true);
    expect(isUniqueViolation(unique, 'a_uq')).toBe(true);
    expect(isUniqueViolation(unique, 'other')).toBe(false);
    expect(isUniqueViolation(wrapped(pgError('23514')))).toBe(false);

    const check = wrapped(pgError('23514', { constraint: 'c_chk' }));
    expect(isCheckViolation(check)).toBe(true);
    expect(isCheckViolation(check, 'c_chk')).toBe(true);
    expect(isCheckViolation(wrapped(pgError('23514')), 'insufficient')).toBe(false);
    expect(isCheckViolation(wrapped(Object.assign(pgError('23514'), { message: 'insufficient_funds' })), 'insufficient')).toBe(true);

    expect(isForeignKeyViolation(wrapped(pgError('23503')))).toBe(true);
    expect(isForeignKeyViolation(wrapped(pgError('23001')))).toBe(true); // ON DELETE RESTRICT
    expect(isForeignKeyViolation(wrapped(pgError('23505')))).toBe(false);
    expect(isForeignKeyViolation(wrapped(pgError('23503', { constraint: 'fk_a' })), 'fk_b')).toBe(false);
  });

  it('ApiError carries status, code, details and Retry-After seconds', () => {
    const e = new ApiError(429, 'rate_limited', 'slow down', { retryAfterSeconds: 7, details: { x: 1 } });
    expect(e).toBeInstanceOf(Error);
    expect(e).toMatchObject({ status: 429, code: 'rate_limited', message: 'slow down', retryAfterSeconds: 7, details: { x: 1 } });
    expect(new ApiError(404, 'nf', 'no').details).toBeUndefined();
  });
});

describe('logger', () => {
  it('writes one JSON object per line, drops lines below the level and cannot be forged through field values', () => {
    const lines: string[] = [];
    const log = createLogger({ level: 'info', write: (l) => lines.push(l), now: () => Date.UTC(2026, 0, 2, 3, 4, 5) });
    log.debug('hidden');
    log.info('hello', { user: 'a\n{"msg":"forged","level":"error"}', msg: 'override attempt', level: 'x' });
    log.error('bad', errorFields(Object.assign(new Error('boom'), { code: 'E_BOOM' })));
    expect(lines).toHaveLength(2);
    const first = JSON.parse(lines[0] as string) as Record<string, unknown>;
    expect(first).toMatchObject({ ts: '2026-01-02T03:04:05.000Z', level: 'info', msg: 'hello', field_msg: 'override attempt', field_level: 'x' });
    expect(lines[0]).not.toContain('\n');
    const second = JSON.parse(lines[1] as string) as Record<string, unknown>;
    expect(second).toMatchObject({ level: 'error', err: 'boom', code: 'E_BOOM' });
    expect(typeof second['stack']).toBe('string');
    expect(errorFields('just a string')).toEqual({ err: 'just a string' });
  });
});
