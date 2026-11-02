// assets_test.mjs: end-to-end test of client/net/assets.cpp. Serves fixture
// asset sets from an in-process HTTP server on 127.0.0.1 (plus a second
// "mirror" server for a cross-origin redirect) and runs the native
// assets_test.exe against them, one scenario per run.
//
//   node tests/assets_test.mjs --exe <path\to\assets_test.exe> [--work <dir>] [--only <substring>]
//
// Test data goes to <work>\assets-test-data (default work: the OS temp dir);
// that folder alone is wiped at the start and removed after a passing run.
// Exit 0 only when every scenario passes and the scenario count is the
// expected one (--only runs a subset and always exits 1 on a count mismatch).
//
// The downloader's counter probes with "Range: bytes=0-0"; the log marks those
// requests `probe` so request counts below are about real file fetches.

import http from 'node:http';
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const EXPECTED_SCENARIOS = 29;

// ---- args ----------------------------------------------------------------
function argValue(name) {
  const i = process.argv.indexOf(name);
  return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : null;
}
const exe = argValue('--exe') ?? process.env.ASSETS_TEST_EXE;
if (!exe || !fs.existsSync(exe)) {
  console.error(`assets_test.mjs: --exe <assets_test.exe> is required (got ${exe ?? 'nothing'}); tests dir: ${here}`);
  process.exit(2);
}
const onlyFilter = argValue('--only');
const work = path.resolve(argValue('--work') ?? os.tmpdir());
const root = path.join(work, 'assets-test-data');
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(root, { recursive: true });

// ---- fixtures ------------------------------------------------------------
// Deterministic high-entropy bytes (SHA-256 in counter mode over a seed).
function blob(seed, size) {
  const out = Buffer.alloc(size);
  for (let off = 0, i = 0; off < size; off += 32, i++)
    crypto.createHash('sha256').update(`${seed}:${i}`).digest().copy(out, off);
  return out;
}
const hashHex = (alg, buf) => crypto.createHash(alg).update(buf).digest('hex');
const md5 = (buf) => hashHex('md5', buf);
const lastModOf = (buf) => new Date(Date.UTC(2026, 9, 1) + (buf[0] * 3600 + buf[1]) * 1000).toUTCString();
const mtimeHex = (buf) => (0x65ea0000 + buf[0] * 256 + buf[1]).toString(16);

const SIZES = [2000, 32, 100 * 1024, 3 * 1024 * 1024 + 17, 777, 64 * 1024 + 1, 1024 * 1024];
const HAS_ETAG = [false, true, true, true, false, true, false];
const CHUNK_FLAGS = ['--chunk-min', String(2 * 1024 * 1024), '--chunk', String(256 * 1024)];

function freshSet(count, version) {
  return SIZES.slice(0, count).map((n, i) => blob(`f${i}v${version}`, n));
}

let state;
function fixture(over = {}) {
  state = {
    mode: 'nginx',        // nginx | raw (no headers) | catchall (200 html past the end)
    files: freshSet(7, 1),
    etagKind: 'sha16',    // sha16 (opaque, HAS_ETAG files only) | md5 | fake32 | sizemtime (all files)
    pastEnd: null,        // null = by mode | '404' | '502' | 'reset' | 'html200'
    resetDelayMs: 0,
    slow: new Set(),      // indices served at 64 KiB / 10 ms
    ranges: false,        // answer Range with 206
    ifRangeIgnored: false,
    flip: null,           // { index, b }: the first full GET gets the old bytes, then the file becomes b
    faults: {},           // index -> { kind: '500'|'503'|'reset', times, persist }
    corrupt: {},          // index -> how many full GETs get one flipped byte
    sizeLie: -1,          // size-mtime ETag of this index names a wrong size
    stall: -1,            // full GETs of this index send half the body, then hang
    noFiles: false,       // /file/0 -> 302 -> 404
    ...over,
  };
}

function etagFor(n, buf) {
  if (state.mode === 'raw') return null;
  switch (state.etagKind) {
    case 'md5': return `"${md5(buf)}"`;
    case 'fake32': return `"${hashHex('sha1', buf).slice(0, 32)}"`;
    case 'sizemtime': return `"${(buf.length + (n === state.sizeLie ? 1 : 0)).toString(16)}-${mtimeHex(buf)}"`;
    default: return HAS_ETAG[n] ? `"${hashHex('sha1', buf).slice(0, 16)}"` : null;
  }
}

