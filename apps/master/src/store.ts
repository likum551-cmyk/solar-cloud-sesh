// The server list store. The interface is async and owns the invariants (per-address cap,
// total cap, expiry) so a Postgres implementation can enforce them in SQL (unique key, one
// conditional insert) instead of trusting callers to check first.

export interface ServerRecord {
  /** "ip:port" or "[ipv6]:port": unique, and the key of the public list. */
  key: string;
  ip: string;
  /** Identity for the per-address cap: the IPv4 address, or the IPv6 /64. */
  ipGroup: string;
  port: number;
  name: string;
  gamemode: string;
  lang: string;
  url: string | null;
  players: number;
  maxPlayers: number;
  protocol: number;
  firstSeen: number;
  lastSeen: number;
  expiresAt: number;
}

/** What a caller supplies; the store stamps the times. */
export type NewServerRecord = Omit<ServerRecord, 'firstSeen' | 'lastSeen' | 'expiresAt'>;

export interface StoreLimits {
  maxPerIp: number;
  maxEntries: number;
  ttlMs: number;
}

export type UpsertResult =
  | { ok: true; created: boolean; record: ServerRecord }
  | { ok: false; reason: 'ip-cap' | 'store-full' };

export interface ServerStore {
  /**
   * Insert a new entry or refresh an existing one (new fields, new expiry, firstSeen kept).
   * A new entry is refused when its address already holds `maxPerIp` live entries or the
   * store holds `maxEntries`; refreshing a live entry is never refused.
   */
  upsert(rec: NewServerRecord, limits: StoreLimits, now: number): Promise<UpsertResult>;
  /** True when a live (unexpired) entry exists for `key`. */
  has(key: string, now: number): Promise<boolean>;
  /** Live entries only. */
  list(now: number): Promise<ServerRecord[]>;
  /** Physically remove expired entries; returns how many. */
  sweep(now: number): Promise<number>;
}

export class MemoryStore implements ServerStore {
  private readonly records = new Map<string, ServerRecord>();
  private readonly byGroup = new Map<string, Set<string>>();

  /** Stored entries including expired ones the sweeper has not collected yet (for tests and metrics). */
  get size(): number {
    return this.records.size;
  }

  upsert(rec: NewServerRecord, limits: StoreLimits, now: number): Promise<UpsertResult> {
    return Promise.resolve(this.upsertSync(rec, limits, now));
  }

  has(key: string, now: number): Promise<boolean> {
    const r = this.records.get(key);
    return Promise.resolve(r !== undefined && r.expiresAt > now);
  }

  list(now: number): Promise<ServerRecord[]> {
    const out: ServerRecord[] = [];
    for (const r of this.records.values()) {
      if (r.expiresAt > now) out.push({ ...r });
    }
    return Promise.resolve(out);
  }

  sweep(now: number): Promise<number> {
    return Promise.resolve(this.sweepSync(now));
  }

  private upsertSync(rec: NewServerRecord, limits: StoreLimits, now: number): UpsertResult {
    const existing = this.records.get(rec.key);
    if (existing && existing.expiresAt > now) {
      const updated: ServerRecord = { ...rec, firstSeen: existing.firstSeen, lastSeen: now, expiresAt: now + limits.ttlMs };
      this.records.set(rec.key, updated);
      return { ok: true, created: false, record: { ...updated } };
    }

    // A new entry. First forget an expired one under the same key so it neither counts
    // toward the caps nor survives as stale state.
    if (existing) this.remove(existing);

    if (this.liveInGroup(rec.ipGroup, now) >= limits.maxPerIp) return { ok: false, reason: 'ip-cap' };
    if (this.records.size >= limits.maxEntries) this.sweepSync(now);
    if (this.records.size >= limits.maxEntries) return { ok: false, reason: 'store-full' };

    const created: ServerRecord = { ...rec, firstSeen: now, lastSeen: now, expiresAt: now + limits.ttlMs };
    this.records.set(rec.key, created);
    let group = this.byGroup.get(rec.ipGroup);
    if (!group) {
      group = new Set();
      this.byGroup.set(rec.ipGroup, group);
    }
    group.add(rec.key);
    return { ok: true, created: true, record: { ...created } };
  }

  private liveInGroup(group: string, now: number): number {
    const keys = this.byGroup.get(group);
    if (!keys) return 0;
    let live = 0;
    for (const key of keys) {
      const r = this.records.get(key);
      if (r && r.expiresAt > now) live++;
    }
    return live;
  }

  private sweepSync(now: number): number {
    let removed = 0;
    for (const r of [...this.records.values()]) {
      if (r.expiresAt <= now) {
        this.remove(r);
        removed++;
      }
    }
    return removed;
  }

  private remove(r: ServerRecord): void {
    this.records.delete(r.key);
    const group = this.byGroup.get(r.ipGroup);
    if (group) {
      group.delete(r.key);
      if (group.size === 0) this.byGroup.delete(r.ipGroup);
    }
  }
}
