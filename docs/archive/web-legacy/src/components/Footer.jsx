import { IconAlert, IconCheck } from './icons.jsx';
import { Key, Spinner } from './ui.jsx';

/**
 * The menu's foot, as in the game: what is happening on the left (the busy
 * spinner and its words), the instructional buttons on the right. Each hint
 * is also a button, as the game's are on PC.
 */
export default function Footer({ status, hints }) {
  return (
    <footer className="foot app-no-drag">
      {/* one persistent live region: text swaps inside it so changes are announced */}
      <div className="foot-status" role="status" aria-live="polite">
        {status && (
          <>
            {status.icon === 'busy' && <Spinner className="h-[18px] w-[18px]" />}
            {status.icon === 'ok' && <IconCheck className="h-[18px] w-[18px] text-[var(--gold)]" />}
            {status.icon === 'stop' && <IconAlert className="h-[18px] w-[18px] text-[var(--red-text)]" />}
            <span className="truncate">{status.text}</span>
          </>
        )}
      </div>
      <div className="hints">
        {hints.map((h) => {
          const body = (
            <>
              {h.keys.map((k) => (
                <Key key={k}>{k}</Key>
              ))}
              <span>{h.label}</span>
            </>
          );
          // a hint with nothing to click (a key that acts on the focused row) is a label
          return h.onClick ? (
            <button key={h.label} type="button" className="hint" onClick={h.onClick} disabled={h.disabled}>
              {body}
            </button>
          ) : (
            <span key={h.label} className="hint">
              {body}
            </span>
          );
        })}
      </div>
    </footer>
  );
}