let log = [];

// ---- servers -------------------------------------------------------------
let mainPort = 0;
let mirrorPort = 0;

function finish(res, entry, status, headers, body) {
  entry.status = status;
  res.writeHead(status, headers);
  res.end(body);
}

function serveFile(req, res, n, entry) {
  let buf = state.files[n];
  if (state.flip && state.flip.index === n && !req.headers.range && !state.flip.done) {
    state.flip.done = true; // this request streams the old bytes; everyone after sees the new ones
    state.files[n] = state.flip.b;
  }
  const etag = etagFor(n, buf);
  const headers = {};
  if (state.mode !== 'raw') headers['Content-Type'] = 'application/octet-stream';
  if (etag) {
    headers.ETag = etag;
    headers['Last-Modified'] = lastModOf(buf);
    if (req.headers['if-none-match'] === etag) {
      entry.status = 304;
      res.writeHead(304, { ETag: etag });
      return res.end();
    }
  }
  let body = buf;
  let status = 200;
  const range = req.headers.range;
  if (range && state.ranges) {
    const m = /^bytes=(\d+)-(\d*)$/.exec(range);
    const ifRange = req.headers['if-range'];
    if (m && (!ifRange || state.ifRangeIgnored || ifRange === etag)) {
      const s = Number(m[1]);
      const e = m[2] ? Math.min(Number(m[2]), buf.length - 1) : buf.length - 1;
      status = 206;
      headers['Content-Range'] = `bytes ${s}-${e}/${buf.length}`;
      body = buf.subarray(s, e + 1);
    }
  }
  if (!entry.probe && status === 200 && state.corrupt[n] > 0) {
    state.corrupt[n]--;
    body = Buffer.from(buf);
    body[body.length >> 1] ^= 0xff;
  }
  headers['Content-Length'] = String(body.length);
  entry.status = status;
  entry.bytes = body.length;
  res.writeHead(status, headers);
  if (n === state.stall && !entry.probe && status === 200) {
    res.write(body.subarray(0, body.length >> 1)); // and never more
    entry.stalled = true;
    return;
  }
  if (!state.slow.has(n) || entry.probe) return res.end(body);
  let off = 0;
  let closed = false;
  res.on('close', () => { closed = true; });
  const step = () => {
    if (closed) return;
    if (off >= body.length) return res.end();
    res.write(body.subarray(off, off + 65536));
    off += 65536;
    setTimeout(step, 10);
  };
  step();
}

function pastEnd(req, res, entry) {
  const kind = state.pastEnd ?? (state.mode === 'raw' ? 'reset' : state.mode === 'catchall' ? 'html200' : '404');
  if (kind === 'reset') {
    entry.status = 'reset'; // raw RAGE:MP: no HTTP answer at all
    if (state.resetDelayMs) setTimeout(() => req.socket.destroy(), state.resetDelayMs);
    else req.socket.destroy();
    return;
  }
  if (kind === 'html200')
    return finish(res, entry, 200, { 'Content-Type': 'text/html; charset=utf-8' }, '<html><body>Welcome</body></html>');
  if (kind === '502')
    return finish(res, entry, 502, { 'Content-Type': 'text/html' }, '<html><body>502 Bad Gateway</body></html>');
  finish(res, entry, 404, { 'Content-Type': 'text/html' }, '<html><body>404 Not Found</body></html>');
}

