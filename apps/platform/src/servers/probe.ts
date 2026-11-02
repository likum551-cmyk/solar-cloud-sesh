// Ported from apps/master/src/probe.ts (the GAGE UDP query, apps/master/PROTOCOL.md). Keep the wire format in sync with query-protocol.ts.
import { createSocket } from 'node:dgram';
import { randomBytes } from 'node:crypto';
import { performance } from 'node:perf_hooks';
import { normalizeIp } from '../util/ip.js';
import {
  QUERY_NONCE_SIZE,
  decodeQueryReply,
  encodeQueryRequest,
  type QueryReply,
} from './query-protocol.js';

export type ProbeResult =
  | ({ ok: true; rttMs: number } & QueryReply)
  | { ok: false; reason: 'timeout' | 'bad-reply' | 'send-error'; detail?: string };

export interface ProbeOptions {
  /** Overall deadline from the first send. */
  timeoutMs: number;
  /** How many times the request is sent (evenly spaced inside the deadline); UDP may drop one. */
  attempts: number;
  /** Test seam; defaults to crypto.randomBytes. */
  randomBytes?: (size: number) => Buffer;
}

export type Prober = (ip: string, port: number) => Promise<ProbeResult>;

/**
 * Send the GAGE UDP query to ip:port and wait for a valid reply.
 *   ok          a reply from exactly that address and port that echoes our nonce
 *   bad-reply   a datagram from that address and port that is not a valid reply (wrong
 *               magic, size, kind or nonce): something answers there, but it is not a GAGE server
 *   timeout     nothing valid arrived before the deadline (also what a closed port looks like)
 *   send-error  the socket could not be opened or written
 * Datagrams from any other source are ignored.
 */
export function probeServer(ip: string, port: number, opts: ProbeOptions): Promise<ProbeResult> {
  return new Promise<ProbeResult>((resolve) => {
    const target = normalizeIp(ip);
    if (target === null) {
      resolve({ ok: false, reason: 'send-error', detail: 'target is not an IP address' });
      return;
    }
    const nonce = (opts.randomBytes ?? randomBytes)(QUERY_NONCE_SIZE);
    const request = encodeQueryRequest(nonce);
    const socket = createSocket(target.includes(':') ? 'udp6' : 'udp4');
    const timers: NodeJS.Timeout[] = [];
    let firstSentAt: number | null = null;
    let settled = false;

    const finish = (result: ProbeResult): void => {
      if (settled) return;
      settled = true;
      for (const t of timers) clearTimeout(t);
      socket.close();
      resolve(result);
    };

    socket.on('error', (err) => finish({ ok: false, reason: 'send-error', detail: err.message }));

    socket.on('message', (msg, rinfo) => {
      if (rinfo.port !== port || normalizeIp(rinfo.address) !== target) return;
      const reply = decodeQueryReply(msg, nonce);
      if (reply === null) {
        finish({ ok: false, reason: 'bad-reply' });
        return;
      }
      finish({ ok: true, rttMs: performance.now() - (firstSentAt ?? performance.now()), ...reply });
    });

    const send = (): void => {
      if (settled) return;
      firstSentAt ??= performance.now();
      socket.send(request, port, target, (err) => {
        if (err) finish({ ok: false, reason: 'send-error', detail: err.message });
      });
    };

    const spacing = opts.timeoutMs / opts.attempts;
    for (let i = 0; i < opts.attempts; i++) timers.push(setTimeout(send, i * spacing));
    timers.push(setTimeout(() => finish({ ok: false, reason: 'timeout' }), opts.timeoutMs));
  });
}
