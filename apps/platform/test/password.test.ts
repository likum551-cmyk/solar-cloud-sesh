import { eq } from 'drizzle-orm';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { checkPassword, hashPassword, needsRehash, normalizePassword, PASSWORD_MAX_LENGTH, PASSWORD_MIN_LENGTH, verifyPassword } from '../src/auth/password.js';
import { isCommonPassword } from '../src/auth/common-passwords.js';
import { loadConfig } from '../src/config.js';
import { schema } from '../src/db/index.js';
import { createTestEnv, freshIp, makeUser, type TestEnv } from './helpers.js';

const CHEAP = { memoryKib: 1024, timeCost: 1, parallelism: 1 };

describe('hashing', () => {
  it('uses argon2id with the production defaults (OWASP: m=19456 KiB, t=2, p=1) when nothing is configured', async () => {
    // ENABLE_DEV_ROUTES=1 is only the explicit development opt-in loadConfig insists on; it does not touch argon2.
    const params = loadConfig({ ENABLE_DEV_ROUTES: '1' }).argon2;
    expect(params).toEqual({ memoryKib: 19_456, timeCost: 2, parallelism: 1, maxConcurrent: 2, maxQueue: 64 });
    const hashed = await hashPassword('a-long-passphrase-for-testing', params);
    expect(hashed).toMatch(/^\$argon2id\$v=19\$m=19456,t=2,p=1\$/);
    expect(await verifyPassword(hashed, 'a-long-passphrase-for-testing')).toBe(true);
    expect(await verifyPassword(hashed, 'a-long-passphrase-for-testing ')).toBe(false);
    expect(needsRehash(hashed, params)).toBe(false);
  });

  it('salts every hash and never stores the password', async () => {
    const a = await hashPassword('same-password-twice-1', CHEAP);
    const b = await hashPassword('same-password-twice-1', CHEAP);
    expect(a).not.toBe(b);
    expect(a).not.toContain('same-password-twice-1');
    expect(await verifyPassword(a, 'same-password-twice-1')).toBe(true);
    expect(await verifyPassword(b, 'same-password-twice-1')).toBe(true);
  });

  it('treats visually identical Unicode spellings as one password (NFKC) and is case sensitive', async () => {
    const hashed = await hashPassword('password-ＡＢＣ-123', CHEAP); // full-width letters
    expect(normalizePassword('ＡＢＣ')).toBe('ABC');
    expect(await verifyPassword(hashed, 'password-ABC-123')).toBe(true);
    expect(await verifyPassword(hashed, 'password-abc-123')).toBe(false);
    const accented = await hashPassword('café-au-lait-2024', CHEAP);
    expect(await verifyPassword(accented, 'café-au-lait-2024')).toBe(true); // precomposed vs combining accent
  });

  it('a malformed or empty stored hash is a plain false, never an exception', async () => {
    for (const bad of ['', 'not-a-hash', '$argon2id$garbage', '$2b$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ012']) {
      expect(await verifyPassword(bad, 'whatever-password')).toBe(false);
    }
  });

  it('knows when a hash is weaker than the current policy', async () => {
    const weak = await hashPassword('rehash-me-please-1', { memoryKib: 8, timeCost: 1, parallelism: 1 });
    expect(needsRehash(weak, CHEAP)).toBe(true);
    const strong = await hashPassword('rehash-me-please-1', { memoryKib: 2048, timeCost: 2, parallelism: 1 });
    expect(needsRehash(strong, CHEAP)).toBe(false); // stronger than required is fine
    expect(needsRehash('garbage', CHEAP)).toBe(true);
    const otherParallelism = await hashPassword('rehash-me-please-1', { memoryKib: 1024, timeCost: 1, parallelism: 2 });
    expect(needsRehash(otherParallelism, CHEAP)).toBe(true);
  });
});

