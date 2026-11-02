import { BlockList, isIP } from 'node:net';

// Address handling for the announce path. Everything downstream (rate-limit buckets, per-IP
// caps, list keys, UDP probe target) uses the canonical form returned by normalizeIp, so one
// address cannot hide behind two spellings.

/**
 * Canonical text of an IP address, or null when `raw` is not a plain IP literal (ports, zone
 * ids, brackets and hostnames are all rejected). IPv6 is lower-case and compressed; an
 * IPv4-mapped IPv6 address (::ffff:a.b.c.d, as a dual-stack socket reports IPv4 peers) is
 * unwrapped to plain IPv4.
 */
export function normalizeIp(raw: string): string | null {
  const s = raw.trim();
  const family = isIP(s);
  if (family === 4) return s;
  if (family !== 6 || s.includes('%')) return null;

  // WHATWG URL serialises an IPv6 host in canonical compressed form (and turns an embedded
  // dotted quad into hex groups), so there is no hand-written IPv6 parser here.
  let host: string;
  try {
    host = new URL(`http://[${s}]/`).hostname;
  } catch {
    return null; // isIP accepted it but URL did not: treat as invalid rather than guess
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

const nonPublic = new BlockList();
for (const [net, prefix] of [
  ['0.0.0.0', 8], // "this network"
  ['10.0.0.0', 8], // RFC 1918
  ['100.64.0.0', 10], // carrier-grade NAT
  ['127.0.0.0', 8], // loopback
  ['169.254.0.0', 16], // link-local
  ['172.16.0.0', 12], // RFC 1918
  ['192.0.0.0', 24], // IETF protocol assignments
  ['192.0.2.0', 24], // documentation
  ['192.168.0.0', 16], // RFC 1918
  ['198.18.0.0', 15], // benchmarking
  ['198.51.100.0', 24], // documentation
  ['203.0.113.0', 24], // documentation
  ['224.0.0.0', 4], // multicast
  ['240.0.0.0', 4], // reserved, includes 255.255.255.255
] as const) {
  nonPublic.addSubnet(net, prefix, 'ipv4');
}
for (const [net, prefix] of [
  ['::', 96], // unspecified, loopback, deprecated IPv4-compatible
  ['64:ff9b::', 96], // NAT64 (embeds an arbitrary IPv4 address)
  ['100::', 64], // discard-only
  ['2001:db8::', 32], // documentation
  ['fc00::', 7], // unique local
  ['fe80::', 10], // link-local
  ['ff00::', 8], // multicast
] as const) {
  nonPublic.addSubnet(net, prefix, 'ipv6');
}

/** True when `ip` (as returned by normalizeIp) is a globally routable unicast address. */
export function isPublicIp(ip: string): boolean {
  const family = isIP(ip);
  if (family === 0) return false;
  return !nonPublic.check(ip, family === 4 ? 'ipv4' : 'ipv6');
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
 * The identity that rate limits and per-address caps apply to: the address itself for IPv4,
 * the /64 for IPv6 (one subscriber owns a whole /64, so per-address limits on IPv6 would be
 * free to dodge).
 */
export function ipGroup(ip: string): string {
  if (isIP(ip) !== 6) return ip;
  const groups = expandV6(ip).slice(0, 4);
  return `${groups.map((g) => g.padStart(4, '0')).join(':')}::/64`;
}

/** "ip:port" for IPv4, "[ip]:port" for IPv6: the key of the list and what the launcher parses. */
export function formatEndpoint(ip: string, port: number): string {
  return isIP(ip) === 6 ? `[${ip}]:${port}` : `${ip}:${port}`;
}

/** First hop of an X-Forwarded-For header value (several header lines arrive comma-joined). */
export function firstForwardedFor(header: string | undefined): string | null {
  if (header === undefined) return null;
  const first = header.split(',')[0];
  if (first === undefined) return null;
  return normalizeIp(first);
}

/** True for the listen addresses that only the local machine can reach. */
export function isLoopbackHost(host: string): boolean {
  return host === 'localhost' || host === '::1' || host === '[::1]' || /^127(?:\.\d{1,3}){3}$/.test(host);
}
