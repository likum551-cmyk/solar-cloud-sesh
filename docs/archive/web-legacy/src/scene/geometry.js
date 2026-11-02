import * as THREE from 'three';
import { mergeGeometries } from 'three/addons/utils/BufferGeometryUtils.js';

const UP = new THREE.Vector3(0, 1, 0);
const v = (x, y, z) => new THREE.Vector3(x, y, z);

// A round steel member between two points.
function member(a, b, r, seg = 8) {
  const dir = new THREE.Vector3().subVectors(b, a);
  const len = dir.length();
  const geo = new THREE.CylinderGeometry(r, r, len, seg, 1, true);
  const q = new THREE.Quaternion().setFromUnitVectors(UP, dir.normalize());
  geo.applyQuaternion(q);
  const mid = new THREE.Vector3().addVectors(a, b).multiplyScalar(0.5);
  geo.translate(mid.x, mid.y, mid.z);
  return geo;
}

function box(w, h, d, x, y, z) {
  const geo = new THREE.BoxGeometry(w, h, d);
  geo.translate(x, y, z);
  return geo;
}

// Index-less geometries can't merge with indexed ones; normalise everything.
function merge(parts) {
  const norm = parts.map((g) => (g.index ? g.toNonIndexed() : g));
  for (const g of norm) {
    for (const k of Object.keys(g.attributes)) {
      if (k !== 'position' && k !== 'normal' && k !== 'uv') g.deleteAttribute(k);
    }
  }
  const out = mergeGeometries(norm, false);
  for (const g of parts) g.dispose();
  return out;
}

// Heights are real ones: the catwalk clears the lanes by 5 m, so the drive
// camera (eye 3.9 m) passes under it rather than through it.
export const GANTRY = {
  x0: 0,
  x1: 21.2,
  yb: 7.4,
  yt: 8.6,
  zf: 0.5,
  zb: -0.5,
  // catwalk deck: its 1 m front rail tops out at the exit plate's bottom edge
  // (SIGN_Y - SIGN_H / 2 = 5.85), so the rail never crosses the legend
  cwY: 4.85,
};

/** Galvanised box-truss sign bridge spanning one carriageway. */
export function buildGantryTruss() {
  const { x0, x1, yb, yt, zf, zb } = GANTRY;
  const p = [];
  const chord = 0.085;
  const lace = 0.05;
  for (const y of [yb, yt]) for (const z of [zf, zb]) p.push(member(v(x0, y, z), v(x1, y, z), chord));
  const n = 15;
  for (let i = 0; i <= n; i++) {
    const x = x0 + ((x1 - x0) * i) / n;
    p.push(member(v(x, yb, zf), v(x, yt, zf), lace));
    p.push(member(v(x, yb, zb), v(x, yt, zb), lace));
    p.push(member(v(x, yt, zf), v(x, yt, zb), lace));
    p.push(member(v(x, yb, zf), v(x, yb, zb), lace));
    if (i < n) {
      const xn = x0 + ((x1 - x0) * (i + 1)) / n;
      const up = i % 2 === 0;
      // Warren lacing, as heavy as the verticals: seen from below while
      // passing under, thin diagonals vanish and the truss reads as a grid
      p.push(member(v(x, up ? yb : yt, zf), v(xn, up ? yt : yb, zf), lace * 1.1));
      p.push(member(v(x, up ? yb : yt, zb), v(xn, up ? yt : yb, zb), lace * 1.1));
      p.push(member(v(x, yt, up ? zf : zb), v(xn, yt, up ? zb : zf), lace));
      p.push(member(v(x, yb, up ? zf : zb), v(xn, yb, up ? zb : zf), lace));
    }
  }
  // end towers: four legs with K-lacing, on base plates
  for (const xc of [x0, x1]) {
    const hw = 0.45;
    const top = yt + 0.12;
    const bottom = xc === x0 ? 0.81 : 0.0; // the median tower stands on the barrier
    const legs = [
      [xc - hw, zf],
      [xc + hw, zf],
      [xc + hw, zb],
      [xc - hw, zb],
    ];
    for (const [lx, lz] of legs) p.push(member(v(lx, bottom, lz), v(lx, top, lz), 0.075));
    const levels = 7;
    for (let k = 0; k <= levels; k++) {
      const y = bottom + 0.3 + ((top - bottom - 0.3) * k) / levels;
      for (let e = 0; e < 4; e++) {
        const [ax, az] = legs[e];
        const [bx, bz] = legs[(e + 1) % 4];
        p.push(member(v(ax, y, az), v(bx, y, bz), lace * 0.8));
        if (k < levels) {
          const yn = bottom + 0.3 + ((top - bottom - 0.3) * (k + 1)) / levels;
          p.push(member(v(ax, y, az), v(bx, yn, bz), lace * 0.7));
        }
      }
    }
    p.push(box(1.3, 0.05, 1.3, xc, bottom + 0.025, 0));
  }
  // maintenance catwalk under the main sign, with a front rail
  const { cwY } = GANTRY;
  const cwX0 = 1.2;
  const cwX1 = 18.6; // under both panels
  p.push(box(cwX1 - cwX0, 0.05, 0.8, (cwX0 + cwX1) / 2, cwY, zf + 0.55));
  p.push(member(v(cwX0, cwY + 1.0, zf + 0.92), v(cwX1, cwY + 1.0, zf + 0.92), 0.03));
  p.push(member(v(cwX0, cwY + 0.5, zf + 0.92), v(cwX1, cwY + 0.5, zf + 0.92), 0.022));
  for (let x = cwX0; x <= cwX1 + 0.01; x += 1.275) {
    p.push(member(v(x, cwY, zf + 0.92), v(x, cwY + 1.0, zf + 0.92), 0.022));
    // hangers from the truss
    p.push(member(v(x, cwY, zf + 0.2), v(x, yb, zf), 0.025));
  }
  // sign luminaires: short arms and slim shoebox housings tilted up at the
  // sign face, so from the road only their backs show
  for (const x of [3.0, 6.4, 9.8, 14.0, 16.0, 17.8]) {
    p.push(member(v(x, cwY, zf + 0.5), v(x, cwY - 0.03, zf + 1.05), 0.025));
    const hsg = new THREE.CylinderGeometry(0.12, 0.16, 0.42, 6, 1);
    hsg.rotateZ(Math.PI / 2);
    hsg.scale(1, 0.5, 1);
    hsg.rotateX(-0.85);
    hsg.translate(x, cwY - 0.02, zf + 1.1);
    p.push(hsg);
  }
  const geo = merge(p);
  trussGrime(geo, (x1 - x0) / n);
  return geo;
}

