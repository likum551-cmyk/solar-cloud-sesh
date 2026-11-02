// Render the setup wizard artwork (wizard.html) to the PNG sizes Inno Setup
// 6.6+ picks from per DPI. No npm dependencies: drives a Chromium over the raw
// DevTools protocol (Node 22+ for the built-in WebSocket). The browser is
// $CHROME, else Playwright's Chromium under %LOCALAPPDATA%\ms-playwright.
// Usage, from anywhere: node installer/art/render.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '..', '..');
const OUT = path.join(HERE, 'out');

// WizardImageFile / WizardSmallImageFile areas at 100..250% (Inno Setup help)
const SIDE = [[202, 386], [269, 515], [336, 643], [403, 772], [430, 824], [498, 953], [534, 1022]];
const MARK = [58, 77, 97, 116, 124, 143, 159];

function findChrome() {
  if (process.env.CHROME) return process.env.CHROME;
  const base = path.join(os.homedir(), 'AppData', 'Local', 'ms-playwright');
  const dirs = fs.existsSync(base) ? fs.readdirSync(base).filter((d) => /^chromium-\d+$/.test(d)).sort().reverse() : [];
  for (const d of dirs) {
    const exe = path.join(base, d, 'chrome-win', 'chrome.exe');
    if (fs.existsSync(exe)) return exe;
  }
  return null;
}
const CHROME = findChrome();
if (!CHROME) { console.error('no Chromium: set CHROME=<path to chrome.exe>'); process.exit(2); }
if (typeof WebSocket !== 'function') { console.error('Node 22+ needed (global WebSocket)'); process.exit(2); }

const TYPES = { '.html': 'text/html; charset=utf-8', '.svg': 'image/svg+xml', '.webp': 'image/webp' };
const srv = http.createServer((req, res) => {
  const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const file = path.resolve(ROOT, '.' + rel);
  if (!file.startsWith(ROOT + path.sep) || !fs.existsSync(file)) {
    res.writeHead(404).end();
    return;
  }
  res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => srv.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${srv.address().port}/installer/art/wizard.html`;

const PORT = 9400 + Math.floor(Math.random() * 400);
const udd = fs.mkdtempSync(path.join(os.tmpdir(), 'wizart-'));
const chrome = spawn(CHROME, ['--headless=new', `--remote-debugging-port=${PORT}`, `--user-data-dir=${udd}`, '--no-first-run', '--hide-scrollbars', '--force-device-scale-factor=1', 'about:blank'], { stdio: 'ignore' });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const jobs = [
  ...SIDE.map(([w, h]) => ({ kind: 'side', w, h, name: `wizard-${w}x${h}.png` })),
  ...MARK.map((s) => ({ kind: 'mark', w: s, h: s, name: `mark-${s}.png` })),
];
let written = 0;
try {
  let up = false;
  for (let i = 0; i < 60 && !up; i++) { try { await (await fetch(`http://127.0.0.1:${PORT}/json/version`)).json(); up = true; } catch { await sleep(250); } }
  if (!up) throw new Error('Chromium did not start');
  const t = await (await fetch(`http://127.0.0.1:${PORT}/json/new?about:blank`, { method: 'PUT' })).json();
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  let id = 0;
  const pending = new Map();
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) { const { r, j } = pending.get(d.id); pending.delete(d.id); d.error ? j(new Error(JSON.stringify(d.error))) : r(d.result); }
  };
  const send = (method, params = {}) => new Promise((r, j) => { const i = ++id; pending.set(i, { r, j }); ws.send(JSON.stringify({ id: i, method, params })); });
  await send('Page.enable');
  await send('Runtime.enable');
  fs.mkdirSync(OUT, { recursive: true });
  for (const j of jobs) {
    // The mark is drawn on a transparent page (the header is light or dark per Windows theme).
    const alpha = j.kind === 'mark' ? 0 : 1;
    await send('Emulation.setDefaultBackgroundColorOverride', { color: { r: 0, g: 0, b: 0, a: alpha } });
    await send('Emulation.setDeviceMetricsOverride', { width: j.w, height: j.h, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: `${base}?kind=${j.kind}` });
    let ready = false;
    for (let i = 0; i < 80 && !ready; i++) {
      await sleep(50);
      try {
        const r = await send('Runtime.evaluate', { expression: `(async () => { if (!window.ready) return false; await window.ready; return innerWidth === ${j.w} && innerHeight === ${j.h}; })()`, awaitPromise: true, returnByValue: true });
        ready = r.result.value === true;
      } catch { /* still navigating */ }
    }
    if (!ready) throw new Error(`${j.name}: page not ready`);
    const shot = await send('Page.captureScreenshot', { format: 'png', clip: { x: 0, y: 0, width: j.w, height: j.h, scale: 1 } });
    fs.writeFileSync(path.join(OUT, j.name), Buffer.from(shot.data, 'base64'));
    written++;
  }
  ws.close();
} catch (e) {
  console.error(String((e && e.stack) || e));
} finally {
  chrome.kill();
  srv.close();
}
if (written !== jobs.length) {
  console.error(`rendered ${written}/${jobs.length}`);
  process.exit(1);
}
console.log(`rendered ${written} images into ${OUT}`);
