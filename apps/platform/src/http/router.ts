import { Hono } from 'hono';
import type { Context, MiddlewareHandler } from 'hono';
import { bodyLimit } from 'hono/body-limit';
import { z } from 'zod';
import { assertNotBanned, hasRole, getRoles } from '../auth/accounts.js';
import { authenticate } from '../auth/sessions.js';
import { enforce } from '../auth/throttle.js';
import type { Deps } from '../deps.js';
import { authenticateServer } from '../servers/service.js';
import { ApiError } from '../util/errors.js';
import type { AppEnv } from './types.js';

/**
 * How a route authenticates:
 *   none     anyone
 *   session  a signed-in account (Authorization: Bearer <session token>)
 *   mod      session whose account has the mod or admin role and is not banned (403 account_banned)
 *   admin    session whose account has the admin role and is not banned (403 account_banned)
 *   server   a game server (X-Gage-Server-Id + X-Gage-Server-Secret, from an allowlisted IP)
 *   webhook  a payment provider (signature checked inside the handler over the raw body)
 */
export type AuthKind = 'none' | 'session' | 'mod' | 'admin' | 'server' | 'webhook';

export interface LimitSpec {
  /** Counter name (also the key prefix). */
  name: string;
  scope: 'ip' | 'account';
  limit: number;
  windowSeconds: number;
}

export type Surface = 'public' | 'internal' | 'both';

export interface RouteDef<B extends z.ZodType | undefined = undefined, Q extends z.ZodType | undefined = undefined> {
  method: 'get' | 'post' | 'put' | 'patch' | 'delete';
  /** Hono path syntax (`:id` parameters). */
  path: string;
  summary: string;
  description?: string;
  auth: AuthKind;
  /** Which listener serves it. Default: internal for /internal/*, otherwise public. */
  surface?: Surface;
  tags: string[];
  /** JSON request body, validated before the handler runs. */
  body?: B;
  /** Query string, validated before the handler runs. */
  query?: Q;
  /** What a successful response looks like (prose; the reference in docs/api.md has examples). */
  response: string;
  /** Error codes this route can return besides the generic ones (invalid_request, unauthorized, rate_limited, internal_error). */
  errors?: string[];
  limits?: LimitSpec[];
  /** Override the default body size limit. */
  maxBodyBytes?: number;
  successStatus?: 200 | 201 | 202 | 204;
  /** false = documented in the reference but not mounted on this instance (dev-only routes). Default true. */
  enabled?: boolean;
}

/** Everything the reference generator needs about a registered route. */
export interface RouteMeta {
  method: string;
  path: string;
  summary: string;
  description: string | undefined;
  auth: AuthKind;
  surface: Surface;
  tags: string[];
  body: z.ZodType | undefined;
  query: z.ZodType | undefined;
  response: string;
  errors: string[];
  limits: LimitSpec[];
  maxBodyBytes: number | undefined;
  successStatus: number;
}

type Infer<T> = T extends z.ZodType ? z.output<T> : undefined;
export type Ctx = Context<AppEnv>;

export interface RouteInput<B, Q> {
  body: B;
  query: Q;
}

/** Hono path -> OpenAPI path (`:id` -> `{id}`). */
export function openApiPath(path: string): string {
  return path.replace(/:([A-Za-z0-9_]+)/g, '{$1}');
}

export function issuesOf(error: z.ZodError): { path: string; message: string }[] {
  return error.issues.slice(0, 10).map((i) => ({ path: i.path.join('.'), message: i.message }));
}

/** Registers routes on a Hono app, applying auth, limits, body limits and validation from the route definition, and records each definition for the reference. */
export class Router {
  readonly registry: RouteMeta[] = [];

  constructor(
    readonly app: Hono<AppEnv>,
    private readonly deps: Deps,
    private readonly serve: ReadonlySet<'public' | 'internal'>,
  ) {}

  add<B extends z.ZodType | undefined = undefined, Q extends z.ZodType | undefined = undefined>(
    def: RouteDef<B, Q>,
    handler: (c: Ctx, input: RouteInput<Infer<B>, Infer<Q>>) => Promise<Response> | Response,
  ): void {
    const surface: Surface = def.surface ?? (def.path.startsWith('/internal/') ? 'internal' : 'public');
    this.registry.push({
      method: def.method,
      path: def.path,
      summary: def.summary,
      description: def.description,
      auth: def.auth,
      surface,
      tags: def.tags,
      body: def.body,
      query: def.query,
      response: def.response,
      errors: def.errors ?? [],
      limits: def.limits ?? [],
      maxBodyBytes: def.maxBodyBytes,
      successStatus: def.successStatus ?? 200,
    });
    const served = surface === 'both' ? this.serve.size > 0 : this.serve.has(surface);
    if (!served || def.enabled === false) return;

    const chain: MiddlewareHandler<AppEnv>[] = [];
    const limits = def.limits ?? [];
    for (const l of limits.filter((x) => x.scope === 'ip')) chain.push(this.ipLimit(l));
    chain.push(this.authGuard(def.auth));
    for (const l of limits.filter((x) => x.scope === 'account')) chain.push(this.accountLimit(l));
    if (def.body !== undefined || def.method === 'post' || def.method === 'put' || def.method === 'patch') {
      const max = def.maxBodyBytes ?? this.deps.config.bodyLimitBytes;
      chain.push(
        bodyLimit({
          maxSize: max,
          onError: () => {
            throw new ApiError(413, 'payload_too_large', `The request body is larger than ${max} bytes.`);
          },
        }),
      );
    }

    const run: MiddlewareHandler<AppEnv> = async (c) => {
      const body = def.body ? await parseBody(c, def.body) : undefined;
      const query = def.query ? parseQuery(c, def.query) : undefined;
      return handler(c, { body: body as Infer<B>, query: query as Infer<Q> });
    };
    // Hono's overloads do not accept a spread middleware list; the call shape is (method, path, ...handlers).
    const register = this.app.on.bind(this.app) as unknown as (method: string, path: string, ...handlers: MiddlewareHandler<AppEnv>[]) => void;
    register(def.method.toUpperCase(), def.path, ...chain, run);
  }

