import { useEffect, useRef, useState } from 'react';
import { displayName, serverKey } from '../lib/format.js';
import bandStill from '../assets/freeway/band-still.webp';
import { FreewayScene } from '../scene/freeway.js';
import {
  SIGN_CANVAS_H,
  SIGN_CANVAS_W,
  drawSign,
  loadSignFaces,
  signContent,
  signFacesReady,
} from '../scene/signs.js';
import { Pricedown } from './gta.jsx';

// Unfocused for this long (and not interacting) => stop rendering.
const IDLE_PAUSE_MS = 20000;
// how long a failed launch holds the grey frame and its title
const WASTED_MS = 2200;

function promptFor(status, count) {
  if (status === 'loading') return { kind: 'prompt', title: 'Fetching servers', sub: 'Reading the server list' };
  // `aim: 'right'` points the sign's arrow down-right, at direct connect
  if (status === 'error') return { kind: 'prompt', title: 'Server list offline', sub: 'Connect directly, or retry the list', aim: 'right' };
  if (count === 0) return { kind: 'prompt', title: 'No servers listed', sub: 'Connect directly with an address', aim: 'right' };
  return { kind: 'prompt', title: 'Choose a server', sub: `${count} ${count === 1 ? 'destination' : 'destinations'} ahead` };
}

function usePrefersReducedMotion() {
  const [reduced, setReduced] = useState(
    () => typeof window !== 'undefined' && window.matchMedia('(prefers-reduced-motion: reduce)').matches,
  );
  useEffect(() => {
    const mq = window.matchMedia('(prefers-reduced-motion: reduce)');
    const on = () => setReduced(mq.matches);
    mq.addEventListener('change', on);
    return () => mq.removeEventListener('change', on);
  }, []);
  return reduced;
}

/**
 * The night-freeway band. `phase` drives the camera: 'idle', 'launching'
 * (swing onto the off-ramp, then freeze so the game gets the GPU), 'done'
 * (stay frozen until the window is focused again), 'failed' (drive back).
 */
