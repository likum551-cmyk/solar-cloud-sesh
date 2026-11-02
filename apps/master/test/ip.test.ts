import { describe, expect, it } from 'vitest';
import { firstForwardedFor, formatEndpoint, ipGroup, isLoopbackHost, isPublicIp, normalizeIp } from '../src/ip.js';

describe('normalizeIp', () => {
  it('keeps IPv4 and canonicalises IPv6', () => {
    expect(normalizeIp('8.8.8.8')).toBe('8.8.8.8');
    expect(normalizeIp(' 8.8.8.8 ')).toBe('8.8.8.8');
    expect(normalizeIp('2606:4700:4700:0:0:0:0:1111')).toBe('2606:4700:4700::1111');
    expect(normalizeIp('2606:4700:4700::1111')).toBe('2606:4700:4700::1111');
    expect(normalizeIp('2606:4700:4700::ABCD')).toBe('2606:4700:4700::abcd');
  });

  it('unwraps IPv4-mapped IPv6 in dotted and hex spelling', () => {
    expect(normalizeIp('::ffff:8.8.8.8')).toBe('8.8.8.8');
    expect(normalizeIp('::ffff:808:808')).toBe('8.8.8.8');
    expect(normalizeIp('0:0:0:0:0:ffff:7f00:1')).toBe('127.0.0.1');
  });

  it('rejects everything that is not a bare IP literal', () => {
    for (const bad of ['', 'unknown', 'example.org', '8.8.8.8:80', '[::1]', 'fe80::1%eth0', '999.1.1.1', '8.8.8', '08.8.8.8', '1.1.1.1, 2.2.2.2']) {
      expect(normalizeIp(bad), JSON.stringify(bad)).toBeNull();
    }
  });
});

describe('isPublicIp', () => {
  it('accepts routable unicast addresses', () => {
    for (const ip of ['8.8.8.8', '1.1.1.1', '93.184.216.34', '172.15.255.255', '172.32.0.1', '100.63.255.255', '100.128.0.1', '2606:4700:4700::1111', '2a01:4f8::1']) {
      expect(isPublicIp(ip), ip).toBe(true);
    }
  });

  it('rejects private, loopback, link-local, shared, reserved and documentation ranges', () => {
    for (const ip of [
      '0.0.0.0', '0.1.2.3', '10.0.0.1', '10.255.255.255', '100.64.0.1', '100.127.255.255', '127.0.0.1', '127.255.255.254',
      '169.254.1.1', '172.16.0.1', '172.31.255.255', '192.0.0.1', '192.0.2.1', '192.168.0.1', '198.18.0.1', '198.19.255.255',
      '198.51.100.1', '203.0.113.1', '224.0.0.1', '239.255.255.255', '240.0.0.1', '255.255.255.255',
      '::', '::1', 'fc00::1', 'fd12:3456::1', 'fe80::1', 'febf::1', 'ff02::1', '2001:db8::1', '64:ff9b::808:808', '100::1',
    ]) {
      expect(isPublicIp(ip), ip).toBe(false);
    }
  });

  it('is false for text that is not an address', () => {
    expect(isPublicIp('localhost')).toBe(false);
  });
});

describe('ipGroup', () => {
  it('is the address for IPv4 and the /64 for IPv6', () => {
    expect(ipGroup('8.8.8.8')).toBe('8.8.8.8');
    expect(ipGroup('2606:4700:4700::1111')).toBe('2606:4700:4700:0000::/64');
    expect(ipGroup('2606:4700:4700::2222')).toBe(ipGroup('2606:4700:4700::1111'));
    expect(ipGroup('2606:4700:4700:1::1')).not.toBe(ipGroup('2606:4700:4700::1'));
    expect(ipGroup('2001:db8:1:2:3:4:5:6')).toBe('2001:0db8:0001:0002::/64');
  });
});

describe('formatEndpoint and forwarded-for', () => {
  it('brackets IPv6 like the launcher expects', () => {
    expect(formatEndpoint('8.8.8.8', 24800)).toBe('8.8.8.8:24800');
    expect(formatEndpoint('2606:4700:4700::1111', 24800)).toBe('[2606:4700:4700::1111]:24800');
  });

  it('takes the first hop and normalises it', () => {
    expect(firstForwardedFor('9.9.9.9, 10.0.0.1, 127.0.0.1')).toBe('9.9.9.9');
    expect(firstForwardedFor('::ffff:9.9.9.9')).toBe('9.9.9.9');
    expect(firstForwardedFor(undefined)).toBeNull();
    expect(firstForwardedFor('')).toBeNull();
    expect(firstForwardedFor('unknown, 9.9.9.9')).toBeNull();
  });
});

describe('isLoopbackHost', () => {
  it('recognises listen addresses reachable only locally', () => {
    for (const h of ['127.0.0.1', '127.1.2.3', 'localhost', '::1']) expect(isLoopbackHost(h), h).toBe(true);
    for (const h of ['0.0.0.0', '::', '10.0.0.5', '8.8.8.8']) expect(isLoopbackHost(h), h).toBe(false);
  });
});
