// Overhead guide signs, drawn to canvases and used as textures on the
// gantries. Original artwork: generic freeway sign grammar (green plate,
// inset white border, exit tab, legend, arrow), no real-world route marks.

export const SIGN_CANVAS_W = 1280;
export const SIGN_CANVAS_H = 640;
// Physical size of the sign mesh in metres (plate + exit tab above it).
export const SIGN_W = 8.6;
export const SIGN_H = 4.3;

const TAB_H = 150; // px, exit tab band at the top of the canvas
const TAB_W = 420;
const SEAM = Math.round((SIGN_CANVAS_H / SIGN_H) * 0.305); // px per 12-inch panel
const GREEN = '#0d6b42';
const GREEN_DEEP = '#0a5a37';
const WHITE = '#f2f1ea';
const FONT = "'Overpass', 'Bahnschrift', 'Segoe UI', sans-serif";

let speckle = null;
function speckleCanvas() {
  if (speckle) return speckle;
  // Retroreflective sheeting has a fine prismatic grain; fake it with a
  // seeded speckle so every sign carries the same material.
  const c = document.createElement('canvas');
  c.width = 160;
  c.height = 160;
  const g = c.getContext('2d');
  let seed = 1337;
  const rnd = () => {
    seed = (seed * 16807) % 2147483647;
    return seed / 2147483647;
  };
  for (let i = 0; i < 2600; i++) {
    const v = rnd();
    g.fillStyle = v > 0.5 ? 'rgba(255,255,255,0.05)' : 'rgba(0,0,0,0.07)';
    g.fillRect(rnd() * 160, rnd() * 160, 1 + rnd() * 1.5, 1 + rnd() * 1.5);
  }
  speckle = c;
  return c;
}

function rr(g, x, y, w, h, r) {
  g.beginPath();
  g.roundRect(x, y, w, h, r);
}

function plate(g, x, y, w, h, radius) {
  rr(g, x, y, w, h, radius);
  const grad = g.createLinearGradient(0, y, 0, y + h);
  // lit from the luminaires on the catwalk below
  grad.addColorStop(0, GREEN_DEEP);
  grad.addColorStop(1, GREEN);
  g.fillStyle = grad;
  g.fill();
  g.save();
  rr(g, x, y, w, h, radius);
  g.clip();
  g.fillStyle = g.createPattern(speckleCanvas(), 'repeat');
  g.fillRect(x, y, w, h);
  // Guide signs are stacked 12-inch extruded aluminium panels: a faint seam
  // (shadowed joint, lit lip below it) every SEAM px, about 0.3 m.
  for (let sy = y + SEAM; sy < y + h - 8; sy += SEAM) {
    g.fillStyle = 'rgba(0,0,0,0.16)';
    g.fillRect(x, sy - 1, w, 2);
    g.fillStyle = 'rgba(255,255,255,0.05)';
    g.fillRect(x, sy + 1, w, 1);
  }
  // faint road grime settling along the bottom edge
  const grime = g.createLinearGradient(0, y + h * 0.7, 0, y + h);
  grime.addColorStop(0, 'rgba(20,16,10,0)');
  grime.addColorStop(1, 'rgba(20,16,10,0.22)');
  g.fillStyle = grime;
  g.fillRect(x, y, w, h);
  g.restore();
  const inset = 14;
  rr(g, x + inset, y + inset, w - inset * 2, h - inset * 2, Math.max(4, radius - 10));
  g.lineWidth = 9;
  g.strokeStyle = WHITE;
  g.stroke();
}

function fitFont(g, text, weight, maxSize, minSize, maxWidth, spacing = 0) {
  for (let size = maxSize; size >= minSize; size -= 4) {
    g.font = `${weight} ${size}px ${FONT}`;
    g.letterSpacing = `${spacing}px`;
    if (g.measureText(text).width <= maxWidth) return size;
  }
  return 0;
}

function ellipsize(g, text, maxWidth) {
  if (g.measureText(text).width <= maxWidth) return text;
  let s = text;
  while (s.length > 1 && g.measureText(`${s}…`).width > maxWidth) s = s.slice(0, -1);
  return `${s.trimEnd()}…`;
}