// Vertex-colour weathering for the truss: zinc dulls and darkens where
// members meet (water and dirt sit in the joints) and toward the footings
// (road spray), so the steel stops reading as one clean colour.
function trussGrime(geo, panel) {
  const pos = geo.attributes.position;
  const col = new Float32Array(pos.count * 3);
  const { x0, x1, yb, yt } = GANTRY;
  for (let i = 0; i < pos.count; i++) {
    const x = pos.getX(i);
    const y = pos.getY(i);
    // distance to the nearest panel point along the span, and to a chord
    const dNode = Math.abs(x - x0 - Math.round((x - x0) / panel) * panel);
    const dChord = Math.min(Math.abs(y - yb), Math.abs(y - yt));
    const inTower = Math.min(Math.abs(x - x0), Math.abs(x - x1)) < 0.6;
    let joint = 1 - Math.min(1, dNode / 0.32);
    joint *= 1 - Math.min(1, dChord / 0.3);
    // tower lacing levels have their own joints along the legs
    if (inTower) joint = Math.max(joint, 0.35);
    const spray = 1 - Math.min(1, y / 3.2);
    let k = 1 - 0.38 * joint - 0.45 * spray * spray;
    // the bottom chord catches more grime than the top
    if (Math.abs(y - yb) < 0.12) k *= 0.88;
    col[i * 3] = k * 1.02;
    col[i * 3 + 1] = k;
    col[i * 3 + 2] = k * 0.96;
  }
  geo.setAttribute('color', new THREE.BufferAttribute(col, 3));
}

