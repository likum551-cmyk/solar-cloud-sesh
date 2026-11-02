import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { IP_A, IP_B, IP_C, IP_V6, json, makeHarness, validBody } from './helpers.js';

const T0 = 1_700_000_000_000;

beforeEach(() => {
  vi.useFakeTimers({ now: T0 });
});
afterEach(() => {
  vi.useRealTimers();
});

async function listed(h: ReturnType<typeof makeHarness>): Promise<Record<string, any>> {
  return json(await h.get('/servers'));
}

describe('POST /announce basics', () => {
  it('lists the server under ip:port taken from the connection, and tells it how often to heartbeat', async () => {
    const h = makeHarness();
    const res = await h.announce(validBody({ port: 24801 }));
    expect(res.status).toBe(200);
    expect(await json(res)).toEqual({ status: 'ok', key: '8.8.8.8:24801', created: true, ttlSeconds: 90, heartbeatSeconds: 30 });
    expect(Object.keys(await listed(h))).toEqual(['8.8.8.8:24801']);
  });

  it('ignores any address the client claims in the body', async () => {
    const h = makeHarness();
    await h.announce(validBody({ ip: '1.2.3.4', host: '5.6.7.8' }));
    expect(Object.keys(await listed(h))).toEqual(['8.8.8.8:24800']);
  });

  it('a repeat announce refreshes the entry instead of duplicating it', async () => {
    const h = makeHarness();
    expect((await json(await h.announce(validBody({ players: 1 })))).created).toBe(true);
    const second = await json(await h.announce(validBody({ players: 7, name: 'Renamed' })));
    expect(second.created).toBe(false);
    const list = await listed(h);
    expect(Object.keys(list)).toHaveLength(1);
    expect(list['8.8.8.8:24800']).toMatchObject({ players: 7, name: 'Renamed' });
  });

  it('answers 415 unless the body is declared application/json', async () => {
    const h = makeHarness();
    for (const contentType of ['text/plain', 'application/x-www-form-urlencoded', 'application/jsonp', null]) {
      const res = await h.announce(validBody(), { contentType });
      expect(res.status, String(contentType)).toBe(415);
    }
    expect((await h.announce(validBody(), { contentType: 'application/json; charset=utf-8' })).status).toBe(200);
  });

  it('answers 413 for a body over 4096 bytes, with or without a Content-Length header', async () => {
    const h = makeHarness();
    const big = JSON.stringify(validBody({ name: 'x'.repeat(5000) }));
    expect((await h.announce(undefined, { raw: big })).status).toBe(413);
    const declared = await h.announce(undefined, { raw: '{}', headers: { 'content-length': '100000' } });
    expect(declared.status).toBe(413);
    expect(await listed(h)).toEqual({});
  });
});

describe('TTL expiry', () => {
  it('drops an entry exactly TTL_SECONDS after its last heartbeat', async () => {
    const h = makeHarness();
    await h.announce(validBody());
    vi.advanceTimersByTime(89_999);
    expect(Object.keys(await listed(h))).toHaveLength(1);
    vi.advanceTimersByTime(1);
    expect(await listed(h)).toEqual({});
  });

  it('a heartbeat restarts the clock', async () => {
    const h = makeHarness();
    await h.announce(validBody());
    vi.advanceTimersByTime(60_000);
    await h.announce(validBody());
    vi.advanceTimersByTime(89_999);
    expect(Object.keys(await listed(h))).toHaveLength(1);
    vi.advanceTimersByTime(1);
    expect(await listed(h)).toEqual({});
  });

  it('honours TTL_SECONDS', async () => {
    const h = makeHarness({ TTL_SECONDS: '30' });
    const res = await json(await h.announce(validBody()));
    expect(res).toMatchObject({ ttlSeconds: 30, heartbeatSeconds: 10 });
    vi.advanceTimersByTime(29_999);
    expect(Object.keys(await listed(h))).toHaveLength(1);
    vi.advanceTimersByTime(1);
    expect(await listed(h)).toEqual({});
  });

  it('an expired server that announces again is listed as new', async () => {
    const h = makeHarness();
    await h.announce(validBody());
    vi.advanceTimersByTime(120_000);
    expect((await json(await h.announce(validBody()))).created).toBe(true);
  });
});