// Split a legend into two balanced lines at a space, if it has one.
function splitTwo(text) {
  const words = text.split(/\s+/);
  if (words.length < 2) return null;
  let best = null;
  for (let i = 1; i < words.length; i++) {
    const a = words.slice(0, i).join(' ');
    const b = words.slice(i).join(' ');
    const score = Math.abs(a.length - b.length);
    if (!best || score < best.score) best = { a, b, score };
  }
  return [best.a, best.b];
}

// diagonal "take this exit" arrow, pointing up and to the right
function exitArrow(g, cx, cy, s) {
  angledArrow(g, cx, cy, s, Math.PI / 4);
}

// The same arrow at any heading (radians clockwise from straight up).
function angledArrow(g, cx, cy, s, angle) {
  g.save();
  g.translate(cx, cy);
  g.rotate(angle);
  g.fillStyle = WHITE;
  g.beginPath();
  const shaft = s * 0.17;
  g.moveTo(-shaft, s * 0.62);
  g.lineTo(-shaft, -s * 0.12);
  g.lineTo(-s * 0.42, -s * 0.12);
  g.lineTo(0, -s * 0.66);
  g.lineTo(s * 0.42, -s * 0.12);
  g.lineTo(shaft, -s * 0.12);
  g.lineTo(shaft, s * 0.62);
  g.closePath();
  g.fill();
  g.restore();
}

function downArrow(g, cx, cy, s) {
  g.save();
  g.translate(cx, cy);
  g.fillStyle = WHITE;
  g.beginPath();
  const shaft = s * 0.15;
  g.moveTo(-shaft, -s * 0.55);
  g.lineTo(shaft, -s * 0.55);
  g.lineTo(shaft, s * 0.05);
  g.lineTo(s * 0.38, s * 0.05);
  g.lineTo(0, s * 0.55);
  g.lineTo(-s * 0.38, s * 0.05);
  g.lineTo(-shaft, s * 0.05);
  g.closePath();
  g.fill();
  g.restore();
}

// Work-zone panel bolted over the arrow after a failed launch: the exit
// exists, it just could not be taken this time.
function closedPanel(g, W, plateY, plateH) {
  const w = 500;
  const h = 150;
  const x = W - w - 36;
  const y = plateY + plateH * 0.55 - h / 2;
  g.save();
  g.shadowColor = 'rgba(0,0,0,0.45)';
  g.shadowBlur = 18;
  g.shadowOffsetY = 6;
  rr(g, x, y, w, h, 16);
  g.fillStyle = '#e07b1f';
  g.fill();
  g.restore();
  g.save();
  rr(g, x, y, w, h, 16);
  g.clip();
  g.fillStyle = g.createPattern(speckleCanvas(), 'repeat');
  g.fillRect(x, y, w, h);
  g.restore();
  rr(g, x + 10, y + 10, w - 20, h - 20, 9);
  g.lineWidth = 7;
  g.strokeStyle = '#141210';
  g.stroke();
  g.fillStyle = '#141210';
  g.textAlign = 'center';
  const size = fitFont(g, 'EXIT CLOSED', 800, 78, 50, w - 70, 4) || 50;
  g.font = `800 ${size}px ${FONT}`;
  g.letterSpacing = '4px';
  g.fillText('EXIT CLOSED', x + w / 2, y + h / 2 + size * 0.36);
  g.textAlign = 'left';
  g.fillStyle = WHITE;
}

/**
 * content:
 *   { kind: 'server', name, host, port, players, ping, closed? }
 *   { kind: 'prompt', title, sub, aim? }  aim 'right': one down-right arrow
 */
