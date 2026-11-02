import { createSocket, type Socket } from 'node:dgram';
import { createHash } from 'node:crypto';
import type { Hono } from 'hono';
import { expect } from 'vitest';
import { loadConfig, type Config } from '../src/config.js';
import { openDatabase, schema, type DbHandle } from '../src/db/index.js';
import type { EmailMessage, EmailSender } from '../src/email.js';
import { createApp } from '../src/http/app.js';
import type { AppEnv } from '../src/http/types.js';
import { buildDeps, createEmailSender } from '../src/platform.js';
import type { Deps } from '../src/deps.js';
import { FakeProvider, FAKE_SIGNATURE_HEADER } from '../src/shop/fake-provider.js';
import { decodeQueryRequest, encodeQueryReply } from '../src/servers/query-protocol.js';
import type { Prober } from '../src/servers/status.js';
import { createLogger } from '../src/util/logger.js';

export const STRONG_PASSWORD = 'correct-horse-battery-staple';

export class TestClock {
  private ms = Date.now();
  now = (): Date => new Date(this.ms);
  advance(ms: number): void {
    this.ms += ms;
  }
  advanceSeconds(s: number): void {
    this.advance(s * 1000);
  }
}

export interface ApiResult<T = Record<string, unknown>> {
  status: number;
  body: T;
  headers: Headers;
}

export interface CallOptions {
  method?: string;
  json?: unknown;
  rawBody?: string;
  token?: string;
  /** Socket peer address (and so the client IP unless TRUST_PROXY=1). */
  ip?: string;
  headers?: Record<string, string>;
}

export interface TestEnv {
  deps: Deps;
  handle: DbHandle;
  app: Hono<AppEnv>;
  config: Config;
  clock: TestClock;
  fake: FakeProvider;
  logs: string[];
  /** Every message the platform handed to its EmailSender, oldest first, tokens included (what a recipient sees). */
  mail: CapturedMail[];
  call<T = Record<string, unknown>>(path: string, opts?: CallOptions): Promise<ApiResult<T>>;
  /** Build another app over the same dependencies (e.g. only the public or internal surface). */
  appFor(surface: 'public' | 'internal' | 'all'): Hono<AppEnv>;
  close(): Promise<void>;
}

export interface EnvOptions {
  env?: Record<string, string>;
  prober?: Prober;
  resolveHost?: (host: string) => Promise<string>;
  fetchImpl?: typeof fetch;
}

let ipCounter = 0;
/** A fresh documentation-range IPv4 address so a test's rate limits never collide with another test's. */
export function freshIp(): string {
  ipCounter++;
  return `198.51.${(ipCounter >> 8) & 255}.${(ipCounter & 255) || 1}`;
}

export async function createTestEnv(opts: EnvOptions = {}): Promise<TestEnv> {
  const config = loadConfig({
    NODE_ENV: 'test',
    // The /v1/dev/* routes are opt-in; most suites pay orders through them (hardening.test.ts turns them off).
    ENABLE_DEV_ROUTES: '1',
    // Cheap argon2 for speed; the hashing test checks the production defaults separately.
    ARGON2_MEMORY_KIB: '1024',
    ARGON2_TIME_COST: '1',
    FAKE_WEBHOOK_SECRET: 'test-fake-webhook-secret-0123456789',
    DISCORD_CLIENT_ID: 'test-client-id',
    DISCORD_CLIENT_SECRET: 'test-client-secret',
    PUBLIC_API_URL: 'http://platform.test',
    ...opts.env,
  });
  const logs: string[] = [];
  const logger = createLogger({ level: 'debug', write: (line) => logs.push(line) });
  const clock = new TestClock();
  const handle = await openDatabase('pglite:memory');
  await handle.migrate();
  const fake = new FakeProvider(config.fakeWebhookSecret, config.publicApiUrl, clock.now);
  // The outbox driver stores a REDACTED copy (that is the point), so tests read the live token from
  // what the sender was handed, the way a recipient would from their inbox.
  const mail: CapturedMail[] = [];
  const email = new RecordingEmailSender(createEmailSender(config, handle.db, clock.now, logger), mail);
  const { deps } = await buildDeps({
    config,
    logger,
    handle,
    clock: clock.now,
    email,
    payments: [fake],
    ...(opts.prober ? { prober: opts.prober } : {}),
    ...(opts.resolveHost ? { resolveHost: opts.resolveHost } : {}),
    ...(opts.fetchImpl ? { fetchImpl: opts.fetchImpl } : {}),
  });
  const appFor = (surface: 'public' | 'internal' | 'all') => createApp(deps, surface).app;
  const app = appFor('all');

  const call = async <T>(path: string, o: CallOptions = {}): Promise<ApiResult<T>> => {
    const headers: Record<string, string> = { ...o.headers };
    if (o.token) headers['authorization'] = `Bearer ${o.token}`;
    let body: string | undefined;
    if (o.rawBody !== undefined) body = o.rawBody;
    else if (o.json !== undefined) {
      body = JSON.stringify(o.json);
      headers['content-type'] ??= 'application/json';
    }
    const res = await app.request(
      path,
      { method: o.method ?? (body === undefined ? 'GET' : 'POST'), headers, ...(body === undefined ? {} : { body }) },
      { incoming: { socket: { remoteAddress: o.ip ?? '203.0.113.7', remotePort: 50000, remoteFamily: 'IPv4' } } },
    );
    const text = await res.text();
    let parsed: unknown = {};
    if (text !== '') {
      try {
        parsed = JSON.parse(text);
      } catch {
        parsed = { text };
      }
    }
    return { status: res.status, body: parsed as T, headers: res.headers };
  };

  return { deps, handle, app, config, clock, fake, logs, mail, call, appFor, close: () => handle.close() };
}

