/* Temporary smoke test for dist/index.html (run with `node smoke.cjs`). */
const fs = require('fs');
const path = require('path');
const { JSDOM, VirtualConsole } = require('jsdom');

const target =
  process.argv[2] || path.join(__dirname, 'dist', 'index.html');
let html = fs.readFileSync(target, 'utf8');
// The single-file bundle is an ESM-free IIFE; jsdom doesn't execute
// `<script type="module">`. Downgrade the tag AND move the script to the
// end of <body>, because a plain <script> in <head> would run before
// #root exists (module scripts are deferred, plain ones are not).
const m = html.match(/<script type="module" crossorigin>([\s\S]*?)<\/script>/);
if (!m) throw new Error('bundle script tag not found');
html = html.split(m[0]).join('');
html = html.split('</body>').join('<script>' + m[1] + '</script></body>');

const vc = new VirtualConsole();
const errors = [];
vc.on('jsdomError', (e) => errors.push(String(e.message || e)));
vc.on('error', (...a) => errors.push(a.join(' ')));

const dom = new JSDOM(html, {
  runScripts: 'dangerously',
  resources: 'usable',
  pretendToBeVisual: true,
  virtualConsole: vc,
  url: 'https://localhost/',
});

const { window } = dom;

function waitFor(pred, timeout = 5000) {
  return new Promise((resolve, reject) => {
    const t0 = Date.now();
    const iv = setInterval(() => {
      let ok = false;
      try { ok = pred(); } catch {}
      if (ok) { clearInterval(iv); resolve(); }
      else if (Date.now() - t0 > timeout) { clearInterval(iv); reject(new Error('timeout')); }
    }, 50);
  });
}

window.addEventListener('error', (e) => errors.push(String(e.message)));

const root = () => window.document.getElementById('root');
// Scope all text to #root so we don't match the inline bundle's own source
// (the mock data strings live inside the <script>). Real rows contain
// buttons; skeleton rows do not, so this predicate proves the mock list
// (delivered ~500ms after `ready`) actually rendered.
waitFor(() => root() && root().children.length > 0, 6000)
  .then(() => waitFor(() =>
    root().querySelector('tbody tr button') !== null, 8000))
  .then(() => {
    const text = root().textContent;
    const checks = {
      'wordmark GAGE-MP': text.includes('GAGE-MP'),
      'tabs present': ['Browse', 'Favorites', 'Recents', 'Settings'].every((t) => text.includes(t)),
      'server rows rendered (mock)': text.includes('GAGE Europe #1'),
      'direct connect section': text.includes('Direct connect'),
      'launch section': text.includes('Launch'),
      'preview badge (mock gated)': text.includes('preview'),
      'ping cell rendered': /ms/.test(text),
      'no console errors': errors.length === 0,
    };
    let fail = false;
    for (const [k, v] of Object.entries(checks)) {
      console.log(`${v ? 'PASS' : 'FAIL'}  ${k}`);
      if (!v) fail = true;
    }
    if (errors.length) console.log('errors:', errors);
    window.close();
    process.exit(fail ? 1 : 0);
  })
  .catch((e) => {
    console.error('SMOKE FAIL:', e.message);
    if (errors.length) console.log('errors:', errors);
    process.exit(1);
  });