export function drawSign(canvas, content) {
  const g = canvas.getContext('2d');
  const W = canvas.width;
  const H = canvas.height;
  g.clearRect(0, 0, W, H);
  g.textBaseline = 'alphabetic';
  g.fillStyle = WHITE;

  const [, plateY, , plateH] = PLATES.main;

  if (content.kind === 'server') {
    // exit tab, right-aligned on top of the plate
    const tabW = TAB_W;
    plate(g, ...PLATES.tab);
    g.fillStyle = WHITE;
    g.font = `700 50px ${FONT}`;
    g.letterSpacing = '6px';
    const exitW = g.measureText('EXIT').width;
    const port = String(content.port || '');
    const pSize = fitFont(g, port, 800, 92, 50, tabW - exitW - 110, 2) || 50;
    g.font = `800 ${pSize}px ${FONT}`;
    g.letterSpacing = '2px';
    const portW = g.measureText(port).width;
    const total = exitW + 24 + portW;
    const tx = W - tabW - 6 + (tabW - total) / 2;
    g.font = `700 50px ${FONT}`;
    g.letterSpacing = '6px';
    g.fillText('EXIT', tx, 108);
    g.font = `800 ${pSize}px ${FONT}`;
    g.letterSpacing = '2px';
    g.fillText(port, tx + exitW + 24, 112);
  }

  plate(g, ...PLATES.main);
  g.fillStyle = WHITE;

  if (content.kind === 'server') {
    const left = 84;
    const arrowZone = 230;
    const maxW = W - left - arrowZone - 40;
    const name = (content.name || content.host || '').trim();
    let lines = [name];
    let size = fitFont(g, name, 800, 170, 104, maxW, 1);
    if (!size) {
      const two = splitTwo(name);
      if (two) {
        size = Math.min(
          fitFont(g, two[0], 800, 120, 72, maxW, 1) || 72,
          fitFont(g, two[1], 800, 120, 72, maxW, 1) || 72,
        );
        lines = two;
      } else {
        size = 104;
      }
    }
    // "distance" line: live facts the launcher actually knows
    const facts = [];
    if (content.players >= 0) facts.push(`${content.players} PLAYERS`);
    if (content.ping >= 0) facts.push(`${content.ping} MS`);
    // the list says "no reply" for it: an exit that may lead nowhere says so
    else if (content.noReply) facts.push('NO REPLY');
    const hasAddress = name !== (content.host || '').trim();

    g.font = `800 ${size}px ${FONT}`;
    g.letterSpacing = '1px';
    const lineH = size * 0.98;
    // with no facts line the legend and address sit centred on the plate,
    // not top-weighted over an empty foot
    const blockH = size * 0.78 + (lines.length - 1) * lineH + (hasAddress ? 84 : 0);
    const blockTop = facts.length ? plateY + 86 : plateY + (plateH - blockH) / 2;
    lines.forEach((ln, i) => {
      g.fillText(ellipsize(g, ln, maxW), left, blockTop + size * 0.78 + i * lineH);
    });
    const afterLegend = blockTop + size * 0.78 + (lines.length - 1) * lineH;

    // address line, unless the legend already is the address
    if (hasAddress) {
      g.font = `600 52px ${FONT}`;
      g.letterSpacing = '4px';
      g.globalAlpha = 0.92;
      g.fillText(ellipsize(g, (content.host || '').toUpperCase(), maxW), left, afterLegend + 84);
      g.globalAlpha = 1;
    }

    if (facts.length) {
      g.font = `700 50px ${FONT}`;
      g.letterSpacing = '5px';
      // well inside the bottom border: the catwalk rail tops out at the plate edge
      g.fillText(facts.join('   '), left, H - 78);
    }
    // a closed exit's panel is bolted over where the arrow points
    if (content.closed) closedPanel(g, W, plateY, plateH);
    else exitArrow(g, W - 160, plateY + plateH * 0.55, 230);
  } else {
    const title = (content.title || '').toUpperCase();
    const size = fitFont(g, title, 800, 128, 64, W - 200, 3) || 64;
    g.font = `800 ${size}px ${FONT}`;
    g.letterSpacing = '3px';
    g.textAlign = 'center';
    g.fillText(ellipsize(g, title, W - 200), W / 2, plateY + 80 + size * 0.8);
    if (content.sub) {
      g.font = `600 50px ${FONT}`;
      g.letterSpacing = '3px';
      g.globalAlpha = 0.92;
      g.fillText(ellipsize(g, content.sub, W - 220), W / 2, plateY + 80 + size * 0.8 + 86);
      g.globalAlpha = 1;
    }
    g.textAlign = 'left';
    if (content.aim === 'right') {
      // one arrow down and to the right: toward direct connect, not the list
      // sized and lifted to stay inside the plate's white border
      angledArrow(g, W / 2, H - 114, 128, (Math.PI * 3) / 4);
    } else {
      downArrow(g, W / 2 - 190, H - 104, 120);
      downArrow(g, W / 2 + 190, H - 104, 120);
    }
  }
}

