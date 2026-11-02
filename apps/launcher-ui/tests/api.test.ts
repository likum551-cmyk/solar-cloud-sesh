import { describe, expect, it, vi } from 'vitest';
import { createApi, newIdempotencyKey, readResponse, ROUTES, type FetchLike } from '../src/lib/api';
import { ApiFailure, describeFailure, formatWait } from '../src/lib/errors';
import { meSchema, serversSchema } from '../src/lib/schemas';

const account = {
  id: 'a1', username: 'nightshift', email: 'n@example.com', emailVerified: true, roles: ['player'],
  hasPassword: true, discord: null, ban: null, createdAt: '2026-01-02T10:00:00.000Z',
};

const jsonRes = (status: number, body: unknown, headers: Record<string, string> = {}) =>
  new Response(body === undefined ? null : JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json', ...headers } });
const errRes = (status: number, code: string, message = code, headers: Record<string, string> = {}) =>
  jsonRes(status, { error: { code, message, requestId: 'r1' } }, headers);

async function failureOf(p: Promise<unknown>): Promise<ApiFailure> {
  try {
    await p;
  } catch (e) {
    if (e instanceof ApiFailure) return e;
    throw new Error(`expected ApiFailure, got ${String(e)}`);
  }
  throw new Error('expected the call to fail, it resolved');
}

describe('readResponse: success bodies are validated', () => {
  it('returns the parsed body when it matches the schema', async () => {
    const out = await readResponse(jsonRes(200, { account }), meSchema);
    expect(out).toEqual({ account });
  });

  it('maps a body that does not match to invalid_response and names the field', async () => {
    const f = await failureOf(readResponse(jsonRes(200, { account: { ...account, emailVerified: 'yes' } }), meSchema));
    expect(f.kind).toBe('invalid_response');
    expect(f.message).toContain('account.emailVerified');
  });

  it('maps a non-JSON 200 to invalid_response', async () => {
    const f = await failureOf(readResponse(new Response('<html>', { status: 200 }), meSchema));
    expect(f.kind).toBe('invalid_response');
  });

  it('rejects a date without an offset (the platform always sends ISO with Z)', async () => {
    const f = await failureOf(readResponse(jsonRes(200, { account: { ...account, createdAt: '2026-01-02 10:00' } }), meSchema));
    expect(f.kind).toBe('invalid_response');
  });

  it('returns undefined for a 204 or a schema-less call', async () => {
    expect(await readResponse(new Response(null, { status: 204 }), meSchema)).toBeUndefined();
    expect(await readResponse(jsonRes(200, { anything: 1 }), null)).toBeUndefined();
  });
});

describe('readResponse: error mapping', () => {
  it('401 unauthorized is a dead session', async () => {
    const f = await failureOf(readResponse(errRes(401, 'unauthorized'), meSchema));
    expect([f.kind, f.status, f.code]).toEqual(['unauthorized', 401, 'unauthorized']);
  });

  it('401 with no API body is a dead session too', async () => {
    const f = await failureOf(readResponse(new Response('nope', { status: 401 }), meSchema));
    expect(f.kind).toBe('unauthorized');
  });

  it('401 invalid_credentials is a refusal, not a dead session', async () => {
    const f = await failureOf(readResponse(errRes(401, 'invalid_credentials', 'wrong'), meSchema));
    expect([f.kind, f.code]).toEqual(['refused', 'invalid_credentials']);
    expect(describeFailure(f)).toBe('That email or username and password do not match.');
  });

  it('429 carries Retry-After seconds', async () => {
    const f = await failureOf(readResponse(errRes(429, 'rate_limited', 'slow down', { 'Retry-After': '540' }), meSchema));
    expect([f.kind, f.retryAfterSeconds]).toEqual(['rate_limited', 540]);
    expect(describeFailure(f)).toBe('Too many attempts. Try again in 9 min.');
  });

  it('429 without a usable Retry-After has no wait', async () => {
    const f = await failureOf(readResponse(errRes(429, 'rate_limited', 'x', { 'Retry-After': 'soon' }), meSchema));
    expect(f.retryAfterSeconds).toBeNull();
  });

  it('503 with an API code is a deliberate refusal', async () => {
    const f = await failureOf(readResponse(errRes(503, 'server_maintenance', 'Back at 18:00'), meSchema));
    expect([f.kind, f.code, f.status]).toEqual(['refused', 'server_maintenance', 503]);
  });

  it('503 from a proxy (no API body) is a server fault', async () => {
    const f = await failureOf(readResponse(new Response('<h1>Bad gateway</h1>', { status: 503 }), meSchema));
    expect(f.kind).toBe('server');
  });

  it('500 internal_error and 502 are server faults', async () => {
    expect((await failureOf(readResponse(errRes(500, 'internal_error'), meSchema))).kind).toBe('server');
    expect((await failureOf(readResponse(errRes(502, 'bad_gateway'), meSchema))).kind).toBe('server');
  });

  it('a 4xx with a code is refused and keeps details', async () => {
    const res = jsonRes(400, { error: { code: 'weak_password', message: 'Use at least 10 characters.', details: { min: 10 } } });
    const f = await failureOf(readResponse(res, meSchema));
    expect([f.kind, f.code, f.details]).toEqual(['refused', 'weak_password', { min: 10 }]);
    expect(describeFailure(f)).toBe('Use at least 10 characters.');
  });
});

describe('createApi: transport', () => {
  it('fails not_configured without touching the network when no base URL is set', async () => {
    const fetchImpl = vi.fn<FetchLike>();
    const api = createApi({ baseUrl: '', getToken: () => 't', fetchImpl });
    expect((await failureOf(api.servers())).kind).toBe('not_configured');
    expect(fetchImpl).not.toHaveBeenCalled();
  });

  it('fails unauthorized locally when an authed call has no token', async () => {
    const fetchImpl = vi.fn<FetchLike>();
    const api = createApi({ baseUrl: 'https://api.example', getToken: () => null, fetchImpl });
    expect((await failureOf(api.me())).kind).toBe('unauthorized');
    expect(fetchImpl).not.toHaveBeenCalled();
  });

  it('sends the bearer token, JSON body, no cookies, and strips a trailing slash from the base', async () => {
    let seenUrl = '';
    let seenInit: RequestInit | null = null;
    const fetchImpl: FetchLike = async (url, init) => {
      seenUrl = url;
      seenInit = init;
      return jsonRes(200, { token: 'join-token-123', expiresAt: '2026-10-29T10:00:00.000Z', server: { id: 's1', name: 'Downtown', host: '203.0.113.5', port: 22005 } });
    };
    const api = createApi({ baseUrl: 'https://api.example/', getToken: () => 'tok-abc', fetchImpl });
    const out = await api.join('s1');
    expect(out.server.port).toBe(22005);
    expect(seenUrl).toBe(`https://api.example${ROUTES.join}`);
    const init = seenInit as unknown as RequestInit;
    const headers = new Headers(init.headers);
    expect(headers.get('Authorization')).toBe('Bearer tok-abc');
    expect(headers.get('Content-Type')).toBe('application/json');
    expect(init.credentials).toBe('omit');
    expect(JSON.parse(String(init.body))).toEqual({ serverId: 's1' });
  });

  it('sends the Idempotency-Key on checkout', async () => {
    let key: string | null = null;
    const fetchImpl: FetchLike = async (_url, init) => {
      key = new Headers(init.headers).get('Idempotency-Key');
      return errRes(503, 'payment_unavailable');
    };
    const api = createApi({ baseUrl: 'https://api.example', getToken: () => 'tok', fetchImpl });
    const f = await failureOf(api.checkout('vip-30', 1, 'abcdefgh1234'));
    expect(key).toBe('abcdefgh1234');
    expect(describeFailure(f)).toBe('Payments are unavailable right now. Nothing was charged.');
  });

  it('maps a fetch throw to offline', async () => {
    const api = createApi({ baseUrl: 'https://api.example', getToken: () => null, fetchImpl: async () => { throw new TypeError('Failed to fetch'); } });
    const f = await failureOf(api.servers());
    expect([f.kind, f.message]).toEqual(['offline', 'Failed to fetch']);
  });

  it('maps an aborted request to timeout', async () => {
    const fetchImpl: FetchLike = (_url, init) => new Promise((_resolve, reject) => {
      init.signal?.addEventListener('abort', () => reject(new DOMException('aborted', 'AbortError')));
    });
    const api = createApi({ baseUrl: 'https://api.example', getToken: () => null, fetchImpl, timeoutMs: 20 });
    expect((await failureOf(api.servers())).kind).toBe('timeout');
  });

  it('validates list responses item by item', async () => {
    const api = createApi({ baseUrl: 'https://api.example', getToken: () => null, fetchImpl: async () => jsonRes(200, { servers: [{ id: 's1' }] }) });
    expect((await failureOf(api.servers())).kind).toBe('invalid_response');
    expect(serversSchema.safeParse({ servers: [] }).success).toBe(true);
  });
});

describe('helpers', () => {
  it('newIdempotencyKey is 24 url-safe chars and unique', () => {
    const keys = new Set(Array.from({ length: 200 }, () => newIdempotencyKey()));
    expect(keys.size).toBe(200);
    for (const k of keys) expect(k).toMatch(/^[A-Za-z0-9_-]{24}$/);
  });

  it('formatWait reads like a person', () => {
    expect([formatWait(0), formatWait(45), formatWait(60), formatWait(540), formatWait(3600), formatWait(7500)])
      .toEqual(['0 s', '45 s', '1 min', '9 min', '1 h', '2 h 5 min']);
  });
});
