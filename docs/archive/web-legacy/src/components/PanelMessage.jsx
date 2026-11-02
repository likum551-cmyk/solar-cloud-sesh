import { Key, Spinner } from './ui.jsx';

/**
 * A panel's empty, busy or error body: the message, then its actions as menu
 * rows (white when picked, like every row), each with its key when it has
 * one. `busy` puts the game's spinner above the title: the one busy sign.
 */
export function PanelMessage({ title, detail, hint, actions = [], tone = 'plain', busy = false }) {
  return (
    <div className={`msg is-${tone}`} role={busy ? 'status' : undefined}>
      {busy && <Spinner className="msg-spin h-7 w-7" />}
      <h3 className="msg-title">{title}</h3>
      {detail && <p className="msg-detail">{detail}</p>}
      {hint && <p className="msg-hint">{hint}</p>}
      {actions.length > 0 && (
        <div className="msg-actions">
          {actions.map((a) => (
            <button key={a.label} type="button" className="menu-btn" onClick={a.onClick}>
              <span>{a.label}</span>
              {a.keyHint && <Key>{a.keyHint}</Key>}
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
