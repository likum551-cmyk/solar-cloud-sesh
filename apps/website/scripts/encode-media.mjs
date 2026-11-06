// Encodes the site's own media from the owner's local folders (never committed, only these derivatives are):
//   fresh_videos/   one continuous 15 s, 1920x1080 AV1 drone shot each
//   fresh_images/   official GTA V artwork, 2880x1800 (2560x1600 for one), with the GTA V logo burned in at
//                   the bottom left and the Rockstar badge at the bottom right
//
// GI, 2026-11-06: "too many videos and it's a bit laggy; use better quality images". So:
//   - ONE video, the hero: one shot played forward and then backward (a boomerang, both turnarounds eased),
//     so it loops with no seam and no dissolve; the 3D S stands in the plate its light is taken from.
//   - stills for the title cards and the close, taken straight from the source recordings (not from the
//     web encodes), in the same warm grade;
//   - the cast and the server cards cropped from the official artwork. Every crop is checked against the
//     logo and badge boxes and refused if it touches one (no Rockstar or GTA V marks on the site).
//
// Outputs in public/media:
//   hero.webm (VP9, two-pass), hero.mp4 (H.264, +faststart), hero.webp (poster: frame 0, also the S's light)
//   still-<name>.webp (1920x1080), cast-<name>.webp and server-<name>.webp (the crop at source resolution)
//   node scripts/encode-media.mjs            make what is missing
//   node scripts/encode-media.mjs --force    make everything again
//   node scripts/encode-media.mjs --dry-run  print the plan only
// Needs ffmpeg (FFMPEG=path, else the repo toolchain in .toolchain/ffmpeg).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SITE = path.resolve(HERE, '..');
const REPO = path.resolve(SITE, '..', '..');
const SRC = path.join(REPO, 'fresh_videos');
const ART_SRC = path.join(REPO, 'fresh_images');
const OUT = path.join(SITE, 'public', 'media');
const force = process.argv.includes('--force');
const dry = process.argv.includes('--dry-run');

// The shared grade: blue and magenta out of the highlights, warm mid-tones, shadows kept neutral (a
// shadow push turned the canal water olive). Measured on a frame of each shot: the skyline's sky went
// from RGB 199 187 212 (lavender) to 208 195 172, the canal's water stays slate.
const WARM = 'colorbalance=rs=0.015:gs=-0.015:bs=-0.015:rm=0.06:gm=0.01:bm=-0.07:rh=0.06:gh=0.04:bh=-0.2,eq=saturation=1.05';

// The night shots keep their own colour: WARM would push the pool's blue to teal.
const NIGHT = 'eq=saturation=1.0';
// The observatory's floodlit lawns were the one cold, saturated green in a warm page: pulled toward olive.
// Measured at a lit lawn (x 1500, y 800): RGB 58 72 15 -> 58 62 15; a city light (1290, 360) 253 251 176 ->
// 253 240 176. (huesaturation on g+c / y+g moved neither by more than 1.)
const LAWNS = `${WARM},selectivecolor=greens=0 0.5 0 0:yellows=0 0.15 0 0`;

