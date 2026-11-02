import { Hono } from 'hono';
import type { Context, MiddlewareHandler } from 'hono';
import { HTTPException } from 'hono/http-exception';
import { bodyLimit } from 'hono/body-limit';
import { getConnInfo } from '@hono/node-server/conninfo';
import type { HttpBindings } from '@hono/node-server';
import type { Config } from './config.js';
import { errorFields, type Logger } from './logger.js';
import { firstForwardedFor, formatEndpoint, ipGroup, isPublicIp, normalizeIp } from './ip.js';
import type { Prober, ProbeResult } from './probe.js';
import type { TokenBucketLimiter } from './rate-limit.js';
import { announceSchema } from './schema.js';
import type { ServerStore } from './store.js';

export interface AppDeps {
  config: Config;
  store: ServerStore;
  limiter: TokenBucketLimiter;
  /** Present only when VERIFY_UDP=1. */
  probe: Prober | null;
  logger: Logger;
  clock: () => number;
}

export type AppEnv = {
  Bindings: HttpBindings;
  Variables: { clientIp: string; clientGroup: string };
};

type FailStatus = 400 | 403 | 404 | 413 | 415 | 422 | 429 | 500 | 503;

/**
 * One entry of GET /servers, in the shape of RAGE:MP's master list so the launcher's existing
 * parser (libs/net/serverfinder.cpp) reads it unchanged. `protocol` is the one added field.
 */
export interface ServerListEntry {
  name: string;
  url: string;
  lang: string;
  gamemode: string;
  players: number;
  maxplayers: number;
  protocol: number;
}

const MAX_BODY_BYTES = 4096;
const MAX_ISSUES_REPORTED = 10;

