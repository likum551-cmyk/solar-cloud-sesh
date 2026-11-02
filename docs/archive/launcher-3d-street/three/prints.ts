// Prints of the 3D frame for the DOM: the contact-strip thumbnails and the frozen frame on the
// launch log. The stage registers the function (it owns the composer and the canvas); the shell
// calls it without importing the 3D chunk.

/**
 * A crop of the canvas, centred on the frame's subject (the stage projects it): `w` is the
 * width as a fraction (0..1) of the canvas. The height follows from the output aspect, so the
 * print is never stretched; the window is pushed back inside the canvas at the edges.
 */
export interface Crop { w: number }

type Snapshot = (crop: Crop, outW: number, outH: number) => string | null;

let snapshot: Snapshot | null = null;

export function registerSnapshot(fn: Snapshot | null): void {
  snapshot = fn;
}

/** A WebP data URL of the current 3D frame (post-processed), or null before the stage exists. */
export function takeSnapshot(crop: Crop, outW: number, outH: number): string | null {
  try {
    return snapshot ? snapshot(crop, outW, outH) : null;
  } catch (err) {
    console.warn('[prints] snapshot failed:', err);
    return null;
  }
}

/** Half the canvas wide, around the subject: the strip frames and the launch print. */
export const SUBJECT_CROP: Crop = { w: 0.5 };

/**
 * The source rectangle of a crop of `w` x (w * srcW * outH / outW) around the subject at
 * canvas fraction (u, v), clamped inside the canvas.
 */
export function cropRect(srcW: number, srcH: number, crop: Crop, outW: number, outH: number, u: number, v: number): { sx: number; sy: number; sw: number; sh: number } {
  const sw = Math.min(srcW, srcW * crop.w);
  const sh = Math.min(srcH, sw * (outH / outW));
  const clamp = (x: number, max: number) => Math.max(0, Math.min(max, x));
  return { sx: clamp(u * srcW - sw / 2, srcW - sw), sy: clamp(v * srcH - sh / 2, srcH - sh), sw, sh };
}
