import { describe, expect, it } from 'vitest';
import { ConfigError, DEV_FAKE_WEBHOOK_SECRET, loadConfig, type Config } from '../src/config.js';

/**
 * loadConfig refuses an environment that does not say what it is (NODE_ENV unset) unless
 * ENABLE_DEV_ROUTES=1 opts in to development explicitly. These cases are about other rules, so an
 * environment without NODE_ENV gets that opt-in here; hardening.test.ts covers the refusal itself.
 */
const load = (env: Record<string, string>): Config => loadConfig(env.NODE_ENV === undefined ? { ENABLE_DEV_ROUTES: '1', ...env } : env);

const issuesOf = (env: Record<string, string>): string[] => {
  try {
    load(env);
  } catch (err) {
    if (err instanceof ConfigError) return err.issues;
    throw err;
  }
  return [];
};

const PRODUCTION = {
  NODE_ENV: 'production',
  DATABASE_URL: 'postgres://gage:not-a-real-password@db.internal:5432/gage',
  ALLOW_FAKE_PAYMENTS_IN_PRODUCTION: '1',
  FAKE_WEBHOOK_SECRET: 'a-staging-only-secret-0123456789',
};

describe('defaults', () => {
  it('an environment that only opts in to development (ENABLE_DEV_ROUTES=1) gets the documented defaults', () => {
    const c = load({});
    expect(c).toMatchObject({
      nodeEnv: 'development', host: '127.0.0.1', port: 8787, internalHost: '127.0.0.1', internalPort: 8788,
      internalOnPublicListener: false, databaseUrl: 'pglite:memory', corsOrigins: ['https://app.gage'], trustProxy: false,
      bodyLimitBytes: 65_536, sessionTtlDays: 30, sessionMaxDays: 180, requireVerifiedEmail: true,
      joinTokenTtlSeconds: 60, serverStatusTtlMs: 5000, settingsMaxBytes: 16_384, paymentProvider: 'fake',
      argon2: { memoryKib: 19_456, timeCost: 2, parallelism: 1, maxConcurrent: 2, maxQueue: 64 }, discord: null, fakeWebhookSecret: DEV_FAKE_WEBHOOK_SECRET,
      devRoutes: true,
      login: { accountMaxFailures: 100, accountIpMaxFailures: 5, ipMaxFailures: 20 },
    });
  });

  it('unset and empty variables are the same thing', () => {
    expect(load({ PORT: '', CORS_ORIGINS: '', DATABASE_URL: '' }).port).toBe(8787);
    expect(load({ CORS_ORIGINS: '' }).corsOrigins).toEqual(['https://app.gage']);
  });
});

describe('strict parsing', () => {
  it('reports every bad variable at once, by name', () => {
    const issues = issuesOf({ PORT: 'abc', TRUST_PROXY: 'true', LOG_LEVEL: 'loud', SESSION_TTL_DAYS: '0' });
    expect(issues).toHaveLength(4);
    for (const name of ['PORT', 'TRUST_PROXY', 'LOG_LEVEL', 'SESSION_TTL_DAYS']) expect(issues.some((i) => i.startsWith(name)), name).toBe(true);
  });

  it('integers are plain base-10 digits within range: no exponent, hex, sign, space or decimals', () => {
    for (const bad of ['1e3', '0x10', ' 5', '-1', '5.5', '65536', '99999999999', 'five']) {
      expect(issuesOf({ PORT: bad }), bad).not.toEqual([]);
    }
    expect(load({ PORT: '0' }).port).toBe(0);
    expect(load({ PORT: '65535' }).port).toBe(65535);
  });

  it('flags are exactly 0 or 1: "true", "yes" and "on" are errors, never a silent false', () => {
    for (const bad of ['true', 'yes', 'on', 'TRUE', '2']) expect(issuesOf({ TRUST_PROXY: bad }), bad).not.toEqual([]);
    expect(load({ TRUST_PROXY: '1' }).trustProxy).toBe(true);
    expect(load({ TRUST_PROXY: '0' }).trustProxy).toBe(false);
  });

  it('URLs are http(s); a trailing slash is dropped', () => {
    expect(load({ PUBLIC_API_URL: 'https://api.example.test///' }).publicApiUrl).toBe('https://api.example.test');
    expect(issuesOf({ PUBLIC_API_URL: 'ftp://api.example.test' })).not.toEqual([]);
    expect(issuesOf({ PUBLIC_API_URL: 'not a url' })).not.toEqual([]);
  });

  it('cross-variable rules: the session cap and the lockout bounds', () => {
    expect(issuesOf({ SESSION_TTL_DAYS: '60', SESSION_MAX_DAYS: '30' })).toEqual(['SESSION_TTL_DAYS must not exceed SESSION_MAX_DAYS']);
    expect(issuesOf({ LOGIN_LOCKOUT_BASE_SECONDS: '600', LOGIN_LOCKOUT_MAX_SECONDS: '60' })).toEqual(['LOGIN_LOCKOUT_BASE_SECONDS must not exceed LOGIN_LOCKOUT_MAX_SECONDS']);
  });

  it('a bad value never echoes the secret it was given', () => {
    const issues = issuesOf({ FAKE_WEBHOOK_SECRET: 'short-secret' }).join(' ');
    expect(issues).toContain('FAKE_WEBHOOK_SECRET');
    expect(issues).not.toContain('short-secret');
    const dbIssues = issuesOf({ DATABASE_URL: 'mysql://root:hunter2@db/x' }).join(' ');
    expect(dbIssues).toContain('DATABASE_URL');
    expect(dbIssues).not.toContain('hunter2');
  });
});

