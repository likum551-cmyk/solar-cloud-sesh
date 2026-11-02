import { connect } from 'node:net';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { loadConfig } from '../src/config.js';
import { createApp } from '../src/http/app.js';
import { buildDeps, startPlatform } from '../src/platform.js';
import { createLogger } from '../src/util/logger.js';
import { asServer, createTestEnv, freshIp, makeServer, makeStaff, makeUser, STRONG_PASSWORD, type TestEnv } from './helpers.js';

const ORIGIN = 'https://app.gage';
let env: TestEnv;

beforeAll(async () => {
  env = await createTestEnv({ env: { BODY_LIMIT_BYTES: '2048' } });
});
afterAll(async () => {
  await env.close();
});

type ErrBody = { error: { code: string; message: string; requestId?: string; details?: { issues?: { path: string; message: string }[] } } };

describe('CORS (restricted to the launcher origin)', () => {
  it('echoes the allowed origin and exposes the request id; credentials are never allowed', async () => {
    const res = await env.call('/v1/servers', { headers: { origin: ORIGIN } });
    expect(res.status).toBe(200);
    expect(res.headers.get('access-control-allow-origin')).toBe(ORIGIN);
    expect(res.headers.get('access-control-expose-headers')).toContain('X-Request-Id');
    expect(res.headers.get('access-control-allow-credentials')).toBeNull();
  });

  it('gives any other origin no CORS headers at all (null, a look-alike, a different scheme, a subdomain)', async () => {
    for (const origin of ['https://evil.example', 'null', 'http://app.gage', 'https://app.gage.evil.example', 'https://sub.app.gage', 'https://APP.gage']) {
      const res = await env.call('/v1/servers', { headers: { origin } });
      expect(res.status, origin).toBe(200);
      expect(res.headers.get('access-control-allow-origin'), origin).toBeNull();
    }
    // No Origin header (the launcher's own native calls, curl): nothing to add.
    expect((await env.call('/v1/servers')).headers.get('access-control-allow-origin')).toBeNull();
  });

  it('answers a preflight for the allowed origin with the methods and headers the API uses', async () => {
    const res = await env.app.request(
      '/v1/shop/checkout',
      { method: 'OPTIONS', headers: { origin: ORIGIN, 'access-control-request-method': 'POST', 'access-control-request-headers': 'authorization,content-type,idempotency-key' } },
      { incoming: { socket: { remoteAddress: freshIp() } } },
    );
    expect(res.status).toBe(204);
    expect(res.headers.get('access-control-allow-origin')).toBe(ORIGIN);
    expect(res.headers.get('access-control-allow-methods')).toContain('POST');
    const allowed = (res.headers.get('access-control-allow-headers') ?? '').toLowerCase();
    for (const h of ['authorization', 'content-type', 'idempotency-key']) expect(allowed).toContain(h);
    expect(res.headers.get('access-control-max-age')).toBe('600');
  });

  it('a preflight from another origin is not approved', async () => {
    const res = await env.app.request(
      '/v1/shop/checkout',
      { method: 'OPTIONS', headers: { origin: 'https://evil.example', 'access-control-request-method': 'POST' } },
      { incoming: { socket: { remoteAddress: freshIp() } } },
    );
    // Without Allow-Origin the browser refuses the real request, whatever else is listed.
    expect(res.headers.get('access-control-allow-origin')).toBeNull();
  });

  it('error responses carry the CORS header too, so the launcher can read them', async () => {
    const res = await env.call<ErrBody>('/v1/me', { headers: { origin: ORIGIN } });
    expect(res.status).toBe(401);
    expect(res.headers.get('access-control-allow-origin')).toBe(ORIGIN);
    expect(res.body.error.code).toBe('unauthorized');
  });

  it('CORS_ORIGINS is configurable and takes several origins', async () => {
    const two = await createTestEnv({ env: { CORS_ORIGINS: 'https://one.example, http://localhost:5173' } });
    try {
      expect((await two.call('/health', { headers: { origin: 'https://one.example' } })).headers.get('access-control-allow-origin')).toBe('https://one.example');
      expect((await two.call('/health', { headers: { origin: 'http://localhost:5173' } })).headers.get('access-control-allow-origin')).toBe('http://localhost:5173');
      expect((await two.call('/health', { headers: { origin: ORIGIN } })).headers.get('access-control-allow-origin')).toBeNull(); // the default is replaced, not extended
    } finally {
      await two.close();
    }
  });

  it('the internal listener never answers CORS: browsers have no business there', async () => {
    const internal = env.appFor('internal');
    const res = await internal.request('/health', { headers: { origin: ORIGIN } }, { incoming: { socket: { remoteAddress: '127.0.0.1' } } });
    expect(res.status).toBe(200);
    expect(res.headers.get('access-control-allow-origin')).toBeNull();
  });
});

