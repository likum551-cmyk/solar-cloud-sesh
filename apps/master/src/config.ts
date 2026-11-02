import { z } from 'zod';
import type { LogLevel } from './logger.js';

export interface Config {
  host: string;
  port: number;
  ttlSeconds: number;
  trustProxy: boolean;
  allowPrivate: boolean;
  verifyUdp: boolean;
  probeTimeoutMs: number;
  probeAttempts: number;
  maxInflightProbes: number;
  maxPerIp: number;
  maxEntries: number;
  rateLimitBurst: number;
  rateLimitPerMinute: number;
  sweepIntervalSeconds: number;
  shutdownGraceSeconds: number;
  logLevel: LogLevel;
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
const flagVar = z
  .enum(['0', '1'], { error: 'must be 0 or 1' })
  .transform((v) => v === '1');

const envSchema = z.object({
  HOST: z.string().min(1).default('127.0.0.1'),
  PORT: intVar(0, 65535).default(8080),
  TTL_SECONDS: intVar(1, 86_400).default(90),
  TRUST_PROXY: flagVar.default(false),
  ALLOW_PRIVATE: flagVar.default(false),
  VERIFY_UDP: flagVar.default(false),
  PROBE_TIMEOUT_MS: intVar(100, 30_000).default(2000),
  PROBE_ATTEMPTS: intVar(1, 10).default(3),
  MAX_INFLIGHT_PROBES: intVar(1, 10_000).default(64),
  MAX_PER_IP: intVar(1, 1000).default(8),
  MAX_ENTRIES: intVar(1, 100_000).default(2000),
  RATE_LIMIT_BURST: intVar(1, 1000).default(20),
  RATE_LIMIT_PER_MINUTE: intVar(1, 100_000).default(60),
  SWEEP_INTERVAL_SECONDS: intVar(1, 3600).default(10),
  SHUTDOWN_GRACE_SECONDS: intVar(1, 120).default(10),
  LOG_LEVEL: z.enum(['debug', 'info', 'warn', 'error']).default('info'),
});

/** Parse an environment. Unset and empty variables take their default; bad values throw ConfigError listing all of them. */
export function loadConfig(env: Record<string, string | undefined>): Config {
  const present: Record<string, string> = {};
  for (const [k, v] of Object.entries(env)) {
    if (v !== undefined && v !== '') present[k] = v;
  }
  const parsed = envSchema.safeParse(present);
  if (!parsed.success) {
    throw new ConfigError(
      parsed.error.issues.map((i) => `${i.path.join('.') || '(env)'}: ${i.message}`),
    );
  }
  const e = parsed.data;
  return {
    host: e.HOST,
    port: e.PORT,
    ttlSeconds: e.TTL_SECONDS,
    trustProxy: e.TRUST_PROXY,
    allowPrivate: e.ALLOW_PRIVATE,
    verifyUdp: e.VERIFY_UDP,
    probeTimeoutMs: e.PROBE_TIMEOUT_MS,
    probeAttempts: e.PROBE_ATTEMPTS,
    maxInflightProbes: e.MAX_INFLIGHT_PROBES,
    maxPerIp: e.MAX_PER_IP,
    maxEntries: e.MAX_ENTRIES,
    rateLimitBurst: e.RATE_LIMIT_BURST,
    rateLimitPerMinute: e.RATE_LIMIT_PER_MINUTE,
    sweepIntervalSeconds: e.SWEEP_INTERVAL_SECONDS,
    shutdownGraceSeconds: e.SHUTDOWN_GRACE_SECONDS,
    logLevel: e.LOG_LEVEL,
  };
}
