import { z } from 'zod';
import type { LogLevel } from './util/logger.js';

/** Everything the platform reads from the environment, parsed and validated once at start. */
export interface Config {
  nodeEnv: 'development' | 'test' | 'production';
  /** ENABLE_DEV_ROUTES=1: mounts /v1/dev/* (pay any order for free) and allows the public dev webhook secret. Never in production. */
  devRoutes: boolean;
  host: string;
  port: number;
  internalHost: string;
  internalPort: number;
  internalOnPublicListener: boolean;
  databaseUrl: string;
  databasePoolMax: number;
  publicApiUrl: string;
  corsOrigins: string[];
  trustProxy: boolean;
  logLevel: LogLevel;
  bodyLimitBytes: number;
  shutdownGraceSeconds: number;

  sessionTtlDays: number;
  sessionMaxDays: number;
  requireVerifiedEmail: boolean;
  emailVerifyTtlHours: number;
  passwordResetTtlMinutes: number;
  argon2: { memoryKib: number; timeCost: number; parallelism: number; maxConcurrent: number; maxQueue: number };
  login: {
    /** Failures of one typed identifier, from anywhere, before it is locked for everyone. */
    accountMaxFailures: number;
    /** Failures of one typed identifier from one address block (IPv4 /24, IPv6 /64) before that block is locked out of it. */
    accountIpMaxFailures: number;
    ipMaxFailures: number;
    lockoutBaseSeconds: number;
    lockoutMaxSeconds: number;
    failureWindowSeconds: number;
  };

  emailDriver: 'outbox' | 'console' | 'resend';
  emailFrom: string;
  emailLinkBaseUrl: string;
  /** EMAIL_DRIVER=resend: Resend's HTTP API (https://resend.com/docs/api-reference/emails/send-email). Null with any other driver. */
  resend: { apiKey: string; apiBase: string } | null;

  discord: { clientId: string; clientSecret: string; redirectUri: string; apiBase: string; flowTtlSeconds: number } | null;
  /** The website's public origin (scheme://host[:port]). A web Discord link flow returns to <siteOrigin>/account/; never taken from a request. */
  siteOrigin: string;

  paymentProvider: 'fake';
  fakeWebhookSecret: string;
  orderTtlMinutes: number;

  joinTokenTtlSeconds: number;
  gameSessionMaxHours: number;
  serverStatusTtlMs: number;
  serverStatusWaitMs: number;
  probeTimeoutMs: number;
  probeAttempts: number;

  settingsMaxBytes: number;
  maintenanceIntervalSeconds: number;
}

export class ConfigError extends Error {
  readonly issues: string[];
  constructor(issues: string[]) {
    super(`invalid configuration: ${issues.join('; ')}`);
    this.name = 'ConfigError';
    this.issues = issues;
  }
}

/** A base-10 integer within [min, max]; anything else (including "1e3", "0x10", " 5") fails. */
const intVar = (min: number, max: number) =>
  z
    .string()
    .regex(/^\d{1,9}$/, 'must be a non-negative integer')
    .transform(Number)
    .pipe(z.number().min(min).max(max));

/** Strictly "0" or "1": "true" or "yes" is an error, never silently false. */
const flagVar = z.enum(['0', '1'], { error: 'must be 0 or 1' }).transform((v) => v === '1');