describe('error shape, request ids and headers', () => {
  it('unknown routes, bad JSON, wrong content type and invalid bodies all use { error: { code, message, requestId } }', async () => {
    const notFound = await env.call<ErrBody>('/v1/does-not-exist');
    expect(notFound.status).toBe(404);
    expect(notFound.body.error).toMatchObject({ code: 'not_found' });
    expect(notFound.body.error.requestId).toBe(notFound.headers.get('x-request-id'));

    const badJson = await env.call<ErrBody>('/v1/auth/login', { rawBody: '{"identifier":', headers: { 'content-type': 'application/json' }, ip: freshIp() });
    expect(badJson.status).toBe(400);
    expect(badJson.body.error.code).toBe('invalid_json');

    const wrongType = await env.call<ErrBody>('/v1/auth/login', { rawBody: 'identifier=a&password=b', headers: { 'content-type': 'application/x-www-form-urlencoded' }, ip: freshIp() });
    expect(wrongType.status).toBe(415);
    expect(wrongType.body.error.code).toBe('unsupported_media_type');

    const invalid = await env.call<ErrBody>('/v1/auth/register', { json: { email: 'not-an-email', username: 'ab', password: 'x' }, ip: freshIp() });
    expect(invalid.status).toBe(400);
    expect(invalid.body.error.code).toBe('invalid_request');
    expect(invalid.body.error.details?.issues?.map((i) => i.path).sort()).toEqual(['email', 'username']);

    const unauth = await env.call<ErrBody>('/v1/me');
    expect(unauth.status).toBe(401);
    expect(unauth.headers.get('www-authenticate')).toBe('Bearer');
    for (const r of [notFound, badJson, wrongType, invalid, unauth]) {
      expect(Object.keys(r.body)).toEqual(['error']);
      expect(typeof r.body.error.message).toBe('string');
    }
  });

  it('refuses a body over BODY_LIMIT_BYTES with 413 before parsing it', async () => {
    const big = JSON.stringify({ identifier: 'a', password: 'x'.repeat(4000) });
    const res = await env.call<ErrBody>('/v1/auth/login', { rawBody: big, headers: { 'content-type': 'application/json' }, ip: freshIp() });
    expect(res.status).toBe(413);
    expect(res.body.error.code).toBe('payload_too_large');
    const small = await env.call<ErrBody>('/v1/auth/login', { json: { identifier: 'a', password: 'x'.repeat(100) }, ip: freshIp() });
    expect(small.status).toBe(401); // under the limit it is parsed (and fails on credentials)
  });

  it('an unexpected exception is a generic 500 that leaks nothing, and is logged with the request id', async () => {
    const boom = createApp(
      { ...env.deps, serverStatus: { statuses: () => Promise.reject(new Error('password=hunter2 host=10.0.0.5')) } },
      'all',
    ).app;
    const res = await boom.request('/v1/servers', { headers: { 'x-request-id': 'req-for-the-500' } }, { incoming: { socket: { remoteAddress: freshIp() } } });
    expect(res.status).toBe(500);
    const text = await res.text();
    expect(JSON.parse(text)).toEqual({ error: { code: 'internal_error', message: 'Something went wrong on our side.', requestId: 'req-for-the-500' } });
    expect(text).not.toContain('hunter2');
    const logged = env.logs.filter((l) => l.includes('unhandled error') && l.includes('req-for-the-500'));
    expect(logged).toHaveLength(1);
    expect(logged[0]).toContain('hunter2'); // the operator sees the cause in the log, the client does not
  });

  it('generates a request id, echoes a sane inbound one and replaces a hostile one', async () => {
    const generated = await env.call('/health');
    expect(generated.headers.get('x-request-id')).toMatch(/^[0-9a-f-]{36}$/);
    const echoed = await env.call('/health', { headers: { 'x-request-id': 'launcher-req_12345' } });
    expect(echoed.headers.get('x-request-id')).toBe('launcher-req_12345');
    for (const hostile of ['short', 'has spaces in it!!', 'x'.repeat(65), 'a'.repeat(8) + 'é']) {
      const res = await env.call('/health', { headers: { 'x-request-id': hostile } });
      expect(res.headers.get('x-request-id'), hostile).toMatch(/^[0-9a-f-]{36}$/);
    }
  });

  it('every response is no-store, nosniff, no-referrer', async () => {
    for (const path of ['/health', '/v1/servers', '/v1/me', '/nope']) {
      const res = await env.call(path);
      expect(res.headers.get('cache-control'), path).toBe('no-store');
      expect(res.headers.get('x-content-type-options'), path).toBe('nosniff');
      expect(res.headers.get('referrer-policy'), path).toBe('no-referrer');
    }
  });

  it('writes one structured access-log line per request without secrets', async () => {
    const user = await makeUser(env);
    env.logs.length = 0;
    await env.call('/v1/me', { token: user.token, headers: { 'x-request-id': 'req-access-log-1' } });
    const lines = env.logs.map((l) => JSON.parse(l) as Record<string, unknown>).filter((l) => l['msg'] === 'request' && l['requestId'] === 'req-access-log-1');
    expect(lines).toHaveLength(1);
    expect(lines[0]).toMatchObject({ level: 'info', method: 'GET', path: '/v1/me', status: 200, accountId: user.accountId });
    expect(typeof lines[0]?.['ms']).toBe('number');
    expect(env.logs.join('\n')).not.toContain(user.token);
  });
});

