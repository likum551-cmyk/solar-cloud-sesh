// The Solar RP brand art (NOON recolour, brand/solar/solar-rp-noon in the repo). The mark is an S built
// from two hooks on a 50x100 grid: the upper hook is ink, the lower (shadow) hook is the lockup's one
// gold element. The horizontal lockup puts that S in front of OLAR RP on a 430x100 grid, tight bounds.
//
// The files in this folder named gage-mp-* and mark.ts are the retired GAGE-MP "G"; only the 3D emblem
// (three/, behind EMBLEM_3D) still reads them, and it stays off: the new mark has no 3D version.
// The engine's in-game overlay has its own switch: kUseBrandArt in apps/engine/overlay_internal.h.
export const EMBLEM_3D = false;

/** The S mark's two hooks (viewBox 0 0 50 100). */
export const MARK_UPPER = 'M0 0L41 0L50 9L50 31L32 31L32 18L18 18L18 41L30 41L12 59L0 59Z';
export const MARK_LOWER = 'M50 100L9 100L0 91L0 69L18 69L18 82L32 82L32 59L20 59L38 41L50 41Z';

/** Width over height of the horizontal lockup (430x100). */
export const LOCKUP_RATIO = 4.3;
