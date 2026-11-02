import { createSocket } from 'node:dgram';
import { afterEach, describe, expect, it } from 'vitest';
import { probeServer } from '../src/probe.js';
import { decodeQueryRequest, encodeQueryReply } from '../src/query-protocol.js';
import { startFakeResponder, type FakeResponder } from './helpers.js';

const opts = { timeoutMs: 600, attempts: 1 };
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

describe('probeServer against a fake GAGE server', () => {
  it('succeeds on a valid reply and returns what the server reported', async () => {
    const r = await responder('good', { protocol: 1, players: 4, maxPlayers: 20 });
    const result = await probeServer('127.0.0.1', r.port, opts);
    expect(result).toMatchObject({ ok: true, protocol: 1, players: 4, maxPlayers: 20 });
    if (result.ok) expect(result.rttMs).toBeGreaterThanOrEqual(0);
    expect(r.received).toHaveLength(1);
    expect(decodeQueryRequest(r.received[0] ?? Buffer.alloc(0))).not.toBeNull(); // what was sent is a valid request
  });

  it('sends a fresh nonce every time', async () => {
    const r = await responder('good');
    await probeServer('127.0.0.1', r.port, opts);
    await probeServer('127.0.0.1', r.port, opts);
    const [a, b] = r.received.map((d) => decodeQueryRequest(d)?.toString('hex'));
    expect(a).toBeDefined();
    expect(b).toBeDefined();
    expect(a).not.toBe(b);
  });

  it.each(['wrong-nonce', 'bad-magic', 'short'] as const)('fails with bad-reply on a %s reply', async (behavior) => {
    const r = await responder(behavior);
    expect(await probeServer('127.0.0.1', r.port, opts)).toEqual({ ok: false, reason: 'bad-reply' });
  });

  it('fails with bad-reply when something other than a reply comes back (the request echoed)', async () => {
    const r = await responder((req) => req);
    expect(await probeServer('127.0.0.1', r.port, opts)).toEqual({ ok: false, reason: 'bad-reply' });
  });

  it('times out when nothing answers, after about timeoutMs', async () => {
    const r = await responder('silent');
    const t0 = Date.now();
    const result = await probeServer('127.0.0.1', r.port, { timeoutMs: 300, attempts: 1 });
    const elapsed = Date.now() - t0;
    expect(result).toEqual({ ok: false, reason: 'timeout' });
    expect(elapsed).toBeGreaterThanOrEqual(280);
    expect(elapsed).toBeLessThan(1500);
  });

  it('times out on a closed port', async () => {
    const r = await startFakeResponder('silent');
    const { port } = r;
    await r.close();
    expect(await probeServer('127.0.0.1', port, { timeoutMs: 300, attempts: 1 })).toEqual({ ok: false, reason: 'timeout' });
  });

  it('retransmits within the deadline and succeeds when only a later copy is answered', async () => {
    let seen = 0;
    const r = await responder((_req, nonce) => (++seen < 3 ? null : encodeQueryReply(nonce, { protocol: 1, players: 0, maxPlayers: 8 })));
    const result = await probeServer('127.0.0.1', r.port, { timeoutMs: 900, attempts: 3 });
    expect(result.ok).toBe(true);
    expect(r.received).toHaveLength(3);
  });

  it('ignores a valid-looking reply that comes from another port', async () => {
    const stranger = createSocket('udp4');
    const r = await responder((_req, nonce, from) => {
      stranger.send(encodeQueryReply(nonce, { protocol: 1, players: 0, maxPlayers: 8 }), from.port, from.address);
      return null; // the probed port itself stays silent
    });
    const result = await probeServer('127.0.0.1', r.port, { timeoutMs: 300, attempts: 1 });
    stranger.close();
    expect(result).toEqual({ ok: false, reason: 'timeout' });
  });

  it('refuses a target that is not an IP address', async () => {
    expect(await probeServer('localhost', 24800, opts)).toMatchObject({ ok: false, reason: 'send-error' });
  });
});