// --- accounts ---------------------------------------------------------------------------------

export interface TestUser {
  accountId: string;
  username: string;
  email: string;
  password: string;
  token: string;
  sessionId: string;
}

let userCounter = 0;

export interface CapturedMail {
  to: string;
  subject: string;
  /** The full text, live token included. */
  body: string;
  template: string;
  meta: Record<string, unknown>;
  /** The single-use value the message carries (verification or reset token), if any. */
  token: string | undefined;
}

/** Records what the platform sends, then hands it to the real sender (the outbox driver) so that path stays exercised. */
class RecordingEmailSender implements EmailSender {
  constructor(
    private readonly inner: EmailSender,
    private readonly sink: CapturedMail[],
  ) {}

  async send(m: EmailMessage): Promise<void> {
    this.sink.push({ to: m.to, subject: m.subject, body: m.text, template: m.template, meta: m.meta ?? {}, token: m.secrets?.[0] });
    await this.inner.send(m);
  }
}

/** Latest email of `template` sent to `to`, as the recipient got it. */
export async function latestMail(env: TestEnv, to: string, template: string): Promise<CapturedMail | undefined> {
  return env.mail.findLast((m) => m.to === to && m.template === template);
}

export async function mailToken(env: TestEnv, to: string, template: 'verify_email' | 'password_reset'): Promise<string> {
  const mail = await latestMail(env, to, template);
  expect(mail, `an "${template}" email to ${to}`).toBeDefined();
  const token = mail?.token;
  expect(token, `a token in the "${template}" email to ${to}`).toBeTruthy();
  return token as string;
}

/** Register, verify the email through the outbox token, log in. */
export async function makeUser(env: TestEnv, opts: { username?: string; password?: string; verified?: boolean; ip?: string } = {}): Promise<TestUser> {
  userCounter++;
  const username = opts.username ?? `Player_${userCounter}_${Math.random().toString(36).slice(2, 6)}`;
  const email = `${username.toLowerCase()}@example.test`;
  const password = opts.password ?? STRONG_PASSWORD + userCounter;
  const ip = opts.ip ?? freshIp();
  const reg = await env.call('/v1/auth/register', { json: { email, username, password }, ip });
  expect(reg.status, JSON.stringify(reg.body)).toBe(201);
  if (opts.verified !== false) {
    const token = await mailToken(env, email, 'verify_email');
    const v = await env.call('/v1/auth/verify-email', { json: { token }, ip });
    expect(v.status, JSON.stringify(v.body)).toBe(200);
  }
  const login = await env.call<{ token: string; sessionId: string; account: { id: string } }>('/v1/auth/login', { json: { identifier: email, password }, ip });
  expect(login.status, JSON.stringify(login.body)).toBe(200);
  return { accountId: login.body.account.id, username, email, password, token: login.body.token, sessionId: login.body.sessionId };
}

export async function makeStaff(env: TestEnv, role: 'admin' | 'mod', opts: { username?: string } = {}): Promise<TestUser> {
  const u = await makeUser(env, opts);
  await env.handle.db.insert(schema.accountRoles).values({ accountId: u.accountId, role });
  return u;
}

// --- game servers -----------------------------------------------------------------------------

export interface TestServer {
  id: string;
  secret: string;
}

export async function makeServer(
  env: TestEnv,
  admin: TestUser,
  opts: { id?: string; host?: string; port?: number; allowedIps?: string[]; maintenance?: boolean; visible?: boolean } = {},
): Promise<TestServer> {
  const id = opts.id ?? `srv-${Math.random().toString(36).slice(2, 8)}`;
  const res = await env.call<{ secret: string }>('/v1/admin/servers', {
    token: admin.token,
    json: {
      id,
      name: `Server ${id}`,
      region: 'eu',
      host: opts.host ?? '127.0.0.1',
      port: opts.port ?? 24800,
      allowedIps: opts.allowedIps ?? ['127.0.0.1'],
      maintenance: opts.maintenance ?? false,
      visible: opts.visible ?? true,
    },
  });
  expect(res.status, JSON.stringify(res.body)).toBe(201);
  return { id, secret: res.body.secret };
}

/** Call an /internal/game route as a game server. */
export function asServer(env: TestEnv, server: TestServer, ip = '127.0.0.1') {
  return <T = Record<string, unknown>>(path: string, o: { json?: unknown; method?: string } = {}) =>
    env.call<T>(path, {
      ip,
      ...(o.json === undefined ? {} : { json: o.json }),
      ...(o.method === undefined ? {} : { method: o.method }),
      headers: { 'x-gage-server-id': server.id, 'x-gage-server-secret': server.secret },
    });
}