// in-points match apps/launcher-ui/ASSETS.md; `len` is the forward half (the loop is twice that)
const CLIPS = {
  hero: { file: 'fon_25m38', in: 0.2, len: 10, grade: WARM }, // downtown skyline at dusk, the city lit
};
// title cards and the close: one frame each, picked from a sample of the shot. The Vinewood sign shot
// (fon_0m00) was dropped after review: it shows the backs of the letters, which read as rusty billboards.
const STILLS = {
  beach: { file: 'fon_4m05', at: 6, grade: WARM }, // Vespucci Beach at sunset, the pier wheel
  pool: { file: 'fon_2m58', at: 9.8, grade: NIGHT }, // a lit pool straight down at night, dark terrace round it
  canal: { file: 'fon_1m46', at: 3, grade: WARM }, // down the river canal to Downtown, golden hour
  close: { file: 'fon_20m27', at: 0.2, grade: LAWNS }, // the Galileo Observatory at night, the city lit
};
// Artwork crops in source pixels (x, y, w, h). The marks, as fractions of the frame (measured on all ten):
// the GTA V logo with its TM x 0-0.21, y 0.74-1; the Rockstar badge x 0.94-1, y 0.9-1. A crop may touch
// neither. (The logo box was x1 0.19 = 547 px until review zoomed the corners: the TM ends near x 580 of
// 2880, so crops starting at x 560 and 576 passed the check with a sliver of the logo in them. 0.21 = 605.)
const MARKS = [
  { name: 'GTA V logo', x0: 0, x1: 0.21, y0: 0.74, y1: 1 },
  { name: 'Rockstar badge', x0: 0.94, x1: 1, y0: 0.9, y1: 1 },
];
const ART = {
  // the cast triptych: portrait crops on the faces
  'cast-michael': { file: 'official-artwork-michael-business.jpg', crop: [1128, 0, 1200, 1620] },
  // 60 px left of the face-centred crop: the centred one clipped a yellow-rimmed disc in its top right corner
  'cast-franklin': { file: 'official-artwork-franklin-and-chop.jpg', crop: [1068, 0, 1200, 1620] },
  'cast-trevor': { file: 'official-artwork-trevor-firestarter.jpg', crop: [754, 0, 1200, 1620] },
  // the servers: SOL roleplay (a traffic stop on Vinewood), LUNA freeroam (the boulevard at dusk; the
  // beach-weather backdrop it had read flat once greyed for "soon"), TERRA racing (Simeon's showroom)
  'server-sol': { file: 'official-artwork-stop-and-frisk.jpg', crop: [864, 0, 1843, 1620] },
  'server-luna': { file: 'official-artwork-starlet.jpg', crop: [620, 0, 2131, 1620] },
  'server-terra': { file: 'official-artwork-simeon.jpg', crop: [620, 0, 2131, 1620] },
};
const FPS = 30;
const W = 1920;
const H = 1080;

function findFfmpeg() {
  if (process.env.FFMPEG) return process.env.FFMPEG;
  const tc = path.join(REPO, '.toolchain', 'ffmpeg');
  for (const d of fs.existsSync(tc) ? fs.readdirSync(tc) : []) {
    const p = path.join(tc, d, 'bin', process.platform === 'win32' ? 'ffmpeg.exe' : 'ffmpeg');
    if (fs.existsSync(p)) return p;
  }
  return 'ffmpeg';
}
const FFMPEG = findFfmpeg();
const FFPROBE = FFMPEG.replace(/ffmpeg(\.exe)?$/i, (m, ext) => `ffprobe${ext ?? ''}`);

function run(args) {
  if (dry) { console.log(`  ffmpeg ${args.join(' ')}`); return; }
  const r = spawnSync(FFMPEG, ['-hide_banner', '-loglevel', 'error', '-y', ...args], { stdio: 'inherit' });
  if (r.error) throw r.error;
  if (r.status !== 0) throw new Error(`ffmpeg exited ${r.status}: ${args.join(' ')}`);
}

function probeSize(file) {
  const r = spawnSync(FFPROBE, ['-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=width,height', '-of', 'csv=p=0', file], { encoding: 'utf8' });
  const [w, h] = String(r.stdout).trim().split(',').map(Number);
  if (r.status !== 0 || !w || !h) throw new Error(`could not read the size of ${file}: ${r.stderr}`);
  return { w, h };
}

/** Refuses a crop that leaves the frame or touches a burned-in mark. */
function checkCrop(name, crop, size) {
  const [x, y, w, h] = crop;
  if (x < 0 || y < 0 || x + w > size.w || y + h > size.h) throw new Error(`${name}: crop ${crop.join(',')} leaves the ${size.w}x${size.h} frame`);
  for (const m of MARKS) {
    const hitsX = x < m.x1 * size.w && x + w > m.x0 * size.w;
    const hitsY = y < m.y1 * size.h && y + h > m.y0 * size.h;
    if (hitsX && hitsY) throw new Error(`${name}: crop ${crop.join(',')} touches the ${m.name}`);
  }
}

