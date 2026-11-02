// The GAGE-MP mark, copied from the brand kit (brand/svg/gage-mp-mark-*.svg, 64x64 viewBox):
// an oblique, chamfered plate with the G cut out. One source of truth for the 2D mark (GageMark)
// and the extruded 3D emblem (three/Emblem.tsx), so both stay on the mark's construction grid.

/** A polygon as [x, y] pairs in the mark's 64-unit design grid, y pointing down (SVG space). */
export type Poly = ReadonlyArray<readonly [number, number]>;

/** Outer plate: `M16 4H58V50L48 60H6V14Z` (two 45-degree chamfers, top-left and bottom-right). */
export const MARK_PLATE: Poly = [
  [16, 4], [58, 4], [58, 50], [48, 60], [6, 60], [6, 14],
];

/** The G letterform: `M21 10H49V19H24V45H40V36H31V28H49V48L43 54H15V16Z`. */
export const MARK_G: Poly = [
  [21, 10], [49, 10], [49, 19], [24, 19], [24, 45], [40, 45], [40, 36], [31, 36],
  [31, 28], [49, 28], [49, 48], [43, 54], [15, 54], [15, 16],
];

/** The mark's oblique: `matrix(1 0 -0.1405 1 4.5 0)`, i.e. x' = x - 0.1405 * y + 4.5. */
export const MARK_SKEW = -0.1405;
export const MARK_SKEW_OFFSET = 4.5;

/** Apply the mark's skew to one design-grid point. */
export function skewPoint([x, y]: readonly [number, number]): [number, number] {
  return [x + MARK_SKEW * y + MARK_SKEW_OFFSET, y];
}

/** SVG path data (already skewed) for a polygon, for inline <svg> rendering. */
export function polyPath(poly: Poly): string {
  return poly.map((p, i) => `${i === 0 ? 'M' : 'L'}${skewPoint(p).map((v) => +v.toFixed(3)).join(' ')}`).join('') + 'Z';
}

/** The chamfer angle every plate, mask and bevel in the UI is cut with (the mark's corners). */
export const CHAMFER_DEG = 45;