describe('per-address entry cap', () => {
  const ports = (n: number): number[] => Array.from({ length: n }, (_, i) => 24800 + i);

  it('allows 8 servers per address by default and refuses the 9th', async () => {
    const h = makeHarness();
    for (const port of ports(8)) expect((await h.announce(validBody({ port }))).status).toBe(200);
    const ninth = await h.announce(validBody({ port: 24808 }));
    expect(ninth.status).toBe(403);
    expect((await json(ninth)).error).toBe('too_many_servers');
    expect(Object.keys(await listed(h))).toHaveLength(8);
  });

  it('still accepts heartbeats from the 8 listed servers, and other addresses', async () => {
    const h = makeHarness();
    for (const port of ports(8)) await h.announce(validBody({ port }));
    expect((await h.announce(validBody({ port: 24803, players: 9 }))).status).toBe(200);
    expect((await h.announce(validBody({ port: 24808 }), { ip: IP_B })).status).toBe(200);
  });

  it('honours MAX_PER_IP', async () => {
    const h = makeHarness({ MAX_PER_IP: '2' });
    expect((await h.announce(validBody({ port: 1 }))).status).toBe(200);
    expect((await h.announce(validBody({ port: 2 }))).status).toBe(200);
    expect((await h.announce(validBody({ port: 3 }))).status).toBe(403);
  });

  it('frees a slot when an entry expires', async () => {
    const h = makeHarness({ MAX_PER_IP: '1' });
    await h.announce(validBody({ port: 1 }));
    expect((await h.announce(validBody({ port: 2 }))).status).toBe(403);
    vi.advanceTimersByTime(90_000);
    expect((await h.announce(validBody({ port: 2 }))).status).toBe(200);
  });

  it('counts an IPv6 /64 as one address', async () => {
    const h = makeHarness({ MAX_PER_IP: '2' });
    expect((await h.announce(validBody({ port: 1 }), { ip: '2606:4700:4700::1' })).status).toBe(200);
    expect((await h.announce(validBody({ port: 1 }), { ip: '2606:4700:4700::2' })).status).toBe(200);
    expect((await h.announce(validBody({ port: 1 }), { ip: '2606:4700:4700::3' })).status).toBe(403);
    expect((await h.announce(validBody({ port: 1 }), { ip: '2606:4700:4700:1::1' })).status).toBe(200);
  });

  it('refuses new entries once MAX_ENTRIES is reached', async () => {
    const h = makeHarness({ MAX_ENTRIES: '2' });
    expect((await h.announce(validBody(), { ip: IP_A })).status).toBe(200);
    expect((await h.announce(validBody(), { ip: IP_B })).status).toBe(200);
    const res = await h.announce(validBody(), { ip: IP_C });
    expect(res.status).toBe(503);
    expect((await json(res)).error).toBe('server_list_full');
    expect(res.headers.get('retry-after')).toBe('30');
  });
});

describe('per-address rate limit', () => {
  const limited = (): ReturnType<typeof makeHarness> => makeHarness({ RATE_LIMIT_BURST: '3', RATE_LIMIT_PER_MINUTE: '60' });

  it('allows the burst, then answers 429 with Retry-After', async () => {
    const h = limited();
    for (let i = 0; i < 3; i++) expect((await h.announce(validBody())).status).toBe(200);
    const res = await h.announce(validBody());
    expect(res.status).toBe(429);
    expect(res.headers.get('retry-after')).toBe('1');
    expect(await json(res)).toMatchObject({ error: 'rate_limited', retryAfterSeconds: 1 });
  });

  it('refills at RATE_LIMIT_PER_MINUTE', async () => {
    const h = limited();
    for (let i = 0; i < 3; i++) await h.announce(validBody());
    expect((await h.announce(validBody())).status).toBe(429);
    vi.advanceTimersByTime(1000);
    expect((await h.announce(validBody())).status).toBe(200);
    expect((await h.announce(validBody())).status).toBe(429);
  });

  it('limits each address on its own', async () => {
    const h = limited();
    for (let i = 0; i < 3; i++) await h.announce(validBody());
    expect((await h.announce(validBody())).status).toBe(429);
    expect((await h.announce(validBody(), { ip: IP_B })).status).toBe(200);
  });

  it('charges malformed requests too, including ones refused before the body is read', async () => {
    const h = limited();
    for (let i = 0; i < 2; i++) expect((await h.announce(undefined, { raw: 'nope' })).status).toBe(400);
    expect((await h.announce(validBody(), { contentType: 'text/plain' })).status).toBe(415);
    expect((await h.announce(validBody())).status).toBe(429);
  });

  it('limits an IPv6 /64 as one address', async () => {
    const h = limited();
    for (let i = 1; i <= 3; i++) expect((await h.announce(validBody(), { ip: `2606:4700:4700::${i}` })).status).toBe(200);
    expect((await h.announce(validBody(), { ip: '2606:4700:4700::9' })).status).toBe(429);
  });
});

