import type { z } from 'zod';
import { ApiFailure } from './errors';
import {
  balancesSchema, discordPollSchema, discordStartSchema, errorBodySchema, joinResultSchema, loginResultSchema, meSchema,
  okSchema, orderEnvelopeSchema, ordersSchema, ownedItemsSchema, passwordChangedSchema, productsSchema, questClaimSchema,
  questListSchema, questStartSchema, revokedSchema, sentSchema, serversSchema, sessionsSchema, settingsViewSchema,
} from './schemas';

// The launcher's only door to the platform API. Every route path lives here (ROUTES) so the
// client can be re-aligned with the platform's docs/api.md in one file. Every response is
// validated with zod; every failure becomes an ApiFailure (see errors.ts).

// No register route: accounts are created on the website (lib/site.ts SIGNUP_URL).
export const ROUTES = {
  login: '/v1/auth/login',
  logout: '/v1/auth/logout',
  verifyEmail: '/v1/auth/verify-email',
  resendVerification: '/v1/auth/resend-verification',
  forgotPassword: '/v1/auth/forgot-password',
  resetPassword: '/v1/auth/reset-password',
  discordStart: '/v1/auth/discord/start',
  discordPoll: '/v1/auth/discord/poll',
  discordComplete: '/v1/auth/discord/complete',
  // No Discord link or unlink route: both need the password and run on the website (lib/site.ts ACCOUNT_URL).
  me: '/v1/me',
  password: '/v1/me/password',
  sessions: '/v1/me/sessions',
  settings: '/v1/me/settings',
  wallet: '/v1/me/wallet',
  items: '/v1/me/items',
  /** GET the list; POST `${quests}/<slug>/start` and `${quests}/<slug>/claim`. */
  quests: '/v1/me/quests',
  servers: '/v1/servers',
  join: '/v1/game/join',
  products: '/v1/shop/products',
  checkout: '/v1/shop/checkout',
  orders: '/v1/shop/orders',
} as const;

export type FetchLike = (input: string, init: RequestInit) => Promise<Response>;

export interface ApiOptions {
  /** API origin, e.g. "https://api.solar-rp.com". Empty = not configured (every call fails `not_configured`). */
  baseUrl: string;
  /** The current session token, or null when signed out. Read on every call. */
  getToken: () => string | null;
  /** Injected for tests and the mock network. Defaults to window.fetch. */
  fetchImpl?: FetchLike;
  /** Per-request timeout. */
  timeoutMs?: number;
}

interface Call<S extends z.ZodType | null> {
  method: 'GET' | 'POST' | 'PUT' | 'DELETE';
  path: string;
  body?: unknown;
  auth?: boolean;
  headers?: Record<string, string>;
  schema: S;
}