const urlVar = z
  .string()
  .url()
  .refine((u) => /^https?:\/\//i.test(u), 'must be an http(s) URL')
  .transform((u) => u.replace(/\/+$/, ''));

/**
 * Environment variables. Unset and empty variables take the default. docs/api.md ("Environment")
 * lists each one; test/docs.test.ts fails when a key here is missing from that table.
 */
export const envSchema = z.object({
  // No default: an unset NODE_ENV is refused by loadConfig (see there) instead of silently meaning "development".
  NODE_ENV: z.enum(['development', 'test', 'production']).optional(),
  ENABLE_DEV_ROUTES: flagVar.default(false),
  HOST: z.string().min(1).default('127.0.0.1'),
  PORT: intVar(0, 65535).default(8787),
  INTERNAL_HOST: z.string().min(1).default('127.0.0.1'),
  INTERNAL_PORT: intVar(0, 65535).default(8788),
  INTERNAL_ON_PUBLIC_LISTENER: flagVar.default(false),
  DATABASE_URL: z.string().min(1).optional(),
  DATABASE_POOL_MAX: intVar(1, 200).default(10),
  PUBLIC_API_URL: urlVar.default('http://127.0.0.1:8787'),
  CORS_ORIGINS: z.string().default('https://app.gage'),
  TRUST_PROXY: flagVar.default(false),
  LOG_LEVEL: z.enum(['debug', 'info', 'warn', 'error']).default('info'),
  BODY_LIMIT_BYTES: intVar(1024, 4_194_304).default(65_536),
  SHUTDOWN_GRACE_SECONDS: intVar(1, 120).default(10),

  SESSION_TTL_DAYS: intVar(1, 365).default(30),
  SESSION_MAX_DAYS: intVar(1, 3650).default(180),
  REQUIRE_VERIFIED_EMAIL: flagVar.default(true),
  EMAIL_VERIFY_TTL_HOURS: intVar(1, 720).default(24),
  PASSWORD_RESET_TTL_MINUTES: intVar(5, 1440).default(60),
  ARGON2_MEMORY_KIB: intVar(8, 4_194_304).default(19_456),
  ARGON2_TIME_COST: intVar(1, 20).default(2),
  ARGON2_PARALLELISM: intVar(1, 16).default(1),
  ARGON2_MAX_CONCURRENT: intVar(1, 64).default(2),
  ARGON2_MAX_QUEUE: intVar(0, 100_000).default(64),
  LOGIN_ACCOUNT_IP_MAX_FAILURES: intVar(1, 1000).default(5),
  LOGIN_ACCOUNT_MAX_FAILURES: intVar(1, 100_000).default(100),
  LOGIN_IP_MAX_FAILURES: intVar(1, 100_000).default(20),
  LOGIN_LOCKOUT_BASE_SECONDS: intVar(1, 86_400).default(30),
  LOGIN_LOCKOUT_MAX_SECONDS: intVar(1, 604_800).default(3600),
  LOGIN_FAILURE_WINDOW_SECONDS: intVar(1, 604_800).default(900),

  EMAIL_DRIVER: z.enum(['outbox', 'console', 'resend']).default('outbox'),
  EMAIL_FROM: z.string().min(3).default('Solar RP <no-reply@gage-mp.invalid>'),
  EMAIL_LINK_BASE_URL: z.string().min(3).default('gage-mp://auth'),
  RESEND_API_KEY: z.string().min(1).optional(),
  RESEND_API_BASE: urlVar.default('https://api.resend.com'),

  DISCORD_CLIENT_ID: z.string().min(1).optional(),
  DISCORD_CLIENT_SECRET: z.string().min(1).optional(),
  DISCORD_REDIRECT_URI: urlVar.optional(),
  DISCORD_API_BASE: urlVar.default('https://discord.com/api/v10'),
  DISCORD_FLOW_TTL_SECONDS: intVar(60, 3600).default(600),
  SITE_ORIGIN: z.string().default('https://solar-rp.com'),

  PAYMENT_PROVIDER: z.enum(['fake']).default('fake'),
  ALLOW_FAKE_PAYMENTS_IN_PRODUCTION: flagVar.default(false),
  FAKE_WEBHOOK_SECRET: z.string().min(16).optional(),
  ORDER_TTL_MINUTES: intVar(5, 10_080).default(1440),

  JOIN_TOKEN_TTL_SECONDS: intVar(5, 600).default(60),
  GAME_SESSION_MAX_HOURS: intVar(1, 720).default(24),
  SERVER_STATUS_TTL_MS: intVar(0, 600_000).default(5000),
  SERVER_STATUS_WAIT_MS: intVar(0, 10_000).default(400),
  PROBE_TIMEOUT_MS: intVar(100, 30_000).default(1500),
  PROBE_ATTEMPTS: intVar(1, 10).default(2),

  SETTINGS_MAX_BYTES: intVar(256, 1_048_576).default(16_384),
  MAINTENANCE_INTERVAL_SECONDS: intVar(1, 86_400).default(300),
});

/** Dev-only default for the fake provider's webhook HMAC secret. Used only with ENABLE_DEV_ROUTES=1, never in production. */
export const DEV_FAKE_WEBHOOK_SECRET = 'dev-only-fake-webhook-secret';

/**
 * Parse an environment. Unset and empty variables take their default; bad values throw
 * ConfigError listing all of them. Cross-variable rules (production safety) are checked here too.
 */
export function loadConfig(env: Record<string, string | undefined>): Config {
  const present: Record<string, string> = {};
  for (const [k, v] of Object.entries(env)) {
    if (v !== undefined && v !== '') present[k] = v;
  }
  const parsed = envSchema.safeParse(present);
  if (!parsed.success) {
    throw new ConfigError(parsed.error.issues.map((i) => `${i.path.join('.') || '(env)'}: ${i.message}`));
  }
  const e = parsed.data;
  const issues: string[] = [];

  const corsOrigins = e.CORS_ORIGINS.split(',')
    .map((o) => o.trim())
    .filter((o) => o !== '');
  for (const o of corsOrigins) {
    // An origin is scheme://host[:port] with nothing else; "*" would defeat the restriction.
    if (!/^https?:\/\/[^/\s*]+$/i.test(o)) issues.push(`CORS_ORIGINS: "${o}" is not an origin (scheme://host[:port], no path, no wildcard)`);
  }

  // The web Discord link redirects the browser here with the claim: an origin and nothing else, https in production.
  const siteOrigin = e.SITE_ORIGIN.trim().replace(/\/+$/, '');
  if (!/^https?:\/\/[^/\s*?#@]+$/i.test(siteOrigin)) issues.push(`SITE_ORIGIN: "${siteOrigin}" is not an origin (scheme://host[:port], no path)`);
  else if (e.NODE_ENV === 'production' && !/^https:\/\//i.test(siteOrigin)) issues.push('SITE_ORIGIN must be https when NODE_ENV=production');

  if (e.SESSION_TTL_DAYS > e.SESSION_MAX_DAYS) issues.push('SESSION_TTL_DAYS must not exceed SESSION_MAX_DAYS');
  if (e.LOGIN_LOCKOUT_BASE_SECONDS > e.LOGIN_LOCKOUT_MAX_SECONDS) {
    issues.push('LOGIN_LOCKOUT_BASE_SECONDS must not exceed LOGIN_LOCKOUT_MAX_SECONDS');
  }
  if (e.LOGIN_ACCOUNT_IP_MAX_FAILURES > e.LOGIN_ACCOUNT_MAX_FAILURES) {
    issues.push('LOGIN_ACCOUNT_IP_MAX_FAILURES must not exceed LOGIN_ACCOUNT_MAX_FAILURES (the account-wide threshold is the higher one)');
  }

  // An unset NODE_ENV used to mean "development": in-memory database, the public dev webhook secret
  // and /v1/dev/* mounted, on a box whose operator merely forgot a variable. Now it is refused;
  // development is something you say (NODE_ENV=development, or ENABLE_DEV_ROUTES=1).
  const nodeEnv = e.NODE_ENV ?? 'development';
  const production = nodeEnv === 'production';
  const devRoutes = e.ENABLE_DEV_ROUTES;
  if (e.NODE_ENV === undefined && !devRoutes) {
    issues.push('NODE_ENV is not set: refusing to guess. Set NODE_ENV=production for a deployment, NODE_ENV=development for local work, or ENABLE_DEV_ROUTES=1 to opt in to development mode explicitly');
  }
  if (production && devRoutes) {
    issues.push('ENABLE_DEV_ROUTES=1 is refused when NODE_ENV=production: /v1/dev/* lets anyone pay any order for free');
  }
  if (production && e.PAYMENT_PROVIDER === 'fake' && !e.ALLOW_FAKE_PAYMENTS_IN_PRODUCTION) {
    issues.push('PAYMENT_PROVIDER=fake grants items for free: refused when NODE_ENV=production (set ALLOW_FAKE_PAYMENTS_IN_PRODUCTION=1 only for a staging box)');
  }
  // The built-in secret is public (it is in the source): only a deliberately enabled dev setup may sign with it.
  if (e.PAYMENT_PROVIDER === 'fake' && e.FAKE_WEBHOOK_SECRET === undefined && !devRoutes) {
    issues.push(production
      ? 'FAKE_WEBHOOK_SECRET is required with the fake provider when NODE_ENV=production'
      : 'FAKE_WEBHOOK_SECRET is required (at least 16 characters) unless ENABLE_DEV_ROUTES=1: the built-in dev secret is public');
  }
  if (e.DISCORD_CLIENT_ID !== undefined && e.DISCORD_CLIENT_SECRET === undefined) {
    issues.push('DISCORD_CLIENT_SECRET is required when DISCORD_CLIENT_ID is set');
  }
  if (e.DISCORD_CLIENT_ID === undefined && e.DISCORD_CLIENT_SECRET !== undefined) {
    issues.push('DISCORD_CLIENT_ID is required when DISCORD_CLIENT_SECRET is set');
  }
  // Resend delivers for real: it needs its key, and a sender on a domain verified in Resend (the built-in .invalid address never is).
  if (e.EMAIL_DRIVER === 'resend') {
    if (e.RESEND_API_KEY === undefined) issues.push('RESEND_API_KEY is required when EMAIL_DRIVER=resend');
    if (/\.invalid>?\s*$/i.test(e.EMAIL_FROM)) {
      issues.push('EMAIL_FROM must be an address on your Resend-verified domain when EMAIL_DRIVER=resend (e.g. "Solar RP <no-reply@solar-rp.com>")');
    }
  }
  const databaseUrl = e.DATABASE_URL ?? (production ? undefined : 'pglite:memory');
  if (databaseUrl === undefined) issues.push('DATABASE_URL is required when NODE_ENV=production');
  else if (!/^(postgres(ql)?:\/\/|pglite:)/.test(databaseUrl)) issues.push('DATABASE_URL must start with postgres://, postgresql:// or pglite:');
  else if (production && databaseUrl.startsWith('pglite:')) issues.push('DATABASE_URL=pglite: is for development and tests, not production');

  if (issues.length > 0) throw new ConfigError(issues);

  return {
    nodeEnv,
    devRoutes,
    host: e.HOST,
    port: e.PORT,
    internalHost: e.INTERNAL_HOST,
    internalPort: e.INTERNAL_PORT,
    internalOnPublicListener: e.INTERNAL_ON_PUBLIC_LISTENER,
    databaseUrl: databaseUrl as string,
    databasePoolMax: e.DATABASE_POOL_MAX,
    publicApiUrl: e.PUBLIC_API_URL,
    corsOrigins,
    trustProxy: e.TRUST_PROXY,
    logLevel: e.LOG_LEVEL,
    bodyLimitBytes: e.BODY_LIMIT_BYTES,
    shutdownGraceSeconds: e.SHUTDOWN_GRACE_SECONDS,

    sessionTtlDays: e.SESSION_TTL_DAYS,
    sessionMaxDays: e.SESSION_MAX_DAYS,
    requireVerifiedEmail: e.REQUIRE_VERIFIED_EMAIL,
    emailVerifyTtlHours: e.EMAIL_VERIFY_TTL_HOURS,
    passwordResetTtlMinutes: e.PASSWORD_RESET_TTL_MINUTES,
    argon2: {
      memoryKib: e.ARGON2_MEMORY_KIB,
      timeCost: e.ARGON2_TIME_COST,
      parallelism: e.ARGON2_PARALLELISM,
      maxConcurrent: e.ARGON2_MAX_CONCURRENT,
      maxQueue: e.ARGON2_MAX_QUEUE,
    },
    login: {
      accountMaxFailures: e.LOGIN_ACCOUNT_MAX_FAILURES,
      accountIpMaxFailures: e.LOGIN_ACCOUNT_IP_MAX_FAILURES,
      ipMaxFailures: e.LOGIN_IP_MAX_FAILURES,
      lockoutBaseSeconds: e.LOGIN_LOCKOUT_BASE_SECONDS,
      lockoutMaxSeconds: e.LOGIN_LOCKOUT_MAX_SECONDS,
      failureWindowSeconds: e.LOGIN_FAILURE_WINDOW_SECONDS,
    },

    emailDriver: e.EMAIL_DRIVER,
    emailFrom: e.EMAIL_FROM,
    emailLinkBaseUrl: e.EMAIL_LINK_BASE_URL.replace(/\/+$/, ''),
    resend: e.EMAIL_DRIVER === 'resend' && e.RESEND_API_KEY !== undefined ? { apiKey: e.RESEND_API_KEY, apiBase: e.RESEND_API_BASE } : null,

    discord:
      e.DISCORD_CLIENT_ID !== undefined && e.DISCORD_CLIENT_SECRET !== undefined
        ? {
            clientId: e.DISCORD_CLIENT_ID,
            clientSecret: e.DISCORD_CLIENT_SECRET,
            redirectUri: e.DISCORD_REDIRECT_URI ?? `${e.PUBLIC_API_URL}/v1/auth/discord/callback`,
            apiBase: e.DISCORD_API_BASE,
            flowTtlSeconds: e.DISCORD_FLOW_TTL_SECONDS,
          }
        : null,
    siteOrigin: siteOrigin.toLowerCase(),

    paymentProvider: e.PAYMENT_PROVIDER,
    fakeWebhookSecret: e.FAKE_WEBHOOK_SECRET ?? DEV_FAKE_WEBHOOK_SECRET,
    orderTtlMinutes: e.ORDER_TTL_MINUTES,

    joinTokenTtlSeconds: e.JOIN_TOKEN_TTL_SECONDS,
    gameSessionMaxHours: e.GAME_SESSION_MAX_HOURS,
    serverStatusTtlMs: e.SERVER_STATUS_TTL_MS,
    serverStatusWaitMs: e.SERVER_STATUS_WAIT_MS,
    probeTimeoutMs: e.PROBE_TIMEOUT_MS,
    probeAttempts: e.PROBE_ATTEMPTS,

    settingsMaxBytes: e.SETTINGS_MAX_BYTES,
    maintenanceIntervalSeconds: e.MAINTENANCE_INTERVAL_SECONDS,
  };
}
