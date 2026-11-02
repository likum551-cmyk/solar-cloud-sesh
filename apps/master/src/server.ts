import { Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import { serve } from '@hono/node-server';
import type { Hono } from 'hono';
import { createApp, type AppEnv } from './app.js';
import type { Config } from './config.js';
import { isLoopbackHost } from './ip.js';
import type { Logger } from './logger.js';
import { probeServer, type Prober } from './probe.js';
import { TokenBucketLimiter } from './rate-limit.js';
import { MemoryStore, type ServerStore } from './store.js';
import { startSweeper } from './sweeper.js';

export interface ServiceOverrides {
  store?: ServerStore;
  clock?: () => number;
  /** Replace the UDP prober (null = no verification whatever VERIFY_UDP says). */
  probe?: Prober | null;
}

export interface Service {
  app: Hono<AppEnv>;
  store: ServerStore;
  limiter: TokenBucketLimiter;
  clock: () => number;
}

/** Wire store, limiter, prober and the HTTP app from a Config (no sockets are opened). */
export function createService(config: Config, logger: Logger, overrides: ServiceOverrides = {}): Service {
  // Arrow functions, not `Date.now` itself: the clock must follow a faked Date.
  const clock = overrides.clock ?? (() => Date.now());
  const store = overrides.store ?? new MemoryStore();
  const limiter = new TokenBucketLimiter({
    capacity: config.rateLimitBurst,
    refillPerSecond: config.rateLimitPerMinute / 60,
  });
  const probe: Prober | null =
    overrides.probe !== undefined
      ? overrides.probe
      : config.verifyUdp
        ? (ip, port) => probeServer(ip, port, { timeoutMs: config.probeTimeoutMs, attempts: config.probeAttempts })
        : null;

  const app = createApp({ config, store, limiter, probe, logger, clock });
  return { app, store, limiter, clock };
}

export interface RunningServer {
  address: AddressInfo;
  app: Hono<AppEnv>;
  /** Stop accepting, let in-flight requests finish for up to the grace period, then drop the rest. Idempotent. */
  close(): Promise<void>;
}

/** createService plus the HTTP listener and the sweeper. */
export function startServer(config: Config, logger: Logger, overrides: ServiceOverrides = {}): Promise<RunningServer> {
  if (config.trustProxy && !isLoopbackHost(config.host)) {
    logger.warn('TRUST_PROXY=1 while listening on a non-loopback address: any client that can reach this port can forge X-Forwarded-For', { host: config.host });
  }
  if (config.allowPrivate) logger.warn('ALLOW_PRIVATE=1: private and loopback announces are accepted (local testing only)');
  if (!config.verifyUdp) logger.warn('VERIFY_UDP=0: announced servers are listed without a reachability check');

  const { app, store, limiter, clock } = createService(config, logger, overrides);
  const sweeper = startSweeper({ store, limiter, clock, logger, intervalMs: config.sweepIntervalSeconds * 1000 });

  return new Promise<RunningServer>((resolve, reject) => {
    const server = serve({ fetch: app.fetch, port: config.port, hostname: config.host }, (address) => {
      logger.info('listening', { host: address.address, port: address.port, ttlSeconds: config.ttlSeconds, verifyUdp: config.verifyUdp, trustProxy: config.trustProxy });
      resolve({ address, app, close });
    });

    let closing: Promise<void> | null = null;
    const close = (): Promise<void> => {
      closing ??= new Promise<void>((done, failed) => {
        sweeper.stop();
        let forced: NodeJS.Timeout | undefined;
        server.close((err) => {
          if (forced) clearTimeout(forced);
          if (err) failed(err);
          else done();
        });
        if (server instanceof Server) {
          server.closeIdleConnections();
          forced = setTimeout(() => {
            logger.warn('grace period over, dropping open connections', { graceSeconds: config.shutdownGraceSeconds });
            server.closeAllConnections();
          }, config.shutdownGraceSeconds * 1000);
        }
      });
      return closing;
    };

    // A listen failure (EADDRINUSE, EACCES) arrives as an event after serve() returns.
    server.once('error', (err) => {
      sweeper.stop();
      reject(err);
    });
  });
}