// The through-route panel beside the exit sign: plate only, no tab.
export const THRU_CANVAS_W = 680; // 4.6 m wide, same px per metre as the exit sign
export const THRU_CANVAS_H = 500;
// its plate is as tall as the exit sign's plate
export const THRU_H = SIGN_H * ((SIGN_CANVAS_H - (TAB_H - 16) - 4) / SIGN_CANVAS_H);

// Each drawn plate's outline in canvas px: [x, y, w, h, corner radius]. The
// drawing uses these, and so does the steel behind each face (geometry.js),
// which must hide behind the plate, rounded corners and all.
export const PLATES = {
  tab: [SIGN_CANVAS_W - TAB_W - 6, 4, TAB_W, TAB_H + 10, 26],
  main: [4, TAB_H - 16, SIGN_CANVAS_W - 8, SIGN_CANVAS_H - (TAB_H - 16) - 4, 34],
  thru: [4, 4, THRU_CANVAS_W - 8, THRU_CANVAS_H - 8, 34],
};

export function drawThruSign(canvas, { title, sub }) {
  const g = canvas.getContext('2d');
  const W = canvas.width;
  const H = canvas.height;
  g.clearRect(0, 0, W, H);
  g.textBaseline = 'alphabetic';
  plate(g, ...PLATES.thru);
  g.fillStyle = WHITE;
  g.textAlign = 'center';
  const words = title.toUpperCase().split(' ');
  const lines = words.length > 1 ? [words.slice(0, -1).join(' '), words[words.length - 1]] : words;
  // Laid out from the bottom up: the arrow stands 30 px clear of the inner
  // border, the subtitle 28 px above the arrow's tip, and the title shrinks
  // until its last line clears the subtitle.
  const ARROW = 84;
  const arrowY = H - 4 - 14 - 30 - ARROW * 0.62;
  const subBase = arrowY - ARROW * 0.66 - 28;
  const lastBase = sub ? subBase - 66 : subBase;
  const TOP = 60;
  const fitH = Math.floor((lastBase - TOP) / (0.8 + 0.98 * (lines.length - 1)));
  const size = Math.min(fitH, ...lines.map((l) => fitFont(g, l, 800, 112, 60, W - 120, 3) || 60));
  g.font = `800 ${size}px ${FONT}`;
  g.letterSpacing = '3px';
  lines.forEach((l, i) => g.fillText(l, W / 2, TOP + size * 0.8 + i * size * 0.98));
  if (sub) {
    g.font = `600 46px ${FONT}`;
    g.letterSpacing = '3px';
    g.globalAlpha = 0.92;
    g.fillText(ellipsize(g, sub, W - 120), W / 2, subBase);
    g.globalAlpha = 1;
  }
  g.textAlign = 'left';
  // straight on: one arrow per lane under the panel
  angledArrow(g, W / 2, arrowY, ARROW, 0);
}

// Overpass ships one face per script subset, each loaded on first use. Canvas
// text painted before its subset arrives is drawn in a fallback face and is
// never repainted, so sign painters wait for the faces their text needs.
const SIGN_FACES = ["800 100px 'Overpass'", "700 50px 'Overpass'", "600 50px 'Overpass'"];
const legendText = (content) =>
  [content?.name, content?.host, content?.title, content?.sub, 'EXIT PLAYERS MS 0123456789'].filter(Boolean).join(' ');

export function signFacesReady(content) {
  const text = legendText(content);
  return SIGN_FACES.every((f) => document.fonts.check(f, text));
}

// Never rejects: a face that fails to load leaves the fallback face in use.
export function loadSignFaces(content) {
  const text = legendText(content);
  return Promise.all(SIGN_FACES.map((f) => document.fonts.load(f, text))).then(
    () => {},
    (err) => console.warn('sign face not loaded', err),
  );
}

export function signContent(server) {
  if (!server) return null;
  return {
    kind: 'server',
    name: server.name || server.host,
    host: server.host,
    port: server.port,
    players: Number.isFinite(server.players) ? server.players : -1,
    ping: Number.isFinite(server.ping) ? server.ping : -1,
  };
}