describe('CORS_ORIGINS', () => {
  it('takes a comma separated list of bare origins', () => {
    expect(load({ CORS_ORIGINS: 'https://one.example, http://localhost:5173 ,https://two.example:8443' }).corsOrigins).toEqual(['https://one.example', 'http://localhost:5173', 'https://two.example:8443']);
  });

  it('refuses a wildcard, a path, a missing scheme or whitespace inside an origin (a wildcard would defeat the restriction)', () => {
    for (const bad of ['*', 'https://*.example.test', 'https://app.gage/', 'https://app.gage/path', 'app.gage', 'https://a b.example']) {
      const issues = issuesOf({ CORS_ORIGINS: bad });
      expect(issues.some((i) => i.startsWith('CORS_ORIGINS')), bad).toBe(true);
    }
  });
});

describe('production safety', () => {
  it('a complete production environment is accepted', () => {
    const c = load(PRODUCTION);
    expect(c.nodeEnv).toBe('production');
    expect(c.databaseUrl).toBe(PRODUCTION.DATABASE_URL);
    expect(c.fakeWebhookSecret).toBe(PRODUCTION.FAKE_WEBHOOK_SECRET);
  });

  it('refuses to start without a Postgres DATABASE_URL (no silent in-memory database)', () => {
    const { DATABASE_URL: _drop, ...without } = PRODUCTION;
    expect(issuesOf(without)).toEqual(['DATABASE_URL is required when NODE_ENV=production']);
    expect(issuesOf({ ...PRODUCTION, DATABASE_URL: 'pglite:memory' })).toEqual(['DATABASE_URL=pglite: is for development and tests, not production']);
    expect(issuesOf({ ...PRODUCTION, DATABASE_URL: 'sqlite://x.db' })).toEqual(['DATABASE_URL must start with postgres://, postgresql:// or pglite:']);
    expect(load({ ...PRODUCTION, DATABASE_URL: 'postgresql://u:p@h/db' }).databaseUrl).toBe('postgresql://u:p@h/db');
  });

  it('refuses the fake payment provider (it grants items for free) unless explicitly allowed for staging', () => {
    const { ALLOW_FAKE_PAYMENTS_IN_PRODUCTION: _a, ...refused } = PRODUCTION;
    expect(issuesOf(refused).join(' ')).toContain('PAYMENT_PROVIDER=fake grants items for free');
    expect(issuesOf({ ...PRODUCTION, ALLOW_FAKE_PAYMENTS_IN_PRODUCTION: '0' }).join(' ')).toContain('PAYMENT_PROVIDER=fake');
    expect(issuesOf(PRODUCTION)).toEqual([]);
  });

  it('refuses the well-known development webhook secret in production: one must be provided', () => {
    const { FAKE_WEBHOOK_SECRET: _s, ...noSecret } = PRODUCTION;
    expect(issuesOf(noSecret)).toEqual(['FAKE_WEBHOOK_SECRET is required with the fake provider when NODE_ENV=production']);
    // ...while development with dev routes explicitly enabled falls back to the dev secret (never otherwise: hardening.test.ts).
    expect(load({ NODE_ENV: 'test', ENABLE_DEV_ROUTES: '1' }).fakeWebhookSecret).toBe(DEV_FAKE_WEBHOOK_SECRET);
  });
});

describe('Discord', () => {
  it('is off unless both id and secret are set, and half a pair is an error', () => {
    expect(load({}).discord).toBeNull();
    expect(issuesOf({ DISCORD_CLIENT_ID: 'x' })).toEqual(['DISCORD_CLIENT_SECRET is required when DISCORD_CLIENT_ID is set']);
    expect(issuesOf({ DISCORD_CLIENT_SECRET: 'x' })).toEqual(['DISCORD_CLIENT_ID is required when DISCORD_CLIENT_SECRET is set']);
  });

  it('derives the redirect URI from PUBLIC_API_URL unless given, and defaults to the real API base', () => {
    const c = load({ DISCORD_CLIENT_ID: 'id', DISCORD_CLIENT_SECRET: 'secret', PUBLIC_API_URL: 'https://api.example.test/' });
    expect(c.discord).toEqual({ clientId: 'id', clientSecret: 'secret', redirectUri: 'https://api.example.test/v1/auth/discord/callback', apiBase: 'https://discord.com/api/v10', flowTtlSeconds: 600 });
    const custom = load({ DISCORD_CLIENT_ID: 'id', DISCORD_CLIENT_SECRET: 'secret', DISCORD_REDIRECT_URI: 'https://other.example/cb', DISCORD_FLOW_TTL_SECONDS: '120' });
    expect(custom.discord).toMatchObject({ redirectUri: 'https://other.example/cb', flowTtlSeconds: 120 });
  });

  it('SITE_ORIGIN (where a web link returns) defaults to https://solar-rp.com and must be a bare origin, https in production', () => {
    expect(load({}).siteOrigin).toBe('https://solar-rp.com');
    expect(load({ SITE_ORIGIN: 'https://Site.Example.test/' }).siteOrigin).toBe('https://site.example.test');
    expect(load({ SITE_ORIGIN: 'http://localhost:5173' }).siteOrigin).toBe('http://localhost:5173');
    for (const bad of ['https://solar-rp.com/account', 'solar-rp.com', 'javascript:alert(1)', 'https://evil.test#x', 'https://a@evil.test', 'https://solar-rp.com?x=1', 'gage-mp://auth']) {
      expect(issuesOf({ SITE_ORIGIN: bad }), bad).toHaveLength(1);
    }
    expect(loadConfig({ ...PRODUCTION }).siteOrigin).toBe('https://solar-rp.com');
    expect(() => loadConfig({ ...PRODUCTION, SITE_ORIGIN: 'http://solar-rp.com' })).toThrow(/SITE_ORIGIN must be https/);
  });
});
