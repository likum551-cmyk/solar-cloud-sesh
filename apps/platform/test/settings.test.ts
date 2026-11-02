import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { validateSettings } from '../src/settings.js';
import { createTestEnv, makeUser, type TestEnv, type TestUser } from './helpers.js';

let env: TestEnv;
let user: TestUser;
let max: number;

beforeAll(async () => {
  env = await createTestEnv();
  user = await makeUser(env);
  max = env.config.settingsMaxBytes;
});
afterAll(async () => {
  await env.close();
});

type ErrBody = { error: { code: string; message: string; details?: { problems?: string[]; currentVersion?: number } } };
interface SettingsBody { settings: Record<string, unknown>; version: number; updatedAt: string | null }

const get = (u = user) => env.call<SettingsBody>('/v1/me/settings', { token: u.token });
const put = (settings: unknown, extra: Record<string, unknown> = {}, u = user) => env.call<SettingsBody & ErrBody>('/v1/me/settings', { method: 'PUT', token: u.token, json: { settings, ...extra } });
const putRaw = (raw: string, u = user) => env.call<SettingsBody & ErrBody>('/v1/me/settings', { method: 'PUT', token: u.token, rawBody: raw, headers: { 'content-type': 'application/json' } });

/** A settings object whose JSON is exactly `bytes` long, using keys with strings under the per-string cap. */
function settingsOfSize(bytes: number): Record<string, string> {
  const out: Record<string, string> = {};
  for (let i = 0; ; i++) {
    const key = `k${i}`;
    out[key] = '';
    const pad = bytes - Buffer.byteLength(JSON.stringify(out));
    if (pad < 0) throw new Error(`cannot build settings of ${bytes} bytes`);
    if (pad <= 2000) {
      out[key] = 'x'.repeat(pad);
      return out;
    }
    out[key] = 'x'.repeat(2000);
  }
}

describe('reading and writing', () => {
  it('a new account has empty settings at version 0', async () => {
    const fresh = await makeUser(env);
    expect((await get(fresh)).body).toEqual({ settings: {}, version: 0, updatedAt: null });
  });

  it('replaces the whole object (no merge), counts versions, and round-trips unicode', async () => {
    const first = await put({ language: 'ka', gameFolder: 'D:\\Games\\GTA V', minimised: true, nested: { a: [1, 2, { b: null }] }, name: 'გიორგი 🚗' });
    expect(first.status).toBe(200);
    expect(first.body.version).toBe(1);
    expect(first.body.updatedAt).not.toBeNull();
    const read = await get();
    expect(read.body.settings).toEqual({ language: 'ka', gameFolder: 'D:\\Games\\GTA V', minimised: true, nested: { a: [1, 2, { b: null }] }, name: 'გიორგი 🚗' });

    const second = await put({ language: 'en' });
    expect(second.body.version).toBe(2);
    expect((await get()).body.settings).toEqual({ language: 'en' });
    expect((await put({})).body).toMatchObject({ settings: {}, version: 3 });
  });

  it('keeps every account\'s settings separate and requires a session', async () => {
    const a = await makeUser(env);
    const b = await makeUser(env);
    await put({ owner: 'a' }, {}, a);
    await put({ owner: 'b' }, {}, b);
    expect((await get(a)).body.settings).toEqual({ owner: 'a' });
    expect((await get(b)).body.settings).toEqual({ owner: 'b' });
    expect((await env.call('/v1/me/settings')).status).toBe(401);
    expect((await env.call('/v1/me/settings', { method: 'PUT', json: { settings: {} } })).status).toBe(401);
  });
});