export default function Hero({ selected, pingState, servers, status, phase }) {
  const canvasRef = useRef(null);
  const sceneRef = useRef(null);
  const [failed, setFailed] = useState(false);
  const [ready, setReady] = useState(false);
  const reduced = usePrefersReducedMotion();
  const holdRef = useRef(false); // launch hold: never render while the game boots
  const exitRef = useRef(null); // the running skycam move, if any
  const [returned, setReturned] = useState(false);
  useEffect(() => setReturned(false), [phase]);
  // a failed launch gets the game's "wasted" beat: the frame drains to grey
  // under a red title, then colour returns on the closed exit
  const [wasted, setWasted] = useState(false);
  useEffect(() => {
    if (phase !== 'failed') return undefined;
    setWasted(true);
    const t = window.setTimeout(() => setWasted(false), WASTED_MS);
    return () => {
      window.clearTimeout(t);
      setWasted(false);
    };
  }, [phase]);
  const lastKey = useRef(null);
  const applyRunning = useRef(() => {});
  // bumped when a lost WebGL context comes back: the scene is rebuilt
  const [gen, setGen] = useState(0);

  // Create the scene once; recreate if reduced-motion flips or the GPU
  // context was lost and restored.
  useEffect(() => {
    const canvas = canvasRef.current;
    let scene;
    try {
      // No GPU or a blocked context throws here; the band then shows a
      // designed fallback plate instead of going blank, for good.
      scene = new FreewayScene(canvas, { reducedMotion: reduced });
    } catch (err) {
      console.error('freeway scene failed', err);
      setFailed(true);
      return undefined;
    }
    sceneRef.current = scene;
    setFailed(false);

    const parent = canvas.parentElement;
    const ro = new ResizeObserver(() => scene.setSize(parent.clientWidth, parent.clientHeight));
    ro.observe(parent);

    let idleTimer = 0;
    let unfocusedIdle = false;
    const run = () => {
      const visible = document.visibilityState === 'visible';
      if (visible && !holdRef.current && !unfocusedIdle && !reduced) scene.start();
      else scene.stop();
    };
    applyRunning.current = run;
    const onVis = () => run();
    const onBlur = () => {
      clearTimeout(idleTimer);
      idleTimer = setTimeout(() => {
        unfocusedIdle = true;
        run();
      }, IDLE_PAUSE_MS);
    };
    const onFocus = () => {
      clearTimeout(idleTimer);
      unfocusedIdle = false;
      run();
    };
    // A GPU reset (likeliest while the game boots) loses the context: show
    // the fallback until the browser hands it back, then rebuild the scene.
    const onRestored = () => setGen((g) => g + 1);
    const onLost = () => {
      setFailed(true);
      canvas.addEventListener('webglcontextrestored', onRestored, { once: true });
    };
    document.addEventListener('visibilitychange', onVis);
    window.addEventListener('blur', onBlur);
    window.addEventListener('focus', onFocus);
    canvas.addEventListener('freeway-lost', onLost);

    let cancelled = false;
    // Sign legends are canvas text: wait for the bundled face first, and for
    // the sourced palms and textures (scene.ready never rejects: it falls back).
    Promise.all([loadSignFaces(), scene.ready])
      .then(() => {
        if (cancelled) return;
        setReady(true);
        run();
      });

    return () => {
      cancelled = true;
      clearTimeout(idleTimer);
      ro.disconnect();
      document.removeEventListener('visibilitychange', onVis);
      window.removeEventListener('blur', onBlur);
      window.removeEventListener('focus', onFocus);
      canvas.removeEventListener('freeway-lost', onLost);
      canvas.removeEventListener('webglcontextrestored', onRestored);
      scene.dispose();
      sceneRef.current = null;
      lastKey.current = null;
      setReady(false);
    };
  }, [reduced, gen]);

  // Sign content: the selected server, or a prompt that tells the truth
  // about the list (loading, offline, empty). A failed launch closes the exit.
  // (as in the list's PingCell, a measured ping always wins over "no reply")
  const content = selected
    ? { ...signContent(selected), closed: phase === 'failed', noReply: pingState === 'failed' }
    : promptFor(status, servers.length);
  const key = selected ? serverKey(selected) : `prompt:${content.title}`;
  const contentSig = JSON.stringify(content);

  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene || !ready) return undefined;
    let alive = true;
    const paint = () => {
      if (!alive || sceneRef.current !== scene) return;
      // a new server turns the next sign's slats; anything else repaints in place
      if (lastKey.current === null || lastKey.current === key || !selected) {
        scene.redrawSign(content);
      } else {
        scene.setSign(content);
      }
      lastKey.current = key;
    };
    // a legend in a script not seen yet (Cyrillic) waits the few ms its face takes
    if (signFacesReady(content)) paint();
    else loadSignFaces(content).then(paint);
    return () => {
      alive = false;
    };
    // contentSig carries the value identity of content
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, key, contentSig]);

  // Launch choreography and the render hold.
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene || !ready) return undefined;
    let alive = true;
    // Hold rendering only once the skycam has finished rising, whichever
    // phase we are in by then (a fast launch can finish mid-move).
    const holdAfterExit = () =>
      (exitRef.current || Promise.resolve()).then(() => {
        if (!alive) return;
        holdRef.current = true;
        applyRunning.current();
      });
    if (phase === 'launching') {
      holdRef.current = false;
      applyRunning.current();
      exitRef.current = scene.takeExit();
      holdAfterExit();
    } else if (phase === 'done') {
      holdAfterExit();
      // back in the launcher later: drop back down onto the freeway
      const onFocus = () => {
        alive = false;
        exitRef.current = null;
        holdRef.current = false;
        setReturned(true);
        applyRunning.current();
        scene.returnFromExit();
      };
      window.addEventListener('focus', onFocus, { once: true });
      return () => {
        alive = false;
        window.removeEventListener('focus', onFocus);
      };
    } else {
      exitRef.current = null;
      holdRef.current = false;
      applyRunning.current();
      if (scene.exit > 0) scene.returnFromExit();
    }
    return () => {
      alive = false;
    };
  }, [phase, ready]);

  const frozen = phase === 'launching' || (phase === 'done' && !returned);

  return (
    <div className={`hero ${frozen ? 'is-frozen' : ''} ${ready ? 'is-ready' : ''} ${wasted ? 'is-wasted' : ''}`} aria-hidden="true">
      <canvas ref={canvasRef} className="hero-canvas" />
      {failed && <FallbackBand content={content} contentSig={contentSig} />}
      <div className="hero-shade" />
      {/* the game's "mission passed" shard, over the skycam's held frame */}
      {phase === 'done' && !returned && (
        <div className="shard">
          <Pricedown k="launched" size={58} className="shard-title" />
          {selected && <span className="shard-sub">Joining {displayName(selected)}</span>}
        </div>
      )}
      {wasted && (
        <div className="shard is-failed">
          <Pricedown k="failed" size={58} className="shard-title" />
        </div>
      )}
    </div>
  );
}

