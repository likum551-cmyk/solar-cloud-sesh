import { forwardRef, useEffect, useMemo, useRef, useState } from 'react';
import { canPlayLoops, clipUrl, posterUrl, reelFor, REEL_FADE_MS, stillUrl, type ClipName, type StillName } from './catalog';

// The full-bleed picture behind every screen: a muted GTA V reel (Play, sign-in, Quests) or a
// GTA V still with a slow drift (Shop, Profile, Settings). A new scene fades in over the old one.
// A reel is two or three held shots from its lead clip (catalog.ts `reelFor`); near the end of each
// shot it sinks to the ground (`is-leaving`) as the next one rises over REEL_FADE_MS, and after
// the last the reel wraps to the lead, so no shot ever cuts or dissolves into itself. Once the
// lead plays, the layer hides its poster (`is-rolling`), so the ground under a pass is the ground.
//
// Shots are fetched whole and played from a blob: URL. The host serves assets as whole-file 200
// responses with no Range support (NATIVE-INTEGRATION.md), and a <video> over such a response
// is not seekable, so it cannot rely on looping back to the start; a blob always seeks.
// The video plays only while `active` (the same policy that pauses the emblem: hidden, idle and
// unfocused, launching, in game). Under reduced motion, or without a VP9 decoder, the
// poster (the lead shot's first frame) stands in for it.

/**
 * `frame`: where the footage sits in the window (CSS object-position, default centre), so a screen
 * can keep a shot's subject clear of its panel. `reel`: a screen's own sequence when the lead's
 * reel (catalog.ts `reelFor`, shared with Play) does not suit it; it starts with `clip`.
 */
export type BackdropScene = { clip: ClipName; frame?: string; reel?: readonly ClipName[] } | { still: StillName; drift?: boolean };

/** The same clip at another framing or in another reel is another picture (it fades in like a new scene). */
const sceneKey = (s: BackdropScene) => ('clip' in s ? `clip:${s.clip}@${s.frame ?? ''}/${s.reel?.join('+') ?? ''}` : `still:${s.still}`);

const blobs = new Map<string, Promise<string>>();
/** One fetch per loop per session; a failed fetch is forgotten so a later visit can retry. */
function loopBlob(url: string): Promise<string> {
  let p = blobs.get(url);
  if (!p) {
    p = fetch(url).then(async (r) => {
      if (!r.ok) throw new Error(`HTTP ${r.status} for ${url}`);
      return URL.createObjectURL(await r.blob());
    });
    p.catch(() => blobs.delete(url));
    blobs.set(url, p);
  }
  return p;
}

interface Props {
  scene: BackdropScene;
  active: boolean;
  reducedMotion: boolean;
  /** Called once, when the first picture has decoded (the boot cover lifts on it). */
  onReady?: () => void;
}

interface LayerEntry { id: number; scene: BackdropScene }

export const Backdrop = forwardRef<HTMLDivElement, Props>(function Backdrop({ scene, active, reducedMotion, onReady }, ref) {
  const key = sceneKey(scene);
  const nextId = useRef(1);
  const [layers, setLayers] = useState<LayerEntry[]>(() => [{ id: 0, scene }]);
  const readyOnce = useRef(false);

  useEffect(() => {
    setLayers((ls) => (ls.length && sceneKey(ls[ls.length - 1]!.scene) === key ? ls : [...ls, { id: nextId.current++, scene }]));
    // Keyed on `key` alone: a new scene object with the same key is the same picture.
  }, [key]);

  /** The top layer is on screen: drop the ones under it once its fade has finished. */
  const onShown = (id: number) => {
    if (!readyOnce.current) {
      readyOnce.current = true;
      onReady?.();
    }
    window.setTimeout(() => {
      setLayers((ls) => {
        const i = ls.findIndex((l) => l.id === id);
        return i > 0 ? ls.slice(i) : ls;
      });
    }, 900);
  };

  return (
    <div ref={ref} className={`backdrop ${active ? '' : 'is-paused'}`} aria-hidden>
      {layers.map((l, i) => (
        <Layer key={l.id} scene={l.scene} top={i === layers.length - 1} active={active} reducedMotion={reducedMotion} onShown={() => onShown(l.id)} />
      ))}
    </div>
  );
});

/** One shot of a reel on screen: `n` counts every shot the layer has started (React key). */
interface Shot { n: number; clip: ClipName; src: string }

