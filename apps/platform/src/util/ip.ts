import { isIP } from 'node:net';

// Address handling, ported from apps/master/src/ip.ts. Everything that keys on an address (rate limits,
// lockouts, the game-server allowlist) uses the canonical form returned by normalizeIp, so one
// address cannot hide behind two spellings.

/**
 * Canonical text of an IP address, or null when `raw` is not a plain IP literal (ports, zone ids,
 * brackets and hostnames are rejected). IPv6 is lower-case and compressed; an IPv4-mapped IPv6
 * address (::ffff:a.b.c.d, as a dual-stack socket reports IPv4 peers) is unwrapped to IPv4.
 */
export function normalizeIp(raw: string): string | null {
  const s = raw.trim();
  const family = isIP(s);
  if (family === 4) return s;
  if (family !== 6 || s.includes('%')) return null;

  // WHATWG URL serialises an IPv6 host in canonical compressed form (and turns an embedded dotted
  // quad into hex groups), so there is no hand-written IPv6 parser here.
  let host: string;
  try {
    host = new URL(`http://[${s}]/`).hostname;
  } catch {
    return null;
  }
  const canonical = host.slice(1, -1);
  const mapped = /^::ffff:([0-9a-f]{1,4}):([0-9a-f]{1,4})$/.exec(canonical);
  if (mapped) {
    const hi = parseInt(mapped[1] as string, 16);
    const lo = parseInt(mapped[2] as string, 16);
    return `${hi >> 8}.${hi & 0xff}.${lo >> 8}.${lo & 0xff}`;
  }
  return canonical;
}

/** Expand a canonical compressed IPv6 address into its eight 16-bit groups. */
function expandV6(canonical: string): string[] {
  const [head = '', tail, extra] = canonical.split('::');
  if (extra !== undefined) throw new Error(`not a canonical IPv6 address: ${canonical}`);
  const left = head === '' ? [] : head.split(':');
  const right = tail === undefined || tail === '' ? [] : tail.split(':');
  const fill = tail === undefined ? 0 : 8 - left.length - right.length;
  return [...left, ...Array<string>(fill).fill('0'), ...right];
}

/**
 * The identity that rate limits apply to: the address itself for IPv4, the /64 for IPv6 (one
 * subscriber owns a whole /64, so per-address limits on IPv6 would be free to dodge).
 */
export function ipGroup(ip: string): string {
  if (isIP(ip) !== 6) return ip;
  const groups = expandV6(ip).slice(0, 4);
  return `${groups.map((g) => g.padStart(4, '0')).join(':')}::/64`;
}

/**
 * The address block a login lockout applies to: the IPv4 /24 or the IPv6 /64. Coarser than
 * ipGroup for IPv4 on purpose: an attacker who holds a block of neighbouring addresses must not
 * get a fresh guess budget per address, while a victim on another network is not affected.
 */
export function ipLockGroup(ip: string): string {
  if (isIP(ip) === 4) return `${ip.split('.').slice(0, 3).join('.')}.0/24`;
  return ipGroup(ip);
}

/** True for the listen addresses that only the local machine can reach. */
export function isLoopbackHost(host: string): boolean {
  return host === 'localhost' || host === '::1' || host === '[::1]' || /^127(?:\.\d{1,3}){3}$/.test(host);
}

/**
 * The client address a single trusted reverse proxy appended to X-Forwarded-For: the LAST entry
 * (a proxy that appends, like nginx's $proxy_add_x_forwarded_for, leaves any client-supplied
 * entries in front of it; a proxy that overwrites sends exactly one). Null when absent or not an IP.
 */
export function lastForwardedFor(header: string | undefined): string | null {
  if (header === undefined) return null;
  const parts = header.split(',');
  const last = parts[parts.length - 1];
  if (last === undefined) return null;
  return normalizeIp(last);
}
