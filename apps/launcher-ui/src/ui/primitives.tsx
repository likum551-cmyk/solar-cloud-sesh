import { forwardRef, useId, type ButtonHTMLAttributes, type InputHTMLAttributes, type ReactNode } from 'react';
import lockupFullSvg from '../brand/solar-rp-horizontal-noon-full.svg?raw';
import lockupInkSvg from '../brand/solar-rp-horizontal-noon-ink.svg?raw';
import { LOCKUP_RATIO, MARK_LOWER, MARK_UPPER } from '../brand/brand';

// The component kit laid over the footage: chamfered plates (a 45-degree top-right cut),
// fields, toggles, small caps labels and status chips. Solar gold is reserved for the one active
// choice on a screen (PLAY, the chosen card, the active tab, focus) and the logo's one gold hook.

/** The Solar RP S mark (2D, 1:2). `size` is its height. `full`: ink upper hook, gold lower hook;
 *  `ink`: both ink, for footage without a scrim behind it. */
export function GageMark({ size = 24, tone = 'full', title }: { size?: number; tone?: 'full' | 'ink'; title?: string }) {
  return (
    <svg className="mark" width={size / 2} height={size} viewBox="0 0 50 100" role={title ? 'img' : undefined} aria-label={title} aria-hidden={title ? undefined : true}>
      <path fill="var(--ink)" d={MARK_UPPER} />
      <path fill={tone === 'full' ? 'var(--sun)' : 'var(--ink)'} d={MARK_LOWER} />
    </svg>
  );
}

// The kit's SVGs carry their own <title>, role and label; the wrapping span names the lockup once.
const bare = (svg: string) => svg.replace(/<title>[\s\S]*?<\/title>/, '').replace(/ role="img"/, ' aria-hidden="true"').replace(/ aria-label="[^"]*"/, '');
const LOCKUP = { full: bare(lockupFullSvg), ink: bare(lockupInkSvg) };

/** The horizontal Solar RP lockup from the brand kit, inlined (tight bounds: `height` is the letters'
 *  height). `height` is px, or any CSS length when the lockup scales with the window. */
export function Lockup({ height = 20, tone = 'full' }: { height?: number | string; tone?: 'full' | 'ink' }) {
  const width = typeof height === 'number' ? height * LOCKUP_RATIO : `calc(${height} * ${LOCKUP_RATIO})`;
  return <span className="lockup" style={{ height, width }} role="img" aria-label="Solar RP" dangerouslySetInnerHTML={{ __html: LOCKUP[tone] }} />;
}

type IconName = 'min' | 'max' | 'close' | 'arrow' | 'external' | 'refresh' | 'folder' | 'check' | 'back' | 'alert';
/** Authored line icons, 16px grid, 1.5px stroke. */
export function Icon({ name, size = 16 }: { name: IconName; size?: number }) {
  const d: Record<IconName, ReactNode> = {
    // the caption glyphs: drawn at a 1px stroke (.win-btn .icon) on half-pixel lines, 10px across,
    // so every straight edge lands on whole pixels
    min: <path d="M3.5 8.5h9" />,
    max: <path d="M3.5 3.5h9v9h-9z" />,
    close: <path d="M3.5 3.5l9 9M12.5 3.5l-9 9" />,
    arrow: <path d="M3 8h9.5M9 4.5 12.5 8 9 11.5" />,
    back: <path d="M13 8H3.5M7 4.5 3.5 8 7 11.5" />,
    external: <path d="M9.5 3H13v3.5M13 3 7.5 8.5M11 9.5V13H3V5h3.5" />,
    refresh: <path d="M12.5 6.5A4.75 4.75 0 1 0 12 10.6M12.8 3v3.6H9.2" />,
    folder: <path d="M2.5 4.5h4l1.5 1.5h5.5v6.5h-11z" />,
    check: <path d="M3.5 8.5 6.5 11.5 12.5 4.5" />,
    alert: <path d="M8 2.5 14 13H2zM8 6.5v3M8 11v.5" />,
  };
  return (
    <svg className="icon" width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth={1.5} strokeLinecap="square" aria-hidden>
      {d[name]}
    </svg>
  );
}