describe('the strength policy', () => {
  it('enforces 10 to 128 characters, counted as code points after NFKC', () => {
    expect(PASSWORD_MIN_LENGTH).toBe(10);
    expect(PASSWORD_MAX_LENGTH).toBe(128);
    expect(checkPassword('Zq7!kd2Lm')).toBe('too_short'); // 9
    expect(checkPassword('Zq7!kd2Lmx')).toBeNull(); // 10
    expect(checkPassword('Zq7!'.repeat(32))).toBeNull(); // 128
    expect(checkPassword('Zq7!'.repeat(32) + 'x')).toBe('too_long'); // 129
    expect(checkPassword('🔑'.repeat(10))).toBeNull(); // 10 code points, 20 UTF-16 units
    expect(checkPassword('🔑'.repeat(9))).toBe('too_short');
    expect(checkPassword('')).toBe('too_short');
  });

  it('rejects common passwords, their digit-suffixed, upper-cased and leetspeak variants', () => {
    for (const p of ['password', 'password1234', 'PASSWORD123456', 'Passw0rd!!', 'p@ssw0rd2024', 'qwertyuiop', 'iloveyou123', 'letmein2024!']) {
      const problem = checkPassword(p);
      expect(['common', 'too_short'], p).toContain(problem);
    }
    expect(checkPassword('password1234')).toBe('common');
    expect(checkPassword('Passw0rd!!!!')).toBe('common');
    expect(isCommonPassword('Password')).toBe(true);
    expect(isCommonPassword('xK9#vL2$qW8!')).toBe(false);
  });

  it('rejects a password that contains the username or the email name', () => {
    expect(checkPassword('xx-GreatPlayer-9!', { username: 'greatplayer' })).toBe('contains_username');
    expect(checkPassword('xx-great-player-9!', { username: 'greatplayer' })).toBeNull();
    expect(checkPassword('my-pet-names-sparky-99', { email: 'sparky@example.test' })).toBe('contains_email');
    // Short names are too common as substrings to be a meaningful rule.
    expect(checkPassword('a-bob-friendly-passphrase', { username: 'bo' })).toBeNull();
    expect(checkPassword('jo-is-in-this-passphrase', { email: 'jo@example.test' })).toBeNull();
  });
});

describe('through the API', () => {
  let env: TestEnv;
  beforeAll(async () => {
    env = await createTestEnv();
  });
  afterAll(async () => {
    await env.close();
  });

  it('every reason has its own problem code in the 400 details', async () => {
    const register = (password: string, username: string) =>
      env.call<{ error: { code: string; details?: { problem?: string } } }>('/v1/auth/register', { json: { email: `${username.toLowerCase()}@example.test`, username, password }, ip: freshIp() });
    const cases: [string, string, string][] = [
      ['short', 'Pw_Short_1', 'too_short'],
      ['x'.repeat(129), 'Pw_Long_1', 'too_long'],
      ['password1234', 'Pw_Common_1', 'common'],
      ['my-Pw_Name_1-secret', 'Pw_Name_1', 'contains_username'],
    ];
    for (const [password, username, problem] of cases) {
      const res = await register(password, username);
      expect(res.status, problem).toBe(400);
      expect(res.body.error.code).toBe('weak_password');
      expect(res.body.error.details?.problem).toBe(problem);
    }
    expect((await register('a-perfectly-fine-passphrase', 'Pw_Fine_1')).status).toBe(201);
  });

  it('upgrades a weaker stored hash to the current parameters after a successful login', async () => {
    const u = await makeUser(env, { password: 'upgrade-me-passphrase-1' });
    const weak = await hashPassword('upgrade-me-passphrase-1', { memoryKib: 8, timeCost: 1, parallelism: 1 });
    await env.handle.db.update(schema.accounts).set({ passwordHash: weak }).where(eq(schema.accounts.id, u.accountId));
    const wrong = await env.call('/v1/auth/login', { json: { identifier: u.email, password: 'wrong-passphrase-123' }, ip: freshIp() });
    expect(wrong.status).toBe(401);
    expect((await env.handle.db.select().from(schema.accounts).where(eq(schema.accounts.id, u.accountId)))[0]?.passwordHash).toBe(weak); // a failed login changes nothing
    const ok = await env.call('/v1/auth/login', { json: { identifier: u.email, password: 'upgrade-me-passphrase-1' }, ip: freshIp() });
    expect(ok.status).toBe(200);
    const stored = (await env.handle.db.select().from(schema.accounts).where(eq(schema.accounts.id, u.accountId)))[0]?.passwordHash as string;
    expect(stored).not.toBe(weak);
    expect(stored).toMatch(/^\$argon2id\$v=19\$m=1024,t=1,p=1\$/);
    expect(await verifyPassword(stored, 'upgrade-me-passphrase-1')).toBe(true);
  });
});