export function createApp(deps: AppDeps): Hono<AppEnv> {
  const { config, store, limiter, probe, logger, clock } = deps;
  const startedAt = clock();
  const ttlMs = config.ttlSeconds * 1000;
  const inflight = new Map<string, Promise<ProbeResult>>();

  const app = new Hono<AppEnv>();

  const fail = (
    c: Context<AppEnv>,
    status: FailStatus,
    error: string,
    message: string,
    extra: Record<string, unknown> = {},
  ): Response => {
    c.header('Cache-Control', 'no-store');
    return c.json({ error, message, ...extra }, status);
  };

  const peerAddress = (c: Context<AppEnv>): string | undefined => {
    try {
      return getConnInfo(c).remote.address;
    } catch (err) {
      logger.error('cannot read the connection address', errorFields(err));
      return undefined;
    }
  };

  // Who is announcing, whether they may, and how often. Runs before the body is read.
  const clientGate: MiddlewareHandler<AppEnv> = async (c, next) => {
    const peer = peerAddress(c);
    let ip: string | null;
    if (config.trustProxy) {
      ip = firstForwardedFor(c.req.header('x-forwarded-for'));
      if (ip === null) {
        logger.error('TRUST_PROXY=1 but X-Forwarded-For is missing or not an IP: check the proxy config', { peer });
        return fail(c, 400, 'bad_forwarded_for', 'X-Forwarded-For is missing or invalid');
      }
    } else {
      ip = peer === undefined ? null : normalizeIp(peer);
      if (ip === null) {
        logger.error('connection has no usable remote address', { peer });
        return fail(c, 500, 'no_peer_address', 'cannot determine the client address');
      }
    }

    if (!config.allowPrivate && !isPublicIp(ip)) {
      logger.info('announce rejected', { reason: 'private_address', ip });
      return fail(c, 403, 'private_address', 'announces from private or loopback addresses are not accepted');
    }

    const group = ipGroup(ip);
    const taken = limiter.take(group, clock());
    if (!taken.allowed) {
      const retryAfterSeconds = Math.max(1, Math.ceil(taken.retryAfterMs / 1000));
      logger.debug('announce rejected', { reason: 'rate_limited', ip, retryAfterSeconds });
      c.header('Retry-After', String(retryAfterSeconds));
      return fail(c, 429, 'rate_limited', 'too many announces from this address', { retryAfterSeconds });
    }

    c.set('clientIp', ip);
    c.set('clientGroup', group);
    return next();
  };

  // A browser page cannot send application/json cross-origin without a preflight (and this
  // service sends no CORS headers), so requiring it stops a web page from listing its
  // visitors' addresses.
  const requireJson: MiddlewareHandler<AppEnv> = async (c, next) => {
    const contentType = c.req.header('content-type') ?? '';
    if (!/^application\/json\s*(;|$)/i.test(contentType)) {
      return fail(c, 415, 'unsupported_media_type', 'Content-Type must be application/json');
    }
    return next();
  };

  const startProbe = (key: string, ip: string, port: number): Promise<ProbeResult> | null => {
    const running = inflight.get(key);
    if (running) return running;
    if (!probe || inflight.size >= config.maxInflightProbes) return null;
    const p = probe(ip, port)
      .catch(
        (err: unknown): ProbeResult => ({
          ok: false,
          reason: 'send-error',
          detail: err instanceof Error ? err.message : String(err),
        }),
      )
      .finally(() => inflight.delete(key));
    inflight.set(key, p);
    return p;
  };

  app.post(
    '/announce',
    clientGate,
    requireJson,
    bodyLimit({
      maxSize: MAX_BODY_BYTES,
      onError: (c) => fail(c as Context<AppEnv>, 413, 'payload_too_large', `body exceeds ${MAX_BODY_BYTES} bytes`),
    }),
    async (c) => {
      const ip = c.get('clientIp');
      const group = c.get('clientGroup');

      let raw: unknown;
      try {
        raw = await c.req.json();
      } catch (err) {
        logger.info('announce rejected', { reason: 'invalid_json', ip, ...errorFields(err) });
        return fail(c, 400, 'invalid_json', 'body is not valid JSON');
      }

      const parsed = announceSchema.safeParse(raw);
      if (!parsed.success) {
        const issues = parsed.error.issues
          .slice(0, MAX_ISSUES_REPORTED)
          .map((i) => ({ path: i.path.join('.'), message: i.message }));
        logger.info('announce rejected', { reason: 'validation_failed', ip, issues });
        return fail(c, 400, 'validation_failed', 'announce body is invalid', { issues });
      }
      const a = parsed.data;
      const key = formatEndpoint(ip, a.port);

      // A new entry is probed before it is listed; a heartbeat for a live entry is not.
      if (probe && !(await store.has(key, clock()))) {
        const running = startProbe(key, ip, a.port);
        if (running === null) {
          logger.warn('announce deferred', { reason: 'probe_capacity', ip, key });
          c.header('Retry-After', '5');
          return fail(c, 503, 'probe_busy', 'too many reachability checks in progress, retry shortly');
        }
        const result = await running;
        if (!result.ok) {
          logger.info('announce rejected', { reason: 'unreachable', ip, key, probe: result.reason, detail: result.detail });
          return fail(c, 422, 'unreachable', `no GAGE server answered the UDP query at ${key} (${result.reason}); is UDP port ${a.port} open?`, {
            probe: result.reason,
          });
        }
        if (result.protocol !== a.protocol) {
          logger.info('announce rejected', { reason: 'protocol_mismatch', ip, key, announced: a.protocol, replied: result.protocol });
          return fail(c, 422, 'protocol_mismatch', 'the server answers the UDP query with a different protocol version than it announced', {
            announced: a.protocol,
            replied: result.protocol,
          });
        }
      }

      const now = clock();
      const stored = await store.upsert(
        {
          key,
          ip,
          ipGroup: group,
          port: a.port,
          name: a.name,
          gamemode: a.gamemode,
          lang: a.lang,
          url: a.url === undefined || a.url === '' ? null : a.url,
          players: a.players,
          maxPlayers: a.maxPlayers,
          protocol: a.protocol,
        },
        { maxPerIp: config.maxPerIp, maxEntries: config.maxEntries, ttlMs },
        now,
      );
      if (!stored.ok) {
        if (stored.reason === 'ip-cap') {
          logger.warn('announce rejected', { reason: 'ip_cap', ip, key, maxPerIp: config.maxPerIp });
          return fail(c, 403, 'too_many_servers', `this address already lists ${config.maxPerIp} servers`);
        }
        logger.error('announce rejected', { reason: 'store_full', ip, key, maxEntries: config.maxEntries });
        c.header('Retry-After', '30');
        return fail(c, 503, 'server_list_full', 'the server list is full');
      }

      if (stored.created) logger.info('server listed', { key, name: a.name, protocol: a.protocol });
      else logger.debug('server refreshed', { key });

      c.header('Cache-Control', 'no-store');
      return c.json({
        status: 'ok',
        key,
        created: stored.created,
        ttlSeconds: config.ttlSeconds,
        // Announce at least this often (a lost heartbeat or two must not expire the entry).
        heartbeatSeconds: Math.max(1, Math.floor(config.ttlSeconds / 3)),
      });
    },
  );

  app.get('/servers', async (c) => {
    const records = await store.list(clock());
    // Busiest first, key as the tie-break: stable output between refreshes.
    records.sort((x, y) => y.players - x.players || (x.key < y.key ? -1 : x.key > y.key ? 1 : 0));
    const body: Record<string, ServerListEntry> = {};
    for (const r of records) {
      body[r.key] = {
        name: r.name,
        url: r.url ?? '',
        lang: r.lang,
        gamemode: r.gamemode,
        players: r.players,
        maxplayers: r.maxPlayers,
        protocol: r.protocol,
      };
    }
    c.header('Cache-Control', 'public, max-age=10');
    return c.json(body);
  });

  app.get('/healthz', async (c) => {
    const now = clock();
    c.header('Cache-Control', 'no-store');
    try {
      const servers = (await store.list(now)).length;
      return c.json({ status: 'ok', servers, uptimeSeconds: Math.floor((now - startedAt) / 1000) });
    } catch (err) {
      logger.error('health check failed: the store is not readable', errorFields(err));
      return c.json({ status: 'error' }, 503);
    }
  });

  app.notFound((c) => fail(c, 404, 'not_found', 'no such endpoint'));

  app.onError((err, c) => {
    if (err instanceof HTTPException) return err.getResponse();
    logger.error('unhandled error', { path: c.req.path, ...errorFields(err) });
    return c.json({ error: 'internal', message: 'internal error' }, 500);
  });

  return app;
}
