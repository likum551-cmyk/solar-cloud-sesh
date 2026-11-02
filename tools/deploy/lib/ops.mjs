#!/usr/bin/env node
// Small helpers for the tools/deploy shell scripts, so no script needs inline node/python. Node >= 18, no
// dependencies. One sub-command per job; every sub-command exits 0 on success and non-zero on failure.
//
//   ops.mjs udp-query <ipv4-or-host> <port> [--timeout-ms N] [--attempts N]
//       The GAGE UDP query (docs/api/server.md section 8; libs/protocol/query.h): one 24-byte datagram out,
//       one 24-byte reply back. Prints one JSON line {"ok":true,"rttMs":..,"protocol":..,"players":..,"maxPlayers":..}
//       or {"ok":false,"reason":".."}. Exit 0 = valid reply, 1 = none.
//   ops.mjs admin-cmd <port> <console command...>
//       The loopback admin channel (docs/api/server.md section 6). The token comes from the environment variable
//       GAGE_ADMIN_TOKEN only, is sent as the first line and is never printed. Prints the server's "output".
//       Exit 0 = {"ok":true}, 1 = refused or failed, 2 = usage.
//   ops.mjs json-get <file> <key> [default]
//       Top-level key of a JSON file. Strings print raw, everything else as JSON. A missing key prints <default>
//       (exit 0) or, without a default, exits 3. An unreadable or invalid file exits 2.
//   ops.mjs manifest-info <version.json> <field>
//       A field of the SIGNED payload inside a launcher update manifest (version, url, sha256, size, mandatory,
//       updateUrl, issued). Does not verify the signature: run tools/update-sign/verify.mjs for that.
//   ops.mjs sanitize-config <server.json> <out.json>
//       A copy of a server.json that is safe to start next to the live server: admin channel, master announce and
//       persistent bans off.
//   ops.mjs free-udp-port
//       Prints a UDP port that is free on 0.0.0.0 right now.
//   ops.mjs file-contains <file> <text>
//       Does the file hold <text> as plain bytes ("ascii") or as UTF-16LE ("utf16le", how a wide string literal
//       such as L"http://..." is stored in a PE)? Prints which; exit 0 if found, 1 if not. Used to prove that the
//       baked update URL and public key really are inside the built launcher exe (SolarLauncher.exe).
import dgram from 'node:dgram';
import net from 'node:net';
import { randomBytes } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';

const [cmd, ...args] = process.argv.slice(2);
const bail = (code, msg) => { process.stderr.write(`ops: ${msg}\n`); process.exit(code); };

function readJson(path) {
  let text;
  try { text = readFileSync(path, 'utf8'); } catch (e) { bail(2, `cannot read ${path}: ${e.message}`); }
  try { return JSON.parse(text.replace(/^\uFEFF/, '')); } catch (e) { bail(2, `${path} is not valid JSON: ${e.message}`); }
}

function parseOpts(list, spec) {
  const pos = [];
  const opt = { ...spec.defaults };
  for (let i = 0; i < list.length; i++) {
    const a = list[i];
    if (a.startsWith('--')) {
      const key = a.slice(2);
      if (!(key in spec.defaults)) bail(2, `unknown option ${a}`);
      const v = list[++i];
      if (v === undefined || !/^\d+$/.test(v)) bail(2, `${a} needs a whole number`);
      opt[key] = Number(v);
    } else pos.push(a);
  }
  return { pos, opt };
}

async function udpQuery() {
  const { pos, opt } = parseOpts(args, { defaults: { 'timeout-ms': 1500, attempts: 3 } });
  if (pos.length !== 2) bail(2, 'usage: udp-query <host> <port> [--timeout-ms N] [--attempts N]');
  const host = pos[0];
  const port = Number(pos[1]);
  if (!Number.isInteger(port) || port < 1 || port > 65535) bail(2, `bad port ${pos[1]}`);
  const nonce = randomBytes(8);
  const req = Buffer.alloc(24);
  req.write('GAGE', 0, 'latin1');
  req[4] = 0x01; // kind: request
  req[5] = 0x01; // query version
  nonce.copy(req, 8);

  const sock = dgram.createSocket('udp4');
  const started = process.hrtime.bigint();
  const result = await new Promise((resolve) => {
    let done = false;
    const finish = (r) => { if (!done) { done = true; resolve(r); } };
    sock.on('error', (e) => finish({ ok: false, reason: `socket error: ${e.message}` }));
    sock.on('message', (msg) => {
      if (msg.length !== 24 || msg.toString('latin1', 0, 4) !== 'GAGE' || msg[4] !== 0x81 || msg[5] !== 0x01) return;
      if (!msg.subarray(8, 16).equals(nonce)) return; // someone else's reply
      const rttMs = Number(process.hrtime.bigint() - started) / 1e6;
      finish({ ok: true, rttMs: Math.round(rttMs * 10) / 10, protocol: msg.readUInt16LE(6), players: msg.readUInt16LE(16), maxPlayers: msg.readUInt16LE(18) });
    });
    const gap = Math.max(50, Math.floor(opt['timeout-ms'] / opt.attempts));
    let sent = 0;
    const tick = () => {
      if (done) return;
      if (sent >= opt.attempts) { setTimeout(() => finish({ ok: false, reason: 'timeout' }), gap); return; }
      sent++;
      sock.send(req, port, host, (e) => { if (e) finish({ ok: false, reason: `send failed: ${e.message}` }); });
      setTimeout(tick, gap);
    };
    tick();
  });
  sock.close();
  process.stdout.write(JSON.stringify(result) + '\n');
  process.exit(result.ok ? 0 : 1);
}

