// Encodes the site's footage from the owner's fresh_videos/ recordings (one continuous 15 s, 1920x1080
// AV1 drone shot each; the folder is never committed, only these derivatives are).
//
// Every clip is ONE shot played forward and then backward (a boomerang), so it loops without a seam
// and without a dissolve: no double exposure, no jump, and the 3D S always stands in the plate its
// lighting was taken from. Every clip gets the same warm grade (the NOON direction: golden haze, slate
// water, no lavender); shots that are already warm get a lighter one.
//
// Outputs in public/media, per clip: <name>.webm (VP9, two-pass), <name>.mp4 (H.264, +faststart, for
// browsers without VP9 WebM) and <name>.webp (the poster: frame 0, also the S's light for hero / close).
//   node scripts/encode-media.mjs            encode what is missing
//   node scripts/encode-media.mjs --force    encode everything again
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
const OUT = path.join(SITE, 'public', 'media');
const force = process.argv.includes('--force');
const dry = process.argv.includes('--dry-run');

// The shared grade: blue and magenta out of the highlights, warm mid-tones, shadows kept neutral (a
// shadow push turned the canal water olive). Measured on a frame of each shot: the skyline's sky went
// from RGB 199 187 212 (lavender) to 208 195 172, the canal's water stays slate.
const WARM = 'colorbalance=rs=0.015:gs=-0.015:bs=-0.015:rm=0.06:gm=0.01:bm=-0.07:rh=0.06:gh=0.04:bh=-0.2,eq=saturation=1.05';
// the street block by day is already warm (frame mean 167 141 111): the full grade turned it mustard
const WARM_LIGHT = 'colorbalance=rm=0.02:bm=-0.025:rh=0.02:bh=-0.06';

// in-points match apps/launcher-ui/ASSETS.md; `len` is the forward half (the loop is twice that)
const CLIPS = {
  hero: { file: 'fon_25m38', in: 0.2, len: 10, grade: WARM }, // downtown skyline at dusk, the city lit
  beach: { file: 'fon_4m05', in: 0.2, len: 10, grade: WARM }, // Vespucci Beach at sunset, the pier wheel
  streets: { file: 'fon_29m37', in: 0.2, len: 10, grade: WARM_LIGHT }, // a city block straight down, by day
  canal: { file: 'fon_1m46', in: 0.2, len: 10, grade: WARM }, // down the river canal to Downtown, golden hour
  close: { file: 'fon_20m27', in: 0.2, len: 10, grade: WARM }, // the Galileo Observatory at night, city lit
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

function run(args) {
  if (dry) { console.log(`  ffmpeg ${args.join(' ')}`); return; }
  const r = spawnSync(FFMPEG, ['-hide_banner', '-loglevel', 'error', '-y', ...args], { stdio: 'inherit' });
  if (r.error) throw r.error;
  if (r.status !== 0) throw new Error(`ffmpeg exited ${r.status}: ${args.join(' ')}`);
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
  const probe = FFMPEG.replace(/ffmpeg(\.exe)?$/i, (m, ext) => `ffprobe${ext ?? ''}`);
  const r = spawnSync(probe, ['-v', 'error', '-count_frames', '-select_streams', 'v:0', '-show_entries', 'stream=nb_read_frames', '-of', 'csv=p=0', file], { encoding: 'utf8' });
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

const vp9 = ['-c:v', 'libvpx-vp9', '-b:v', '0', '-crf', '33', '-g', '150', '-row-mt', '1', '-tile-columns', '2',
  '-auto-alt-ref', '1', '-lag-in-frames', '25', '-deadline', 'good',
  '-color_primaries', 'bt709', '-color_trc', 'bt709', '-colorspace', 'bt709', '-an'];
const h264 = ['-c:v', 'libx264', '-preset', 'slow', '-crf', '23', '-profile:v', 'high', '-pix_fmt', 'yuv420p',
  '-g', '150', '-movflags', '+faststart',
  '-color_primaries', 'bt709', '-color_trc', 'bt709', '-colorspace', 'bt709', '-an'];

const missing = Object.values(CLIPS).map((c) => path.join(SRC, `${c.file}.webm`)).filter((p) => !fs.existsSync(p));
if (missing.length) {
  console.error(`encode-media: ${missing.length} source file(s) missing:\n  ${missing.join('\n  ')}`);
  process.exit(1);
}
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
  // SOL's server card: the launcher's Downtown still, in the same grade as the footage
  const solSrc = path.join(REPO, 'apps', 'launcher-ui', 'public', 'assets', 'stills', 'downtown.webp');
  const sol = path.join(OUT, 'sol.webp');
  if (!fs.existsSync(solSrc)) throw new Error(`missing ${solSrc}`);
  if (force || !fs.existsSync(sol)) {
    console.log('media: sol.webp (graded downtown still)');
    run(['-i', solSrc, '-vf', `format=yuv444p,${WARM},format=rgb24`, '-c:v', 'libwebp', '-quality', '84', sol]);
  }
} finally {
  fs.rmSync(passDir, { recursive: true, force: true });
}
console.log('encode-media: done');