export type PlateTone = 'primary' | 'ghost' | 'quiet' | 'danger';

/** A chamfered plate button. `primary` is Solar gold with dark text (the one active choice on a screen). */
export const Plate = forwardRef<HTMLButtonElement, ButtonHTMLAttributes<HTMLButtonElement> & { tone?: PlateTone; size?: 'sm' | 'md' | 'lg' | 'xl'; busy?: boolean }>(
  function Plate({ tone = 'ghost', size = 'md', busy = false, className = '', children, disabled, ...rest }, ref) {
    return (
      <button
        ref={ref}
        type="button"
        className={`plate plate--${tone} plate--${size} ${busy ? 'is-busy' : ''} ${className}`}
        disabled={disabled || busy}
        aria-busy={busy || undefined}
        {...rest}
      >
        <span className="plate__label">{children}</span>
      </button>
    );
  },
);

/** A small caps label: field names, regions, units. */
export function Code({ children, tone = 'dim', className = '' }: { children: ReactNode; tone?: 'dim' | 'ink'; className?: string }) {
  return <span className={`code code--${tone} ${className}`}>{children}</span>;
}

export type ChipTone = 'done' | 'pending' | 'void' | 'demo' | 'warn' | 'plain';

/** A status chip: order and item states, demo labels, roles. Straight-edged, never rotated. */
export function Chip({ children, tone = 'plain', className = '' }: { children: ReactNode; tone?: ChipTone; className?: string }) {
  return (
    <span className={`chip chip--${tone} ${className}`}>
      {tone === 'done' && <Icon name="check" size={12} />}
      {children}
    </span>
  );
}

/** Live state as a dot: green only for online. */
export function StatusDot({ state }: { state: 'online' | 'offline' | 'unknown' | 'maintenance' }) {
  return <span className={`dot dot--${state}`} aria-hidden />;
}

/** A labelled text field. Errors are announced and tied to the input. */
export function Field({ label, error, hint, id, className = '', ...rest }: InputHTMLAttributes<HTMLInputElement> & { label: string; error?: string | null; hint?: ReactNode }) {
  const auto = useId();
  const fid = id ?? auto;
  return (
    <label className={`field ${error ? 'has-error' : ''} ${className}`} htmlFor={fid}>
      <span className="field__label">{label}</span>
      <input id={fid} className="field__input" aria-invalid={error ? true : undefined} aria-describedby={error ? `${fid}-err` : undefined} {...rest} />
      {error ? <span className="field__error" id={`${fid}-err`} role="alert">{error}</span> : hint ? <span className="field__hint">{hint}</span> : null}
    </label>
  );
}

/** A two-state switch drawn as a chamfered slot. */
export function Toggle({ label, checked, onChange, hint, disabled }: { label: string; checked: boolean; onChange: (v: boolean) => void; hint?: string; disabled?: boolean }) {
  const id = useId();
  return (
    <div className="toggle">
      <div className="toggle__text">
        <label htmlFor={id} className="toggle__label">{label}</label>
        {hint && <span className="toggle__hint">{hint}</span>}
      </div>
      <button id={id} type="button" role="switch" aria-checked={checked} disabled={disabled} className={`toggle__switch ${checked ? 'is-on' : ''}`} onClick={() => onChange(!checked)}>
        <span className="toggle__knob" aria-hidden />
        {/* The switch's name comes from its <label>; the state is aria-checked. */}
        <span className="toggle__opt toggle__opt--off" aria-hidden>OFF</span>
        <span className="toggle__opt toggle__opt--on" aria-hidden>ON</span>
      </button>
    </div>
  );
}