describe('health', () => {
  it('reports ok with uptime and version', async () => {
    env.clock.advanceSeconds(90);
    const res = await env.call<{ status: string; uptimeSeconds: number; version: string }>('/health');
    expect(res.status).toBe(200);
    expect(res.body.status).toBe('ok');
    expect(res.body.uptimeSeconds).toBeGreaterThanOrEqual(90);
    expect(res.body.version).toMatch(/^\d+\.\d+\.\d+$/);
  });

  it('is 503 database_unavailable when the database does not answer, without leaking the cause', async () => {
    const sick = createApp({ ...env.deps, dbPing: () => Promise.reject(new Error('connect ECONNREFUSED 10.1.2.3:5432')) }, 'all').app;
    const res = await sick.request('/health', {}, { incoming: { socket: { remoteAddress: '127.0.0.1' } } });
    expect(res.status).toBe(503);
    const text = await res.text();
    expect(JSON.parse(text).error.code).toBe('database_unavailable');
    expect(text).not.toContain('10.1.2.3');
    expect(env.logs.some((l) => l.includes('health check failed') && l.includes('ECONNREFUSED'))).toBe(true);
  });
});

describe('the generated reference', () => {
  interface OpenApi { openapi: string; info: { version: string }; servers: { url: string }[]; paths: Record<string, Record<string, { summary: string; security?: unknown[]; requestBody?: { content: { 'application/json': { schema: { properties?: Record<string, unknown>; required?: string[] } } } }; responses: Record<string, unknown> }>> }

  it('describes the mounted routes as OpenAPI 3.1 with schemas taken from the validators', async () => {
    const res = await env.call<OpenApi>('/v1/openapi.json');
    expect(res.status).toBe(200);
    expect(res.body.openapi).toBe('3.1.0');
    expect(res.body.servers[0]?.url).toBe('http://platform.test');
    const register = res.body.paths['/v1/auth/register']?.['post'];
    expect(register?.requestBody?.content['application/json'].schema.properties).toHaveProperty('username');
    expect(register?.requestBody?.content['application/json'].schema.required).toEqual(expect.arrayContaining(['email', 'username', 'password']));
    expect(res.body.paths['/v1/admin/servers/{id}']).toBeDefined(); // :id became {id}
    expect(Object.keys(res.body.paths).length).toBeGreaterThan(50);
    expect(Object.keys(res.body.paths).some((p) => p.includes(':'))).toBe(false);
  });

  it('a listener documents only what it serves', async () => {
    const peer = { incoming: { socket: { remoteAddress: '127.0.0.1' } } };
    const pub = (await (await env.appFor('public').request('/v1/openapi.json', {}, peer)).json()) as OpenApi;
    const internal = (await (await env.appFor('internal').request('/v1/openapi.json', {}, peer)).json()) as OpenApi;
    expect(Object.keys(pub.paths).some((p) => p.startsWith('/internal/'))).toBe(false);
    expect(Object.keys(pub.paths)).toContain('/v1/auth/login');
    expect(Object.keys(internal.paths).filter((p) => p.startsWith('/internal/')).length).toBeGreaterThan(5);
    expect(Object.keys(internal.paths)).not.toContain('/v1/auth/login');
    expect(Object.keys(internal.paths)).toContain('/health');
  });
});

