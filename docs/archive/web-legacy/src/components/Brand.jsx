import lockupSrc from '../assets/brand/gage-mp-lockup-colour-on-dark.svg';

// gage-mp-logo/svg/gage-mp-lockup-colour-on-dark.svg: the freemode-blue mark
// and the white wordmark, viewBox 290.24 x 64.
const LOCKUP_RATIO = 290.24 / 64;

/** The GAGE-MP lockup at `height` px. Decorative: pair it with real text for assistive tech. */
export function Lockup({ height, className = '' }) {
  return (
    <img
      className={`lockup ${className}`}
      src={lockupSrc}
      width={Math.round(height * LOCKUP_RATIO)}
      height={height}
      alt=""
      draggable={false}
    />
  );
}