function handler(which) {
  return (req, res) => {
    const entry = {
      which, url: req.url, inm: req.headers['if-none-match'] ?? null, range: req.headers.range ?? null,
      probe: req.headers.range === 'bytes=0-0', status: 0,
    };
    log.push(entry);
    const url = req.url;
    if (which === 'mirror') {
      if (url === '/m/2' && state.files.length > 2) return serveFile(req, res, 2, entry);
      return finish(res, entry, 404, { 'Content-Type': 'text/html' }, 'nope');
    }
    if (url === '/missing') return finish(res, entry, 404, { 'Content-Type': 'text/html' }, 'nope');
    if (url === '/r/2') return finish(res, entry, 301, { Location: `http://127.0.0.1:${mirrorPort}/m/2` }, '');
    const m = /^\/file\/(\d+)$/.exec(url);
    if (!m) return finish(res, entry, 404, { 'Content-Type': 'text/html' }, 'nope');
    const n = Number(m[1]);
    if (n === 0 && state.noFiles) return finish(res, entry, 302, { Location: '/missing' }, '');
    if (n >= state.files.length) return pastEnd(req, res, entry);
    const f = state.faults[n];
    if (f && (f.persist || (!entry.probe && f.times > 0))) {
      if (!f.persist) f.times--;
      entry.fault = f.kind;
      if (f.kind === 'reset') {
        entry.status = 'reset';
        req.socket.destroy();
        return;
      }
      return finish(res, entry, Number(f.kind), { 'Content-Type': 'text/plain' }, 'boom');
    }
    if (n === 2 && state.mode !== 'raw') return finish(res, entry, 302, { Location: '/r/2' }, '');
    serveFile(req, res, n, entry);
  };
}

function listen(server) {
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => resolve(server.address().port));
  });
}

const mainServer = http.createServer(handler('main'));
const mirrorServer = http.createServer(handler('mirror'));
for (const s of [mainServer, mirrorServer]) s.on('clientError', (_err, sock) => sock.destroy());
mainPort = await listen(mainServer);
mirrorPort = await listen(mirrorServer);
const base = `http://127.0.0.1:${mainPort}`;

// A port nothing listens on: bind, note it, close.
async function closedPort() {
  const s = http.createServer();
  const port = await listen(s);
  await new Promise((r) => s.close(r));
  return port;
}