// The steel slab behind one drawn plate: the plate's own outline (signs.js
// PLATES, canvas px mapped onto the face's metres), corners rounded to match,
// so no square corner shows behind a rounded plate.
function plateSlab(face, [x, y, w, h, r]) {
  const sx = face.w / face.cw;
  const sy = face.h / face.ch;
  const inset = 1; // px: just inside the plate's antialiased edge
  const hw = ((w - inset * 2) * sx) / 2;
  const hh = ((h - inset * 2) * sy) / 2;
  const pr = Math.min(r * Math.min(sx, sy), hw, hh);
  const s = new THREE.Shape();
  s.moveTo(-hw + pr, -hh);
  s.absarc(hw - pr, -hh + pr, pr, -Math.PI / 2, 0, false);
  s.absarc(hw - pr, hh - pr, pr, 0, Math.PI / 2, false);
  s.absarc(-hw + pr, hh - pr, pr, Math.PI / 2, Math.PI, false);
  s.absarc(-hw + pr, -hh + pr, pr, Math.PI, Math.PI * 1.5, false);
  const depth = 0.07;
  const geo = new THREE.ExtrudeGeometry(s, { depth, bevelEnabled: false, curveSegments: 6 });
  const cx = (x + w / 2) * sx - face.w / 2;
  const cy = face.h / 2 - (y + h / 2) * sy;
  geo.translate(cx, cy, -0.04 - depth / 2);
  return { geo, cx, bottom: cy - hh, width: hw * 2 };
}

// Back skin of a sign panel plus its stiffeners; the face is a textured plane.
// face: the face mesh's size in metres ({ w, h }) and its canvas in px ({ cw, ch }).
export function buildSignBack(face, rect) {
  const { geo, cx, bottom, width } = plateSlab(face, rect);
  const parts = [geo];
  // Z-bar stiffeners behind
  for (let i = 0; i < 3; i++) {
    parts.push(box(width * 0.98, 0.09, 0.09, cx, bottom + 0.6 + i * 1.1, -0.12));
  }
  return merge(parts);
}

// The exit tab's own backing, shown only when the sign carries an exit.
export function buildTabBack(face, rect) {
  return plateSlab(face, rect).geo;
}

/** Median pole with two cobra-head arms reaching over both carriageways. */
export function buildLampPole() {
  const p = [];
  p.push(new THREE.CylinderGeometry(0.085, 0.16, 11.4, 12, 1, true).translate(0, 0.81 + 5.7, 0));
  p.push(box(0.46, 0.06, 0.46, 0, 0.84, 0));
  for (const s of [1, -1]) {
    const curve = new THREE.QuadraticBezierCurve3(v(0, 11.6, 0), v(s * 1.6, 12.6, 0), v(s * 6.4, 12.35, 0));
    p.push(new THREE.TubeGeometry(curve, 18, 0.055, 8, false));
    const head = new THREE.SphereGeometry(0.5, 18, 10);
    head.scale(1.45, 0.3, 0.62);
    head.translate(s * 6.95, 12.28, 0);
    p.push(head);
  }
  return merge(p);
}

export function buildLampLens() {
  const p = [];
  for (const s of [1, -1]) {
    const lens = new THREE.SphereGeometry(0.42, 16, 6, 0, Math.PI * 2, Math.PI * 0.5, Math.PI * 0.5);
    lens.scale(1.3, 0.22, 0.5);
    lens.translate(s * 7.0, 12.2, 0);
    p.push(lens);
  }
  return merge(p);
}

/** Concrete safety-shape median barrier, extruded along the freeway. */
export function buildBarrier(zStart, length) {
  const s = new THREE.Shape();
  s.moveTo(-0.31, 0);
  s.lineTo(-0.31, 0.08);
  s.lineTo(-0.2, 0.33);
  s.lineTo(-0.085, 0.81);
  s.lineTo(0.085, 0.81);
  s.lineTo(0.2, 0.33);
  s.lineTo(0.31, 0.08);
  s.lineTo(0.31, 0);
  s.closePath();
  const geo = new THREE.ExtrudeGeometry(s, { depth: length, bevelEnabled: false, steps: 1 });
  geo.translate(0, 0, zStart);
  return geo;
}

/** W-beam guardrail strip (the beam only; posts are instanced). */
export function buildGuardrail(x, zStart, length, facing) {
  const s = new THREE.Shape();
  const d = 0.05 * facing;
  s.moveTo(0, 0.48);
  s.lineTo(d, 0.53);
  s.lineTo(0, 0.6);
  s.lineTo(d, 0.67);
  s.lineTo(0, 0.79);
  s.lineTo(-0.02 * facing, 0.79);
  s.lineTo(-0.02 * facing, 0.48);
  s.closePath();
  const geo = new THREE.ExtrudeGeometry(s, { depth: length, bevelEnabled: false, steps: 1 });
  geo.translate(x, 0, zStart);
  return geo;
}