// The turnarounds are eased: over the first and the last EASE_S seconds of the forward half the shot
// slows to EASE_FLOOR of its speed (a linear speed ramp, time-remapped with setpts and filled in by
// blending neighbouring frames), so the camera never flips direction at full speed.
const EASE_S = 1.5;
const EASE_FLOOR = 0.15;

/** setpts expression (seconds -> seconds) that stretches both ends of an `len` s shot by the ramp. */
function easeExpr(len) {
  const D = EASE_S;
  const k = 1 - EASE_FLOOR;
  const S = (D / k) * Math.log(1 / EASE_FLOOR); // the stretched length of one eased stretch
  const a = `(${D / k})*log((${EASE_FLOOR}+${k}*T/${D})/${EASE_FLOOR})`; // speed rising from the floor to 1
  const b = `(${S}+T-${D})`;
  const c = `(${S}+${len - 2 * D}-(${D / k})*log(1-${k}*(T-${len - D})/${D}))`; // speed falling to the floor
  return `if(lt(T,${D}),${a},if(lt(T,${len - D}),${b},${c}))/TB`;
}

/** Step 1: the graded, eased forward half, as a near-lossless intermediate. */
function forwardArgs(c, out) {
  return ['-ss', String(c.in), '-t', String(c.len), '-i', path.join(SRC, `${c.file}.webm`),
    '-vf', `fps=${FPS},scale=${W}:${H}:flags=lanczos,format=yuv420p,setsar=1,${c.grade},format=yuv420p,` +
      `setpts=PTS-STARTPTS,setpts='${easeExpr(c.len)}',framerate=fps=${FPS}:interp_start=0:interp_end=255:scene=100,format=yuv420p`,
    '-c:v', 'libx264', '-preset', 'veryfast', '-crf', '8', '-an', out];
}

function countFrames(file) {
  if (dry) return 400;
  const r = spawnSync(FFPROBE, ['-v', 'error', '-count_frames', '-select_streams', 'v:0', '-show_entries', 'stream=nb_read_frames', '-of', 'csv=p=0', file], { encoding: 'utf8' });
  const n = Number(String(r.stdout).trim());
  if (r.status !== 0 || !Number.isInteger(n) || n < 60) throw new Error(`could not count the frames of ${file}: ${r.stderr}`);
  return n;
}

/** Step 2: forward, then backward without repeating either end frame: f0..fN-1, fN-2..f1, then the loop's f0. */
function boomerang(fwd, n) {
  return {
    inputs: ['-i', fwd],
    graph: [
      '[0:v]setpts=PTS-STARTPTS,split[f][rsrc]',
      `[rsrc]reverse,trim=start_frame=1:end_frame=${n - 1},setpts=PTS-STARTPTS[r]`,
      '[f][r]concat=n=2:v=1:a=0,format=yuv420p[v]',
    ].join(';'),
    frames: 2 * n - 2,
  };
}

// CRF 28: at 33 the one remaining loop averaged 651 kb/s and showed a block grid in the sky when upscaled
const vp9 = ['-c:v', 'libvpx-vp9', '-b:v', '0', '-crf', '28', '-g', '150', '-row-mt', '1', '-tile-columns', '2',
  '-auto-alt-ref', '1', '-lag-in-frames', '25', '-deadline', 'good',
  '-color_primaries', 'bt709', '-color_trc', 'bt709', '-colorspace', 'bt709', '-an'];
const h264 = ['-c:v', 'libx264', '-preset', 'slow', '-crf', '23', '-profile:v', 'high', '-pix_fmt', 'yuv420p',
  '-g', '150', '-movflags', '+faststart',
  '-color_primaries', 'bt709', '-color_trc', 'bt709', '-colorspace', 'bt709', '-an'];