describe('validation', () => {
  it('only a JSON object is accepted', async () => {
    for (const bad of [[], 'text', 5, null, true]) {
      const res = await put(bad);
      expect(res.status, JSON.stringify(bad)).toBe(400);
    }
    expect((await env.call('/v1/me/settings', { method: 'PUT', token: user.token, json: {} })).status).toBe(400); // "settings" missing
    expect((await putRaw('[1,2]')).status).toBe(400);
  });

  it('rejects odd keys, prototype keys, depth, long strings, long arrays and huge objects with the reasons', async () => {
    const reason = async (settings: unknown) => {
      const res = await put(settings);
      expect(res.status, JSON.stringify(settings).slice(0, 80)).toBe(400);
      expect(res.body.error.code).toBe('invalid_settings');
      return (res.body.error.details?.problems ?? []).join(' | ');
    };
    expect(await reason({ 'has space': 1 })).toContain('key must match');
    expect(await reason({ ['a'.repeat(65)]: 1 })).toContain('key must match');
    expect(await reason({ '': 1 })).toContain('key must match');
    expect(await reason({ 'ключ': 1 })).toContain('key must match');
    expect(await reason({ a: { b: { c: { d: { e: { f: { g: { h: 1 } } } } } } } })).toContain('nested deeper than 6');
    expect(await reason({ s: 'x'.repeat(2049) })).toContain('string longer than 2048');
    expect(await reason({ list: Array.from({ length: 201 }, (_, i) => i) })).toContain('array longer than 200');
    expect(await reason(Object.fromEntries(Array.from({ length: 201 }, (_, i) => [`k${i}`, 1])))).toContain('more than 200 keys');
    // Nothing was stored by any refused write.
    expect((await get()).body.settings).not.toHaveProperty('has space');
  });

  it('refuses __proto__, constructor and prototype keys sent as raw JSON, and does not pollute anything', async () => {
    for (const raw of ['{"settings":{"__proto__":{"polluted":true}}}', '{"settings":{"constructor":{"x":1}}}', '{"settings":{"ok":{"prototype":1}}}']) {
      const res = await putRaw(raw);
      expect(res.status, raw).toBe(400);
      expect(res.body.error.code, raw).toBe('invalid_settings');
    }
    expect(({} as Record<string, unknown>)['polluted']).toBeUndefined();
    expect(Object.prototype.hasOwnProperty.call(Object.prototype, 'polluted')).toBe(false);
  });

  it('the unit validator also rejects non-finite numbers, undefined and functions', () => {
    expect(validateSettings({ a: Number.NaN }, max)).toEqual([expect.stringContaining('not finite')]);
    expect(validateSettings({ a: Number.POSITIVE_INFINITY }, max)).toHaveLength(1);
    expect(validateSettings({ a: undefined }, max)).toEqual([expect.stringContaining('unsupported value type')]);
    expect(validateSettings({ a: () => 1 }, max)).toHaveLength(1);
    expect(validateSettings({ a: 1, b: [true, null, 'x'] }, max)).toEqual([]);
    expect(validateSettings([], max)).toEqual(['settings must be a JSON object']);
  });
});

describe('the size cap', () => {
  it('accepts exactly SETTINGS_MAX_BYTES and refuses one byte more', async () => {
    const exact = settingsOfSize(max);
    expect(Buffer.byteLength(JSON.stringify(exact))).toBe(max);
    expect((await put(exact)).status).toBe(200);
    const over = settingsOfSize(max + 1);
    expect(Buffer.byteLength(JSON.stringify(over))).toBe(max + 1);
    const res = await put(over);
    expect(res.status).toBe(400);
    expect(res.body.error.details?.problems).toEqual([`settings larger than ${max} bytes`]);
  });

  it('counts bytes, not characters', async () => {
    // 3-byte characters: well under the cap in characters, over it in bytes.
    // Sized to stay under the request body limit (SETTINGS_MAX_BYTES + 1024) so the settings rule is what refuses it.
    const settings: Record<string, string> = { k0: 'ა'.repeat(1800), k1: 'ა'.repeat(1800), k2: 'ა'.repeat(1800), k3: 'ა'.repeat(100) };
    const chars = Object.values(settings).join('').length;
    expect(Buffer.byteLength(JSON.stringify(settings))).toBeLessThan(max + 1024);
    expect(chars).toBeLessThan(max);
    expect(Buffer.byteLength(JSON.stringify(settings))).toBeGreaterThan(max);
    expect((await put(settings)).body.error.code).toBe('invalid_settings');
  });

  it('a request body far past the cap is cut off at the door with 413', async () => {
    const res = await putRaw(JSON.stringify({ settings: { blob: 'x'.repeat(max + 5000) } }));
    expect(res.status).toBe(413);
    expect(res.body.error.code).toBe('payload_too_large');
  });
});

