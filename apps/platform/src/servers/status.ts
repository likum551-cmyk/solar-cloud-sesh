import { lookup } from 'node:dns/promises';
import type { Config } from '../config.js';
import { normalizeIp } from '../util/ip.js';
import type { Logger } from '../util/logger.js';
import { probeServer, type ProbeResult } from './probe.js';

/** Live state of one official server as the launcher shows it. */
export interface ServerStatus {
  /** online: answered the UDP query. offline: probe failed (timeout, bad reply, DNS). unknown: no probe has finished yet. */
  state: 'online' | 'offline' | 'unknown';
  players: number | null;
  maxPlayers: number | null;
  /** UDP round trip in milliseconds (rounded). */
  pingMs: number | null;
  /** Game protocol version the server reported. */
  protocol: number | null;
  checkedAt: Date | null;
  /** Why the last probe failed, for operators (not secret). */
  detail?: string;
}

export interface StatusTarget {
  id: string;
  host: string;
  port: number;
}

export interface ServerStatusService {
  /**
   * Status of each target. Never waits on a slow server: values younger than the cache TTL are
   * returned as they are, older ones are returned immediately while a refresh runs in the
   * background, and a target that has never been probed gets at most `serverStatusWaitMs` for
   * its first probe before it is reported `unknown`.
   */
  statuses(targets: StatusTarget[]): Promise<Map<string, ServerStatus>>;
}

export type Prober = (ip: string, port: number) => Promise<ProbeResult>;

const UNKNOWN: ServerStatus = { state: 'unknown', players: null, maxPlayers: null, pingMs: null, protocol: null, checkedAt: null };

interface Entry {
  status: ServerStatus | null;
  /** Epoch ms of the last finished probe. */
  at: number;
  inflight: Promise<void> | null;
}

export interface UdpStatusOptions {
  ttlMs: number;
  waitMs: number;
  /** Cache of DNS answers for host names, ms. */
  dnsTtlMs?: number;
  prober: Prober;
  /** Resolve a host name to an IP; defaults to dns.lookup. Tests inject one. */
  resolve?: (host: string) => Promise<string>;
  clock: () => Date;
  logger: Logger;
}

/** Build a Prober that sends the real GAGE UDP query with the configured timeout and attempts. */
export function udpProber(config: Pick<Config, 'probeTimeoutMs' | 'probeAttempts'>): Prober {
  return (ip, port) => probeServer(ip, port, { timeoutMs: config.probeTimeoutMs, attempts: config.probeAttempts });
}

/**
 * Cached UDP status provider (apps/master/PROTOCOL.md query). One probe per target at a time (single
 * flight), results cached for `ttlMs` (default 5 s), so request volume to /v1/servers never turns
 * into UDP volume towards the game servers.
 */
export class UdpServerStatusService implements ServerStatusService {
  private readonly entries = new Map<string, Entry>();
  private readonly dnsCache = new Map<string, { ip: string; at: number }>();

  constructor(private readonly opts: UdpStatusOptions) {}

  async statuses(targets: StatusTarget[]): Promise<Map<string, ServerStatus>> {
    const nowMs = this.opts.clock().getTime();
    const cold: Promise<void>[] = [];

    for (const t of targets) {
      const key = `${t.host}:${t.port}`;
      let entry = this.entries.get(key);
      if (!entry) {
        entry = { status: null, at: 0, inflight: null };
        this.entries.set(key, entry);
      }
      const fresh = entry.status !== null && nowMs - entry.at < this.opts.ttlMs;
      if (!fresh && entry.inflight === null) entry.inflight = this.refresh(entry, t);
      if (entry.status === null && entry.inflight !== null) cold.push(entry.inflight);
    }

    if (cold.length > 0 && this.opts.waitMs > 0) {
      let timer: NodeJS.Timeout | undefined;
      const deadline = new Promise<void>((resolve) => {
        timer = setTimeout(resolve, this.opts.waitMs);
      });
      await Promise.race([Promise.all(cold).then(() => undefined), deadline]);
      clearTimeout(timer);
    }

    const out = new Map<string, ServerStatus>();
    for (const t of targets) out.set(t.id, this.entries.get(`${t.host}:${t.port}`)?.status ?? UNKNOWN);
    return out;
  }

  private async refresh(entry: Entry, t: StatusTarget): Promise<void> {
    try {
      entry.status = await this.probe(t);
    } catch (err) {
      // probe() reports failures as values; this is a bug, not a network condition.
      this.opts.logger.error('status probe crashed', { server: t.id, err: (err as Error).message });
      entry.status = { ...UNKNOWN, state: 'offline', checkedAt: this.opts.clock(), detail: 'internal_error' };
    } finally {
      entry.at = this.opts.clock().getTime();
      entry.inflight = null;
    }
  }

  private async probe(t: StatusTarget): Promise<ServerStatus> {
    const checkedAt = this.opts.clock();
    let ip: string;
    try {
      ip = await this.resolveHost(t.host);
    } catch {
      return { state: 'offline', players: null, maxPlayers: null, pingMs: null, protocol: null, checkedAt, detail: 'dns' };
    }
    const r = await this.opts.prober(ip, t.port);
    if (r.ok) {
      return {
        state: 'online',
        players: r.players,
        maxPlayers: r.maxPlayers,
        pingMs: Math.max(0, Math.round(r.rttMs)),
        protocol: r.protocol,
        checkedAt,
      };
    }
    return { state: 'offline', players: null, maxPlayers: null, pingMs: null, protocol: null, checkedAt, detail: r.reason };
  }

  private async resolveHost(host: string): Promise<string> {
    const literal = normalizeIp(host);
    if (literal !== null) return literal;
    const now = this.opts.clock().getTime();
    const cached = this.dnsCache.get(host);
    if (cached && now - cached.at < (this.opts.dnsTtlMs ?? 60_000)) return cached.ip;
    const ip = this.opts.resolve ? await this.opts.resolve(host) : (await lookup(host)).address;
    const normalized = normalizeIp(ip);
    if (normalized === null) throw new Error('resolver returned a non-IP');
    this.dnsCache.set(host, { ip: normalized, at: now });
    return normalized;
  }
}
