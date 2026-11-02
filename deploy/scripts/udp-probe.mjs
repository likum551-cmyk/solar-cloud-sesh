#!/usr/bin/env node
// Send the GAGE UDP query to a game server and print the reply as one JSON line.
//
//   node udp-probe.mjs <ip> <port> [--timeout-ms N] [--attempts N] [--module PATH]
//
// It reuses the master's own probe (dist/probe.js of the deployed master release), so the wire
// format has exactly one implementation. <ip> must be an IP address, not a host name.
//
// stdout   {"ok":true,"rttMs":0.4,"protocol":1,"players":3,"maxPlayers":32}
//          {"ok":false,"reason":"timeout"}   (also "bad-reply", "send-error")
// exit     0 valid reply | 1 no valid reply | 2 usage error | 3 probe module not loadable
import { existsSync } from 'node:fs';
import { isIP } from 'node:net';
import { parseArgs } from 'node:util';
import { pathToFileURL } from 'node:url';

const DEFAULT_MODULE = '/opt/gage/master/current/dist/probe.js';

function fail(code, message) {
  process.stderr.write(`udp-probe: ${message}\n`);
  process.exit(code);
}

let parsed;
try {
  parsed = parseArgs({
    allowPositionals: true,
    options: {
      'timeout-ms': { type: 'string', default: '2000' },
      attempts: { type: 'string', default: '3' },
      module: { type: 'string', default: process.env.GAGE_PROBE_MODULE ?? DEFAULT_MODULE },
    },
  });
} catch (err) {
  fail(2, err instanceof Error ? err.message : String(err));
}

const [ip, portText] = parsed.positionals;
const port = Number(portText);
const timeoutMs = Number(parsed.values['timeout-ms']);
const attempts = Number(parsed.values.attempts);
if (parsed.positionals.length !== 2 || isIP(ip) === 0) fail(2, 'usage: udp-probe.mjs <ip> <port> [--timeout-ms N] [--attempts N] [--module PATH]');
if (!Number.isInteger(port) || port < 1 || port > 65535) fail(2, `bad port: ${portText}`);
if (!Number.isInteger(timeoutMs) || timeoutMs < 100 || timeoutMs > 30000) fail(2, 'timeout-ms must be 100..30000');
if (!Number.isInteger(attempts) || attempts < 1 || attempts > 10) fail(2, 'attempts must be 1..10');

const modulePath = parsed.values.module;
if (!existsSync(modulePath)) fail(3, `probe module not found: ${modulePath} (deploy the master first, or pass --module)`);

let probeServer;
try {
  ({ probeServer } = await import(pathToFileURL(modulePath).href));
} catch (err) {
  fail(3, `cannot load ${modulePath}: ${err instanceof Error ? err.message : String(err)}`);
}
if (typeof probeServer !== 'function') fail(3, `${modulePath} does not export probeServer()`);

const result = await probeServer(ip, port, { timeoutMs, attempts });
if (result.ok) {
  const { rttMs, protocol, players, maxPlayers } = result;
  process.stdout.write(`${JSON.stringify({ ok: true, rttMs: Math.round(rttMs * 10) / 10, protocol, players, maxPlayers })}\n`);
  process.exit(0);
}
process.stdout.write(`${JSON.stringify({ ok: false, reason: result.reason, ...(result.detail ? { detail: result.detail } : {}) })}\n`);
process.exit(1);
