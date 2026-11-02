// Regression tests for the hardening items of the security review that are not one of the six
// exploits in security.test.ts: the argon2 load limiter, startup refusals around the dev routes and
// the fake-provider secret, mail that must not leave live tokens in the database, password changes
// that void outstanding reset tokens, and the per-account limit on POST /v1/me/password.
//
// New exports are imported inside the tests (dynamic import) so that, run against the unfixed
// code, each test fails on its own assertion instead of the whole file failing to load.
import { eq } from 'drizzle-orm';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { ConfigError, DEV_FAKE_WEBHOOK_SECRET, loadConfig } from '../src/config.js';
import { schema } from '../src/db/index.js';
import { createTestEnv, freshIp, mailToken, makeUser, type TestEnv } from './helpers.js';

type ErrBody = { error: { code: string; message: string } };

function problems(env: Record<string, string>): string[] {
  try {
    loadConfig(env);
    return [];
  } catch (err) {
    if (err instanceof ConfigError) return err.issues;
    throw err;
  }
}

describe('startup refusals', () => {
  const SECRET = 'a-webhook-secret-of-16+chars';

  it('refuses to start when NODE_ENV is unset, unless dev routes are explicitly enabled', () => {
    expect(problems({ FAKE_WEBHOOK_SECRET: SECRET }).join('\n')).toMatch(/NODE_ENV is not set/);
    expect(problems({})).not.toEqual([]);
    // Explicit opt-in: a developer machine.
    expect(problems({ ENABLE_DEV_ROUTES: '1' })).toEqual([]);
    const c = loadConfig({ ENABLE_DEV_ROUTES: '1' });
    expect(c.nodeEnv).toBe('development');
    expect(c.devRoutes).toBe(true);
  });

  it('never falls back to the hardcoded dev webhook secret unless dev routes are enabled', () => {
    expect(problems({ NODE_ENV: 'development' }).join('\n')).toMatch(/FAKE_WEBHOOK_SECRET/);
    expect(problems({ NODE_ENV: 'test' }).join('\n')).toMatch(/FAKE_WEBHOOK_SECRET/);
    expect(loadConfig({ NODE_ENV: 'development', ENABLE_DEV_ROUTES: '1' }).fakeWebhookSecret).toBe(DEV_FAKE_WEBHOOK_SECRET);
    const own = loadConfig({ NODE_ENV: 'development', FAKE_WEBHOOK_SECRET: SECRET });
    expect(own.fakeWebhookSecret).toBe(SECRET);
    expect(own.devRoutes).toBe(false);
    expect(problems({ NODE_ENV: 'development', FAKE_WEBHOOK_SECRET: 'short' }).join('\n')).toMatch(/FAKE_WEBHOOK_SECRET/);
  });

  it('refuses dev routes in production, whatever else is set', () => {
    const production = { NODE_ENV: 'production', DATABASE_URL: 'postgres://u:p@h/db', ALLOW_FAKE_PAYMENTS_IN_PRODUCTION: '1', FAKE_WEBHOOK_SECRET: SECRET };
    expect(problems(production)).toEqual([]);
    expect(problems({ ...production, ENABLE_DEV_ROUTES: '1' }).join('\n')).toMatch(/ENABLE_DEV_ROUTES/);
  });

  it('mounts /v1/dev/* only when ENABLE_DEV_ROUTES=1', async () => {
    const orderId = '11111111-1111-4111-8111-111111111111';
    const off = await createTestEnv({ env: { ENABLE_DEV_ROUTES: '0' } });
    const on = await createTestEnv({ env: { ENABLE_DEV_ROUTES: '1' } });
    try {
      const offRes = await off.call<ErrBody>(`/v1/dev/fake-checkout/${orderId}`);
      expect(offRes.status).toBe(404);
      expect(offRes.body.error.code).toBe('not_found'); // the route does not exist
      expect((await off.call<ErrBody>(`/v1/dev/fake-checkout/${orderId}/complete`, { method: 'POST', json: {} })).body.error.code).toBe('not_found');
      const onRes = await on.call<ErrBody>(`/v1/dev/fake-checkout/${orderId}`);
      expect(onRes.status).toBe(404);
      expect(onRes.body.error.code).toBe('order_not_found'); // the route exists and found no such order
    } finally {
      await off.close();
      await on.close();
    }
  });
});

