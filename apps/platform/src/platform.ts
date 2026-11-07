import type { Server } from 'node:http';
import { serve } from '@hono/node-server';
import { HttpDiscordClient, type DiscordClient } from './auth/discord-client.js';
import { hashLimiter } from './auth/password.js';
import type { Config } from './config.js';
import type { Deps } from './deps.js';
import { openDatabase, type Db, type DbHandle } from './db/index.js';
import { ConsoleEmailSender, OutboxEmailSender, type EmailSender } from './email.js';
import { ResendEmailSender } from './email-resend.js';
import { createApp } from './http/app.js';
import { startMaintenance } from './maintenance.js';
import { UdpServerStatusService, udpProber, type Prober, type ServerStatusService } from './servers/status.js';
import { FakeProvider } from './shop/fake-provider.js';
import type { PaymentProvider } from './shop/provider.js';
import { isLoopbackHost } from './util/ip.js';
import { errorFields, type Logger } from './util/logger.js';

/** Everything that can be swapped. Production passes only `config` and `logger`; tests pass fakes. */
export interface PlatformOptions {
  config: Config;
  logger: Logger;
  handle?: DbHandle;
  clock?: () => Date;
  email?: EmailSender;
  /** `null` disables Discord even if configured. */
  discord?: DiscordClient | null;
  payments?: PaymentProvider[];
  serverStatus?: ServerStatusService;
  prober?: Prober;
  resolveHost?: (host: string) => Promise<string>;
  /** Passed to the Discord client (tests play Discord with it). */
  fetchImpl?: typeof fetch;
}

export interface Built {
  deps: Deps;
  handle: DbHandle;
}

/**
 * The mail driver. `resend` delivers; `outbox` / `console` deliver nothing. All keep live tokens out of
 * the database and the log except in development, where the link is logged so a developer can follow it.
 */
export function createEmailSender(config: Config, db: Db, clock: () => Date, logger: Logger, fetchImpl?: typeof fetch): EmailSender {
  const logLinks = config.nodeEnv === 'development';
  if (config.emailDriver === 'resend' && config.resend) {
    // Real delivery; a redacted copy still goes to the outbox table (never the link: logLinks off) for support.
    return new ResendEmailSender({
      apiKey: config.resend.apiKey,
      apiBase: config.resend.apiBase,
      from: config.emailFrom,
      logger,
      record: new OutboxEmailSender(db, clock, {}),
      ...(fetchImpl ? { fetchImpl } : {}),
    });
  }
  return config.emailDriver === 'console' ? new ConsoleEmailSender(logger, { logLinks }) : new OutboxEmailSender(db, clock, { logger, logLinks });
}

/** Wire the dependencies. Opens the database (unless one is passed in) but does not migrate or listen. */
export async function buildDeps(opts: PlatformOptions): Promise<Built> {
  const { config, logger } = opts;
  const clock = opts.clock ?? (() => new Date());
  const handle = opts.handle ?? (await openDatabase(config.databaseUrl, { poolMax: config.databasePoolMax, logger }));
  const db = handle.db;
  // One limiter for every argon2 call in the process (login, register, reset, password change).
  hashLimiter.configure({ maxConcurrent: config.argon2.maxConcurrent, maxQueue: config.argon2.maxQueue });

  const email = opts.email ?? createEmailSender(config, db, clock, logger, opts.fetchImpl);
  const discord =
    opts.discord !== undefined ? opts.discord : config.discord ? new HttpDiscordClient(config.discord, opts.fetchImpl ?? fetch) : null;
  const payments = opts.payments ?? [new FakeProvider(config.fakeWebhookSecret, config.publicApiUrl, clock)];
  const serverStatus =
    opts.serverStatus ??
    new UdpServerStatusService({
      ttlMs: config.serverStatusTtlMs,
      waitMs: config.serverStatusWaitMs,
      prober: opts.prober ?? udpProber(config),
      ...(opts.resolveHost ? { resolve: opts.resolveHost } : {}),
      clock,
      logger,
    });

  if (config.nodeEnv === 'production') {
    if (!opts.email && config.emailDriver !== 'resend') {
      logger.warn(`EMAIL_DRIVER=${config.emailDriver} does not deliver mail: set EMAIL_DRIVER=resend with RESEND_API_KEY before launch (verification and reset emails will not reach players)`);
    }
    if (payments.some((p) => p instanceof FakeProvider)) logger.warn('the FAKE payment provider is active in production (ALLOW_FAKE_PAYMENTS_IN_PRODUCTION=1): orders can be paid with a forged signature only if FAKE_WEBHOOK_SECRET leaks, but nothing is charged');
  }

  const deps: Deps = { config, db, dbPing: () => handle.ping(), logger, clock, email, discord, payments, serverStatus };
  return { deps, handle };
}