  private ipLimit(l: LimitSpec): MiddlewareHandler<AppEnv> {
    return async (c, next) => {
      await enforce(this.deps.db, `${l.name}:ip:${c.get('clientGroup')}`, l.limit, l.windowSeconds, this.deps.clock());
      await next();
    };
  }

  private accountLimit(l: LimitSpec): MiddlewareHandler<AppEnv> {
    return async (c, next) => {
      const session = c.get('session');
      if (!session) throw new Error(`route limit "${l.name}" has scope account but the route has no session`);
      await enforce(this.deps.db, `${l.name}:acct:${session.account.id}`, l.limit, l.windowSeconds, this.deps.clock());
      await next();
    };
  }

  private authGuard(kind: AuthKind): MiddlewareHandler<AppEnv> {
    const { deps } = this;
    if (kind === 'none' || kind === 'webhook') return async (_c, next) => next();

    if (kind === 'server') {
      return async (c, next) => {
        const peer = c.get('peerIp');
        try {
          const server = await authenticateServer(deps.db, c.req.header('x-gage-server-id'), c.req.header('x-gage-server-secret'), peer);
          c.set('server', server);
        } catch (err) {
          if (err instanceof ApiError) deps.logger.warn('internal call refused', { code: err.code, peer, serverId: (c.req.header('x-gage-server-id') ?? '').slice(0, 64), path: c.req.path });
          throw err;
        }
        await next();
      };
    }

    return async (c, next) => {
      const header = c.req.header('authorization') ?? '';
      const m = /^Bearer\s+(\S+)$/i.exec(header);
      const auth = m?.[1] ? await authenticate(deps.db, deps.config, m[1], deps.clock()) : null;
      if (!auth) {
        c.header('WWW-Authenticate', 'Bearer');
        throw new ApiError(401, 'unauthorized', 'Sign in required.');
      }
      const roles = await getRoles(deps.db, auth.account.id);
      if (kind === 'mod' && !hasRole(roles, 'mod')) throw new ApiError(403, 'forbidden', 'Moderator access required.');
      if (kind === 'admin' && !hasRole(roles, 'admin')) throw new ApiError(403, 'forbidden', 'Admin access required.');
      // A banned moderator or admin keeps the account (to read the ban) but none of the powers.
      if (kind === 'mod' || kind === 'admin') await assertNotBanned(deps.db, auth.account.id, deps.clock());
      c.set('session', { id: auth.sessionId, account: auth.account, roles });
      await next();
    };
  }
}

async function parseBody<B extends z.ZodType>(c: Ctx, schema: B): Promise<z.output<B>> {
  const text = await c.req.text();
  let json: unknown;
  // An empty body is "no body" (undefined), which only schemas that accept it (optional) pass.
  if (text.trim() !== '') {
    const contentType = c.req.header('content-type') ?? '';
    if (!/^application\/json\s*(;|$)/i.test(contentType)) {
      throw new ApiError(415, 'unsupported_media_type', 'Content-Type must be application/json.');
    }
    try {
      json = JSON.parse(text);
    } catch {
      throw new ApiError(400, 'invalid_json', 'The request body is not valid JSON.');
    }
  }
  const parsed = schema.safeParse(json);
  if (!parsed.success) throw new ApiError(400, 'invalid_request', 'The request is not valid.', { details: { issues: issuesOf(parsed.error) } });
  return parsed.data;
}

function parseQuery<Q extends z.ZodType>(c: Ctx, schema: Q): z.output<Q> {
  const raw: Record<string, string> = {};
  for (const [k, v] of new URL(c.req.url).searchParams) if (!(k in raw)) raw[k] = v;
  const parsed = schema.safeParse(raw);
  if (!parsed.success) throw new ApiError(400, 'invalid_request', 'The query string is not valid.', { details: { issues: issuesOf(parsed.error) } });
  return parsed.data;
}

const UUID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

/** A UUID path parameter, or 400. (Any UUID shape Postgres accepts, not only RFC variants.) */
export function uuidParam(c: Ctx, name: string): string {
  const v = c.req.param(name);
  if (v === undefined || !UUID_PATTERN.test(v)) {
    throw new ApiError(400, 'invalid_request', `Path parameter "${name}" must be a UUID.`);
  }
  return v;
}
