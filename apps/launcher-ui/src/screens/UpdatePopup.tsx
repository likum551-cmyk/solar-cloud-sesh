import { useEffect, useId, useRef, type CSSProperties } from 'react';
import { useApp } from '../state/AppState';
import { keyAction, nextFocus, scrimClickAction, type UpdateView } from '../state/updatePopup';
import { Chip, Code, Plate } from '../ui/primitives';

// The forced-update popup: an opaque chamfered panel centred over a flat veil, above the screen and
// the launch overlay and under the rail (so minimise and close-to-tray still work). What it shows
// and whether it can be dismissed is decided by state/updatePopup.ts; this file only draws it.
// While the update is mandatory there is no close control and Esc / a click outside do nothing.

const STEP_NAMES = ['Download', 'Verify', 'Install'] as const;
const FOCUSABLE = 'button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';

export function UpdateDialog({ view, onRetry, onDismiss }: { view: UpdateView; onRetry: () => void; onDismiss: () => void }) {
  const titleId = useId();
  const bodyId = useId();
  const panel = useRef<HTMLElement>(null);
  const { open, kind } = view;

  // Focus goes into the dialog when it opens and comes back to where it was when it closes.
  useEffect(() => {
    if (!open) return;
    const before = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    return () => before?.focus?.();
  }, [open]);
  // ...and follows the content: a Try again plate that appears takes focus; with no control the panel itself holds it.
  useEffect(() => {
    if (!open) return;
    const el = panel.current;
    if (!el) return;
    const first = el.querySelector<HTMLElement>(FOCUSABLE);
    (first ?? el).focus({ preventScroll: true });
  }, [open, kind]);

  // Esc and Tab, captured at the document so nothing behind the dialog sees them.
  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent) => {
      const a = keyAction(e.key, view);
      if (a === 'dismiss') { e.preventDefault(); e.stopPropagation(); onDismiss(); return; }
      if (a === 'swallow') { e.preventDefault(); e.stopPropagation(); return; }
      if (e.key !== 'Tab' || !panel.current) return;
      const els = Array.from(panel.current.querySelectorAll<HTMLElement>(FOCUSABLE));
      e.preventDefault();
      const next = nextFocus(els.indexOf(document.activeElement as HTMLElement), els.length, e.shiftKey);
      (next < 0 ? panel.current : els[next]!).focus({ preventScroll: true });
    };
    document.addEventListener('keydown', onKey, true);
    return () => document.removeEventListener('keydown', onKey, true);
  }, [open, view, onDismiss]);

  if (!open) return null;
  const alert = kind === 'failed';
  return (
    <div
      className={`upd upd--${kind}`}
      onMouseDown={(e) => { if (e.target === e.currentTarget && scrimClickAction(view) === 'dismiss') onDismiss(); }}
    >
      <section
        ref={panel}
        className="upd__panel"
        role="dialog"
        aria-modal="true"
        aria-labelledby={titleId}
        aria-describedby={bodyId}
        data-mandatory={view.mandatory ? 'true' : 'false'}
        tabIndex={-1}
      >
        <div className="upd__meta">
          {view.chip && <Chip tone={view.chip.tone}>{view.chip.label}</Chip>}
          {view.version && <Code>v{view.version}</Code>}
        </div>
        <h2 className="upd__title" id={titleId}>{view.title}</h2>
        <p className="upd__body" id={bodyId}>{view.body}</p>

        {view.steps && (
          <div className="upd__progress">
            <div className="upd__now">
              {view.percent !== null && <span className="upd__figure" aria-hidden>{view.percent}<small>%</small></span>}
              {view.status && <span className="upd__status" aria-hidden>{view.status}</span>}
            </div>
            <div
              className="upd__rail"
              role="progressbar"
              aria-label="Update progress"
              aria-valuemin={0}
              aria-valuemax={100}
              aria-valuenow={view.valueNow ?? undefined}
              aria-valuetext={view.announce}
            >
              {view.steps.map((st, i) => (
                <div key={STEP_NAMES[i]} className={`upd__step is-${st}${st === 'now' && view.progress === null ? ' is-sweep' : ''}`}>
                  <span className="upd__seg" style={st === 'now' && view.progress !== null ? ({ '--fill': view.progress } as CSSProperties) : undefined} />
                  <span className="upd__segname">{STEP_NAMES[i]}</span>
                </div>
              ))}
            </div>
          </div>
        )}

        {view.detail && <p className="upd__detail">{view.detail}</p>}

        {(view.actions.retry || view.actions.close) && (
          <div className="upd__actions">
            {view.actions.retry && <Plate tone="primary" busy={view.busy} onClick={onRetry}>Try again</Plate>}
            {view.actions.close && (
              <Plate tone={view.actions.retry ? 'quiet' : 'primary'} onClick={onDismiss}>{view.actions.retry ? 'Close' : 'OK'}</Plate>
            )}
          </div>
        )}

        {/* One polite line for screen readers: the download moves in 10% steps, a failure is an alert. */}
        <p className="sr-only" role={alert ? 'alert' : 'status'} aria-live={alert ? 'assertive' : 'polite'}>{view.announce}</p>
      </section>
    </div>
  );
}

/** The popup wired to the app: shown whenever the host reports a required update or refuses PLAY with 14 / 15. */
export function UpdatePopup() {
  const { updateView, retryUpdate, dismissUpdate } = useApp();
  return <UpdateDialog view={updateView} onRetry={retryUpdate} onDismiss={dismissUpdate} />;
}