describe('public and internal surfaces', () => {
  it('the public listener does not mount /internal/*, the internal one does not mount /v1/auth/*', async () => {
    const peer = { incoming: { socket: { remoteAddress: '127.0.0.1' } } };
    const admin = await makeStaff(env, 'admin');
    const server = await makeServer(env, admin);
    const headers = { 'x-gage-server-id': server.id, 'x-gage-server-secret': server.secret, 'content-type': 'application/json' };
    const onPublic = await env.appFor('public').request('/internal/game/verify', { method: 'POST', headers, body: '{}' }, peer);
    expect(onPublic.status).toBe(404);
    const onInternal = await env.appFor('internal').request('/internal/game/verify', { method: 'POST', headers, body: JSON.stringify({ token: 'x'.repeat(40) }) }, peer);
    expect(onInternal.status).not.toBe(404);
    expect((await env.appFor('internal').request('/v1/auth/login', { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{}' }, peer)).status).toBe(404);
    expect((await env.appFor('public').request('/health', {}, peer)).status).toBe(200);
    expect((await env.appFor('internal').request('/health', {}, peer)).status).toBe(200);
  });
});

describe('client address handling', () => {
  const sessionIp = async (e: TestEnv, headers: Record<string, string>, socketIp: string) => {
    const username = `Ip_${Math.random().toString(36).slice(2, 8)}`;
    const email = `${username.toLowerCase()}@example.test`;
    const reg = await e.call('/v1/auth/register', { json: { email, username, password: `${STRONG_PASSWORD}-ip` }, ip: socketIp, headers });
    expect(reg.status, JSON.stringify(reg.body)).toBe(201);
    const login = await e.call<{ token: string }>('/v1/auth/login', { json: { identifier: email, password: `${STRONG_PASSWORD}-ip` }, ip: socketIp, headers });
    const sessions = await e.call<{ sessions: { ip: string }[] }>('/v1/me/sessions', { token: login.body.token, ip: socketIp, headers });
    return sessions.body.sessions[0]?.ip;
  };

  it('ignores X-Forwarded-For by default (a client could write anything there)', async () => {
    expect(await sessionIp(env, { 'x-forwarded-for': '6.6.6.6' }, '203.0.113.50')).toBe('203.0.113.50');
  });

  it('TRUST_PROXY=1 uses the LAST entry (what the proxy appended), never one the client wrote', async () => {
    const proxied = await createTestEnv({ env: { TRUST_PROXY: '1' } });
    try {
      expect(await sessionIp(proxied, { 'x-forwarded-for': '6.6.6.6, 198.51.100.77' }, '10.0.0.2')).toBe('198.51.100.77');
      expect(await sessionIp(proxied, { 'x-forwarded-for': '2001:db8::1' }, '10.0.0.2')).toBe('2001:db8::1');
      // Missing or malformed header: fall back to the socket and say so in the log.
      expect(await sessionIp(proxied, { 'x-forwarded-for': 'not-an-ip' }, '10.0.0.3')).toBe('10.0.0.3');
      expect(await sessionIp(proxied, {}, '10.0.0.4')).toBe('10.0.0.4');
      expect(proxied.logs.some((l) => l.includes('TRUST_PROXY=1 but X-Forwarded-For is missing or invalid'))).toBe(true);
    } finally {
      await proxied.close();
    }
  });

  it('rate limits follow the resolved client, so one proxy address cannot exhaust everyone', async () => {
    const proxied = await createTestEnv({ env: { TRUST_PROXY: '1' } });
    try {
      const flood = async (client: string, n: number) => {
        const statuses: number[] = [];
        for (let i = 0; i < n; i++) statuses.push((await proxied.call('/v1/auth/discord/start', { method: 'POST', ip: '10.0.0.9', headers: { 'x-forwarded-for': `1.1.1.1, ${client}` } })).status);
        return statuses;
      };
      const first = await flood('198.51.100.1', 22);
      expect(first.filter((s) => s === 429).length).toBeGreaterThan(0);
      expect((await flood('198.51.100.2', 1))[0]).not.toBe(429);
    } finally {
      await proxied.close();
    }
  });

  it('/internal/* judges the socket peer even with TRUST_PROXY=1: a spoofed header cannot pass the server allowlist', async () => {
    const proxied = await createTestEnv({ env: { TRUST_PROXY: '1' } });
    try {
      const admin = await makeStaff(proxied, 'admin');
      const server = await makeServer(proxied, admin, { allowedIps: ['127.0.0.1'] });
      const spoof = await proxied.call<ErrBody>('/internal/game/session-end', {
        json: { accountId: '11111111-1111-4111-8111-111111111111' },
        ip: '198.51.100.200',
        headers: { 'x-gage-server-id': server.id, 'x-gage-server-secret': server.secret, 'x-forwarded-for': '127.0.0.1' },
      });
      expect(spoof.status).toBe(403);
      expect(spoof.body.error.code).toBe('ip_not_allowed');
      const genuine = await asServer(proxied, server)<ErrBody>('/internal/game/session-end', { json: { accountId: '11111111-1111-4111-8111-111111111111' } });
      expect(genuine.status).toBe(200);
    } finally {
      await proxied.close();
    }
  });

  it('an IPv4-mapped IPv6 peer is the same client as its IPv4 address, and an IPv6 client is limited by /64', async () => {
    expect(await sessionIp(env, {}, '::ffff:203.0.113.60')).toBe('203.0.113.60');
    const flood = async (ip: string, n: number) => {
      const out: number[] = [];
      for (let i = 0; i < n; i++) out.push((await env.call('/v1/auth/discord/start', { method: 'POST', ip })).status);
      return out;
    };
    const a = await flood('2001:db8:aaaa:1:0:0:0:1', 20);
    expect(a.every((s) => s === 200)).toBe(true);
    // A different address inside the same /64 shares the budget...
    expect((await flood('2001:db8:aaaa:1:ffff:0:0:2', 1))[0]).toBe(429);
    // ...another /64 does not.
    expect((await flood('2001:db8:aaaa:2:0:0:0:1', 1))[0]).toBe(200);
  });
});

describe('a real listener and graceful shutdown', () => {
  it('serves on separate public and internal ports, lets an in-flight request finish on shutdown, then stops listening', async () => {
    const lines: string[] = [];
    const config = loadConfig({
      NODE_ENV: 'test', FAKE_WEBHOOK_SECRET: 'test-fake-webhook-secret-0123456789', HOST: '127.0.0.1', PORT: '0', INTERNAL_HOST: '127.0.0.1', INTERNAL_PORT: '0',
      DATABASE_URL: 'pglite:memory', ARGON2_MEMORY_KIB: '1024', ARGON2_TIME_COST: '1', SHUTDOWN_GRACE_SECONDS: '10',
      MAINTENANCE_INTERVAL_SECONDS: '3600',
    });
    const built = await buildDeps({ config, logger: createLogger({ level: 'info', write: (l) => lines.push(l) }) });
    await built.handle.migrate();
    const running = await startPlatform(built);
    let closed = false;
    let closedWhileBodyPending: boolean | null = null;
    try {
      expect(running.publicPort).toBeGreaterThan(0);
      expect(running.internalPort).toBeGreaterThan(0);
      expect(running.internalPort).not.toBe(running.publicPort);

      const health = await fetch(`http://127.0.0.1:${running.publicPort}/health`);
      expect(health.status).toBe(200);
      expect((await health.json() as { status: string }).status).toBe('ok');
      expect((await fetch(`http://127.0.0.1:${running.internalPort}/health`)).status).toBe(200);
      // The game-server API is only on the internal port.
      expect((await fetch(`http://127.0.0.1:${running.publicPort}/internal/game/verify`, { method: 'POST' })).status).toBe(404);
      expect((await fetch(`http://127.0.0.1:${running.internalPort}/internal/game/verify`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{}' })).status).toBe(401);
      expect((await fetch(`http://127.0.0.1:${running.internalPort}/v1/servers`)).status).toBe(404);

      // A request whose body is still arriving when shutdown starts.
      const payload = JSON.stringify({ identifier: 'nobody@example.test', password: 'whatever-whatever' });
      const half = Math.floor(payload.length / 2);
      const response = await new Promise<string>((resolve, reject) => {
        const socket = connect(running.publicPort, '127.0.0.1');
        let received = '';
        socket.on('data', (d) => (received += d.toString('utf8')));
        socket.on('error', reject);
        socket.on('close', () => resolve(received));
        socket.write(`POST /v1/auth/login HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: ${payload.length}\r\n\r\n${payload.slice(0, half)}`);
        setTimeout(() => {
          void running.close().then(() => { closed = true; });
          setTimeout(() => {
            closedWhileBodyPending = closed;
            socket.write(payload.slice(half));
          }, 200);
        }, 100);
      });
      expect(closedWhileBodyPending, 'close() must wait for the in-flight request').toBe(false);
      expect(response.startsWith('HTTP/1.1 401')).toBe(true);
      expect(response).toContain('invalid_credentials');

      await running.close(); // idempotent: resolves once everything has stopped
      expect(closed).toBe(true);
      await expect(fetch(`http://127.0.0.1:${running.publicPort}/health`)).rejects.toThrow();
      await expect(fetch(`http://127.0.0.1:${running.internalPort}/health`)).rejects.toThrow();
      expect(lines.some((l) => l.includes('"listening"'))).toBe(true);
    } finally {
      await running.close();
    }
  });
});