describe('mail does not leave live tokens behind', () => {
  let env: TestEnv;
  beforeAll(async () => {
    env = await createTestEnv();
  });
  afterAll(async () => {
    await env.close();
  });

  it('the outbox row holds a redacted body and no token in meta; the log has none either', async () => {
    const email = 'outbox.redact@example.test';
    const reg = await env.call('/v1/auth/register', { json: { email, username: 'Outbox_Redact', password: 'a-long-unusual-passphrase-1' }, ip: freshIp() });
    expect(reg.status).toBe(201);
    const token = await mailToken(env, email, 'verify_email'); // what the recipient would get
    expect(token.length).toBeGreaterThanOrEqual(40);

    const rows = (await env.handle.db.select().from(schema.emailOutbox).where(eq(schema.emailOutbox.toAddress, email))).filter((r) => r.template === 'verify_email');
    expect(rows).toHaveLength(1);
    const row = rows[0] as (typeof rows)[number];
    for (const stored of [row.body, JSON.stringify(row.meta), row.subject]) {
      expect(stored).not.toContain(token);
      expect(stored).not.toContain(encodeURIComponent(token));
    }
    expect(row.body).toContain('[redacted]');
    expect(row.body).toContain('Outbox_Redact'); // the rest of the message is still useful in development
    expect(env.logs.join('\n')).not.toContain(token);

    // A reset mail is redacted the same way.
    await env.call('/v1/auth/forgot-password', { json: { email }, ip: freshIp() });
    const resetToken = await mailToken(env, email, 'password_reset');
    const reset = (await env.handle.db.select().from(schema.emailOutbox).where(eq(schema.emailOutbox.toAddress, email))).find((r) => r.template === 'password_reset');
    expect(reset?.body).not.toContain(resetToken);
    expect(JSON.stringify(reset?.meta)).not.toContain(resetToken);
  });

  it('the outbox logs the link only in development', async () => {
    const dev = await createTestEnv({ env: { NODE_ENV: 'development' } });
    try {
      const email = 'dev.link@example.test';
      await dev.call('/v1/auth/register', { json: { email, username: 'Dev_Link_1', password: 'a-long-unusual-passphrase-2' }, ip: freshIp() });
      const token = await mailToken(dev, email, 'verify_email');
      expect(dev.logs.join('\n')).toContain(`verify-email?token=${encodeURIComponent(token)}`);
      // Still nothing readable in the table.
      const row = (await dev.handle.db.select().from(schema.emailOutbox).where(eq(schema.emailOutbox.toAddress, email)))[0];
      expect(row?.body).not.toContain(token);
    } finally {
      await dev.close();
    }
  });
});

describe('password changes', () => {
  let env: TestEnv;
  beforeAll(async () => {
    env = await createTestEnv();
  });
  afterAll(async () => {
    await env.close();
  });

  it('changing the password voids an outstanding reset token', async () => {
    const u = await makeUser(env);
    expect((await env.call('/v1/auth/forgot-password', { json: { email: u.email }, ip: freshIp() })).status).toBe(202);
    const token = await mailToken(env, u.email, 'password_reset');
    const changed = await env.call('/v1/me/password', { token: u.token, json: { currentPassword: u.password, newPassword: 'changed-before-the-reset-1' }, ip: freshIp() });
    expect(changed.status).toBe(200);
    const res = await env.call<ErrBody>('/v1/auth/reset-password', { json: { token, password: 'reset-after-the-change-9' }, ip: freshIp() });
    expect(res.status).toBe(400);
    expect(res.body.error.code).toBe('invalid_token');
    // The new password still works: the voided link changed nothing.
    expect((await env.call('/v1/auth/login', { json: { identifier: u.email, password: 'changed-before-the-reset-1' }, ip: freshIp() })).status).toBe(200);
  });

  it('POST /v1/me/password is limited per account (10 per hour), also for correct passwords', async () => {
    const u = await makeUser(env);
    let current = u.password;
    for (let i = 0; i < 10; i++) {
      const next = `rotation-passphrase-number-${i}-x`;
      const res = await env.call('/v1/me/password', { token: u.token, json: { currentPassword: current, newPassword: next }, ip: freshIp() });
      expect(res.status, `change ${i}`).toBe(200);
      current = next;
    }
    const eleventh = await env.call<ErrBody>('/v1/me/password', { token: u.token, json: { currentPassword: current, newPassword: 'rotation-passphrase-number-11-x' }, ip: freshIp() });
    expect(eleventh.status).toBe(429);
    expect(eleventh.body.error.code).toBe('rate_limited');
    expect(Number(eleventh.headers.get('retry-after'))).toBeGreaterThan(0);
    // Another account is unaffected.
    const other = await makeUser(env);
    expect((await env.call('/v1/me/password', { token: other.token, json: { currentPassword: other.password, newPassword: 'another-accounts-passphrase-1' }, ip: freshIp() })).status).toBe(200);
  });
});