// stills: high-quality WebP (the poster-grade 84 left banding in the skies)
const webp = (q) => ['-c:v', 'libwebp', '-quality', String(q), '-compression_level', '6'];

const missing = [
  ...Object.values(CLIPS).map((c) => path.join(SRC, `${c.file}.webm`)),
  ...Object.values(STILLS).map((s) => path.join(SRC, `${s.file}.webm`)),
  ...Object.values(ART).map((a) => path.join(ART_SRC, a.file)),
].filter((p) => !fs.existsSync(p));
if (missing.length) {
  console.error(`encode-media: ${missing.length} source file(s) missing:\n  ${missing.join('\n  ')}`);
  process.exit(1);
}
// every crop is checked before anything is written
for (const [name, a] of Object.entries(ART)) checkCrop(name, a.crop, probeSize(path.join(ART_SRC, a.file)));

fs.mkdirSync(OUT, { recursive: true });
const passDir = fs.mkdtempSync(path.join(os.tmpdir(), 'solar-encode-'));
try {
  for (const [name, c] of Object.entries(CLIPS)) {
    const webm = path.join(OUT, `${name}.webm`);
    const mp4 = path.join(OUT, `${name}.mp4`);
    const poster = path.join(OUT, `${name}.webp`);
    if (!force && [webm, mp4, poster].every((f) => fs.existsSync(f))) { console.log(`media: ${name} is there`); continue; }
    const fwd = path.join(passDir, `${name}-forward.mp4`);
    run(forwardArgs(c, fwd));
    const n = countFrames(fwd);
    const b = boomerang(fwd, n);
    const mapArgs = ['-filter_complex', b.graph, '-map', '[v]', '-frames:v', String(b.frames)];
    console.log(`media: ${name} = ${c.file} from ${c.in} s, ${c.len} s eased to ${(n / FPS).toFixed(2)} s, forward + back = ${(b.frames / FPS).toFixed(2)} s loop`);
    if (force || !fs.existsSync(webm)) {
      const log = path.join(passDir, name);
      run([...b.inputs, ...mapArgs, ...vp9, '-pass', '1', '-passlogfile', log, '-cpu-used', '4', '-f', 'null', '-']);
      run([...b.inputs, ...mapArgs, ...vp9, '-pass', '2', '-passlogfile', log, '-cpu-used', '1', webm]);
    }
    if (force || !fs.existsSync(mp4)) run([...b.inputs, ...mapArgs, ...h264, mp4]);
    if (force || !fs.existsSync(poster)) run(['-i', mp4, '-frames:v', '1', '-c:v', 'libwebp', '-quality', '84', poster]);
    fs.rmSync(fwd, { force: true });
  }
  for (const [name, s] of Object.entries(STILLS)) {
    const out = path.join(OUT, `still-${name}.webp`);
    if (!force && fs.existsSync(out)) { console.log(`media: still-${name} is there`); continue; }
    console.log(`media: still-${name} = ${s.file} at ${s.at} s`);
    run(['-ss', String(s.at), '-i', path.join(SRC, `${s.file}.webm`), '-frames:v', '1',
      '-vf', `scale=${W}:${H}:flags=lanczos,format=yuv444p,${s.grade},format=rgb24`, ...webp(90), out]);
  }
  for (const [name, a] of Object.entries(ART)) {
    const out = path.join(OUT, `${name}.webp`);
    if (!force && fs.existsSync(out)) { console.log(`media: ${name} is there`); continue; }
    const [x, y, w, h] = a.crop;
    console.log(`media: ${name} = ${a.file}, crop ${w}x${h} at ${x},${y}`);
    run(['-i', path.join(ART_SRC, a.file), '-vf', `crop=${w}:${h}:${x}:${y},format=rgb24`, ...webp(86), out]);
  }
} finally {
  fs.rmSync(passDir, { recursive: true, force: true });
}
console.log('encode-media: done');