// ---- running the exe -----------------------------------------------------
function startExe(args, timeoutMs = 120000) {
  let out = '';
  let timedOut = false;
  const child = spawn(exe, args, { stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
  const done = new Promise((resolve) => {
    child.stdout.on('data', (d) => { out += d; });
    child.stderr.on('data', (d) => { out += d; });
    const timer = setTimeout(() => { timedOut = true; child.kill(); }, timeoutMs); // our own child only
    child.on('error', (e) => { clearTimeout(timer); resolve({ code: -1, out: out + String(e), timedOut }); });
    child.on('close', (code) => { clearTimeout(timer); resolve({ code, out, timedOut }); });
  });
  return { child, done };
}
const runExe = (args, timeoutMs) => startExe(args, timeoutMs).done;

function parseResult(out) {
  const line = out.split(/\r?\n/).find((l) => l.startsWith('RESULT '));
  if (!line) return null;
  const r = {};
  const head = line.slice(7, line.indexOf(' error='));
  for (const kv of head.split(' ')) {
    const [k, v] = kv.split('=');
    r[k] = Number(v);
  }
  r.error = line.slice(line.indexOf(' error=') + 7);
  return r;
}

// ---- cache checks --------------------------------------------------------
function verifyCache(dir, files, fail) {
  const fileDir = path.join(dir, 'file');
  const names = fs.existsSync(fileDir) ? fs.readdirSync(fileDir).sort() : [];
  const want = files.map((_, i) => String(i)).sort();
  if (names.join(',') !== want.join(',')) fail(`file\\ holds [${names}] want [${want}]`);
  files.forEach((buf, i) => {
    const p = path.join(fileDir, String(i));
    if (fs.existsSync(p) && !fs.readFileSync(p).equals(buf)) fail(`file\\${i} bytes differ from the server's`);
  });
  const idxPath = path.join(dir, 'files.idx');
  const lines = fs.existsSync(idxPath) ? fs.readFileSync(idxPath, 'utf8').split('\n').filter(Boolean) : [];
  if (lines.length !== files.length) fail(`files.idx has ${lines.length} line(s), want ${files.length}`);
  lines.forEach((l, i) => {
    const f = l.split('\t');
    if (f.length !== 5 || f[0] !== String(i) || f[1] !== String(files[i]?.length) || f[4] !== md5(files[i] ?? Buffer.alloc(0)))
      fail(`files.idx line ${i} is "${l}" (want 5 columns, size and md5 of the server's file)`);
  });
  const complete = path.join(dir, 'complete');
  const c = fs.existsSync(complete) ? fs.readFileSync(complete, 'utf8') : null;
  if (c !== String(files.length)) fail(`complete marker is ${JSON.stringify(c)}, want "${files.length}"`);
  for (const n of fs.readdirSync(dir)) if (n.endsWith('.tmp') || n.endsWith('.journal')) fail(`leftover ${n}`);
}

// The exe's PROGRESS lines as objects ({t, files, total, ..., bigIndex, bigSize, bigDone}).
function progressLines(out) {
  return out.split(/\r?\n/).filter((l) => l.startsWith('PROGRESS ')).map((l) => {
    const o = {};
    for (const kv of l.slice(9).trim().split(' ')) {
      const [k, v] = kv.split('=');
      o[k] = Number(v);
    }
    return o;
  });
}

// The in-flight report (what the launch screen says a long wait is on): the
// slow 3 MB file 3 shows up as the largest in flight with its real size and a
// partial count, and no tick ever claims more than the sizes allow.
function checkInFlight(out, fail, mustSee = true) {
  const ticks = progressLines(out);
  const size = state.files[3].length;
  if (mustSee && !ticks.some((p) => p.bigIndex === 3 && p.bigSize === size && p.bigDone > 0 && p.bigDone < size && p.active >= 1))
    fail(`no tick showed file 3 in flight with its size ${size} and part of its bytes`);
  const bad = ticks.find((p) => p.bigDone > p.bigSize || p.activeLeft < p.bigSize - p.bigDone || p.active < 0
    || (p.bigIndex === -1 && (p.bigSize !== 0 || p.bigDone !== 0)));
  if (bad) fail(`inconsistent in-flight tick: ${JSON.stringify(bad)}`);
}

const hits = (pred) => log.filter(pred);
const fetches = (url) => hits((e) => e.url === url && !e.probe && !e.range).length; // whole-file GETs
const rangeGets = (url, status) => hits((e) => e.url === url && !e.probe && e.range && e.status === status).length;

// ---- scenarios -----------------------------------------------------------
const results = [];

function record(name, reasons, r, out) {
  results.push({ name, ok: reasons.length === 0 });
  const tail = r ? `  [${r.files} files, ${r.downloaded} dl, ${r.fromCache} cached, upToDate=${r.upToDate}, total=${r.total}, ${r.ms} ms${r.error ? `, error="${r.error}"` : ''}]` : '';
  console.log(`${reasons.length ? 'FAIL' : 'ok  '}  ${name}${tail}`);
  for (const why of reasons) console.log(`        - ${why}`);
  if (reasons.length && out) console.log(out.split(/\r?\n/).filter((l) => !l.startsWith('PROGRESS')).map((l) => `        | ${l}`).join('\n'));
}

async function scenario(name, setup, expect, checks) {
  if (onlyFilter && !name.includes(onlyFilter)) return;
  const reasons = [];
  const fail = (why) => reasons.push(why);
  log = [];
  const args = await setup();
  const run = await runExe(args);
  const r = parseResult(run.out);
  if (run.timedOut) fail('exe timed out');
  if (expect && run.code !== 0) fail(`exe exit ${run.code} (result mismatch)`);
  if (expect && !r) fail('no RESULT line');
  if (checks) checks(r, run, fail);
  record(name, reasons, r, run.out);
}
const dl = (dir, files, down, cache, up, ...flags) => [base, dir, String(files), String(down), String(cache), String(up), ...flags];
const dirOf = (n) => path.join(root, n);
const dirA = dirOf('a');
const dirE = dirOf('e');

// ---- the original 13 (expectations changed only where the rework changes behaviour) ----

await scenario('(a) cold download: 7 files, 302/301 cross-origin redirect, slow 3 MB body, total reported',
  () => { fixture({ slow: new Set([3]) }); return dl(dirA, 7, 7, 0, 0); }, true,
  (r, _run, fail) => {
    verifyCache(dirA, state.files, fail);
    if (!hits((e) => e.which === 'mirror' && e.url === '/m/2' && e.status === 200 && !e.probe).length) fail('mirror never served /m/2');
    if (!hits((e) => e.url === '/r/2' && e.status === 301).length) fail('second redirect hop never taken');
    if (!r || r.progress < 1) fail('no progress callback during a ~0.5 s download');
    if (!r || r.total !== 7 || r.counted !== 7) fail(`progress total ${r?.total} / counted ${r?.counted}, want 7`);
    checkInFlight(_run.out, fail);
  });

await scenario('(b) second run: /file/0 unchanged -> upToDate fast path',
  () => { state.slow = new Set(); return dl(dirA, 7, 0, 7, 1); }, true,
  (_r, _run, fail) => {
    const urls = log.map((e) => `${e.which}${e.url}`);
    if (urls.join(',') !== 'main/file/0') fail(`requests were [${urls}], want only main/file/0`);
  });

await scenario('(c) file 3 (new bytes + ETag) and file 0 (same size) changed',
  () => {
    state.files[3] = blob('f3v2', SIZES[3]);
    state.files[0] = blob('f0v2', SIZES[0]);
    return dl(dirA, 7, 2, 5, 0);
  }, true,
  (_r, _run, fail) => {
    verifyCache(dirA, state.files, fail);
    if (!hits((e) => e.which === 'mirror' && e.url === '/m/2' && e.inm && e.status === 304).length)
      fail('If-None-Match did not survive the redirect chain to the mirror (no 304 there)');
    if (!hits((e) => e.url === '/file/1' && e.status === 304).length) fail('/file/1 was not revalidated with a 304');
  });

await scenario('(d) server shrinks to 5 files -> file\\5, file\\6 removed',
  () => {
    state.files = state.files.slice(0, 5);
    state.files[0] = blob('f0v3', SIZES[0]);
    return dl(dirA, 5, 1, 4, 0);
  }, true,
  (_r, _run, fail) => verifyCache(dirA, state.files, fail));

await scenario('(f) /file/0 redirects to a 404 -> ok, no files, marker cleared, blobs kept',
  () => { state.noFiles = true; return dl(dirA, 0, 0, 0, 0); }, true,
  (_r, _run, fail) => {
    if (fs.existsSync(path.join(dirA, 'complete'))) fail('complete marker still present');
    for (let i = 0; i < 5; i++) if (!fs.existsSync(path.join(dirA, 'file', String(i)))) fail(`blob file\\${i} was deleted`);
  });

await scenario('(e) raw server: no headers, past-the-end socket reset',
  () => { fixture({ mode: 'raw', files: freshSet(4, 1) }); return dl(dirE, 4, 4, 0, 0); }, true,
  (_r, _run, fail) => {
    verifyCache(dirE, state.files, fail);
    if (!hits((e) => e.url === '/file/4' && e.status === 'reset').length) fail('/file/4 was never reset');
  });

await scenario('(e2) raw server, manifest changed: unchanged files kept by the same-size check',
  () => { state.files[0] = blob('f0v2', SIZES[0]); return dl(dirE, 4, 1, 3, 0); }, true,
  (_r, _run, fail) => verifyCache(dirE, state.files, fail));

let deadPort = 0;
await scenario('(g) unreachable base (closed port) -> ok=false naming host:port',
  async () => {
    deadPort = await closedPort();
    return [`http://127.0.0.1:${deadPort}`, dirOf('g'), '-1', '0', '0', '0'];
  }, true,
  (r, _run, fail) => {
    if (r && !r.error.includes(`127.0.0.1:${deadPort}`)) fail(`error "${r.error}" does not name 127.0.0.1:${deadPort}`);
    if (fs.existsSync(path.join(dirOf('g'), 'complete'))) fail('complete marker written');
  });

await scenario('(h) catch-all server: 200 text/html past the end',
  () => { fixture({ mode: 'catchall', files: freshSet(3, 1) }); return dl(dirOf('h'), 3, 3, 0, 0); }, true,
  (_r, _run, fail) => verifyCache(dirOf('h'), state.files, fail));

// Changed by the rework: a failing file below the end now gets 3 retries
// (250 ms, 1 s, 3 s) instead of 1, so 4 whole-file GETs instead of 2.
await scenario('(i) HTTP 500 on file 4 -> 3 retries with backoff, then ok=false',
  () => { fixture({ faults: { 4: { kind: '500', persist: true } } }); return [base, dirOf('i'), '-1', '0', '0', '0']; }, true,
  (r, _run, fail) => {
    if (r && !r.error.includes('HTTP 500 (file 4)')) fail(`error "${r.error}" does not say HTTP 500 (file 4)`);
    const n = fetches('/file/4');
    if (n !== 4) fail(`/file/4 fetched ${n} time(s), want 4 (3 retries)`);
    if (fs.existsSync(path.join(dirOf('i'), 'complete'))) fail('complete marker written after a failed run');
  });

await scenario('(j) DownloadServerAssets(127.0.0.1, gamePort) fetches from gamePort + 1',
  () => { fixture(); return ['server', '127.0.0.1', String(mainPort - 1), dirOf('j'), '7', '7', '0', '0']; }, true,
  (_r, _run, fail) => {
    verifyCache(dirOf('j'), state.files, fail);
    if (!hits((e) => e.which === 'main' && e.url === '/file/0').length) fail('main server never saw /file/0');
  });

await scenario('(id1) ServerAssetId self-checks (no-arg mode)',
  () => [], false,
  (_r, run, fail) => {
    if (run.code !== 0) fail(`exit ${run.code}`);
    if (!/5 check\(s\), 0 failed/.test(run.out)) fail('self-check summary missing');
  });

await scenario('(id2) ServerAssetId equals Node SHA-256 of lowercase "host:port"',
  () => ['id', 'Example.COM', '22005'], false,
  (_r, run, fail) => {
    const want = crypto.createHash('sha256').update('example.com:22005', 'utf8').digest('hex').slice(0, 32);
    const got = (/^ID (\S+)/m.exec(run.out) ?? [])[1];
    if (run.code !== 0 || got !== want) fail(`got ${got}, want ${want}`);
  });

// ---- end of list -----------------------------------------------------------

await scenario('(k) 502 text/html past the end (57.128.211.33 style) -> ok',
  () => { fixture({ pastEnd: '502' }); return dl(dirOf('k'), 7, 7, 0, 0); }, true,
  (_r, _run, fail) => {
    verifyCache(dirOf('k'), state.files, fail);
    if (!hits((e) => e.status === 502).length) fail('no 502 was ever served (fixture not exercised)');
  });

await scenario('(l) raw server, past-the-end reset after 1 s -> ok',
  () => { fixture({ mode: 'raw', files: freshSet(4, 1), resetDelayMs: 1000 }); return dl(dirOf('l'), 4, 4, 0, 0); }, true,
  (_r, _run, fail) => verifyCache(dirOf('l'), state.files, fail));

await scenario('(m) persistent mid-list reset on file 3 -> ok=false, never a short success',
  () => { fixture({ faults: { 3: { kind: 'reset', persist: true } } }); return [base, dirOf('m'), '-1', '0', '0', '0']; }, true,
  (r, _run, fail) => {
    if (r && !r.error.includes('(file 3)')) fail(`error "${r.error}" does not name file 3`);
    if (fs.existsSync(path.join(dirOf('m'), 'complete'))) fail('complete marker written');
  });

await scenario('(n) transient mid-list 503 (twice) on file 3 -> retried, ok',
  () => { fixture({ faults: { 3: { kind: '503', times: 2 } } }); return dl(dirOf('n'), 7, 7, 0, 0); }, true,
  (_r, _run, fail) => {
    verifyCache(dirOf('n'), state.files, fail);
    const n = fetches('/file/3');
    if (n !== 3) fail(`/file/3 fetched ${n} time(s), want 3 (two 503s, then the file)`);
  });

// ---- ranges --------------------------------------------------------------
const rangeTiming = {}; // run times of (o) split vs (p) single stream, printed at the end

await scenario('(o) range-capable server: the 3 MB file is split across connections, bytes identical',
  () => { fixture({ ranges: true, slow: new Set([3]) }); return dl(dirOf('o'), 7, 7, 0, 0, ...CHUNK_FLAGS); }, true,
  (r, _run, fail) => {
    verifyCache(dirOf('o'), state.files, fail);
    const n = rangeGets('/file/3', 206);
    if (n < 2) fail(`/file/3 got ${n} ranged 206 request(s), want >= 2`);
    checkInFlight(_run.out, fail, false); // ~0.3 s: one tick at most, so only consistency
    if (r) rangeTiming.split = r.ms;
  });

await scenario('(p) range-ignoring server (200 to Range): whole file still correct',
  () => { fixture({ ranges: false, slow: new Set([3]) }); return dl(dirOf('p'), 7, 7, 0, 0, ...CHUNK_FLAGS); }, true,
  (r, _run, fail) => {
    verifyCache(dirOf('p'), state.files, fail);
    const tried = hits((e) => e.url === '/file/3' && !e.probe && e.range && e.status === 200).length;
    if (tried < 1) fail('no helper ever tried a range on /file/3 (fixture not exercised)');
    if (tried > 1) fail(`${tried} helpers tried ranges; after the first 200 there should be none`);
    if (r) rangeTiming.single = r.ms;
  });

let flipA = null;
await scenario('(q) ETag changes between owner and helper (server ignores If-Range): bytes never mixed',
  () => {
    fixture({ ranges: true, ifRangeIgnored: true, slow: new Set([3]) });
    flipA = state.files[3];
    state.flip = { index: 3, b: blob('f3flip', SIZES[3]) };
    return dl(dirOf('q'), 7, 7, 0, 0, ...CHUNK_FLAGS);
  }, true,
  (_r, _run, fail) => {
    const p = path.join(dirOf('q'), 'file', '3');
    const got = fs.existsSync(p) ? fs.readFileSync(p) : Buffer.alloc(0);
    if (!got.equals(flipA)) fail(got.equals(state.flip.b) ? 'file\\3 is version B (owner streamed A)' : 'file\\3 mixes versions A and B');
    if (!hits((e) => e.url === '/file/3' && !e.probe && e.range && e.status === 206).length) fail('no helper got a 206 (fixture not exercised)');
  });

// ---- crash recovery ------------------------------------------------------

{
  const name = '(s) killed mid-run, rerun: the journal keeps the finished files';
  if (!onlyFilter || name.includes(onlyFilter)) {
    const reasons = [];
    const fail = (why) => reasons.push(why);
    const dir = dirOf('s');
    fixture({ stall: 6 });
    log = [];
    const { child, done } = startExe(dl(dir, 'x', 'x', 'x', 'x'));
    const journal = path.join(dir, 'files.idx.journal');
    const t0 = Date.now();
    let lines = 0;
    while (Date.now() - t0 < 15000) {
      lines = fs.existsSync(journal) ? fs.readFileSync(journal, 'utf8').split('\n').filter(Boolean).length : 0;
      if (lines >= 6 && hits((e) => e.stalled).length) break;
      await new Promise((r) => setTimeout(r, 50));
    }
    child.kill(); // our own child: TerminateProcess, like a crash
    const first = await done;
    if (lines < 6) fail(`journal had ${lines} line(s) before the kill, want 6 (files 0..5)`);
    if (first.out.includes('RESULT ')) fail('the first run finished before it was killed (stall not hit)');
    if (fs.existsSync(path.join(dir, 'files.idx'))) fail('files.idx exists after the kill (journal not what is tested)');
    state.stall = -1;
    log = [];
    const run = await runExe(dl(dir, 7, 1, 6, 0));
    const r = parseResult(run.out);
    if (run.code !== 0) fail(`rerun exit ${run.code} (want 7 files, 1 downloaded, 6 from cache)`);
    verifyCache(dir, state.files, fail);
    record(name, reasons, r, run.out);
  }
}

// ---- validation ----------------------------------------------------------

await scenario('(r1) MD5 ETags, file 4 always corrupted -> one re-download, then ok=false',
  () => { fixture({ etagKind: 'md5', corrupt: { 4: Infinity } }); return [base, dirOf('r1'), '-1', '0', '0', '0']; }, true,
  (r, _run, fail) => {
    if (r && !r.error.includes('failed its check (file 4)')) fail(`error "${r.error}" does not say file 4 failed its check`);
    if (r && r.hash !== 1) fail(`hash mode ${r.hash}, want 1 (MD5)`);
    const n = fetches('/file/4');
    if (n !== 2) fail(`/file/4 fetched ${n} time(s), want 2 (one re-download)`);
  });

await scenario('(r2) MD5 ETags, file 4 corrupted once -> re-downloaded, ok',
  () => { fixture({ etagKind: 'md5', corrupt: { 4: 1 } }); return dl(dirOf('r2'), 7, 7, 0, 0); }, true,
  (r, _run, fail) => {
    verifyCache(dirOf('r2'), state.files, fail);
    const n = fetches('/file/4');
    if (n !== 2) fail(`/file/4 fetched ${n} time(s), want 2`);
    if (r && r.hash !== 1) fail(`hash mode ${r.hash}, want 1`);
  });

await scenario('(r3) 32-hex ETags that are not MD5s -> checks off, ok',
  () => { fixture({ etagKind: 'fake32' }); return dl(dirOf('r3'), 7, 7, 0, 0); }, true,
  (r, _run, fail) => {
    verifyCache(dirOf('r3'), state.files, fail);
    if (r && r.hash !== 0) fail(`hash mode ${r.hash}, want 0`);
  });

await scenario('(r4a) nginx size-mtime ETags -> size checks on, ok',
  () => { fixture({ etagKind: 'sizemtime' }); return dl(dirOf('r4a'), 7, 7, 0, 0); }, true,
  (r, _run, fail) => {
    verifyCache(dirOf('r4a'), state.files, fail);
    if (r && r.hash !== 2) fail(`hash mode ${r.hash}, want 2`);
  });

await scenario('(r4b) nginx size-mtime ETag of file 5 names the wrong size -> ok=false',
  () => { fixture({ etagKind: 'sizemtime', sizeLie: 5 }); return [base, dirOf('r4b'), '-1', '0', '0', '0']; }, true,
  (r, _run, fail) => {
    if (r && !r.error.includes('failed its check (file 5)')) fail(`error "${r.error}" does not say file 5 failed its check`);
  });

await scenario('(r5) complete marker but file\\4 deleted -> not upToDate, only file 4 fetched',
  () => {
    fixture({ etagKind: 'md5' });
    fs.rmSync(path.join(dirOf('r2'), 'file', '4'));
    return dl(dirOf('r2'), 7, 1, 6, 0);
  }, true,
  (_r, _run, fail) => verifyCache(dirOf('r2'), state.files, fail));

const damage = () => {
  const p = path.join(dirOf('r2'), 'file', '5');
  const b = fs.readFileSync(p);
  b[100] ^= 0xff;
  fs.writeFileSync(p, b);
};
await scenario('(r6a) damaged local blob, same size, normal run -> still upToDate (sizes only)',
  () => { damage(); return dl(dirOf('r2'), 7, 0, 7, 1); }, true, null);

await scenario('(r6b) the same damaged blob with --verify -> detected and re-fetched',
  () => dl(dirOf('r2'), 7, 1, 6, 0, '--verify'), true,
  (r, _run, fail) => {
    verifyCache(dirOf('r2'), state.files, fail);
    if (r && r.verifyBad !== 1) fail(`verifyBad ${r.verifyBad}, want 1`);
  });

mainServer.close();
mirrorServer.close();
mainServer.closeAllConnections?.();
mirrorServer.closeAllConnections?.();

if (rangeTiming.split !== undefined && rangeTiming.single !== undefined)
  console.log(`\nthrottled 3 MB file (64 KiB / 10 ms per connection): split ${rangeTiming.split} ms, single stream ${rangeTiming.single} ms`);
const failed = results.filter((r) => !r.ok).length;
console.log(`\n${results.length} scenario(s), ${failed} failed (expected ${EXPECTED_SCENARIOS})`);
const pass = failed === 0 && results.length === EXPECTED_SCENARIOS;
if (pass) fs.rmSync(root, { recursive: true, force: true });
else console.log(`test data kept in ${root}`);
console.log(pass ? 'PASS' : 'FAIL');
process.exit(pass ? 0 : 1);
