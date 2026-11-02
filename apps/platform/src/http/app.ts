import { randomUUID } from 'node:crypto';
import { getConnInfo } from '@hono/node-server/conninfo';
import { Hono } from 'hono';
import { cors } from 'hono/cors';
import { loadConfig } from '../config.js';
import type { Deps } from '../deps.js';
import { ApiError } from '../util/errors.js';
import { ipGroup, lastForwardedFor, normalizeIp } from '../util/ip.js';
import { createLogger, errorFields } from '../util/logger.js';
import { buildOpenApi } from './openapi.js';
import { registerAdminRoutes } from './routes/admin.js';
import { registerAuthRoutes } from './routes/auth.js';
import { registerInternalRoutes } from './routes/internal.js';
import { registerPlayerRoutes } from './routes/player.js';
import { registerWebhookRoutes } from './routes/webhooks.js';
import { Router, type RouteMeta } from './router.js';
import type { AppEnv } from './types.js';

export const API_VERSION = '0.1.0';

/** Which routes an instance mounts: the public API, the internal game-server API, or both. */
export type SurfaceChoice = 'public' | 'internal' | 'all';

export interface CreatedApp {
  app: Hono<AppEnv>;
  /** Every route definition (including ones this instance does not mount), for the reference. */
  routes: RouteMeta[];
}

const REQUEST_ID = /^[A-Za-z0-9_-]{8,64}$/;

/**
 * Build the Hono app. Cross-cutting behaviour lives here: request ids, client address, structured
 * access log, no-store + nosniff headers, CORS restricted to CORS_ORIGINS (public routes only),
 * the JSON error shape, the health endpoint and the generated reference.
 */
