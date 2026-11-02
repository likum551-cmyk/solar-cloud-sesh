import { createSocket, type Socket } from 'node:dgram';
import type { HttpBindings } from '@hono/node-server';
import type { Hono } from 'hono';
import type { AppEnv } from '../src/app.js';
import { loadConfig, type Config } from '../src/config.js';
import { createLogger, type LogLevel } from '../src/logger.js';
import {
  decodeQueryRequest,
  encodeQueryReply,
  type QueryReply,
} from '../src/query-protocol.js';
import { createService, type Service, type ServiceOverrides } from '../src/server.js';

// Public addresses used as announce sources. Nothing ever connects to them: they only have to
// pass the "is this a routable address" check.
export const IP_A = '8.8.8.8';
export const IP_B = '1.1.1.1';
export const IP_C = '9.9.9.9';
export const IP_V6 = '2606:4700:4700::1111';

export function memoryLogger(level: LogLevel = 'debug') {
  const lines: string[] = [];
  const logger = createLogger({ level, write: (l) => void lines.push(l) });
  return {
    logger,
    lines,
    entries: (): Array<Record<string, unknown>> => lines.map((l) => JSON.parse(l) as Record<string, unknown>),
  };
}

export interface Harness extends Service {
  config: Config;
  logs: ReturnType<typeof memoryLogger>;
  /** POST /announce as if the TCP peer were `ip`. */
  announce(body: unknown, opts?: AnnounceOpts): Promise<Response>;
  get(path: string, ip?: string): Promise<Response>;
}

export interface AnnounceOpts {
  ip?: string;
  headers?: Record<string, string>;
  /** Send this text instead of JSON.stringify(body). */
  raw?: string;
  /** null = no Content-Type header at all. */
  contentType?: string | null;
}

/** The Node adapter's connection bindings, as far as getConnInfo reads them. */
export function envFor(remoteAddress: string): HttpBindings {
  return {
    incoming: {
      socket: {
        remoteAddress,
        remotePort: 40_000,
        remoteFamily: remoteAddress.includes(':') ? 'IPv6' : 'IPv4',
      },
    },
  } as unknown as HttpBindings;
}

export function validBody(over: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    port: 24800,
    name: 'GAGE Test Server',
    gamemode: 'freeroam',
    lang: 'en',
    url: 'https://example.org/gage',
    players: 3,
    maxPlayers: 32,
    protocol: 1,
    ...over,
  };
}

/**
 * The full service on an in-memory store, driven through app.request (no HTTP socket).
 * `env` takes the same variables as the real process. Rate limiting is effectively off
 * unless a test sets it.
 */
export function makeHarness(env: Record<string, string> = {}, overrides: ServiceOverrides = {}): Harness {
  const config = loadConfig({ RATE_LIMIT_BURST: '1000', RATE_LIMIT_PER_MINUTE: '100000', ...env });
  const logs = memoryLogger();
  const service = createService(config, logs.logger, overrides);
  const app: Hono<AppEnv> = service.app;
  return {
    ...service,
    config,
    logs,
    announce(body, opts = {}) {
      const headers: Record<string, string> = { ...(opts.headers ?? {}) };
      if (opts.contentType !== null) headers['content-type'] = opts.contentType ?? 'application/json';
      return Promise.resolve(
        app.request(
          '/announce',
          { method: 'POST', headers, body: opts.raw ?? JSON.stringify(body) },
          envFor(opts.ip ?? IP_A),
        ),
      );
    },
    get(path, ip = IP_A) {
      return Promise.resolve(app.request(path, { method: 'GET' }, envFor(ip)));
    },
  };
}

export async function json(res: Response): Promise<any> {
  return (await res.json()) as unknown;
}

// ---- UDP: a fake GAGE game server ----------------------------------------------------------

export type ResponderBehavior =
  | 'good'
  | 'wrong-nonce'
  | 'bad-magic'
  | 'short'
  | 'silent'
  /** Build the reply yourself (return null to stay silent); may be async to delay it. */
  | ((request: Buffer, nonce: Buffer, from: { address: string; port: number }, socket: Socket) => Buffer | null | Promise<Buffer | null>);

export interface FakeResponder {
  port: number;
  /** Every datagram received, valid request or not. */
  received: Buffer[];
  close(): Promise<void>;
}

/** A UDP socket on 127.0.0.1 that answers the GAGE query the way the C++ server is meant to. */
export async function startFakeResponder(
  behavior: ResponderBehavior = 'good',
  reply: Partial<QueryReply> = {},
): Promise<FakeResponder> {
  const socket = createSocket('udp4');
  const received: Buffer[] = [];
  const fields: QueryReply = { protocol: 1, players: 3, maxPlayers: 32, ...reply };

  socket.on('message', (msg, rinfo) => {
    received.push(Buffer.from(msg));
    const nonce = decodeQueryRequest(msg);
    if (nonce === null || behavior === 'silent') return; // a real server ignores what it cannot parse
    void (async () => {
      let out: Buffer | null;
      if (typeof behavior === 'function') {
        out = await behavior(msg, nonce, rinfo, socket);
      } else {
        out = encodeQueryReply(nonce, fields);
        if (behavior === 'wrong-nonce') out = encodeQueryReply(Buffer.from(nonce).fill(0xee), fields);
        if (behavior === 'bad-magic') out[0] = 0x00;
        if (behavior === 'short') out = out.subarray(0, 23);
      }
      if (out) socket.send(out, rinfo.port, rinfo.address);
    })();
  });

  await new Promise<void>((resolve, reject) => {
    socket.once('error', reject);
    socket.bind(0, '127.0.0.1', () => {
      socket.off('error', reject);
      resolve();
    });
  });
  const port = socket.address().port;
  return {
    port,
    received,
    close: () => new Promise<void>((resolve) => socket.close(() => resolve())),
  };
}