export interface RunningPlatform {
  deps: Deps;
  /** Actual listening ports (useful with PORT=0). */
  publicPort: number;
  internalPort: number | null;
  /** Stop accepting, let in-flight requests finish (up to SHUTDOWN_GRACE_SECONDS), close the database. */
  close(): Promise<void>;
}

function listen(app: { fetch: (req: Request, ...args: never[]) => Response | Promise<Response> }, host: string, port: number): Promise<{ server: Server; port: number }> {
  return new Promise((resolve, reject) => {
    const server = serve({ fetch: app.fetch as never, hostname: host, port }, (info) => resolve({ server: server as unknown as Server, port: info.port })) as unknown as Server;
    server.once('error', reject);
    // Bound slow clients: a request that cannot deliver its headers or body in time is dropped.
    server.headersTimeout = 15_000;
    server.requestTimeout = 30_000;
  });
}

/**
 * Start the HTTP listener(s) and the maintenance timer. The internal game-server API gets its own
 * listener (INTERNAL_HOST:INTERNAL_PORT) so it is never reachable through the public reverse proxy,
 * unless INTERNAL_ON_PUBLIC_LISTENER=1.
 */
export async function startPlatform(built: Built): Promise<RunningPlatform> {
  const { deps, handle } = built;
  const { config, logger } = deps;
  const shared = config.internalOnPublicListener;

  if (!isLoopbackHost(config.host) && shared) {
    logger.warn('INTERNAL_ON_PUBLIC_LISTENER=1 with a non-loopback HOST: /internal/* is reachable from the network; rely on the per-server IP allowlist and a firewall');
  }
  if (!shared && !isLoopbackHost(config.internalHost)) {
    logger.warn('the internal listener is bound to a non-loopback address: firewall it to the game servers', { host: config.internalHost, port: config.internalPort });
  }

  const publicApp = createApp(deps, shared ? 'all' : 'public').app;
  const pub = await listen(publicApp, config.host, config.port);
  let internal: { server: Server; port: number } | null = null;
  try {
    if (!shared) internal = await listen(createApp(deps, 'internal').app, config.internalHost, config.internalPort);
  } catch (err) {
    pub.server.close();
    throw err;
  }
  const stopMaintenance = startMaintenance(deps, config.maintenanceIntervalSeconds);
  logger.info('listening', { host: config.host, port: pub.port, internalHost: shared ? null : config.internalHost, internalPort: internal?.port ?? null, db: handle.kind });

  let closing: Promise<void> | null = null;
  const closeServer = (server: Server): Promise<void> =>
    new Promise((resolve) => {
      server.close(() => resolve());
      server.closeIdleConnections();
    });

  return {
    deps,
    publicPort: pub.port,
    internalPort: internal?.port ?? null,
    close: () =>
      (closing ??= (async () => {
        stopMaintenance();
        const servers = [pub.server, ...(internal ? [internal.server] : [])];
        const drained = Promise.all(servers.map(closeServer));
        const timer = setTimeout(() => {
          logger.warn('grace period over: closing remaining connections', { graceSeconds: config.shutdownGraceSeconds });
          for (const s of servers) s.closeAllConnections();
        }, config.shutdownGraceSeconds * 1000);
        try {
          await drained;
        } finally {
          clearTimeout(timer);
        }
        try {
          await handle.close();
        } catch (err) {
          logger.error('database close failed', errorFields(err));
        }
      })()),
  };
}