describe('optimistic concurrency (expectedVersion)', () => {
  it('writes only if nobody wrote since; the conflict names the current version', async () => {
    const u = await makeUser(env);
    expect((await put({ n: 1 }, { expectedVersion: 0 }, u)).body.version).toBe(1);
    const stale = await put({ n: 2 }, { expectedVersion: 0 }, u);
    expect(stale.status).toBe(409);
    expect(stale.body.error.code).toBe('version_conflict');
    expect(stale.body.error.details?.currentVersion).toBe(1);
    expect((await put({ n: 2 }, { expectedVersion: 1 }, u)).body.version).toBe(2);
    const stale2 = await put({ n: 3 }, { expectedVersion: 1 }, u);
    expect(stale2.status).toBe(409);
    expect(stale2.body.error.details?.currentVersion).toBe(2);
    expect((await get(u)).body).toMatchObject({ settings: { n: 2 }, version: 2 });
    // Writing without expectedVersion overwrites regardless.
    expect((await put({ n: 9 }, {}, u)).body.version).toBe(3);
    expect((await put({ n: 10 }, { expectedVersion: 99 }, u)).status).toBe(409);
    expect((await put({ n: 10 }, { expectedVersion: -1 }, u)).status).toBe(400);
    expect((await put({ n: 10 }, { expectedVersion: 1.5 }, u)).status).toBe(400);
  });

  it('two PCs writing from the same version: one wins, the other is told', async () => {
    const u = await makeUser(env);
    await put({ start: true }, {}, u);
    const results = await Promise.all([put({ pc: 'A' }, { expectedVersion: 1 }, u), put({ pc: 'B' }, { expectedVersion: 1 }, u)]);
    expect(results.map((r) => r.status).sort()).toEqual([200, 409]);
    expect((await get(u)).body.version).toBe(2);
    const first = await Promise.all([put({ pc: 'A' }, { expectedVersion: 0 }, await makeUser(env)), put({ pc: 'B' }, { expectedVersion: 0 }, await makeUser(env))]);
    expect(first.map((r) => r.status)).toEqual([200, 200]); // different accounts do not interfere
  });

  it('simultaneous first writes with expectedVersion 0 create one row', async () => {
    const u = await makeUser(env);
    const results = await Promise.all([put({ pc: 'A' }, { expectedVersion: 0 }, u), put({ pc: 'B' }, { expectedVersion: 0 }, u)]);
    expect(results.map((r) => r.status).sort()).toEqual([200, 409]);
    expect((await get(u)).body.version).toBe(1);
  });
});

describe('write rate limit', () => {
  it('allows 120 writes per account per 10 minutes, then 429 with Retry-After; reads stay open', async () => {
    const u = await makeUser(env);
    let ok = 0;
    let limited: Awaited<ReturnType<typeof put>> | null = null;
    for (let i = 0; i < 125; i++) {
      const res = await put({ i }, {}, u);
      if (res.status === 200) ok++;
      else if (res.status === 429) limited ??= res;
    }
    expect(ok).toBe(120);
    expect(limited?.body.error.code).toBe('rate_limited');
    expect(Number(limited?.headers.get('retry-after'))).toBeGreaterThan(0);
    expect((await get(u)).status).toBe(200);
    env.clock.advanceSeconds(601);
    expect((await put({ again: true }, {}, u)).status).toBe(200);
  });
});