describe('private and loopback addresses', () => {
  const PRIVATE = ['127.0.0.1', '10.1.2.3', '192.168.1.20', '172.16.0.9', '169.254.10.10', '100.64.0.5', '0.0.0.0', '::1', 'fd00::1', 'fe80::1', '::ffff:10.0.0.1', '::ffff:127.0.0.1'];

  it.each(PRIVATE)('rejects an announce from %s', async (ip) => {
    const h = makeHarness();
    const res = await h.announce(validBody(), { ip });
    expect(res.status).toBe(403);
    expect((await json(res)).error).toBe('private_address');
    expect(await listed(h)).toEqual({});
  });

  it('accepts them all with ALLOW_PRIVATE=1', async () => {
    const h = makeHarness({ ALLOW_PRIVATE: '1', MAX_PER_IP: '50' });
    for (const ip of PRIVATE) expect((await h.announce(validBody(), { ip })).status, ip).toBe(200);
    expect(await listed(h)).toHaveProperty(['127.0.0.1:24800']);
  });

  it('accepts a public address, including one reported as IPv4-mapped IPv6', async () => {
    const h = makeHarness();
    expect((await h.announce(validBody(), { ip: IP_A })).status).toBe(200);
    expect((await h.announce(validBody(), { ip: `::ffff:${IP_B}` })).status).toBe(200);
    expect(Object.keys(await listed(h)).sort()).toEqual([`${IP_B}:24800`, `${IP_A}:24800`].sort());
  });

  it('lists an IPv6 server with a bracketed key', async () => {
    const h = makeHarness();
    expect((await h.announce(validBody(), { ip: IP_V6 })).status).toBe(200);
    expect(Object.keys(await listed(h))).toEqual([`[${IP_V6}]:24800`]);
  });
});

describe('TRUST_PROXY', () => {
  const xff = (value: string): Record<string, string> => ({ 'x-forwarded-for': value });

  it('ignores X-Forwarded-For when off', async () => {
    const h = makeHarness();
    await h.announce(validBody(), { ip: IP_A, headers: xff(IP_B) });
    expect(Object.keys(await listed(h))).toEqual([`${IP_A}:24800`]);
  });

  it('is not fooled by X-Forwarded-For when off: a loopback peer is still rejected', async () => {
    const h = makeHarness();
    const res = await h.announce(validBody(), { ip: '127.0.0.1', headers: xff(IP_B) });
    expect(res.status).toBe(403);
  });

  it('uses the first hop of X-Forwarded-For when on, though the TCP peer is loopback', async () => {
    const h = makeHarness({ TRUST_PROXY: '1' });
    const res = await h.announce(validBody(), { ip: '127.0.0.1', headers: xff(`${IP_C}, 10.0.0.1, 127.0.0.1`) });
    expect(res.status).toBe(200);
    expect(Object.keys(await listed(h))).toEqual([`${IP_C}:24800`]);
  });

  it('takes an IPv6 client from the header too', async () => {
    const h = makeHarness({ TRUST_PROXY: '1' });
    await h.announce(validBody(), { ip: '127.0.0.1', headers: xff(IP_V6) });
    expect(Object.keys(await listed(h))).toEqual([`[${IP_V6}]:24800`]);
  });

  it.each([['missing', undefined], ['not an address', 'unknown'], ['a hostname', 'example.org'], ['empty', '']])('answers 400 when the header is %s', async (_label, value) => {
    const h = makeHarness({ TRUST_PROXY: '1' });
    const res = await h.announce(validBody(), { ip: '127.0.0.1', headers: value === undefined ? {} : xff(value) });
    expect(res.status).toBe(400);
    expect((await json(res)).error).toBe('bad_forwarded_for');
    expect(await listed(h)).toEqual({});
    expect(h.logs.entries().some((e) => e.level === 'error' && String(e.msg).includes('X-Forwarded-For'))).toBe(true);
  });

  it('still rejects a private forwarded client', async () => {
    const h = makeHarness({ TRUST_PROXY: '1' });
    const res = await h.announce(validBody(), { ip: '127.0.0.1', headers: xff('10.0.0.7') });
    expect(res.status).toBe(403);
  });

  it('rate-limits and caps per forwarded client, not per proxy', async () => {
    const h = makeHarness({ TRUST_PROXY: '1', RATE_LIMIT_BURST: '1', RATE_LIMIT_PER_MINUTE: '1' });
    expect((await h.announce(validBody(), { ip: '127.0.0.1', headers: xff(IP_A) })).status).toBe(200);
    expect((await h.announce(validBody(), { ip: '127.0.0.1', headers: xff(IP_A) })).status).toBe(429);
    expect((await h.announce(validBody(), { ip: '127.0.0.1', headers: xff(IP_B) })).status).toBe(200);
  });
});

