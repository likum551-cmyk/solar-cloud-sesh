import { describe, expect, it } from 'vitest';
import { ConfigError, loadConfig } from '../src/config.js';

describe('loadConfig', () => {
  it('has the documented defaults', () => {
    expect(loadConfig({})).toEqual({
      host: '127.0.0.1',
      port: 8080,
      ttlSeconds: 90,
      trustProxy: false,
      allowPrivate: false,
      verifyUdp: false,
      probeTimeoutMs: 2000,
      probeAttempts: 3,
      maxInflightProbes: 64,
      maxPerIp: 8,
      maxEntries: 2000,
      rateLimitBurst: 20,
      rateLimitPerMinute: 60,
      sweepIntervalSeconds: 10,
      shutdownGraceSeconds: 10,
      logLevel: 'info',
    });
  });

  it('reads overrides and treats empty values as unset', () => {
    const c = loadConfig({ PORT: '9000', TTL_SECONDS: '30', TRUST_PROXY: '1', ALLOW_PRIVATE: '1', VERIFY_UDP: '1', MAX_PER_IP: '2', HOST: '', LOG_LEVEL: 'debug' });
    expect(c).toMatchObject({ port: 9000, ttlSeconds: 30, trustProxy: true, allowPrivate: true, verifyUdp: true, maxPerIp: 2, host: '127.0.0.1', logLevel: 'debug' });
  });

  it('fails loudly, listing every bad variable', () => {
    let error: unknown;
    try {
      loadConfig({ TRUST_PROXY: 'true', PORT: 'abc', TTL_SECONDS: '0', VERIFY_UDP: '2', MAX_PER_IP: '1e3', LOG_LEVEL: 'loud' });
    } catch (e) {
      error = e;
    }
    expect(error).toBeInstanceOf(ConfigError);
    const issues = (error as ConfigError).issues.join('\n');
    for (const name of ['TRUST_PROXY', 'PORT', 'TTL_SECONDS', 'VERIFY_UDP', 'MAX_PER_IP', 'LOG_LEVEL']) {
      expect(issues, name).toContain(name);
    }
  });
});