/** Create the API client. Pure: no globals besides the injected fetch. */
export function createApi(opts: ApiOptions) {
  const timeoutMs = opts.timeoutMs ?? 12_000;
  const doFetch: FetchLike = opts.fetchImpl ?? ((input, init) => fetch(input, init));

  async function call<S extends z.ZodType>(c: Call<S>): Promise<z.infer<S>>;
  async function call(c: Call<null>): Promise<void>;
  async function call(c: Call<z.ZodType | null>): Promise<unknown> {
    const base = opts.baseUrl.replace(/\/+$/, '');
    if (!base) throw new ApiFailure('not_configured', 'No API base URL configured.');
    const headers: Record<string, string> = { Accept: 'application/json', ...c.headers };
    if (c.body !== undefined) headers['Content-Type'] = 'application/json';
    if (c.auth) {
      const token = opts.getToken();
      if (!token) throw new ApiFailure('unauthorized', 'Not signed in.', { status: 401, code: 'unauthorized' });
      headers.Authorization = `Bearer ${token}`;
    }
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), timeoutMs);
    let res: Response;
    try {
      res = await doFetch(base + c.path, {
        method: c.method,
        headers,
        body: c.body === undefined ? null : JSON.stringify(c.body),
        signal: ctrl.signal,
        credentials: 'omit',
        cache: 'no-store',
      });
    } catch (err) {
      if (ctrl.signal.aborted) throw new ApiFailure('timeout', `No answer within ${timeoutMs} ms.`);
      throw new ApiFailure('offline', err instanceof Error ? err.message : 'Network request failed.');
    } finally {
      clearTimeout(timer);
    }
    return readResponse(res, c.schema);
  }

  return {
    // --- auth -----------------------------------------------------------------------------------
    login: (b: { identifier: string; password: string; deviceName?: string }) =>
      call({ method: 'POST', path: ROUTES.login, body: b, schema: loginResultSchema }),
    logout: () => call({ method: 'POST', path: ROUTES.logout, auth: true, schema: null }),
    verifyEmail: (token: string) => call({ method: 'POST', path: ROUTES.verifyEmail, body: { token }, schema: meSchema }),
    resendVerification: () => call({ method: 'POST', path: ROUTES.resendVerification, auth: true, schema: sentSchema }),
    forgotPassword: (email: string) => call({ method: 'POST', path: ROUTES.forgotPassword, body: { email }, schema: sentSchema }),
    resetPassword: (token: string, password: string) =>
      call({ method: 'POST', path: ROUTES.resetPassword, body: { token, password }, schema: okSchema }),
    discordStart: () => call({ method: 'POST', path: ROUTES.discordStart, body: {}, schema: discordStartSchema }),
    discordPoll: (flowId: string, pollSecret: string) =>
      call({ method: 'POST', path: ROUTES.discordPoll, body: { flowId, pollSecret }, schema: discordPollSchema }),
    discordComplete: (flowId: string, pollSecret: string, username: string) =>
      call({ method: 'POST', path: ROUTES.discordComplete, body: { flowId, pollSecret, username }, schema: loginResultSchema }),

    // --- the signed-in account ------------------------------------------------------------------
    me: () => call({ method: 'GET', path: ROUTES.me, auth: true, schema: meSchema }),
    changePassword: (newPassword: string, currentPassword?: string) =>
      call({ method: 'POST', path: ROUTES.password, auth: true, body: currentPassword ? { currentPassword, newPassword } : { newPassword }, schema: passwordChangedSchema }),
    sessions: () => call({ method: 'GET', path: ROUTES.sessions, auth: true, schema: sessionsSchema }),
    revokeSession: (id: string) => call({ method: 'DELETE', path: `${ROUTES.sessions}/${encodeURIComponent(id)}`, auth: true, schema: null }),
    revokeOtherSessions: () => call({ method: 'DELETE', path: ROUTES.sessions, auth: true, schema: revokedSchema }),
    getSettings: () => call({ method: 'GET', path: ROUTES.settings, auth: true, schema: settingsViewSchema }),
    putSettings: (settings: Record<string, unknown>, expectedVersion?: number) =>
      call({ method: 'PUT', path: ROUTES.settings, auth: true, body: expectedVersion === undefined ? { settings } : { settings, expectedVersion }, schema: settingsViewSchema }),
    wallet: () => call({ method: 'GET', path: ROUTES.wallet, auth: true, schema: balancesSchema }),
    items: () => call({ method: 'GET', path: ROUTES.items, auth: true, schema: ownedItemsSchema }),

    // --- quests (apps/platform docs/api.md "Quests") ----------------------------------------------
    quests: () => call({ method: 'GET', path: ROUTES.quests, auth: true, schema: questListSchema }),
    /** Records the start (idempotent: a repeat keeps the first time); then open `quest.url`. */
    startQuest: (slug: string) => call({ method: 'POST', path: `${ROUTES.quests}/${encodeURIComponent(slug)}/start`, auth: true, schema: questStartSchema }),
    /** Pays the reward once; refusals: quest_wait (409, Retry-After), discord_required (403), already_claimed, ... */
    claimQuest: (slug: string) => call({ method: 'POST', path: `${ROUTES.quests}/${encodeURIComponent(slug)}/claim`, auth: true, schema: questClaimSchema }),

    // --- play -----------------------------------------------------------------------------------
    servers: () => call({ method: 'GET', path: ROUTES.servers, schema: serversSchema }),
    join: (serverId: string) => call({ method: 'POST', path: ROUTES.join, auth: true, body: { serverId }, schema: joinResultSchema }),

    // --- shop -----------------------------------------------------------------------------------
    products: () => call({ method: 'GET', path: ROUTES.products, schema: productsSchema }),
    /** Creates a pending order; open `order.checkoutUrl` in the system browser. Same key = same order on retry. */
    checkout: (sku: string, quantity: number, idempotencyKey: string) =>
      call({ method: 'POST', path: ROUTES.checkout, auth: true, body: { items: [{ sku, quantity }] }, headers: { 'Idempotency-Key': idempotencyKey }, schema: orderEnvelopeSchema }),
    orders: () => call({ method: 'GET', path: `${ROUTES.orders}?limit=50`, auth: true, schema: ordersSchema }),
    order: (id: string) => call({ method: 'GET', path: `${ROUTES.orders}/${encodeURIComponent(id)}`, auth: true, schema: orderEnvelopeSchema }),
  };
}

