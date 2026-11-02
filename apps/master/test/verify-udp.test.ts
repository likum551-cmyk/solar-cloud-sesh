import { afterEach, describe, expect, it } from 'vitest';
import { encodeQueryReply } from '../src/query-protocol.js';
import { json, makeHarness, startFakeResponder, validBody, type FakeResponder } from './helpers.js';

// The announcing "client" is 127.0.0.1 (ALLOW_PRIVATE=1) and the probe goes to the fake
// responder's real UDP port on loopback. No other host is ever contacted.
const VERIFY = { VERIFY_UDP: '1', ALLOW_PRIVATE: '1', PROBE_TIMEOUT_MS: '300', PROBE_ATTEMPTS: '1' };
const LOCAL = { ip: '127.0.0.1' };

let responders: FakeResponder[] = [];
async function responder(...args: Parameters<typeof startFakeResponder>): Promise<FakeResponder> {
  const r = await startFakeResponder(...args);
  responders.push(r);
  return r;
}
afterEach(async () => {
  await Promise.all(responders.map((r) => r.close()));
  responders = [];
});

describe('POST /announce with VERIFY_UDP=1', () => {
  it('lists a server that answers the query', async () => {
    const r = await responder('good', { protocol: 1 });
    const h = makeHarness(VERIFY);
    const res = await h.announce(validBody({ port: r.port }), LOCAL);
    expect(res.status).toBe(200);
    expect(r.received).toHaveLength(1);
    expect(Object.keys(await json(await h.get('/servers')))).toEqual([`127.0.0.1:${r.port}`]);
  });

  it('refuses with 422 unreachable when nothing answers, and lists nothing', async () => {
    const r = await responder('silent');
    const h = makeHarness(VERIFY);
    const res = await h.announce(validBody({ port: r.port }), LOCAL);
    expect(res.status).toBe(422);
    expect(await json(res)).toMatchObject({ error: 'unreachable', probe: 'timeout' });
    expect(await json(await h.get('/servers'))).toEqual({});
  });

  it('refuses with 422 unreachable when the answer is not a valid reply', async () => {
    const r = await responder('wrong-nonce');
    const h = makeHarness(VERIFY);
    const res = await h.announce(validBody({ port: r.port }), LOCAL);
    expect(res.status).toBe(422);
    expect(await json(res)).toMatchObject({ error: 'unreachable', probe: 'bad-reply' });
    expect(await json(await h.get('/servers'))).toEqual({});
  });

  it('refuses with 422 protocol_mismatch when the server answers with another protocol than it announced', async () => {
    const r = await responder('good', { protocol: 2 });
    const h = makeHarness(VERIFY);
    const res = await h.announce(validBody({ port: r.port, protocol: 1 }), LOCAL);
    expect(res.status).toBe(422);
    expect(await json(res)).toMatchObject({ error: 'protocol_mismatch', announced: 1, replied: 2 });
    expect(await json(await h.get('/servers'))).toEqual({});
  });

  it('does not probe a heartbeat for an entry that is already listed', async () => {
    const r = await responder('good');
    const h = makeHarness(VERIFY);
    expect((await h.announce(validBody({ port: r.port }), LOCAL)).status).toBe(200);
    expect(r.received).toHaveLength(1);
    const again = await h.announce(validBody({ port: r.port, players: 9 }), LOCAL);
    expect(again.status).toBe(200);
    expect((await json(again)).created).toBe(false);
    expect(r.received).toHaveLength(1);
  });

  it('sends one probe for concurrent first announces of the same address', async () => {
    const r = await responder('good');
    const h = makeHarness(VERIFY);
    const [a, b] = await Promise.all([h.announce(validBody({ port: r.port }), LOCAL), h.announce(validBody({ port: r.port }), LOCAL)]);
    expect([a.status, b.status]).toEqual([200, 200]);
    expect(r.received).toHaveLength(1);
  });

  it('answers 503 probe_busy when MAX_INFLIGHT_PROBES probes are already running', async () => {
    const slow = await responder(async (_req, nonce) => {
      await new Promise((resolve) => setTimeout(resolve, 150));
      return encodeQueryReply(nonce, { protocol: 1, players: 0, maxPlayers: 8 });
    });
    const other = await responder('good');
    const h = makeHarness({ ...VERIFY, MAX_INFLIGHT_PROBES: '1', PROBE_TIMEOUT_MS: '1000' });
    const first = h.announce(validBody({ port: slow.port }), LOCAL);
    await new Promise((resolve) => setTimeout(resolve, 30)); // let the first probe start
    const busy = await h.announce(validBody({ port: other.port }), LOCAL);
    expect(busy.status).toBe(503);
    expect(await json(busy)).toMatchObject({ error: 'probe_busy' });
    expect(busy.headers.get('retry-after')).toBe('5');
    expect(other.received).toHaveLength(0);
    expect((await first).status).toBe(200);
  });

  it('with VERIFY_UDP=0 (the default) lists a server that would not answer, and sends no datagram', async () => {
    const r = await responder('silent');
    const h = makeHarness({ ALLOW_PRIVATE: '1' });
    expect((await h.announce(validBody({ port: r.port }), LOCAL)).status).toBe(200);
    expect(r.received).toHaveLength(0);
  });
});