async function adminCmd() {
  const port = Number(args[0]);
  const line = args.slice(1).join(' ');
  if (!Number.isInteger(port) || port < 1 || port > 65535 || !line) bail(2, 'usage: admin-cmd <port> <console command...>');
  if (/[\r\n]/.test(line) || Buffer.byteLength(line) > 1000) bail(2, 'command must be one line of at most 1000 bytes');
  const token = process.env.GAGE_ADMIN_TOKEN ?? '';
  if (token.length < 16) bail(2, 'GAGE_ADMIN_TOKEN is not set (16..128 characters)');
  const replies = await new Promise((resolve) => {
    const out = [];
    let buf = '';
    const s = net.connect({ host: '127.0.0.1', port });
    const timer = setTimeout(() => { s.destroy(); resolve({ error: 'timed out waiting for the admin channel' }); }, 8000);
    s.setEncoding('utf8');
    s.on('connect', () => s.write(`auth ${token}\n${line}\n`));
    s.on('error', (e) => { clearTimeout(timer); resolve({ error: `cannot reach the admin channel on 127.0.0.1:${port}: ${e.message}` }); });
    s.on('data', (d) => {
      buf += d;
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        out.push(buf.slice(0, i));
        buf = buf.slice(i + 1);
      }
      if (out.length >= 2) { clearTimeout(timer); s.end(); resolve({ lines: out }); }
    });
    s.on('close', () => { clearTimeout(timer); resolve(out.length ? { lines: out } : { error: 'admin channel closed without a reply' }); });
  });
  if (replies.error) bail(1, replies.error);
  let auth, res;
  try { auth = JSON.parse(replies.lines[0]); res = replies.lines[1] ? JSON.parse(replies.lines[1]) : null; } catch { bail(1, 'unparseable admin reply'); }
  if (!auth.ok) bail(1, `admin login refused: ${auth.error ?? auth.output ?? 'unknown'}`);
  if (!res) bail(1, 'no reply to the command');
  process.stdout.write(`${res.output ?? res.error ?? ''}\n`);
  process.exit(res.ok ? 0 : 1);
}

function jsonGet() {
  const [file, key, ...rest] = args;
  if (!file || !key) bail(2, 'usage: json-get <file> <key> [default]');
  const obj = readJson(file);
  if (obj === null || typeof obj !== 'object' || Array.isArray(obj)) bail(2, `${file}: top level is not an object`);
  if (!(key in obj)) {
    if (rest.length) { process.stdout.write(rest[0] + '\n'); process.exit(0); }
    process.exit(3);
  }
  const v = obj[key];
  process.stdout.write((typeof v === 'string' ? v : JSON.stringify(v)) + '\n');
}

function manifestInfo() {
  const [file, field] = args;
  if (!file || !field) bail(2, 'usage: manifest-info <version.json> <field>');
  const m = readJson(file);
  if (typeof m.signed !== 'string') bail(2, `${file} has no "signed" member`);
  let p;
  try { p = JSON.parse(Buffer.from(m.signed, 'base64').toString('utf8')); } catch { bail(2, `${file}: "signed" is not base64 JSON`); }
  if (!(field in p)) bail(3, `${file}: signed payload has no "${field}"`);
  process.stdout.write(String(p[field]) + '\n');
}

function sanitizeConfig() {
  const [src, dst] = args;
  if (!src || !dst) bail(2, 'usage: sanitize-config <server.json> <out.json>');
  const c = readJson(src);
  if (c === null || typeof c !== 'object' || Array.isArray(c)) bail(2, `${src}: top level is not an object`);
  c.adminPort = 0;
  delete c.adminToken;
  c.masterUrl = '';
  c.bansFile = '';
  writeFileSync(dst, JSON.stringify(c, null, 2) + '\n');
}

async function freeUdpPort() {
  const s = dgram.createSocket('udp4');
  await new Promise((res, rej) => { s.once('error', rej); s.bind(0, '0.0.0.0', res); });
  const { port } = s.address();
  s.close();
  process.stdout.write(`${port}\n`);
}

function fileContains() {
  const [file, text] = args;
  if (!file || !text) bail(2, 'usage: file-contains <file> <text>');
  let bytes;
  try { bytes = readFileSync(file); } catch (e) { bail(2, `cannot read ${file}: ${e.message}`); }
  const found = bytes.includes(Buffer.from(text, 'latin1')) ? 'ascii' : bytes.includes(Buffer.from(text, 'utf16le')) ? 'utf16le' : '';
  process.stdout.write((found || 'absent') + '\n');
  process.exit(found ? 0 : 1);
}

switch (cmd) {
  case 'file-contains': fileContains(); break;
  case 'udp-query': await udpQuery(); break;
  case 'admin-cmd': await adminCmd(); break;
  case 'json-get': jsonGet(); break;
  case 'manifest-info': manifestInfo(); break;
  case 'sanitize-config': sanitizeConfig(); break;
  case 'free-udp-port': await freeUdpPort(); break;
  default: bail(2, 'usage: ops.mjs udp-query|admin-cmd|json-get|manifest-info|sanitize-config|free-udp-port|file-contains ...');
}
