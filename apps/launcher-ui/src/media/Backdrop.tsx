import { forwardRef, useEffect, useRef, useState } from 'react';
import { canPlayLoops, clipUrl, posterUrl, stillUrl, type ClipName, type StillName } from './catalog';

// The full-bleed picture behind every screen: a muted GTA V loop (Play, sign-in) or a GTA V
// still with a slow drift (Shop, Profile, Settings). A new scene fades in over the old one.
//
// Loops are fetched whole and played from a blob: URL. The host serves assets as whole-file 200
// responses with no Range support (NATIVE-INTEGRATION.md), and a <video> over such a response
// is not seekable, so it cannot rely on looping back to the start; a blob always seeks.
// The video plays only while `active` (the same policy that pauses the emblem: hidden, idle and
// unfocused, launching, in game). Under reduced motion, or without an H.264 decoder, the
// poster (the loop's first frame) stands in for it.

export type BackdropScene = { clip: ClipName } | { still: StillName; drift?: boolean };

const sceneKey = (s: BackdropScene) => ('clip' in s ? `clip:${s.clip}` : `still:${s.still}`);

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

function Layer({ scene, top, active, reducedMotion, onShown }: { scene: BackdropScene; top: boolean; active: boolean; reducedMotion: boolean; onShown: () => void }) {
  const [shown, setShown] = useState(false);
  const [src, setSrc] = useState<string | null>(null);
  const [playing, setPlaying] = useState(false);
  const video = useRef<HTMLVideoElement>(null);
  const clip = 'clip' in scene ? scene.clip : null;
  const wantVideo = clip !== null && !reducedMotion && canPlayLoops();
  const drift = 'still' in scene && scene.drift === true && !reducedMotion;
  const shownOnce = useRef(false);

  const show = () => {
    setShown(true);
    if (shownOnce.current) return;
    shownOnce.current = true;
    onShown();
  };

  useEffect(() => {
    if (!wantVideo || clip === null) return;
    let live = true;
    loopBlob(clipUrl(clip)).then(
      (u) => { if (live) setSrc(u); },
      (e: unknown) => console.warn('[backdrop] loop unavailable, keeping its poster:', e),
    );
    return () => { live = false; };
  }, [wantVideo, clip]);

  useEffect(() => {
    const v = video.current;
    if (!v || !src) return;
    if (active && top) {
      v.muted = true;
      v.play().catch((e: unknown) => {
        // A pause right after play() rejects the play promise: expected, not a fault.
        if (!(e instanceof DOMException && e.name === 'AbortError')) console.warn('[backdrop] play failed:', e);
      });
    } else {
      v.pause();
    }
  }, [active, top, src]);

  return (
    <div className={`backdrop__layer ${shown ? 'is-shown' : ''} ${drift ? 'is-drift' : ''}`}>
      <img
        className="backdrop__img"
        src={clip !== null ? posterUrl(clip) : stillUrl((scene as { still: StillName }).still)}
        alt=""
        decoding="async"
        onLoad={show}
        onError={() => { console.warn('[backdrop] picture missing:', sceneKey(scene)); show(); }}
      />
      {src && (
        <video
          ref={video}
          className={`backdrop__video ${playing ? 'is-playing' : ''}`}
          src={src}
          muted
          loop
          playsInline
          preload="auto"
          disablePictureInPicture
          onPlaying={() => setPlaying(true)}
        />
      )}
    </div>
  );
}