export type Api = ReturnType<typeof createApi>;

/** Map a Response to a validated body or an ApiFailure. Exported for tests. */
export async function readResponse(res: Response, schema: z.ZodType | null): Promise<unknown> {
  if (res.ok) {
    if (schema === null || res.status === 204) return undefined;
    let json: unknown;
    try {
      json = await res.json();
    } catch {
      throw new ApiFailure('invalid_response', 'The response was not JSON.', { status: res.status });
    }
    const parsed = schema.safeParse(json);
    if (!parsed.success) {
      throw new ApiFailure('invalid_response', `Unexpected response shape: ${parsed.error.issues.slice(0, 3).map((i) => `${i.path.join('.')}: ${i.message}`).join('; ')}`, { status: res.status });
    }
    return parsed.data;
  }
  let body: z.infer<typeof errorBodySchema> | null = null;
  try {
    const parsed = errorBodySchema.safeParse(await res.json());
    body = parsed.success ? parsed.data : null;
  } catch {
    body = null;
  }
  const code = body?.error.code ?? null;
  const message = body?.error.message ?? `HTTP ${res.status}`;
  const details = body?.error.details;
  // 401 `unauthorized` = no valid session. Other 401 codes (invalid_credentials on login or on a
  // password change) are refusals of the request, not a dead session.
  if (res.status === 401 && (code === null || code === 'unauthorized')) throw new ApiFailure('unauthorized', message, { status: 401, code: 'unauthorized', details });
  if (res.status === 429) {
    const header = Number(res.headers.get('Retry-After'));
    throw new ApiFailure('rate_limited', message, { status: 429, code, retryAfterSeconds: Number.isFinite(header) && header > 0 ? header : null, details });
  }
  // 503 with the API's own code is a deliberate refusal (server_maintenance, payment_unavailable);
  // any other 5xx, or a body that is not the API's error shape (a proxy page), is a server fault.
  if (body === null || code === 'internal_error' || (res.status >= 500 && res.status !== 503)) {
    throw new ApiFailure('server', message, { status: res.status, code, details });
  }
  // A refusal may say when to try again (409 quest_wait sends Retry-After).
  const retry = Number(res.headers.get('Retry-After'));
  throw new ApiFailure('refused', message, { status: res.status, code, details, retryAfterSeconds: Number.isFinite(retry) && retry > 0 ? retry : null });
}

/** A fresh Idempotency-Key (8-64 chars of [A-Za-z0-9_-]) for one checkout attempt. */
export function newIdempotencyKey(): string {
  const bytes = new Uint8Array(18);
  crypto.getRandomValues(bytes);
  let s = '';
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