export function createApp(deps: Deps, surface: SurfaceChoice): CreatedApp {
  const { config, logger } = deps;
  const app = new Hono<AppEnv>();
  const serve = new Set<'public' | 'internal'>(surface === 'all' ? ['public', 'internal'] : [surface]);
  const startedAt = deps.clock().getTime();

  // Request id, client address, headers, access log -------------------------------------------
  app.use('*', async (c, next) => {
    const inbound = c.req.header('x-request-id');
    const requestId = inbound !== undefined && REQUEST_ID.test(inbound) ? inbound : randomUUID();
    c.set('requestId', requestId);
    c.header('X-Request-Id', requestId);

    let peerRaw: string | undefined;
    try {
      peerRaw = getConnInfo(c).remote.address;
    } catch {
      peerRaw = undefined; // no socket (app.request in tests without bindings)
    }
    const peer = peerRaw === undefined ? null : normalizeIp(peerRaw);
    c.set('peerIp', peer);
    let ip = peer;
    // Internal routes never trust a header: their allowlist is about the socket peer.
    if (config.trustProxy && !c.req.path.startsWith('/internal/')) {
      const forwarded = lastForwardedFor(c.req.header('x-forwarded-for'));
      if (forwarded !== null) ip = forwarded;
      else logger.warn('TRUST_PROXY=1 but X-Forwarded-For is missing or invalid: using the socket address', { peer });
    }
    const client = ip ?? 'unknown';
    c.set('clientIp', client);
    c.set('clientGroup', ipGroup(client));

    const t0 = performance.now();
    try {
      await next();
    } finally {
      c.header('Cache-Control', 'no-store');
      c.header('X-Content-Type-Options', 'nosniff');
      c.header('Referrer-Policy', 'no-referrer');
      const quiet = c.req.path === '/health';
      const level = quiet ? 'debug' : 'info';
      logger[level]('request', {
        requestId,
        method: c.req.method,
        path: c.req.path,
        status: c.res.status,
        ms: Math.round(performance.now() - t0),
        ip: client,
        ...(c.get('session') ? { accountId: c.get('session')?.account.id } : {}),
        ...(c.get('server') ? { serverId: c.get('server')?.id } : {}),
      });
    }
  });

  // CORS: only the launcher's WebView2 origin(s). Other origins get no CORS headers at all, so a
  // browser page elsewhere cannot read responses. Credentials are never used (bearer tokens).
  if (serve.has('public')) {
    app.use(
      '*',
      cors({
        origin: (origin) => (config.corsOrigins.includes(origin) ? origin : null),
        allowMethods: ['GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS'],
        allowHeaders: ['Authorization', 'Content-Type', 'Idempotency-Key', 'X-Request-Id'],
        exposeHeaders: ['X-Request-Id', 'Retry-After'],
        maxAge: 600,
      }),
    );
  }

  // Routes --------------------------------------------------------------------------------------
  const router = new Router(app, deps, serve);

  router.add(
    {
      method: 'get', path: '/health', summary: 'Liveness and database check', auth: 'none', surface: 'both', tags: ['meta'],
      description: 'Used by the process supervisor and load balancer. 503 when the database does not answer.',
      response: '200 { status: "ok", uptimeSeconds, version }', errors: ['database_unavailable'],
    },
    async (c) => {
      try {
        await Promise.race([
          deps.dbPing(),
          new Promise((_, reject) => setTimeout(() => reject(new Error('database ping timed out')), 3000).unref()),
        ]);
      } catch (err) {
        logger.error('health check failed', errorFields(err));
        throw new ApiError(503, 'database_unavailable', 'The database is not reachable.');
      }
      return c.json({ status: 'ok', uptimeSeconds: Math.floor((deps.clock().getTime() - startedAt) / 1000), version: API_VERSION });
    },
  );

  registerAuthRoutes(router, deps);
  registerPlayerRoutes(router, deps);
  registerWebhookRoutes(router, deps);
  registerAdminRoutes(router, deps);
  registerInternalRoutes(router, deps);

  router.add(
    {
      method: 'get', path: '/v1/openapi.json', summary: 'Machine-readable reference of the routes this listener serves', auth: 'none', surface: 'both', tags: ['meta'],
      description: 'OpenAPI 3.1, generated from the same schemas the handlers validate with.',
      response: '200 OpenAPI document',
    },
    (c) => {
      const mounted = router.registry.filter((r) => r.surface === 'both' || serve.has(r.surface));
      return c.json(buildOpenApi(mounted, { version: API_VERSION, serverUrl: config.publicApiUrl }));
    },
  );

  // Errors --------------------------------------------------------------------------------------
  app.notFound((c) => c.json({ error: { code: 'not_found', message: 'No such route.', requestId: c.get('requestId') } }, 404));

  app.onError((err, c) => {
    const requestId = c.get('requestId');
    if (err instanceof ApiError) {
      if (err.retryAfterSeconds !== undefined) c.header('Retry-After', String(err.retryAfterSeconds));
      return c.json(
        { error: { code: err.code, message: err.message, requestId, ...(err.details === undefined ? {} : { details: err.details }) } },
        err.status,
      );
    }
    logger.error('unhandled error', { requestId, method: c.req.method, path: c.req.path, ...errorFields(err) });
    return c.json({ error: { code: 'internal_error', message: 'Something went wrong on our side.', requestId } }, 500);
  });

  return { app, routes: router.registry };
}

/**
 * Every route definition, without a database or a running server: for the docs check and tests.
 * Handlers are never called, so the dependencies are inert placeholders.
 */
export function listRoutes(): RouteMeta[] {
  // A throwaway test-mode config (loadConfig refuses an environment that does not say what it is).
  const config = loadConfig({ NODE_ENV: 'test', FAKE_WEBHOOK_SECRET: 'placeholder-for-route-listing' });
  const deps = {
    config,
    db: undefined,
    dbPing: () => Promise.resolve(),
    logger: createLogger({ level: 'error', write: () => undefined }),
    clock: () => new Date(),
    email: { send: () => Promise.resolve() },
    discord: null,
    payments: [],
    serverStatus: { statuses: () => Promise.resolve(new Map()) },
  } as unknown as Deps;
  return createApp(deps, 'all').routes;
}