// One drooping frond: a ribbon that leaves the crown climbing, arcs over and
// hangs, widest a third of the way out and tapering to a point.
function frond(top, yaw, len, lift, droop, width) {
  const seg = 7;
  const dir = v(Math.cos(yaw), 0, Math.sin(yaw));
  const side = v(-Math.sin(yaw), 0, Math.cos(yaw));
  const pos = [];
  const nrm = [];
  const uv = [];
  const idx = [];
  for (let i = 0; i <= seg; i++) {
    const s = i / seg;
    const c = top
      .clone()
      .addScaledVector(dir, s * len)
      .add(v(0, len * (lift * s - droop * s * s), 0));
    const w = width * Math.sin(Math.PI * Math.pow(s, 0.75));
    for (const k of [-1, 1]) {
      const p = c.clone().addScaledVector(side, (k * w) / 2);
      // leaflets fold into a shallow V along the rib
      p.y -= Math.abs(k) * w * 0.18;
      pos.push(p.x, p.y, p.z);
      nrm.push(0, 1, 0);
      uv.push(k < 0 ? 0 : 1, s);
    }
    if (i < seg) {
      const a = i * 2;
      idx.push(a, a + 1, a + 2, a + 1, a + 3, a + 2);
    }
  }
  const g = new THREE.BufferGeometry();
  g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
  g.setAttribute('normal', new THREE.Float32BufferAttribute(nrm, 3));
  g.setAttribute('uv', new THREE.Float32BufferAttribute(uv, 2));
  g.setIndex(idx);
  return g;
}

/**
 * Tall coastal fan palm, about 17 m: a slim trunk with a slight lean and a
 * swollen base, a skirt of dead fronds under a dense drooping crown. Origin
 * at the base; instances scale and turn it.
 */
export function buildPalm() {
  const p = [];
  const H = 16.5;
  const lean = 0.9;
  const at = (t) => v(lean * t * t, t * H, 0);
  const seg = 7;
  for (let i = 0; i < seg; i++) {
    const t0 = i / seg;
    const t1 = (i + 1) / seg;
    const a = at(t0);
    const b = at(t1);
    const r = (t) => 0.17 + 0.16 * Math.exp(-t * 18);
    const dir = new THREE.Vector3().subVectors(b, a);
    const geo = new THREE.CylinderGeometry(r(t1), r(t0), dir.length(), 7, 1, true);
    geo.applyQuaternion(new THREE.Quaternion().setFromUnitVectors(UP, dir.normalize()));
    const mid = new THREE.Vector3().addVectors(a, b).multiplyScalar(0.5);
    geo.translate(mid.x, mid.y, mid.z);
    p.push(geo);
  }
  const top = at(1);
  // skirt of dead fronds hugging the trunk below the crown
  p.push(new THREE.CylinderGeometry(0.62, 0.3, 2.2, 9, 1, true).translate(top.x, top.y - 1.2, top.z));
  const n = 15;
  for (let i = 0; i < n; i++) {
    const yaw = (i / n) * Math.PI * 2 + (i % 2) * 0.21;
    const upper = i % 3 === 0;
    p.push(frond(top, yaw, upper ? 3.4 : 3.9, upper ? 1.05 : 0.7, upper ? 1.35 : 1.6, 1.1));
  }
  return merge(p);
}

/** Distant ridge silhouette behind the city. */
export function buildRidge() {
  // peaks sit in the middle 3600 m; the plane runs far wider so the skycam's
  // wide aerial never sees its ends
  const w = 3600;
  const seg = 420;
  const geo = new THREE.PlaneGeometry(w * 2.5, 1, seg, 1);
  const pos = geo.attributes.position;
  let seed = 4242;
  const rnd = () => {
    seed = (seed * 16807) % 2147483647;
    return seed / 2147483647;
  };
  const peaks = Array.from({ length: 9 }, () => ({ x: (rnd() - 0.5) * w, h: 60 + rnd() * 120, s: 160 + rnd() * 340 }));
  for (let i = 0; i < pos.count; i++) {
    const x = pos.getX(i);
    const top = pos.getY(i) > 0;
    if (!top) {
      pos.setY(i, -20);
      continue;
    }
    let h = 28 + Math.sin(x * 0.004) * 10 + Math.sin(x * 0.013 + 1.3) * 5;
    for (const pk of peaks) h = Math.max(h, pk.h * Math.exp(-((x - pk.x) ** 2) / (2 * pk.s * pk.s)) + 14);
    pos.setY(i, h + (rnd() - 0.5) * 3);
  }
  geo.computeVertexNormals();
  return geo;
}