describe('argon2 load limiter', () => {
  it('runs at most maxConcurrent hashes, queues up to maxQueue and refuses the rest with 503 busy + Retry-After', async () => {
    const { HashLimiter } = await import('../src/auth/password.js');
    const limiter = new HashLimiter({ maxConcurrent: 1, maxQueue: 1 });
    const order: string[] = [];
    let release!: () => void;
    const gate = new Promise<void>((resolve) => {
      release = resolve;
    });
    const first = limiter.run(async () => {
      order.push('first');
      await gate;
    });
    const second = limiter.run(async () => {
      order.push('second');
    });
    expect(limiter.stats()).toEqual({ active: 1, queued: 1 });
    await expect(limiter.run(async () => void order.push('third'))).rejects.toMatchObject({ status: 503, code: 'busy' });
    const refused = await limiter.run(async () => undefined).then(() => null, (e: unknown) => e as { retryAfterSeconds?: number });
    expect(refused?.retryAfterSeconds).toBeGreaterThan(0);
    release();
    await first;
    await second;
    expect(order).toEqual(['first', 'second']);
    expect(limiter.stats()).toEqual({ active: 0, queued: 0 });
  });

  it('never exceeds the cap, hands slots on in order and frees a slot when a task throws', async () => {
    const { HashLimiter } = await import('../src/auth/password.js');
    const limiter = new HashLimiter({ maxConcurrent: 3, maxQueue: 20 });
    let running = 0;
    let peak = 0;
    const finished: number[] = [];
    await Promise.all(
      Array.from({ length: 12 }, (_, i) =>
        limiter.run(async () => {
          running++;
          peak = Math.max(peak, running);
          await new Promise((r) => setTimeout(r, 5));
          running--;
          finished.push(i);
        }),
      ),
    );
    expect(peak).toBe(3);
    expect(finished).toHaveLength(12);
    await expect(limiter.run(async () => Promise.reject(new Error('boom')))).rejects.toThrow('boom');
    expect(limiter.stats()).toEqual({ active: 0, queued: 0 });
  });

  it('every hash and verify path goes through it: a saturated limiter answers 503 busy on login, register, reset and password change', async () => {
    const pw = await import('../src/auth/password.js');
    const env = await createTestEnv();
    try {
      const u = await makeUser(env);
      await env.call('/v1/auth/forgot-password', { json: { email: u.email }, ip: freshIp() });
      const resetToken = await mailToken(env, u.email, 'password_reset');

      pw.hashLimiter.configure({ maxConcurrent: 1, maxQueue: 0 });
      let release!: () => void;
      const held = pw.hashLimiter.run(() => new Promise<void>((resolve) => {
        release = resolve;
      }));
      try {
        const busy = async (path: string, opts: Parameters<TestEnv['call']>[1]) => {
          const res = await env.call<ErrBody>(path, opts);
          expect(res.status, path).toBe(503);
          expect(res.body.error.code, path).toBe('busy');
          expect(Number(res.headers.get('retry-after')), path).toBeGreaterThan(0);
        };
        await busy('/v1/auth/login', { json: { identifier: u.email, password: u.password }, ip: freshIp() });
        await busy('/v1/auth/login', { json: { identifier: 'nobody.at.all@example.test', password: 'whatever-it-is-1' }, ip: freshIp() });
        await busy('/v1/auth/register', { json: { email: 'busy.new@example.test', username: 'Busy_New_1', password: 'a-long-unusual-passphrase-3' }, ip: freshIp() });
        await busy('/v1/auth/reset-password', { json: { token: resetToken, password: 'a-long-unusual-passphrase-4' }, ip: freshIp() });
        await busy('/v1/me/password', { token: u.token, json: { currentPassword: u.password, newPassword: 'a-long-unusual-passphrase-5' }, ip: freshIp() });
      } finally {
        release();
        await held;
        pw.hashLimiter.configure({ maxConcurrent: 2, maxQueue: 64 });
      }
      // Once there is room again everything works, including the dummy hash behind unknown identifiers (a rejected one must not be cached).
      expect((await env.call('/v1/auth/login', { json: { identifier: u.email, password: u.password }, ip: freshIp() })).status).toBe(200);
      expect((await env.call<ErrBody>('/v1/auth/login', { json: { identifier: 'nobody.at.all@example.test', password: 'whatever-it-is-1' }, ip: freshIp() })).body.error.code).toBe('invalid_credentials');
      // A refused request did not spend the reset token.
      expect((await env.call('/v1/auth/reset-password', { json: { token: resetToken, password: 'a-long-unusual-passphrase-6' }, ip: freshIp() })).status).toBe(200);
    } finally {
      await env.close();
    }
  });
});
