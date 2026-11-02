import { createServer } from 'node:net';
import { afterEach, describe, expect, it } from 'vitest';
import { loadConfig } from '../src/config.js';
import { startServer, type RunningServer } from '../src/server.js';
import { memoryLogger, validBody } from './helpers.js';

// Real HTTP over loopback: this is the only place getConnInfo reads a genuine socket address.
let running: RunningServer[] = [];

async function start(env: Record<string, string>): Promise<{ server: RunningServer; base: string; logs: ReturnType<typeof memoryLogger> }> {
  const logs = memoryLogger();
  const config = loadConfig({ HOST: '127.0.0.1', PORT: '0', ...env });
  const server = await startServer(config, logs.logger);
  running.push(server);
  return { server, base: `http://127.0.0.1:${server.address.port}`, logs };
}

afterEach(async () => {
  await Promise.all(running.map((s) => s.close()));
  running = [];
});

const post = (base: string, body: unknown, headers: Record<string, string> = {}): Promise<Response> =>
  fetch(`${base}/announce`, { method: 'POST', headers: { 'content-type': 'application/json', ...headers }, body: JSON.stringify(body) });

describe('the HTTP server', () => {
  it('takes the announce address from the real TCP connection', async () => {
    const { base } = await start({ ALLOW_PRIVATE: '1' });
    const res = await post(base, validBody({ port: 24850 }));
    expect(res.status).toBe(200);
    const list = (await (await fetch(`${base}/servers`)).json()) as Record<string, unknown>;
    expect(Object.keys(list)).toEqual(['127.0.0.1:24850']);
  });

  it('rejects the loopback peer by default', async () => {
    const { base } = await start({});
    expect((await post(base, validBody())).status).toBe(403);
  });

  it('honours X-Forwarded-For over a real connection only when TRUST_PROXY=1', async () => {
    const trusting = await start({ TRUST_PROXY: '1' });
    expect((await post(trusting.base, validBody(), { 'x-forwarded-for': '8.8.8.8' })).status).toBe(200);
    const list = (await (await fetch(`${trusting.base}/servers`)).json()) as Record<string, unknown>;
    expect(Object.keys(list)).toEqual(['8.8.8.8:24800']);

    const plain = await start({});
    expect((await post(plain.base, validBody(), { 'x-forwarded-for': '8.8.8.8' })).status).toBe(403);
  });

  it('serves /healthz', async () => {
    const { base } = await start({});
    const res = await fetch(`${base}/healthz`);
    expect(res.status).toBe(200);
    expect(await res.json()).toMatchObject({ status: 'ok', servers: 0 });
  });

  it('logs structured JSON lines', async () => {
    const { logs } = await start({});
    const listening = logs.entries().find((e) => e.msg === 'listening');
    expect(listening).toMatchObject({ level: 'info', host: '127.0.0.1' });
    expect(typeof listening?.ts).toBe('string');
    expect(logs.entries().some((e) => e.level === 'warn' && String(e.msg).startsWith('VERIFY_UDP=0'))).toBe(true);
  });

  it('shuts down gracefully: stops listening, resolves, and is idempotent', async () => {
    const { server, base } = await start({});
    expect((await fetch(`${base}/healthz`)).status).toBe(200);
    const first = server.close();
    const second = server.close();
    expect(second).toBe(first);
    await first;
    await expect(fetch(`${base}/healthz`)).rejects.toThrow();
  });

  it('fails to start, with the cause, when the port is taken', async () => {
    const blocker = createServer();
    await new Promise<void>((resolve) => blocker.listen(0, '127.0.0.1', resolve));
    const address = blocker.address();
    const port = typeof address === 'object' && address ? address.port : 0;
    try {
      const config = loadConfig({ HOST: '127.0.0.1', PORT: String(port) });
      await expect(startServer(config, memoryLogger().logger)).rejects.toMatchObject({ code: 'EADDRINUSE' });
    } finally {
      await new Promise<void>((resolve) => blocker.close(() => resolve()));
    }
  });
});