describe('GET /servers and /healthz', () => {
  it('serves a short-lived cacheable JSON object', async () => {
    const h = makeHarness();
    await h.announce(validBody());
    const res = await h.get('/servers');
    expect(res.status).toBe(200);
    expect(res.headers.get('cache-control')).toBe('public, max-age=10');
    expect(res.headers.get('content-type')).toContain('application/json');
  });

  it('is an empty object when nothing is listed', async () => {
    const h = makeHarness();
    expect(await listed(h)).toEqual({});
  });

  it('orders by player count, busiest first', async () => {
    const h = makeHarness();
    await h.announce(validBody({ players: 1 }), { ip: IP_A });
    await h.announce(validBody({ players: 9 }), { ip: IP_B });
    await h.announce(validBody({ players: 5 }), { ip: IP_C });
    expect(Object.keys(await listed(h))).toEqual([`${IP_B}:24800`, `${IP_C}:24800`, `${IP_A}:24800`]);
  });

  it('/healthz reports ok and the live count', async () => {
    const h = makeHarness();
    expect(await json(await h.get('/healthz'))).toMatchObject({ status: 'ok', servers: 0 });
    await h.announce(validBody());
    const res = await h.get('/healthz');
    expect(res.headers.get('cache-control')).toBe('no-store');
    expect(await json(res)).toMatchObject({ status: 'ok', servers: 1 });
  });

  it('/healthz answers 503 when the store cannot be read', async () => {
    const failing = {
      upsert: () => Promise.reject(new Error('db down')),
      has: () => Promise.reject(new Error('db down')),
      list: () => Promise.reject(new Error('db down')),
      sweep: () => Promise.reject(new Error('db down')),
    };
    const h = makeHarness({}, { store: failing });
    const res = await h.get('/healthz');
    expect(res.status).toBe(503);
    expect(await json(res)).toEqual({ status: 'error' });
    expect(h.logs.entries().some((e) => e.level === 'error')).toBe(true);
  });

  it('answers unknown paths with a JSON 404 and unexpected failures with a logged JSON 500', async () => {
    const failing = {
      upsert: () => Promise.reject(new Error('db down')),
      has: () => Promise.reject(new Error('db down')),
      list: () => Promise.reject(new Error('db down')),
      sweep: () => Promise.reject(new Error('db down')),
    };
    const h = makeHarness({}, { store: failing });
    const missing = await h.get('/nope');
    expect(missing.status).toBe(404);
    expect((await json(missing)).error).toBe('not_found');
    const broken = await h.get('/servers');
    expect(broken.status).toBe(500);
    expect(await json(broken)).toEqual({ error: 'internal', message: 'internal error' });
    expect(h.logs.entries().some((e) => e.level === 'error' && e.msg === 'unhandled error' && e.err === 'db down')).toBe(true);
  });
});