function Layer({ scene, top, active, reducedMotion, onShown }: { scene: BackdropScene; top: boolean; active: boolean; reducedMotion: boolean; onShown: () => void }) {
  const [shown, setShown] = useState(false);
  const clip = 'clip' in scene ? scene.clip : null;
  const wantVideo = clip !== null && !reducedMotion && canPlayLoops();
  const drift = 'still' in scene && scene.drift === true && !reducedMotion;
  const shownOnce = useRef(false);
  // A layer's scene is fixed for its life (Backdrop keeps it in state), so this is computed once.
  const ownReel = 'clip' in scene ? scene.reel : undefined;
  const reel = useMemo(() => (clip === null ? [] : ownReel && ownReel[0] === clip ? ownReel : reelFor(clip)), [clip, ownReel]);
  // At most two shots are mounted: the one playing and, during a dissolve, the one fading in
  // over it. The older one is unmounted once the dissolve has finished.
  const [shots, setShots] = useState<Shot[]>([]);
  const [playingN, setPlayingN] = useState<ReadonlySet<number>>(() => new Set());
  const videos = useRef(new Map<number, HTMLVideoElement>());
  const handoff = useRef<number | null>(null);

  const show = () => {
    setShown(true);
    if (shownOnce.current) return;
    shownOnce.current = true;
    onShown();
  };

  // The lead shot.
  useEffect(() => {
    if (!wantVideo || reel.length === 0) return;
    let live = true;
    const lead = reel[0]!;
    loopBlob(clipUrl(lead)).then(
      (src) => { if (live) setShots([{ n: 0, clip: lead, src }]); },
      (e: unknown) => console.warn('[backdrop] loop unavailable, keeping its poster:', e),
    );
    return () => { live = false; };
  }, [wantVideo, reel]);

  const current = shots[shots.length - 1] ?? null;

  // Fetch the next shot while this one plays, so the dissolve never waits on the network.
  useEffect(() => {
    if (!current || reel.length < 2) return;
    const next = reel[(reel.indexOf(current.clip) + 1) % reel.length]!;
    loopBlob(clipUrl(next)).catch((e: unknown) => console.warn('[backdrop] next shot unavailable, holding this one:', e));
  }, [current, reel]);

  /** Near the end of the current shot: mount the next one; it fades in over REEL_FADE_MS. */
  const onTime = (shot: Shot, v: HTMLVideoElement) => {
    if (reel.length < 2 || shot !== current || handoff.current === shot.n) return;
    if (!Number.isFinite(v.duration) || v.currentTime < v.duration - REEL_FADE_MS / 1000) return;
    handoff.current = shot.n;
    const next = reel[(reel.indexOf(shot.clip) + 1) % reel.length]!;
    loopBlob(clipUrl(next)).then(
      (src) => {
        // The shot going out holds its last frame instead of wrapping to its first under the dissolve.
        v.loop = false;
        setShots((ss) => (ss[ss.length - 1] === shot ? [...ss.slice(-1), { n: shot.n + 1, clip: next, src }] : ss));
      },
      // Not fetched: this shot keeps looping, and the next pass through its end tries again.
      () => { handoff.current = null; },
    );
  };

  // Once the incoming shot is visibly playing and its dissolve has run, drop the outgoing one.
  useEffect(() => {
    if (shots.length < 2 || !current || !playingN.has(current.n)) return;
    const keep = current.n;
    const t = window.setTimeout(() => {
      setShots((ss) => ss.slice(-1));
      setPlayingN((p) => new Set([...p].filter((n) => n >= keep)));
    }, REEL_FADE_MS + 100);
    return () => window.clearTimeout(t);
  }, [shots, current, playingN]);

  // Play the mounted shots while the layer is the top one and rendering is active.
  useEffect(() => {
    for (const s of shots) {
      const v = videos.current.get(s.n);
      if (!v) continue;
      if (active && top) {
        v.muted = true;
        v.play().catch((e: unknown) => {
          // A pause right after play() rejects the play promise: expected, not a fault.
          if (!(e instanceof DOMException && e.name === 'AbortError')) console.warn('[backdrop] play failed:', e);
        });
      } else {
        v.pause();
      }
    }
  }, [active, top, shots]);

  return (
    <div className={`backdrop__layer ${shown ? 'is-shown' : ''} ${drift ? 'is-drift' : ''} ${playingN.size > 0 ? 'is-rolling' : ''}`} style={'frame' in scene && scene.frame ? { ['--frame' as string]: scene.frame } : undefined}>
      <img
        className="backdrop__img"
        src={clip !== null ? posterUrl(clip) : stillUrl((scene as { still: StillName }).still)}
        alt=""
        decoding="async"
        onLoad={show}
        onError={() => { console.warn('[backdrop] picture missing:', sceneKey(scene)); show(); }}
      />
      {shots.map((s) => (
        <video
          key={s.n}
          ref={(el) => { if (el) videos.current.set(s.n, el); else videos.current.delete(s.n); }}
          className={`backdrop__video ${s.n > 0 ? 'is-reel' : ''} ${playingN.has(s.n) ? 'is-playing' : ''} ${s !== current && current !== null && playingN.has(current.n) ? 'is-leaving' : ''}`}
          src={s.src}
          muted
          loop
          playsInline
          preload="auto"
          disablePictureInPicture
          onPlaying={() => setPlayingN((p) => (p.has(s.n) ? p : new Set(p).add(s.n)))}
          onTimeUpdate={(e) => onTime(s, e.currentTarget)}
        />
      ))}
    </div>
  );
}