/** Issue a join token for `user` on `server` and redeem it as the game server: the player is "in game". */
export async function joinServer(env: TestEnv, user: TestUser, server: TestServer): Promise<{ sessionId: string }> {
  const join = await env.call<{ token: string }>('/v1/game/join', { token: user.token, json: { serverId: server.id } });
  expect(join.status, JSON.stringify(join.body)).toBe(200);
  const verify = await asServer(env, server)<{ ok: boolean; sessionId: string }>('/internal/game/verify', { json: { token: join.body.token } });
  expect(verify.body.ok, JSON.stringify(verify.body)).toBe(true);
  return { sessionId: verify.body.sessionId };
}

// --- UDP responder ----------------------------------------------------------------------------

export interface FakeGameServerUdp {
  port: number;
  /** Valid queries received. */
  queries: number;
  players: number;
  maxPlayers: number;
  protocol: number;
  /** When false the responder stays silent (a hung or firewalled server). */
  respond: boolean;
  /** When true the responder answers every query with a 24-byte datagram that is not a valid reply. */
  garbage: boolean;
  close(): Promise<void>;
}

/** A UDP socket on 127.0.0.1 that answers the GAGE query like the C++ server does (apps/master/PROTOCOL.md). */
export async function startFakeUdpServer(init: { players?: number; maxPlayers?: number; protocol?: number } = {}): Promise<FakeGameServerUdp> {
  const socket: Socket = createSocket('udp4');
  const state: FakeGameServerUdp = {
    port: 0,
    queries: 0,
    players: init.players ?? 3,
    maxPlayers: init.maxPlayers ?? 32,
    protocol: init.protocol ?? 1,
    respond: true,
    garbage: false,
    close: () => new Promise((resolve) => socket.close(() => resolve())),
  };
  socket.on('message', (msg, rinfo) => {
    const nonce = decodeQueryRequest(msg);
    if (nonce === null) return;
    state.queries++;
    if (!state.respond) return;
    if (state.garbage) {
      socket.send(Buffer.alloc(24, 0x5a), rinfo.port, rinfo.address);
      return;
    }
    socket.send(encodeQueryReply(nonce, { protocol: state.protocol, players: state.players, maxPlayers: state.maxPlayers }), rinfo.port, rinfo.address);
  });
  await new Promise<void>((resolve) => socket.bind(0, '127.0.0.1', resolve));
  state.port = socket.address().port;
  return state;
}

// --- payments ---------------------------------------------------------------------------------

let eventCounter = 0;

/** Deliver a signed fake-provider webhook the way a provider would (HTTP, raw body, signature header). */
export async function sendWebhook(
  env: TestEnv,
  event: { type: 'payment.succeeded' | 'payment.failed' | 'refund.succeeded' | 'chargeback.opened'; orderId: string; amountMinor?: number | null; currency?: string | null; id?: string; providerRef?: string | null },
  opts: { signature?: string | null; rawBody?: string } = {},
) {
  eventCounter++;
  const raw = opts.rawBody ?? JSON.stringify({ id: event.id ?? `evt_test_${eventCounter}`, type: event.type, orderId: event.orderId, providerRef: event.providerRef ?? null, amountMinor: event.amountMinor ?? null, currency: event.currency ?? null });
  const headers: Record<string, string> = { 'content-type': 'application/json' };
  const sig = opts.signature === undefined ? env.fake.sign(raw) : opts.signature;
  if (sig !== null) headers[FAKE_SIGNATURE_HEADER] = sig;
  return env.call<{ received?: boolean; duplicate?: boolean; result?: string; error?: { code: string } }>('/v1/webhooks/fake', { rawBody: raw, headers, ip: '203.0.113.99' });
}

export interface TestProduct {
  id: string;
  sku: string;
}

export async function makeProduct(
  env: TestEnv,
  admin: TestUser,
  p: { sku: string; kind: 'currency_pack' | 'vip' | 'cosmetic' | 'bundle'; priceMinor?: number; grants: unknown[]; maxQuantity?: number; currency?: string; active?: boolean },
): Promise<TestProduct> {
  const res = await env.call<{ product: { id: string } }>('/v1/admin/products', {
    token: admin.token,
    json: {
      sku: p.sku,
      name: `Product ${p.sku}`,
      description: 'test product',
      priceMinor: p.priceMinor ?? 499,
      currency: p.currency ?? 'USD',
      kind: p.kind,
      grantPayload: { grants: p.grants },
      maxQuantity: p.maxQuantity ?? 1,
      ...(p.active === undefined ? {} : { active: p.active }),
    },
  });
  expect(res.status, JSON.stringify(res.body)).toBe(201);
  return { id: res.body.product.id, sku: p.sku };
}

/** SHA-256 hex, for assertions about stored hashes. */
export const sha256Hex = (s: string): string => createHash('sha256').update(s).digest('hex');
