// Renders the share card and the touch icon into public/ with headless Chromium over raw CDP.
// usage (Node 22+, from anywhere): node apps/website/scripts/render-og.mjs
// Chromium: $CHROME, else the Playwright cache. Serves the repo root on a throwaway 127.0.0.1 port,
// because Chromium refuses @font-face over file://.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../../..');
const PUBLIC = path.resolve(HERE, '../public');
const JOBS = [
  { page: '/apps/website/scripts/og/og.html', sel: '.card', w: 1200, h: 630, out: 'og.png' },
  { page: '/apps/website/scripts/og/icon.html', sel: '.tile', w: 180, h: 180, out: 'apple-touch-icon.png' },
];

function findChrome() {
  if (process.env.CHROME && fs.existsSync(process.env.CHROME)) return process.env.CHROME;
  const base = path.join(os.homedir(), 'AppData/Local/ms-playwright');
  if (fs.existsSync(base)) {
    for (const d of fs.readdirSync(base).filter((n) => n.startsWith('chromium-')).sort().reverse()) {
      const exe = path.join(base, d, 'chrome-win/chrome.exe');
      if (fs.existsSync(exe)) return exe;
    }
  }
  throw new Error('no Chromium: set CHROME to a chrome.exe');
}

const MIME = { '.html': 'text/html', '.svg': 'image/svg+xml', '.webp': 'image/webp', '.png': 'image/png', '.woff2': 'font/woff2' };
const srv = http.createServer((req, res) => {
  const p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const f = path.join(ROOT, p);
  if (!f.startsWith(ROOT + path.sep) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': MIME[path.extname(f).toLowerCase()] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(res);
});
await new Promise((r) => srv.listen(0, '127.0.0.1', r));
const BASE = `http://127.0.0.1:${srv.address().port}`;
const PORT = 9300 + Math.floor(Math.random() * 400);
const udd = fs.mkdtempSync(path.join(os.tmpdir(), 'og-'));
const chrome = spawn(findChrome(), ['--headless=new', `--remote-debugging-port=${PORT}`, `--user-data-dir=${udd}`, '--no-first-run', '--hide-scrollbars', 'about:blank'], { stdio: 'ignore' });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let code = 0;
try {
  let up = false;
  for (let i = 0; i < 60 && !up; i++) { try { await (await fetch(`http://127.0.0.1:${PORT}/json/version`)).json(); up = true; } catch { await sleep(250); } }
  if (!up) throw new Error('chrome did not start');
  const t = await (await fetch(`http://127.0.0.1:${PORT}/json/new?about:blank`, { method: 'PUT' })).json();
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  let id = 0; const pending = new Map();
  ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pending.has(d.id)) { const { r, j } = pending.get(d.id); pending.delete(d.id); d.error ? j(new Error(JSON.stringify(d.error))) : r(d.result); } };
  const send = (method, params = {}) => new Promise((r, j) => { const i = ++id; pending.set(i, { r, j }); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true }); if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 300)); return r.result.value; };
  await send('Page.enable');
  await send('Runtime.enable');
  for (const job of JOBS) {
    await send('Emulation.setDeviceMetricsOverride', { width: job.w, height: job.h, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: BASE + job.page });
    for (let i = 0; i < 60; i++) { await sleep(100); if (await ev(`document.readyState === 'complete'`)) break; }
    // every face and image must be really loaded, or the card silently ships with a fallback font
    const ready = await ev(`(async () => {
      await document.fonts.ready;
      const faces = [...document.fonts].map((f) => f.family + ':' + f.status);
      const imgs = [...document.images];
      await Promise.all(imgs.map((i) => i.decode().catch(() => null)));
      const bg = getComputedStyle(document.querySelector('${job.sel}')).backgroundImage;
      return { faces, imgs: imgs.map((i) => i.naturalWidth), bg, box: document.querySelector('${job.sel}').getBoundingClientRect().toJSON() };
    })()`);
    const usedFaces = await ev(`Promise.all([...new Set([...document.querySelectorAll('*')].map((e) => getComputedStyle(e).fontFamily.split(',')[0].trim().replace(/['"]/g, '')))].filter((f) => f === 'XC' || f === 'Sofia').map((f) => document.fonts.load('700 20px ' + f).then((l) => f + ':' + l.length)))`);
    if (usedFaces.some((f) => f.endsWith(':0'))) throw new Error(`${job.out}: a face did not load ${usedFaces}`);
    if (ready.imgs.some((w) => !w)) throw new Error(`${job.out}: an image did not load`);
    if (Math.round(ready.box.width) !== job.w || Math.round(ready.box.height) !== job.h) throw new Error(`${job.out}: box ${ready.box.width}x${ready.box.height}`);
    const shot = await send('Page.captureScreenshot', { format: 'png', clip: { x: 0, y: 0, width: job.w, height: job.h, scale: 1 } });
    fs.writeFileSync(path.join(PUBLIC, job.out), Buffer.from(shot.data, 'base64'));
    console.log(`${job.out} ${job.w}x${job.h} faces=${usedFaces.join(' ') || 'none'} imgs=${ready.imgs.join(',') || 'none'}`);
  }
  ws.close();
} catch (e) {
  console.error(e.message);
  code = 1;
} finally {
  chrome.kill();
  srv.close();
}
process.exit(code);