// The fallback still (assets/freeway/band-still.webp): the reduced-motion
// frame rendered at 1680 x 380 CSS px with every exit plate hidden, and the
// near plate's corners in it (TL, TR, BR, BL) as the scene's camera projects
// them. Re-render both together if the camera or gantry changes (ASSETS.md).
const STILL = {
  w: 1680,
  h: 380,
  quad: [
    [627.7, 47.72],
    [889.22, 46.04],
    [889.41, 177.72],
    [626.88, 178.24],
  ],
};

/**
 * No WebGL, or a lost context not yet restored: the band's own night frame,
 * with the sign drawn by the same drawSign() as the 3D plate and pinned onto
 * the plate's place in the frame, so every state (EXIT CLOSED, the offline
 * arrow, NO REPLY) reads the same as it does on the gantry.
 */
function FallbackBand({ content, contentSig }) {
  const boxRef = useRef(null);
  const signRef = useRef(null);
  const [transform, setTransform] = useState('');

  useEffect(() => {
    const box = boxRef.current;
    // the still is laid out like the canvas: object-fit cover, centred
    const fit = () => {
      const w = box.clientWidth;
      const h = box.clientHeight;
      const s = Math.max(w / STILL.w, h / STILL.h);
      const ox = (w - STILL.w * s) / 2;
      const oy = (h - STILL.h * s) / 2;
      setTransform(quadTransform(SIGN_CANVAS_W, SIGN_CANVAS_H, STILL.quad.map(([x, y]) => [ox + x * s, oy + y * s])));
    };
    fit();
    const ro = new ResizeObserver(fit);
    ro.observe(box);
    return () => ro.disconnect();
  }, []);

  useEffect(() => {
    let alive = true;
    // the legend is canvas text: the bundled faces its text needs first, as on the 3D plate
    loadSignFaces(content).then(() => {
      if (alive && signRef.current) drawSign(signRef.current, content);
    });
    return () => {
      alive = false;
    };
    // contentSig carries the value identity of content
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [contentSig]);

  return (
    <div ref={boxRef} className="hero-fallback">
      <img className="hero-fallback-art" src={bandStill} alt="" decoding="async" />
      <canvas
        ref={signRef}
        className="hero-fallback-sign"
        width={SIGN_CANVAS_W}
        height={SIGN_CANVAS_H}
        style={{ transform, visibility: transform ? 'visible' : 'hidden' }}
      />
    </div>
  );
}

// CSS matrix3d taking a w x h box (origin top left) onto a quad
// [TL, TR, BR, BL]: the square-to-quad projective map (Heckbert), scaled
// from the unit square to the box.
function quadTransform(w, h, quad) {
  const [[x0, y0], [x1, y1], [x2, y2], [x3, y3]] = quad;
  const dx1 = x1 - x2;
  const dx2 = x3 - x2;
  const dx3 = x0 - x1 + x2 - x3;
  const dy1 = y1 - y2;
  const dy2 = y3 - y2;
  const dy3 = y0 - y1 + y2 - y3;
  const den = dx1 * dy2 - dx2 * dy1;
  const g = (dx3 * dy2 - dx2 * dy3) / den;
  const k = (dx1 * dy3 - dx3 * dy1) / den;
  const a = x1 - x0 + g * x1;
  const b = x3 - x0 + k * x3;
  const d = y1 - y0 + g * y1;
  const e = y3 - y0 + k * y3;
  // column-major: x' = (a/w x + b/h y + x0) / (g/w x + k/h y + 1), same for y'
  const m = [a / w, d / w, 0, g / w, b / h, e / h, 0, k / h, 0, 0, 1, 0, x0, y0, 0, 1];
  return `matrix3d(${m.map((v) => +v.toFixed(9)).join(',')})`;
}
